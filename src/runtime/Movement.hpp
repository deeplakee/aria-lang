#ifndef ARIA_MOVEMENT_HPP
#define ARIA_MOVEMENT_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/Buffer.hpp"
#include "memory/GC.hpp"
#include "runtime/FrameStack.hpp"
#include "value/Value.hpp"

namespace aria {

    class CodeUnit;
    class ObjFunction;
    class ObjModule;

    // 调用帧(trivially-copyable 聚合,满足 FrameStack 约束,同 LineEntry 风格不带尾下划线)。
    //        每进入一个函数体(或顶层)acquire 一帧,RETURN/异常 unwind 时 pop/truncate。
    struct CallFrame {
        ObjFunction* function; // M1-M3 过渡持 ObjFunction*;M4 闭包落地后换 ObjClosure*
        CodeUnit*    unit;     // 缓存 function->unit(),省每条指令一跳
        ObjModule*   module;   // 缓存 function->module(),供 LOAD/STORE/DEF_GLOBAL 定位模块 globals
        u8*          ip;       // 指令指针(裸指针;raise/行号等冷路径按需算 offset)
        Value*       slots;    // 本帧局部基址(callee 在槽 0,参数从槽 1 起;RETURN 时弹)
    };

    // 执行上下文:一段执行的完整状态(可增长值栈 + 帧栈)。设计见 .claude/reference/runtime/vm-design.md。
    //
    //   - 值栈缓冲经 Buffer<Value> 底座管理(GC 分配/重分配/释放,同 Array;计入
    //     bytes_allocated_),非 unique_ptr。初始定容、push 溢出时 2x 增长:增长经
    //     Buffer::reserve -> GC reallocate(内部 memcpy)搬迁;reallocate 释放旧块,故
    //     top_ 与各活动帧的 slots(原指旧块)须重绑到新块。重绑在搬运**前**(old_base 仍存活)
    //     算好 top_ 与各 slots 相对 old_base 的槽偏移(纯整数),搬运**后**用「新基址 + 偏移」
    //     重算指针,不触碰 dangling 指针(对 dangling 指针做指针减法是 UB,读其值虽实现定义
    //     但无必要)。open upvalue 尚未落地(M4),落地后须在 grow_stack_ 一并重绑(同法,或改索引式
    //     upvalue 免逐条修)。
    //   - 帧栈走 FrameStack 模板(槽位语义,truncate 供异常 unwind 跨帧)。
    //   - 纯 C++ 类(非 Object):值栈/帧不进对象链表,改经 AriaVM 的 vm_roots tracer 在 collect
    //     时直标(值栈 [base,top) + 各帧 function/module),M6 前即接根。M6 协程期升级 ObjMovement
    //     : Object 并增 open upvalue 链头/执行状态机/resume 字段,届时入对象链表。主上下文与协程同构。
    class Movement {
    public:
        static constexpr usize kStackInit = 1024; // 值栈初始容量(Value 槽,8KB);不足时 2x 增长
        static constexpr usize kFrameMax  = 256;  // 调用帧容量

        explicit Movement(GC* gc) noexcept : buf_{gc, kStackInit}, top_{buf_.data()}, frames_{} {}

        ~Movement() = default; // buf_ 自释放值栈;frames_ 定容无资源。

        Movement(const Movement&)            = delete;
        Movement& operator=(const Movement&) = delete;
        Movement(Movement&&)                 = delete;
        Movement& operator=(Movement&&)      = delete;

