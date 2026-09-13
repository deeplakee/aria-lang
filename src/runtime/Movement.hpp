#ifndef ARIA_MOVEMENT_HPP
#define ARIA_MOVEMENT_HPP

#include "common.hpp"
#include "memory/Buffer.hpp"
#include "memory/GC.hpp"
#include "runtime/FrameStack.hpp"
#include "value/Value.hpp"

namespace aria {

    class CodeUnit;
    class ObjClosure;
    class ObjFunction;
    class ObjModule;
    class ObjUpvalue;

    // 调用帧(trivially-copyable 聚合,满足 FrameStack 约束,同 LineEntry 风格不带尾下划线)。
    // 每进入一个函数体(或顶层)acquire 一帧,RETURN/异常 unwind 时 pop/truncate。
    // ip/last_ip 仅作读取游标/锚点,运行期从不经其写字节;last_ip 反推 offset 前提:帧存活期间
    // code 缓冲恒定 -- 非移动 GC + 执行期零 emit。
    struct CallFrame {
        ObjClosure* closure; // M4 起持闭包(callable 收敛为闭包,顶层入口也闭包);元数据经其 function() 取
        CodeUnit*   unit;    // 缓存 closure->function()->unit(),省每条指令一跳
        ObjModule*  module;  // 缓存 closure->function()->module(),供 *_GLOBAL 定位模块 globals
        u8*         ip;      // 指令指针(冷路径按需反推 offset)
        Value*      slots;   // 本帧局部基址(callee 槽 0,参数槽 1..)
        u8*         last_ip; // 最近取指指令起始(循环顶写;行号/unwind 查表锚点)
    };

    // 执行上下文:一段执行的完整状态(可增长值栈 + 帧栈)。设计见 .claude/reference/runtime/vm-design.md。
    //
    //   - 值栈缓冲经 Buffer<Value> 底座管理(GC 分配/重分配/释放,计入 bytes_allocated_),
    //     push 溢出时 2x 增长;top_/活动帧 slots/open upvalue location_ 三类指入值栈的指针
    //     在增长时按「搬运前记槽偏移、搬运后新基址重建」重绑(细节见 grow_stack_ 注释)。
    //   - 帧栈走 FrameStack 模板(槽位语义,truncate 供异常 unwind 跨帧)。
    //   - 纯 C++ 类(非 Object):值栈/帧不进对象链表,由 AriaVM 的 vm_roots tracer 在 collect
    //     时沿 current_ -> previous_ 链逐上下文直标(M6 协程期再升级 ObjMovement : Object)。
    class Movement {
    public:
        static constexpr usize kStackInit = 1024; // 值栈初始容量(Value 槽,8KB);不足时 2x 增长
        static constexpr usize kFrameMax  = 256;  // 调用帧容量

        explicit Movement(GC* gc) noexcept :
            buf_{gc, kStackInit}, top_{buf_.data()}, frames_{}, open_upvalues_{nullptr}, previous_{nullptr} {}

        ~Movement() = default; // buf_ 自释放值栈;frames_ 定容无资源;open_upvalues_ 节点是 GC 对象,归 GC 管。

        Movement(const Movement&)            = delete;
        Movement& operator=(const Movement&) = delete;
        Movement(Movement&&)                 = delete;
        Movement& operator=(Movement&&)      = delete;

        // 清空值栈与帧栈与挂起错误(容量保留,不缩回初始)。先关全部开指再清场:开指槽址全在
        // 值栈内,close 后值已迁入 ObjUpvalue 自持 -- HALT 收场不弹帧,若无此安全网,链上残留的
        // 开指会跨 run 复用同一栈区继续指入(槽值被下一轮覆写),再经闭包读出脏值。
        void reset() noexcept {
            close_upvalues(buf_.data());
            top_ = buf_.data();
            frames_.clear();
            pending_error_.reset();
        }

        // ---- 值栈(热路径裸指针)----

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

