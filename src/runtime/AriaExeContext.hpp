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
    class ObjFunction;
    class ObjFnProto;
    class ObjMovement;
    class ObjString;
    class ObjUpvalue;

    // 供 return fail(...) 隐式转换为失败结果。
    // 勿用于 Opt<标量>：经 bool 转换可能得到有值的 0/false，而非 nullopt。
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

    // 持有值栈、帧栈和异常状态；由 ObjMovement 包装成 GC 对象。
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

        [[nodiscard]]
        Value& peek(const usize n) const noexcept {
            ASSERT(n < stack_size(), "peek beyond stack size");
            return *(top_ - 1 - n);
        }

        // 须在截断值栈前关闭被弃局部的 upvalue，再将错误值放入 catch 参数槽。
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

        // 栈顶须为 [slot0, a1..aN]；slot0 是函数自身或方法接收者，兼作返回槽。
        void enter_frame(ObjFunction* function, u8 argc);

        // 同槽捕获复用同一 upvalue；开链按槽址降序排列，并由上下文标根。
        [[nodiscard]]
        ObjUpvalue* capture_upvalue(Value* slot) noexcept;

        // 关闭并摘除所有槽址 >= from 的 upvalue。
        void close_upvalues(const Value* from) noexcept;

        [[nodiscard]]
        ObjUpvalue* open_upvalues() const noexcept {
            return open_upvalues_;
        }

        // 先关闭 upvalue 再弹帧、截值栈，避免捕获值丢失。
        void exit_frame() noexcept {
            Value* slots = frames_.top().slots;
            close_upvalues(slots);
            frames_.pop();
            set_stack_top_(slots);
        }

        // 仅保留 depth 个帧；一次关闭全部被弃帧的 upvalue。
        void truncate_frames_to(const usize depth) noexcept {
            if (depth >= frames_.size()) {
                return;
            }
            Value* slots = frames_[depth].slots;
            close_upvalues(slots);
            frames_.truncate(depth);
            set_stack_top_(slots);
        }

        // 清空执行状态并保留容量；previous_/state_ 由协程切换点维护。
        void reset() noexcept {
            close_upvalues(buf_.data());
            top_ = buf_.data();
            frames_.clear();
            pending_error_.reset();
        }

        // 错误值进入挂起寄存器后由上下文标根，直至 take_error 取出。
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

        [[nodiscard]]
        const Opt<Value>& pending_error() const noexcept {
            return pending_error_;
        }

        // 装箱并挂起错误，可能触发 GC。消息不含位置，未捕获出口另附堆栈跟踪。
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

        // 元数报错统一入口；元数比较留在调用方。
        [[nodiscard]]
        FailSignal arity_error(u32 argc, u32 expected);

        [[nodiscard]]
        FailSignal arity_error_range(u32 argc, u32 low, u32 high);

        [[nodiscard]]
        FailSignal arity_error_at_least(u32 argc, u32 low);

        [[nodiscard]]
        bool check_arity(const ObjFnProto* proto, u8 argc);

        // 补入缺省印章，将多余实参打包为 rest 列表，返回整形后的参数槽数。
        [[nodiscard]]
        u8 prepare_call_args(const ObjFnProto* proto, u8 argc);

        // 仅供执行中的驱动同步重入；失败时嵌套帧已清理，错误留给外层继续派发。

        [[nodiscard]]
        Opt<Value> execute_function(ObjFunction* function, ArgList args);

        [[nodiscard]]
        Opt<Value> execute_method(Value receiver, ObjFunction* method, ArgList args);

        // previous 指向恢复者；挂起时必须断链，主上下文为链尾。
        [[nodiscard]]
        ObjMovement* previous() const noexcept {
            return previous_;
        }

        void set_previous(ObjMovement* prev) noexcept { previous_ = prev; }

        [[nodiscard]]
        ObjMovement* take_previous() noexcept {
            return util::take(previous_);
        }

        [[nodiscard]]
        ExecState state() const noexcept {
            return state_;
        }

        void set_state(const ExecState state) noexcept { state_ = state; }

        [[nodiscard]]
        u32 ret_frame() const noexcept {
            return ret_frame_;
        }

        [[nodiscard]]
        u32 recursion_depth() const noexcept {
            return recursion_depth_;
        }

        // 须在压入调用区之后、进入被调帧之前保存重入边界。
        [[nodiscard]]
        u32 enter_recursion() noexcept {
            const u32 old_ret = ret_frame_;
            ret_frame_        = frames_.size();
            ASSERT(ret_frame_ != 0, "recursion entry with zero boundary (outermost execution does not recurse)");
            ++recursion_depth_;
            return old_ret;
        }

        void leave_recursion(const u32 saved_ret) noexcept {
            ret_frame_ = saved_ret;
            --recursion_depth_;
        }

        template<util::FixedString key>
        [[nodiscard]]
        ObjString* str() const noexcept {
            constexpr auto found = str_table::index_of(key.view());
            static_assert(found.has_value(), "constant string key is not in the registry");
            return str(*found);
        }

        [[nodiscard]]
        ObjString* str(usize index) const noexcept;

        [[nodiscard]]
        ObjString* char_string(u8 byte) const noexcept;

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

        [[nodiscard]]
        ObjClass* function_class() const noexcept;

        [[nodiscard]]
        ObjClass* bound_method_class() const noexcept;

        [[nodiscard]]
        ObjClass* module_class() const noexcept;

        [[nodiscard]]
        ObjClass* coroutine_class() const noexcept;

        [[nodiscard]]
        ObjClass* class_class() const noexcept;

        [[nodiscard]]
        AriaVM& host() const noexcept {
            return *host_;
        }

        [[nodiscard]]
        GC& gc() const noexcept;

    protected:
        // 仅允许经 Object 的虚析构销毁，禁止通过上下文基类删除。
        ~AriaExeContext() = default;

    private:
        friend class ObjMovement;

        explicit AriaExeContext(GC* gc, AriaVM* host) noexcept :
            host_{host}, buf_{gc, kStackInit}, top_{buf_.data()}, frames_{}, open_upvalues_{nullptr}, pending_error_{},
            previous_{nullptr}, state_{ExecState::Suspended}, ret_frame_{0}, recursion_depth_{0} {}

        void set_stack_top_(Value* t) noexcept {
            ASSERT(t >= buf_.data() && t <= top_, "stack top out of range");
            top_ = t;
        }

        void grow_stack_() noexcept;

        using CallFrameStack = FrameStack<CallFrame, kFrameMax>;

        // 借用宿主，其生存期覆盖上下文；不参与 GC 追踪。
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
