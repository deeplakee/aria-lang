#ifndef ARIA_OBJ_MOVEMENT_HPP
#define ARIA_OBJ_MOVEMENT_HPP

#include "bytecode/CodeUnit.hpp"
#include "common.hpp"
#include "memory/Buffer.hpp"
#include "memory/GC.hpp"
#include "object/Object.hpp"
#include "runtime/FrameStack.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    class ObjClosure;
    class ObjFunction;
    class ObjModule;
    class ObjUpvalue;

    // 调用帧(trivially-copyable 聚合,满足 FrameStack 约束,同 LineEntry 风格不带尾下划线)。
    // ip/last_ip 仅作读取游标/锚点,运行期从不经其写字节;last_ip 反推 offset 前提:帧存活期间
    // code 缓冲恒定 -- 非移动 GC + 执行期零 emit。
    struct CallFrame {
        ObjClosure* closure; // 持闭包(callable 收敛为闭包,顶层入口也闭包);元数据经其 function() 取
        CodeUnit*   unit;    // 缓存 closure->function()->unit(),省每条指令一跳
        ObjModule*  module;  // 缓存 closure->function()->module(),供 *_GLOBAL 定位模块 globals
        u8*         ip;      // 指令指针(冷路径按需反推 offset)
        Value*      slots;   // 本帧局部基址(callee 槽 0,参数槽 1..)
        u8*         last_ip; // 最近取指指令起始(循环顶写;行号/unwind 查表锚点)
    };

    // 执行上下文:一段执行的完整状态(Object 子类 + 可增长值栈 + 帧栈)。设计见 .claude/reference/runtime/vm-design.md。
    //   - Object 子类(ObjType::MOVEMENT):主上下文与协程统一为本类型 GC 对象。trace 自标值栈已用
    //     区间/活动帧/open upvalue 开链/挂起错误寄存器,并经 mark_object(previous_) 沿 resume 链
    //     级联;VM 根 tracer 只标 current_ 一点 -- 链遍历删去后,previous_ 是挂起中 resumer 的
    //     唯一可达边,不标即悬垂。
    //   - 值栈走 Buffer<Value> 底座(GC 分配),push 溢出 2x 增长;top_/活动帧 slots/open upvalue
    //     location_ 三类指入值栈的指针在增长时按「搬运前记槽偏移、搬运后新基址重建」重绑。
    //   - 帧栈走 FrameStack 模板(槽位语义,truncate 供异常 unwind 跨帧);UPtr 底座走 std 分配器,
    //     不进 GC 字节账。
    class ObjMovement final : public Object {
    public:
        static constexpr usize kStackInit = 1024; // 值栈初始容量(Value 槽,NaN-boxing 8KB/TagValue 16KB);不足时 2x 增长
        static constexpr usize kFrameMax  = 256;  // 调用帧容量

        explicit ObjMovement(GC* gc) noexcept :
            Object{ObjType::MOVEMENT}, buf_{gc, kStackInit}, top_{buf_.data()}, frames_{}, open_upvalues_{nullptr},
            previous_{nullptr} {}

        ~ObjMovement() override = default; // buf_ 经自持 GC* 释放值栈(同 ObjString long_chars_ 先例);frames_ UPtr
                                           // 自释放;open upvalue 节点是 GC 对象,归 GC 管。

        ObjMovement(const ObjMovement&)            = delete;
        ObjMovement& operator=(const ObjMovement&) = delete;
        ObjMovement(ObjMovement&&)                 = delete;
        ObjMovement& operator=(ObjMovement&&)      = delete;

        // 清空值栈与帧栈与挂起错误(容量保留,不缩回初始)。先关全部开指再清场:HALT 收场不弹帧,
        // 若无此安全网,链上残留的开指会跨 run 复用同一栈区继续指入(槽值被下一轮覆写),再经闭包
        // 读出脏值。
        void reset() noexcept {
            close_upvalues(buf_.data());
            top_ = buf_.data();
            frames_.clear();
            pending_error_.reset();
        }

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

        // 压栈:先写值、再按需 2x 增长 -- 使 value 先入活跃区随值栈根存活,与「栈即根」纪律一致
        // (当前 grow 永不触 GC,此序为前瞻防御)。进 push 时必有空槽(top_ < base + cap 为
        // 不变式,见构造/reset/grow 的维持),先写不越界。
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

        // 异常派发:回退到第 n 帧(含)并把该帧转入 record 的 catch handler。frames_.truncate
        // 一步弃内层帧(此时栈顶未动,被弃槽区全存活);close_upvalues(catch 槽) 按槽址一关到底
        // -- >= catch 槽的开指一并迁移关闭,catch 槽本身尚无开指、可捕获性保留;值栈顶再截到
        // catch 参数槽,置 ip 跳 record.handle,寄存器载荷 push 落槽(取走后至 push 无分配,不失根)。
        // 全帧未命中不走本函数,交 reset()。
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

        // 进帧:为对 closure 的调用 acquire 一个空帧并就位全部字段,与 exit_frame 成对,锁住
        // 「栈顶帧 slots 即值栈本帧槽 0」不变量。slots 按不变量设为 top - argc - 1(栈顶须形如
        // [callee, a1..aN]);槽 0 语义由调用方在进帧前写定:普通帧 = 闭包自身,方法帧 = this
        // (闭包经 frame.closure 携带不上栈,两态共用本入口)。定义在 .cpp(需 ObjClosure 完整类型)。
        void enter_frame(ObjClosure* closure, u8 argc);

        // open upvalue 开链
        // 链头 open_upvalues_:本上下文全部 open 态 upvalue,按槽址降序(head 最高);局部所在
        // 区间被关闭时(RETURN/unwind/显式 CLOSE_UPVALUE)摘链迁值。链上节点经本类 trace 标根
        // -- 防「闭包已死而 upvalue 仍在链」悬垂。「同一局部只有一份引用」不变式由 capture_upvalue
        // 单点收口(命中复用或建新插链,无旁路)。

        // 捕获:沿降序链一趟完成查等值与插链点定位,等值即复用(内外层共享同一 ObjUpvalue)、更小/
        // 链尾则建新并在该处插链;即使链上出现同槽双节点(不变式被破)也命中复用(自愈)。建新到插链
        // 之间无分配点(白色窗口不被 sweep),入链后即随本对象 trace 根化。定义在 .cpp(需 ObjUpvalue 完整类型)。
        [[nodiscard]]
        ObjUpvalue* capture_upvalue(GC& gc, Value* slot) noexcept;

        // 关闭所有指向 >= from 槽址的开指:值迁入各自 closed_(close),并整段摘链。挂点:exit_frame
        // 内置(本帧区间;RETURN 经此)/显式 CLOSE_UPVALUE(top - 1)/unwind_to_handler(catch 槽)/
        // reset(全链)。降序不变式下 >= from 恒为链头连续前缀。
        void close_upvalues(const Value* from) noexcept;

        // 只读链头(trace 遍历标根用;节点 next 经 ObjUpvalue::next_open)。返非 const 指针:
        // mark_object(Object*) 需要可变指针;遍历只读,不改链。
        [[nodiscard]]
        ObjUpvalue* open_upvalues() const noexcept {
            return open_upvalues_;
        }

        // 出帧:关本帧区间开指 + 弹当前帧 + 值栈顶复位到该帧 slots 基址,与 enter_frame 成对。
        // close_upvalues(frame.slots) 内置于此:本帧被捕获局部随帧销毁,值须在槽区仍存活时迁入
        // 各自 upvalue 自持。先取 slots 再 close/pop:frames_.top() 引用 pop 后悬垂,不可先 pop 再读。
        void exit_frame() noexcept {
            Value* slots = frames_.top().slots;
            close_upvalues(slots); // 关本帧区间开指(值迁入自持;槽区此刻仍存活,迁值读安全)
            frames_.pop();
            set_stack_top_(slots);
        }

        // 挂起错误寄存器(侧信道)
        // 原生函数等冷路径错误不走返回类型,经 raise 写入本寄存器;VM 在 CALL 等安全点检查
        // has_error() 后用 take_error() 取出传播。寄存器随上下文走。载荷为 Value:VM/原生错误
        // 装箱 ObjException 后写入,aria throw 原值入寄存器(catch 绑原值保类型);置入后随本对象
        // trace 标根,取出前跨安全点分配不回收。raise 断言当前无挂起(防嵌套 raise 未取走就再
        // raise);reset() 一并清空。

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

        // 协程 resume 链
        // previous_ = 「谁恢复了我」:A resume B 即 B->previous_ 置 A、VM 的 current_ 换指 B;
        // 自 current_ 沿 previous_ 回走即 resume 链,链尾恒为主上下文。切换收口在 AriaVM
        // (current_),ObjMovement 不自切;切换原语落地前链长恒 1,字段为契约占位。挂起态
        // previous_ 恒 nullptr(yield/RETURN 完成/未捕获跳链三处切换点一律解链)。标根经 trace
        // 尾部的 mark_object(previous_) 级联。
        [[nodiscard]]
        ObjMovement* previous() const noexcept {
            return previous_;
        }

        // 链接/重链(resume 方向:置恢复者)。
        void set_previous(ObjMovement* prev) noexcept { previous_ = prev; }

        // Object 协议(声明序随 Object.hpp)。

        // GC 标记:值栈已用区间、各活动帧 closure/module、open upvalue 开链(「闭包已死而
        // upvalue 仍在链」的悬垂防线)、挂起错误寄存器;尾部 mark_object(previous_) 沿 resume 链
        // 级联(mark_object 容 nullptr 且幂等)。定义在 .cpp(帧成员 mark_object 的基类转换需
        // ObjClosure/ObjModule 完整类型)。
        void trace(GC& gc) const noexcept override;

        // 壳字节数(不含值栈/帧数组等子内存:值栈由 ~ObjMovement 经自持 GC* 释放,帧数组由 UPtr 自释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjMovement);
        }

        // 调试渲染(惰性契约见 Object::debug_repr):<coroutine>。
        [[nodiscard]]
        String debug_repr() const override {
            return "<coroutine>";
        }

    private:
        // 就位一帧:slots 指向槽 0,VM 专有字段(closure/unit/module/ip)从 closure 解引用填充。
        // 与 enter_frame 分工:enter_frame 管 acquire,此函数管填字段;槽 0 语义见 enter_frame 注释。
        void init_frame_(CallFrame& f, ObjClosure* closure, u8 argc) const;

        // 截断栈顶到 t(t 须在 [base, top] 内)。值栈顶复位由 ObjMovement 内部独占(exit_frame /
        // unwind_to_handler / reset),不对外暴露,收紧「值栈顶只由 ObjMovement 自身改」的边界。
        void set_stack_top_(Value* t) noexcept {
            ASSERT(t >= buf_.data() && t <= top_, "stack top out of range");
            top_ = t;
        }

        // 值栈 2x 扩容 + 三类指针重绑(top_ / 各活动帧 slots / open upvalue 链 location_)。
        // 定义在 .cpp(需 ObjUpvalue 完整类型);重绑纪律与 UB 说明见其定义处注释。
        void grow_stack_() noexcept;

        Buffer<Value>                    buf_; // 值栈缓冲底座(GC 分配,可增长)
        Value*                           top_; // 栈顶(下一空闲槽;增长后由 grow_stack_ 重定位)
        FrameStack<CallFrame, kFrameMax> frames_;
        ObjUpvalue*                      open_upvalues_; // open upvalue 开链头(按槽址降序;nullptr 空链)
        Opt<Value>                       pending_error_; // 挂起错误寄存器(置入后随本对象 trace 标根)
        ObjMovement*                     previous_;      // resume 链:恢复者上下文(主上下文恒 nullptr 链尾)
    };

    // VMContext 是 ObjMovement 的别名(.claude/reference/runtime/vm-design.md §1):泛指「一段执行的状态」用
    // VMContext,强调「协程对象」用 ObjMovement。
    using VMContext = ObjMovement;

} // namespace aria

#endif // ARIA_OBJ_MOVEMENT_HPP
