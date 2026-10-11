#ifndef ARIA_ARIA_EXE_CONTEXT_HPP
#define ARIA_ARIA_EXE_CONTEXT_HPP

#include <format>

#include "bytecode/CodeUnit.hpp"
#include "common.hpp"
#include "error/Error.hpp"
#include "memory/Buffer.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "runtime/CallFrame.hpp"
#include "runtime/ExecState.hpp"
#include "runtime/FrameStack.hpp"
#include "runtime/str_table.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    class AriaVM;
    class ObjClass;
    class ObjClosure;
    class ObjFunction;
    class ObjMovement;
    class ObjString;
    class ObjUpvalue;

    // fail 的返回哨兵,失败出口惯用法一行 `return fail(...);`。**Opt<T> 转换只对「不可由 bool
    // 构造」的 T 成立**--标量经 operator bool() 会构造出已初始化的 0/false 把 fail 吞成成功,
    // 标量返回型站点改用 bool + 出参(先例:StringClass 算子钩子契约返 bool、结果写返回槽)。
    struct FailSignal {
        operator bool() const noexcept { return false; }
        template<typename T>
        operator T*() const noexcept {
            return nullptr;
        }
        template<typename T>
        operator Opt<T>() const noexcept {
            return std::nullopt;
        }
    };

    // 执行上下文本体:一段执行的完整状态 -- 可增长值栈 + 帧栈 + open upvalue 链 + 挂起错误
    // 寄存器 + resume 链 + 执行状态 + 嵌套执行边界;三类指针(top_/活动帧 slots/开指 location_)
    // 在栈增长时按「搬运前记槽偏移、搬运后新基址重建」重绑。原生函数与 Object 协议面向本面:
    // 报错族写本上下文自己的寄存器(执行中的 ctx 恒 == current_,写点即调用方上下文),常量串
    // 与七个 bootstrap 类访问器经私有 host_ 回指针委托宿主。GC 对象壳 ObjMovement(唯一派生,
    // 构造私有 + 友元钉死)把本面包成 GC 一等值,主上下文与协程统一本型。
    class AriaExeContext {
    public:
        static constexpr usize kStackInit = 1024; // 值栈初始容量(Value 槽,NaN-boxing 8KB/TagValue 16KB);不足时 2x 增长
        static constexpr usize kFrameMax  = 256;  // 调用帧容量
        static constexpr u32   kMaxRecursionDepth = 32; // 嵌套执行上限(判定直接比上限)

        AriaExeContext(const AriaExeContext&)            = delete;
        AriaExeContext& operator=(const AriaExeContext&) = delete;

        // 值栈(热路径裸指针)

        [[nodiscard]]
        Value* stack_base() noexcept {
            return buf_.data();
        }

        [[nodiscard]]
        Value* stack_top() const noexcept {
            return top_;
        }

        [[nodiscard]]
        usize stack_size() const noexcept {
            return static_cast<usize>(top_ - buf_.data());
        }

        [[nodiscard]]
        usize stack_capacity() const noexcept {
            return buf_.capacity();
        }

        // 压栈:先写值、再按需 2x 增长 -- value 先入活跃区随值栈根存活(与「栈即根」一致;当前
        // grow 永不触 GC,此序为前瞻防御)。
        void push(const Value value) noexcept {
            *top_++ = value;
            if (top_ == buf_.data() + buf_.capacity()) {
                grow_stack_();
            }
        }

        [[nodiscard]]
        Value pop() noexcept {
            ASSERT(top_ > buf_.data(), "value stack underflow");
            return *--top_;
        }

        void drop(const usize n) noexcept {
            ASSERT(n <= stack_size(), "drop exceeds stack size");
            top_ -= n;
        }

        // 距栈顶 n 个槽(0 = 栈顶),可写引用。
        [[nodiscard]]
        Value& peek(const usize n) const noexcept {
            ASSERT(n < stack_size(), "peek beyond stack size");
            return *(top_ - 1 - n);
        }

        // 异常派发:回退到第 n 帧(含)并转入 record 的 catch handler;全帧未命中不走本函数
        //(交 reset())。truncate 一步弃内层帧(栈顶未动,被弃槽区全存活);close_upvalues
        //(catch 槽) 关掉其上开指后,值栈顶截到 catch 参数槽、置 ip 跳 record.handle,寄存器
        // 载荷 push 落槽(取走后至 push 无分配,不失根)。
        void unwind_to_handler(const usize n, const TryRecord& record) noexcept {
            ASSERT(n < frames_.size(), "frame index out of range");
            frames_.truncate(n + 1);
            CallFrame& frame      = frames_.top();
            const auto catch_slot = frame.slots + record.stack_depth;
            close_upvalues(catch_slot);
            set_stack_top_(catch_slot);
            frame.ip = frame.unit->code.data() + record.handle;
            push(*take_error());
        }

        // 帧栈

        [[nodiscard]]
        FrameStack<CallFrame, kFrameMax>& frames() noexcept {
            return frames_;
        }

        [[nodiscard]]
        bool frames_full() const noexcept {
            return frames_.size() >= kFrameMax;
        }

        // 进帧:为对 closure 的调用 acquire 一个空帧并就位字段,与 exit_frame 成对,锁「栈顶帧
        // slots 即值栈本帧槽 0」不变量;slots = top - argc - 1(栈顶须形如 [callee, a1..aN]),
        // 槽 0 语义由调用方进帧前写定(普通帧 = 闭包自身 / 方法帧 = this)。定义在 .cpp。
        void enter_frame(ObjClosure* closure, u8 argc);

        // open upvalue 开链:链头按槽址降序,所在区间被关闭时(RETURN/unwind/CLOSE_UPVALUE)
        // 摘链迁值;链上节点经本对象 trace 标根 -- 防「闭包已死而 upvalue 仍在链」悬垂。
        // 「同一局部只有一份引用」不变式由 capture_upvalue 单点收口。

        // 捕获:沿降序链一趟完成查等值与插链点定位,等值即复用(同槽双节点也复用,自愈)。
        // 建新到插链之间无分配点(白色窗口不被 sweep),入链后即随本对象 trace 根化;定义在 .cpp。
        [[nodiscard]]
        ObjUpvalue* capture_upvalue(Value* slot) noexcept;

        // 关闭所有指向 >= from 槽址的开指:值迁入各自 closed_ 并整段摘链;降序不变式下 >= from
        // 恒为链头连续前缀。
        void close_upvalues(const Value* from) noexcept;

        // 只读链头(trace 遍历标根用;节点 next 经 ObjUpvalue::next_open)。返非 const 指针:
        // mark_object(Object*) 需要可变指针;遍历只读,不改链。
        [[nodiscard]]
        ObjUpvalue* open_upvalues() const noexcept {
            return open_upvalues_;
        }

        // 出帧:关本帧区间开指(被捕获局部随帧销毁,值须在槽区存活时迁入 upvalue 自持)+ 弹帧
        // + 值栈顶复位到本帧 slots,与 enter_frame 成对;须先取 slots 再操作 -- frames_.top()
        // 引用在 pop 后悬垂。
        void exit_frame() noexcept {
            Value* slots = frames_.top().slots;
            close_upvalues(slots); // 关本帧区间开指(值迁入自持;槽区此刻仍存活,迁值读安全)
            frames_.pop();
            set_stack_top_(slots);
        }

        // 一步退到 depth:一链扫关 depth 之上全部开指 + 裸截帧栈 + 值栈顶复位,等价于逐帧
        // exit_frame 但只扫一次链;depth >= 帧数时为无操作。
        void truncate_frames_to(const usize depth) noexcept {
            if (depth >= frames_.size()) {
                return;
            }
            Value* slots = frames_[depth].slots;
            close_upvalues(slots); // 边界 = 首个被弃帧的槽 0:之上全弃,之下(保留帧)不受扰
            frames_.truncate(depth);
            set_stack_top_(slots);
        }

        // 清空值栈/帧栈/挂起错误(容量保留)。先关全部开指再清场 -- 否则链上残留开指跨 run 复用
        // 同一栈区继续指入,经闭包读出脏值。不碰 previous_/state_:那是切换簿记,归切换点处置。
        void reset() noexcept {
            close_upvalues(buf_.data());
            top_ = buf_.data();
            frames_.clear();
            pending_error_.reset();
        }

        // 挂起错误寄存器(侧信道):冷路径错误经 raise 写入,VM 在安全点 has_error() 后
        // take_error() 传播,寄存器随上下文走。载荷为 Value -- VM/原生错误装 ObjException、
        // aria throw 原值入(catch 绑原值保类型);置入后随本对象 trace 标根,取出前跨安全点
        // 分配不回收。
        void raise(const Value err) noexcept {
            ASSERT(!pending_error_.has_value(), "pending error already set (take/clear before re-raise)");
            pending_error_ = err;
        }

        [[nodiscard]]
        bool has_error() const noexcept {
            return pending_error_.has_value();
        }

        [[nodiscard]]
        Opt<Value> take_error() noexcept {
            return util::take(pending_error_);
        }

        // 挂起载荷的只读引用(为空态时无值)。供 trace 标根用(take_error 取走会清空,
        // 不能经它只读查询)。
        [[nodiscard]]
        const Opt<Value>& pending_error() const noexcept {
            return pending_error_;
        }

        // 报错族:从零构造消息一步烘齐(Error::make_message 无位置版 + new_exception),消息
        // **不含位置前缀**(位置由 unwind 未捕获出口的逐帧 at 行给出);装箱分配可能触 GC,载荷
        // 构造后立即入寄存器(tracer 已标根)。fail = raise + 恒返失败信号,[[nodiscard]] 强制
        // `return ctx.fail(...);`。
        template<typename... Args>
        void raise(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            const auto msg = Error::make_message(code, std::format(fmt, std::forward<Args>(args)...));
            raise(Value::from_obj(new_exception(gc(), code, msg)));
        }

        template<typename... Args>
        [[nodiscard]]
        FailSignal fail(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            raise(code, fmt, std::forward<Args>(args)...);
            return FailSignal{};
        }

        // 元数报错的唯一口(「function expects <spec>, got <n>」措辞家族):三形态三名各对应一种
        // 措辞;比较留在各调用点,仅失败路径进本口。被调者报泛称 function 不收名字 -- 真名在
        // 一等公民下不可靠(匿名无区分力、被取出后真名不出现在调用点),at 行已交待调用点。
        [[nodiscard]]
        FailSignal arity_error(u32 argc, u32 expected);

        [[nodiscard]]
        FailSignal arity_error_range(u32 argc, u32 low, u32 high);

        [[nodiscard]]
        FailSignal arity_error_at_least(u32 argc, u32 low);

        // 调用整形面(call_closure 与 coroutine.resume 首启共用;体在本类,失败写本上下文寄存器)。

        // 元数检查:普通函数区间 [min_arity, arity](差额为带默认值参数),有缺省报区间文案、无缺省
        // 报单数;varargs 只保下界,多余实参由 prepare_call_args 打包。
        [[nodiscard]]
        bool check_arity(const ObjFunction* fn, u8 argc);

        // 实参整形:把本上下文栈顶从实参深度整形成帧参数槽深并返回槽深。①缺省垫充:未传的固定
        // 参数槽压缺省印章(体局部槽号按满参编,参数槽 1..n、体局部自 n+1 起);②varargs 打包:
        // 多余实参整段收集为新 list 压 rest 槽(帧槽深 = arity + 1,无多余实参铸空表 -- rest 恒为
        // list 非 nil)。
        [[nodiscard]]
        u8 prepare_call_args(const ObjFunction* fn, u8 argc);

        // 重入口面(嵌套执行同步执行闭包/方法,两口共用 Eval::execute_call;失败载荷留本上下文
        // 寄存器、残帧截净,调用方按 bool 契约交外层续搜;仅可在执行中的驱动内调)。

        // 闭包形态:closure 自身落调用区槽 0(兼返回槽),args 按声明序压其上。
        [[nodiscard]]
        Opt<Value> execute_closure(ObjClosure* closure, Span<const Value> args);

        // 方法形态:receiver 落调用区槽 0(帧槽 0 = this,兼返回槽),method 元数口径 = 声明参数数。
        [[nodiscard]]
        Opt<Value> execute_method(Value receiver, ObjClosure* method, Span<const Value> args);

        // 协程状态与 resume 链:previous_ = 「谁恢复了我」,自 current_ 沿链回走,链尾恒为主
        // 上下文;切换收口在 AriaVM,本类不自切。resume 先置链、yield / RETURN 完成一律解链,
        // 挂起态 previous_ 恒 nullptr。
        [[nodiscard]]
        ObjMovement* previous() const noexcept {
            return previous_;
        }

        // 链接/重链(resume 方向:置恢复者)。
        void set_previous(ObjMovement* prev) noexcept { previous_ = prev; }

        // 取走恢复者并断链(yield / RETURN 完成解链的唯一出口,维持挂起态 previous_ 恒 nullptr;
        // 与 take_error 同族 -- 挂起状态的取走并置空收口)。
        [[nodiscard]]
        ObjMovement* take_previous() noexcept {
            return util::take(previous_);
        }

        // 执行状态(status 直接投影,不做谓词派生)。主上下文的 state_ 无人读(见 ExecState 注)。
        [[nodiscard]]
        ExecState state() const noexcept {
            return state_;
        }

        void set_state(const ExecState state) noexcept { state_ = state; }

        // 驱动循环出口边界与递归深度(嵌套执行簿记)。

        [[nodiscard]]
        u32 ret_frame() const noexcept {
            return ret_frame_;
        }

        [[nodiscard]]
        u32 recursion_depth() const noexcept {
            return recursion_depth_;
        }

        // 递归进场:换防本循环返回边界并取走外层值,递归深度 +1。须在 push 占位 callee 之后、
        // call_closure 进帧之前调用(此刻 frames_.size() 即重入点边界)。
        [[nodiscard]]
        u32 enter_recursion() noexcept {
            const u32 old_ret = ret_frame_;
            ret_frame_        = frames_.size();
            ASSERT(ret_frame_ != 0, "recursion entry with zero boundary (outermost execution does not recurse)");
            ++recursion_depth_;
            return old_ret;
        }

        // 递归退场:归还外层返回边界,递归深度 -1(与 enter_recursion 配对)。外层边界零值
        // 合法:嵌套挂在最外执行(主上下文)下时 saved 即为 0。
        void leave_recursion(const u32 saved_ret) noexcept {
            ret_frame_ = saved_ret;
            --recursion_depth_;
        }

        // 常量串按注册表键直读:键 -> 下标的解析在编译期完成(注册表住 runtime/str_table.hpp),
        // 下标经 str(index) 取表。本访问口不依赖宿主完整类型,使用方无须 include AriaVM.hpp。
        template<util::FixedString key>
        [[nodiscard]]
        ObjString* str() const noexcept {
            constexpr auto found = str_table::index_of(key.view());
            static_assert(found.has_value(), "constant string key is not in the registry");
            return str(*found);
        }

        // 常量串按下标直读(体在 .cpp,委托宿主表;str<key> 的运行期下标形态)。
        [[nodiscard]]
        ObjString* str(usize index) const noexcept;

        // 单字符常量串直读(byte -> 字节值即下标,启动期 bootstrap 满表)。
        [[nodiscard]]
        ObjString* char_string(u8 byte) const noexcept;

        // 七个 bootstrap 类的委托读取(体在 .cpp)。
        [[nodiscard]]
        ObjClass* object_class() const noexcept;

        [[nodiscard]]
        ObjClass* exception_class() const noexcept;

        [[nodiscard]]
        ObjClass* iterator_class() const noexcept;

        [[nodiscard]]
        ObjClass* list_class() const noexcept;

        [[nodiscard]]
        ObjClass* map_class() const noexcept;

        [[nodiscard]]
        ObjClass* string_class() const noexcept;

        [[nodiscard]]
        ObjClass* range_class() const noexcept;

        // 宿主访问口:驱动循环与切换原语的 AriaVM 面经此取用(eval 层消费)。
        [[nodiscard]]
        AriaVM& host() const noexcept {
            return *host_;
        }

        // 宿主分配器。
        [[nodiscard]]
        GC& gc() const noexcept;

    protected:
        // protected 非虚析构:上下文的删除唯一出口 = GC 摘链经 Object*(首基类虚析构),虚化只会
        // 给每个上下文多付一张 vptr;protected 把经本面的误删封死为编译错。
        ~AriaExeContext() = default; // buf_ 经自持 GC* 释放值栈(同 ObjString long_chars_ 先例);frames_
                                     // UPtr 自释放;open upvalue 节点是 GC 对象,归 GC 管。

    private:
        // 唯一派生 ObjMovement 经多继承收编本面;构造私有结构性钉死唯一派生。
        friend class ObjMovement;

        explicit AriaExeContext(GC* gc, AriaVM* host) noexcept :
            host_{host}, buf_{gc, kStackInit}, top_{buf_.data()}, frames_{}, open_upvalues_{nullptr}, pending_error_{},
            previous_{nullptr}, state_{ExecState::Suspended}, ret_frame_{0}, recursion_depth_{0} {}

        // 截断栈顶到 t(t 须在 [base, top] 内);值栈顶复位由本类内部独占,不对外暴露。
        void set_stack_top_(Value* t) noexcept {
            ASSERT(t >= buf_.data() && t <= top_, "stack top out of range");
            top_ = t;
        }

        // 值栈 2x 扩容 + 三类指针重绑(top_ / 各活动帧 slots / open upvalue 链 location_)。
        // 定义在 .cpp(需 ObjUpvalue 完整类型);重绑纪律与 UB 说明见其定义处注释。
        void grow_stack_() noexcept;

        using CallFrameStack = FrameStack<CallFrame, kFrameMax>;

        // 宿主回指针(借用,不 trace 不标根):服务面经它委托宿主公有面;生命周期 = 宿主,
        // 恒晚于本对象(GC 对象随 ~GC 释放)销毁。不得对外暴露。
        AriaVM*        host_;
        Buffer<Value>  buf_;             // 值栈缓冲底座(GC 分配,可增长)
        Value*         top_;             // 栈顶(下一空闲槽;增长后由 grow_stack_ 重定位)
        CallFrameStack frames_;          // 调用帧栈(一次分配定容,指针绝对稳定)
        ObjUpvalue*    open_upvalues_;   // open upvalue 开链头(按槽址降序;nullptr 空链)
        Opt<Value>     pending_error_;   // 挂起错误寄存器(报错族写入;置入后随本对象 trace 标根)
        ObjMovement*   previous_;        // resume 链:恢复者上下文(主上下文恒 nullptr 链尾)
        ExecState      state_;           // 执行状态(五态;主上下文亦参与换位但无人读)
        u32            ret_frame_;       // 返回边界:驱动循环出口帧深(0 = 最外执行)
        u32            recursion_depth_; // 嵌套重入深度,达 kMaxRecursionDepth 即拒
    };

} // namespace aria

#endif // ARIA_ARIA_EXE_CONTEXT_HPP