        // 清空值栈与帧栈与挂起错误(回到空态,可复用;已增长的容量保留,不缩回初始)。
        void reset() noexcept {
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

        // 压栈:先写值、再按需 2x 增长。先写使 v 入栈活跃区(随 reallocate 的 memcpy 一并搬迁;
        // 值栈已接 VM tracer 根,v 在栈即被标),避免「先增长后写」时 v 仍为未根局部、若增长触发
        // GC 而被回收成悬垂。当前 grow_stack_ -> reallocate 永不触发 GC(GC.hpp 核心不变式),故此
        // 序当前不承重,仅为与「栈即根」纪律一致的前瞻防御(将来 reallocate 若接 maybe_collect 即生效)。
        // 先写不越界:top_ < base+cap 为不变式(构造/reset/drop 维持;set_stack_top_ 的 t <= top_
        // 断言保证不顶满;push 写满后立即增长留空槽),故进 push 时必有空槽。
        void push(const Value v) noexcept {
            *top_++ = v;
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

        // 距栈顶 dist 个槽(0 = 栈顶),可写引用。
        [[nodiscard]]
        Value& peek(const usize dist) const noexcept {
            ASSERT(dist < stack_size(), "peek beyond stack size");
            return *(top_ - 1 - dist);
        }

        // ---- 帧栈 ----

        [[nodiscard]]
        FrameStack<CallFrame, kFrameMax>& frames() noexcept {
            return frames_;
        }

        [[nodiscard]]
        bool frames_full() const noexcept {
            return frames_.size() >= kFrameMax;
        }

        // 进帧:为对 fn 的调用 acquire 一个空帧并就位全部字段。slots 按不变量设为
        // top - argc - 1(假定栈顶形如 [callee, a1..aN]:callee 在槽 0、参数 a1..aN 即局部槽 1..argc);
        // function/unit/module/ip 由 fn 解引用填充 -- 故定义在 .cpp(那里 include ObjFunction,
        // 头文件仅前向声明 ObjFunction 即可,避免 Movement.hpp 拖入 object 树)。slots 与值栈
        // 的对应关系在此收口,与 exit_frame 成对,锁住「栈顶帧 slots 即值栈本帧槽 0」的不变量。
        // 注:M1-M3 callee 为 ObjFunction;M4 闭包落地后 CallFrame.function 换 ObjClosure*,
        // 此签名同步换 ObjClosure*。
        void enter_frame(ObjFunction* fn, u8 argc);

        // 出帧:弹出当前帧并把值栈顶复位到该帧 slots 基址(丢弃 callee 与本帧残留)。
        // 与 enter_frame 成对。先取 slots 再 pop:frames_.top() 引用 pop 后悬垂,不可先 pop
        // 再读。把这一体动作收口于此,调用方无法只做其一而破坏值栈与帧栈的对应关系。
        // 复位经私有 set_stack_top_:白赚其 [base, top] 区间断言。
        void exit_frame() noexcept {
            Value* slots = frames_.top().slots;
            frames_.pop();
            set_stack_top_(slots);
        }

        // ---- 挂起错误寄存器(侧信道)----
        // 原生函数等冷路径错误**不走返回类型**(避免把约 56B（libc++)/64B（libstdc++) 的 Error 编进热路径返回值),而是经
        // raise 写入本寄存器;VM 在 CALL 等安全点检查 has_error() 后用 take_error() 取出传播。
        // 寄存器置于执行上下文(而非 AriaVM):错误状态随上下文走,M6 协程期每个协程有独立的
        // 挂起错误(各自 raise/检查,互不串扰)。M1 单一主上下文,等价于 VM 级单寄存器。
        //   - raise:写入。断言当前无挂起错误(防嵌套 raise 未被取走就再 raise 的 bug)。
        //   - has_error / take_error / clear_error:VM 在安全点查询/取出/清空。take_error 取走即清空。
        //   - reset() 一并清空(上下文复用前置干净)。Error 含 String,可移动;Opt<Error> 约
        //   64B（libc++)/72B（libstdc++),非热路径。

        void raise(Error err) noexcept {
            ASSERT(!pending_error_.has_value(),
                   "Movement::raise: pending error already set (take/clear before re-raise)");
            pending_error_ = std::move(err);
        }

        [[nodiscard]]
        bool has_error() const noexcept {
            return pending_error_.has_value();
        }

        [[nodiscard]]
        Opt<Error> take_error() noexcept {
            return std::exchange(pending_error_, std::nullopt);
        }

        void clear_error() noexcept { pending_error_.reset(); }

    private:
        // 就位一帧为对 fn 的调用:slots 指向槽 0,VM 专有字段(function/unit/module/ip)从 fn 填充。
        // 假定栈顶形如 [callee, a1..aN](N=argc),f 为 enter_frame 刚 acquire 的栈顶空帧。
        // 与 enter_frame 分工:enter_frame 管 acquire(帧栈管理),此函数管填字段(修改顶层 frame)。
        void init_frame_(CallFrame& f, ObjFunction* fn, u8 argc) const;

        // 截断栈顶到 t(t 须在 [base, top] 内)。值栈顶复位由 Movement 内部独占
        // (exit_frame / 未来 unwind_to),不对外暴露,收紧「值栈顶只由 Movement 自身改」的边界。
        void set_stack_top_(Value* t) noexcept {
            ASSERT(t >= buf_.data() && t <= top_, "Movement::set_stack_top_ out of range");
            top_ = t;
        }

        // 值栈 2x 扩容:经 Buffer::reserve -> GC reallocate 搬迁(内部 memcpy)。reallocate 释放
        // 旧块,故 top_ 与各活动帧的 slots(原指旧块)须重绑到新块。重绑策略:在搬运**前**(old_base
        // 仍存活、指针减法有定义)算好 top_ 与各 slots 相对 old_base 的槽偏移(纯整数),搬运**后**
        // 用「新基址 + 偏移」重算指针。如此搬运后不再触碰任何 dangling 指针 -- 旧块释放后 top_/
        // slots/old_base 皆成 dangling,对其做指针减法(如 new_base + (p - old_base))是 UB
        // ([expr.add] p5),读其值为实现定义(非 UB)但亦无必要;故偏移必须在搬运前算好(见
        // Buffer::reserve 注释)。活动帧的 slots 必为指入旧块的有效指针(非空),偏移在 [0, cap)
        // 内。若 reallocate 原地扩容(new_base == old_base)则无需重绑、直接返回 -- 当前
        // GC::reallocate 为「先分配新块再释放旧块」,new_base 不可能等于 old_base,此分支为防御性
        // 保留,供将来支持原地扩容的 reallocate。M4 open upvalue 落地时在此一并重绑(同法:搬运前
        // 记偏移、搬运后重算;或改索引式 upvalue 免逐条修)。
        void grow_stack_() noexcept {
            const auto old_base = buf_.data();

            // 搬运前:old_base 仍存活,此时把 top_ 与各活动帧 slots 到 old_base 的偏移算成整数。
            // 偏移而非绝对指针 -- 搬运后旧块释放,绝对指针成 dangling 不可再用。
            const auto  top_offset  = static_cast<usize>(top_ - old_base);
            const usize frame_count = frames_.size();
            usize       slot_offsets[kFrameMax];
            for (usize i = 0; i < frame_count; ++i) {
                slot_offsets[i] = static_cast<usize>(frames_[i].slots - old_base);
            }

            buf_.reserve(buf_.capacity() * 2); // 搬迁:旧块释放,新块就位

            const auto new_base = buf_.data();
            if (new_base == old_base) {
                return; // 原地扩容,基址未变,无需重绑
            }

            // 搬运后:用「新基址 + 偏移」重算指针,不读任何 dangling 指针值。
            top_ = new_base + top_offset;
            for (usize i = 0; i < frame_count; ++i) {
                frames_[i].slots = new_base + slot_offsets[i];
            }
        }

        Buffer<Value>                    buf_; // 值栈缓冲底座(GC 分配,可增长)
        Value*                           top_; // 栈顶(下一空闲槽;增长后由 grow_stack_ 重定位)
        FrameStack<CallFrame, kFrameMax> frames_;
        Opt<Error>                       pending_error_; // 挂起错误寄存器(侧信道;原生函数等冷路径经 raise 写入)
    };

    // VMContext 是 Movement 的别名(.claude/reference/runtime/vm-design.md §1):泛指「一段执行的状态」用
    // VMContext,强调「协程对象」用 ObjMovement(M6 升级为 Object 子类后的类名)。
    using VMContext = Movement;

} // namespace aria

#endif // ARIA_MOVEMENT_HPP