        // 压栈:先写值、再按需 2x 增长 -- 使 value 先入活跃区随值栈根存活,与「栈即根」纪律
        // 一致(当前 grow 永不触 GC,此序为前瞻防御)。进 push 时必有空槽(top_ < base+cap
        // 为不变式,见构造/reset/grow 的维持),先写不越界。
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

        // 丢弃栈顶 n 个。
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

        // 截断值栈顶到 new_size(相对 stack_base 的槽位数,须 <= 当前 stack_size)。异常 unwind
        // 的帧内回退入口:截掉 try 体临时值与本帧残留,保留 handler 帧的 callee/参数/已声明局部;
        // 帧栈回退归 FrameStack::truncate,两者组成 unwind 的完整回退。
        void truncate_stack(const usize new_size) noexcept { set_stack_top_(buf_.data() + new_size); }

        // ---- 帧栈 ----

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
        // (闭包经 frame.closure 携带不上栈,两态共用本入口)。定义在 .cpp(需 ObjClosure
        // 完整类型,避免头文件拖入 object 树)。
        void enter_frame(ObjClosure* closure, u8 argc);

        // ---- open upvalue 开链(M4)----
        // 链头 open_upvalues_:本上下文全部 open 态 upvalue,按槽址降序(head 最高);局部所在
        // 区间被关闭时(RETURN/unwind/显式 CLOSE_UPVALUE)摘链迁值。链上节点经 VM 根 tracer
        // 标根 -- 防「闭包已死而 upvalue 仍在链」悬垂。「同一局部只有一份引用」不变式由
        // capture_upvalue 单点收口(命中复用或建新插链,无旁路)。

        // 捕获:沿降序链一趟完成查等值与插链点定位,等值即复用(内外层共享同一 ObjUpvalue)、
        // 更小/链尾则建新并在该处插链;即使链上出现同槽双节点(不变式被破)也命中复用(自愈)。
        // 建新到插链之间无分配点(白色窗口不被 sweep),入链后即经 tracer 根化。定义在 .cpp
        // (需 ObjUpvalue 完整类型)。
        [[nodiscard]]
        ObjUpvalue* capture_upvalue(GC& gc, Value* slot) noexcept;

        // 关闭所有指向 >= from 槽址的开指:值迁入各自 closed_(close),并整段摘链。挂点:
        // exit_frame 内置(本帧区间;RETURN 与 unwind 未命中经此)/显式 CLOSE_UPVALUE(top - 1)/
        // unwind 命中(slots + stack_depth,先于截栈,handler 帧不退不经 exit_frame)/reset(全链)。
        // 降序不变式下 >= from 恒为链头连续前缀。
        void close_upvalues(const Value* from) noexcept;

        // 只读链头(VM 根 tracer 遍历标根用;节点 next 经 ObjUpvalue::next_open)。返非 const 指针:
        // mark_object(Object*) 需要可变指针;tracer 只读遍历,不改链。
        [[nodiscard]]
        ObjUpvalue* open_upvalues() const noexcept {
            return open_upvalues_;
        }

        // 出帧:关本帧区间开指 + 弹当前帧 + 值栈顶复位到该帧 slots 基址,与 enter_frame 成对。
        // close_upvalues(frame.slots) 内置于此:本帧被捕获局部随帧销毁,值须在槽区仍存活时迁入
        // 各自 upvalue 自持(链空或链头已低于本帧区间时一步即停,零开销)。先取 slots 再
        // close/pop:frames_.top() 引用 pop 后悬垂,不可先 pop 再读。
        void exit_frame() noexcept {
            Value* slots = frames_.top().slots;
            close_upvalues(slots); // 关本帧区间开指(值迁入自持;槽区此刻仍存活,迁值读安全)
            frames_.pop();
            set_stack_top_(slots);
        }

