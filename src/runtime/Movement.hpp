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
    //   - 值栈缓冲经 Buffer<Value> 底座管理(GC 分配/重分配/释放,同 Array;计入
    //     bytes_allocated_),非 unique_ptr。初始定容、push 溢出时 2x 增长:增长经
    //     Buffer::reserve -> GC reallocate(内部 memcpy)搬迁;reallocate 释放旧块,故
    //     top_ 与各活动帧的 slots(原指旧块)须重绑到新块。重绑在搬运**前**(old_base 仍存活)
    //     算好 top_ 与各 slots 相对 old_base 的槽偏移(纯整数),搬运**后**用「新基址 + 偏移」
    //     重算指针,不触碰 dangling 指针(对 dangling 指针做指针减法是 UB,读其值虽实现定义
    //     但无必要)。open upvalue 链(M4 落地)是第三类重绑:链上 location_ 同样指入值栈,
    //     同法偏移两趟(见 grow_stack_)。
    //   - 帧栈走 FrameStack 模板(槽位语义,truncate 供异常 unwind 跨帧)。
    //   - 纯 C++ 类(非 Object):值栈/帧不进对象链表,改经 AriaVM 的 vm_roots tracer 在 collect
    //     时直标(值栈 [base,top) + 各帧 closure/module + open upvalue 开链),M6 协程期升级
    //     ObjMovement : Object 入链表;resume 链(previous_,见下)已前置落地,VM 根 tracer 沿
    //     current_ -> previous_ 链逐个标根。主上下文与协程同构。
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

        // 清空值栈与帧栈与挂起错误(回到空态,可复用;已增长的容量保留,不缩回初始)。
        // 先关全部开指再清场:开指槽址全在值栈内,close 后值已迁入 ObjUpvalue 自持,原槽随清场
        // 即弃 -- HALT 收场不弹帧,若无此安全网,链上残留的开指会跨 run 复用同一栈区继续指入
        // (槽值被下一轮覆写),再经闭包读出脏值。
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

        // 截断值栈顶到 new_size(相对 stack_base 的槽位数,须 <= 当前 stack_size)。
        // 异常 unwind 的帧内回退入口:命中 handler 后截掉 try 体临时值与本帧残留,保留
        // handler 帧的 callee/参数/已声明局部(截到 frame.slots + stack_depth 由调用方算好)。
        // 与帧栈回退分工:FrameStack::truncate 弹内层帧,值栈截断归本方法,两者组成 unwind
        // 的完整回退(pitfalls 坑 #6/#14)。内部经 set_stack_top_ 复用其 [base, top] 区间断言。
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

        // 进帧:为对 closure 的调用 acquire 一个空帧并就位全部字段。slots 按不变量设为
        // top - argc - 1(假定栈顶形如 [callee, a1..aN]:callee 在槽 0、参数 a1..aN 即局部槽 1..argc);
        // closure/unit/module/ip 由 closure 解引用填充 -- 故定义在 .cpp(那里 include ObjClosure,
        // 头文件仅前向声明即可,避免 Movement.hpp 拖入 object 树)。slots 与值栈的对应关系在此
        // 收口,与 exit_frame 成对,锁住「栈顶帧 slots 即值栈本帧槽 0」的不变量。
        // 注:M4 起 callable 收敛为闭包,帧持 ObjClosure*;arity/名字等元数据经 closure->function() 取。
        // 槽 0 的语义由调用方在进帧前写定:普通函数调用 = 闭包自身(压在栈上的 callee),
        // 方法调用 = 接收者(this)替代 bound 对象 -- 闭包不入栈,经 frame.closure 携带
        // (对齐 clox / 旧版 VM 的方法帧形 [this, a1..aN],两态共用本入口,无专用方法进帧)。
        void enter_frame(ObjClosure* closure, u8 argc);

        // ---- open upvalue 开链(M4)----
        // 链头 open_upvalues_:本上下文全部 open 态 upvalue,按槽址降序(head 槽址最高);
        // 局部所在区间被关闭时(RETURN/unwind/显式 CLOSE_UPVALUE)摘链迁值。链上节点经 VM 根
        // tracer 标根 -- 防「闭包已死而 upvalue 仍在链」悬垂(clox 已知坑)。
        // 「同一局部只有一份引用」不变式(「捕获即引用」的共享保证)由 capture_upvalue 单点
        // 收口:命中复用或建新插链,不存在绕过查链直接插链的旁路。

        // 捕获(复用或新建,单趟完成):沿降序链走到首个槽址 <= slot 的节点即停 --
        // 等值(槽已被捕获)返回既有 open upvalue(内外层共享同一 ObjUpvalue);更小/链尾则
        // 经调用方传入的 gc 建新并在该处降序插链。查等值与插链点定位一趟走完,不经两次遍历;
        // 即使链上出现同槽双节点(不变式被破)也命中复用而非再插一个(自愈)。
        // gc 由调用方(VM 驱动)传入 -- Movement 不自持分配器创建对象,对象分配语义归 VM;
        // 建新到插链之间无任何分配点(白色窗口,不被 sweep),入链后即经 tracer 根化。
        // 定义在 .cpp(需 ObjUpvalue 完整类型)。
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

        // 出帧:关本帧区间开指 + 弹当前帧 + 值栈顶复位到该帧 slots 基址(丢弃 callee 与本帧
        // 残留),一体收口 -- 与 enter_frame 成对。close_upvalues(frame.slots) 内置于此:本帧被
        // 捕获局部随帧销毁,值须在槽区仍存活时迁入各自 upvalue 自持(闭包跨帧退出的存活机制;
        // 链空或链头已低于本帧区间时一步即停,零开销)。先取 slots 再 close/pop:frames_.top()
        // 引用 pop 后悬垂,不可先 pop 再读。把这一组动作收口于此,调用方无法只做其一而破坏
        // 值栈/帧栈/开链三者的对应关系。复位经私有 set_stack_top_:白赚其 [base, top] 区间断言。
        void exit_frame() noexcept {
            Value* slots = frames_.top().slots;
            close_upvalues(slots); // 关本帧区间开指(值迁入自持;槽区此刻仍存活,迁值读安全)
            frames_.pop();
            set_stack_top_(slots);
        }

        // ---- 挂起错误寄存器(侧信道)----
        // 原生函数等冷路径错误**不走返回类型**(避免把约 56B（libc++)/64B（libstdc++) 的 Error
        // 编进热路径返回值),而是经 raise 写入本寄存器;VM 在 CALL 等安全点检查 has_error() 后用
        // take_error() 取出传播。寄存器置于执行上下文(而非 AriaVM):错误状态随上下文走,M6
        // 协程期每个协程有独立的挂起错误(各自 raise/检查,互不串扰)。M1 单一主上下文,等价于
        // VM 级单寄存器。
        //   - 载荷类型(M3 起):Value。VM 检测到的运行时错误与原生函数报错装箱为 ObjException
        //     (码 + 完整烘焙消息串,未捕获出口经 uncaught_error_parts 拆件 + from_baked 物化)后写入;
        //     aria 语言自身 throw(M3)抛任意值。
        //     raise 收已构造好的 Value(构造 ObjException 需分配,在调用方 -- 持 GC 者 -- 完成);
        //     寄存器置入后即由 VM 根 tracer 标 pending_error() 保命(见 AriaVM ctor),取出前
        //     跨安全点分配不回收载荷。
        //   - raise:写入。断言当前无挂起错误(防嵌套 raise 未被取走就再 raise 的 bug)。
        //   - has_error / take_error / clear_error:VM 在安全点查询/取出/清空。take_error 取走即清空。
        //   - reset() 一并清空(上下文复用前置干净)。Opt<Value> 尺寸小、非热路径。

        void raise(Value err) noexcept {
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

        void clear_error() noexcept { pending_error_.reset(); }

        // 挂起载荷的只读引用(为空态时无值)。供 VM 根 tracer 标根用(take_error 取走会清空,
        // 不能经它只读查询)。
        [[nodiscard]]
        const Opt<Value>& pending_error() const noexcept {
            return pending_error_;
        }

        // ---- 协程 resume 链(M6 前置落地)----
        // previous_ = 「谁恢复了我」:A resume B 即 B->previous_ 置 A、VM 的 current_ 换指 B;
        // B yield/结束回退 current_ = B->previous_。自 VM 的 current_ 沿 previous_ 回走即 resume 链,
        // 链尾恒为主上下文(其 previous_ 恒 nullptr)-- resume/yield 严格成对的直接推论。VM 根
        // tracer 据此沿链逐个标根(挂起协程的值栈/帧/寄存器都是根,见 AriaVM ctor),并以链尾断言
        // 锁定切换纪律。切换收口在 AriaVM(current_,见其注释),Movement 不自切;M6 前链长恒 1
        // (仅主上下文),字段为契约占位。
        [[nodiscard]]
        Movement* previous() const noexcept {
            return previous_;
        }

        // 链接/重链(resume 方向:置恢复者)。挂起回退时是否清 nullptr 属 M6 设计点(清则无悬指;
        // 不清则重 resume 时重链,但 Movement 升 Object 后 trace 须防标到已结束的旧恢复者)。
        void set_previous(Movement* prev) noexcept { previous_ = prev; }

    private:
        // 就位一帧为对 closure 的调用:slots 指向槽 0,VM 专有字段(closure/unit/module/ip)从
        // closure 解引用填充。假定栈顶形如 [callee, a1..aN](N=argc),f 为 enter_frame 刚 acquire
        // 的栈顶空帧。与 enter_frame 分工:enter_frame 管 acquire(帧栈管理),此函数管填字段。
        // 槽 0 内容即「被调用者语义槽」:普通帧 = 闭包自身、方法帧 = this(调用方进帧前写好;
        // 闭包经 frame.closure 携带不上栈,见 enter_frame 注释)。
        void init_frame_(CallFrame& f, ObjClosure* closure, u8 argc) const;

        // 截断栈顶到 t(t 须在 [base, top] 内)。值栈顶复位由 Movement 内部独占
        // (exit_frame / truncate_stack),不对外暴露,收紧「值栈顶只由 Movement 自身改」的边界。
        void set_stack_top_(Value* t) noexcept {
            ASSERT(t >= buf_.data() && t <= top_, "Movement::set_stack_top_ out of range");
            top_ = t;
        }

        // 值栈 2x 扩容 + 三类指针重绑(top_ / 各活动帧 slots / open upvalue 链 location_)。
        // 定义在 .cpp(链重绑解引用 ObjUpvalue,需完整类型;与 enter_frame 同理避免头文件拖入
        // object 树)。重绑纪律:搬运前记相对 old_base 的整数偏移,搬运后以 new_base + 偏移重建,
        // 从不触碰 dangling 指针(对 dangling 指针做指针减法是 UB,[expr.add] p5)。
        void grow_stack_() noexcept;

        Buffer<Value>                    buf_; // 值栈缓冲底座(GC 分配,可增长)
        Value*                           top_; // 栈顶(下一空闲槽;增长后由 grow_stack_ 重定位)
        FrameStack<CallFrame, kFrameMax> frames_;
        ObjUpvalue*                      open_upvalues_; // open upvalue 开链头(按槽址降序;nullptr 空链)
        Opt<Value>                       pending_error_; // 挂起错误寄存器(侧信道;M3 起载荷为 Value -- ObjException 装箱
                                                         // 或用户 throw 的任意值;置入后由 VM 根 tracer 标根,见上注释)
        Movement* previous_; // resume 链:恢复者上下文(主上下文恒 nullptr 链尾;见上协程 resume 链注释)
    };

    // VMContext 是 Movement 的别名(.claude/reference/runtime/vm-design.md §1):泛指「一段执行的状态」用
    // VMContext,强调「协程对象」用 ObjMovement(M6 升级为 Object 子类后的类名)。
    using VMContext = Movement;

} // namespace aria

#endif // ARIA_MOVEMENT_HPP