        // ---- 挂起错误寄存器(侧信道)----
        // 原生函数等冷路径错误不走返回类型(避免把约 56B 的 Error 编进热路径返回值),经 raise
        // 写入本寄存器;VM 在 CALL 等安全点检查 has_error() 后用 take_error() 取出传播。寄存器
        // 随上下文走(M6 协程期各协程独立 raise/检查)。载荷为 Value:VM/原生错误装箱
        // ObjException 后写入,aria throw 原值入寄存器(catch 绑原值保类型);置入后由 VM 根
        // tracer 标根,取出前跨安全点分配不回收。raise 断言当前无挂起(防嵌套 raise 未取走
        // 就再 raise);reset() 一并清空。

        void raise(const Value err) noexcept {
            ASSERT(!pending_error_.has_value(),
                   "Movement::raise: pending error already set (take/clear before re-raise)");
            pending_error_ = err;
        }

        [[nodiscard]]
        bool has_error() const noexcept {
            return pending_error_.has_value();
        }

        [[nodiscard]]
        Opt<Value> take_error() noexcept {
            return std::exchange(pending_error_, std::nullopt);
        }

        // 挂起载荷的只读引用(为空态时无值)。供 VM 根 tracer 标根用(take_error 取走会清空,
        // 不能经它只读查询)。
        [[nodiscard]]
        const Opt<Value>& pending_error() const noexcept {
            return pending_error_;
        }

        // ---- 协程 resume 链(M6 前置落地)----
        // previous_ = 「谁恢复了我」:A resume B 即 B->previous_ 置 A、VM 的 current_ 换指 B;
        // 自 current_ 沿 previous_ 回走即 resume 链,链尾恒为主上下文。切换收口在 AriaVM
        // (current_),Movement 不自切;M6 前链长恒 1,字段为契约占位。
        [[nodiscard]]
        Movement* previous() const noexcept {
            return previous_;
        }

        // 链接/重链(resume 方向:置恢复者)。挂起回退时是否清 nullptr 属 M6 设计点。
        void set_previous(Movement* prev) noexcept { previous_ = prev; }

    private:
        // 就位一帧:slots 指向槽 0,VM 专有字段(closure/unit/module/ip)从 closure 解引用填充。
        // 与 enter_frame 分工:enter_frame 管 acquire,此函数管填字段;槽 0 语义见 enter_frame 注释。
        void init_frame_(CallFrame& f, ObjClosure* closure, u8 argc) const;

        // 截断栈顶到 t(t 须在 [base, top] 内)。值栈顶复位由 Movement 内部独占
        // (exit_frame / truncate_stack),不对外暴露,收紧「值栈顶只由 Movement 自身改」的边界。
        void set_stack_top_(Value* t) noexcept {
            ASSERT(t >= buf_.data() && t <= top_, "Movement::set_stack_top_ out of range");
            top_ = t;
        }

        // 值栈 2x 扩容 + 三类指针重绑(top_ / 各活动帧 slots / open upvalue 链 location_)。
        // 定义在 .cpp(需 ObjUpvalue 完整类型);重绑纪律与 UB 说明见其定义处注释。
        void grow_stack_() noexcept;

        Buffer<Value>                    buf_; // 值栈缓冲底座(GC 分配,可增长)
        Value*                           top_; // 栈顶(下一空闲槽;增长后由 grow_stack_ 重定位)
        FrameStack<CallFrame, kFrameMax> frames_;
        ObjUpvalue*                      open_upvalues_; // open upvalue 开链头(按槽址降序;nullptr 空链)
        Opt<Value>                       pending_error_; // 挂起错误寄存器(置入后由 VM 根 tracer 标根)
        Movement*                        previous_;      // resume 链:恢复者上下文(主上下文恒 nullptr 链尾)
    };

    // VMContext 是 Movement 的别名(.claude/reference/runtime/vm-design.md §1):泛指「一段执行的状态」用
    // VMContext,强调「协程对象」用 ObjMovement(M6 升级为 Object 子类后的类名)。
    using VMContext = Movement;

} // namespace aria

#endif // ARIA_MOVEMENT_HPP
