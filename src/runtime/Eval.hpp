#ifndef ARIA_EVAL_HPP
#define ARIA_EVAL_HPP

#include "aria.hpp"
#include "common.hpp"
#include "value/Value.hpp"

namespace aria {

    class AriaVM;
    class ObjBoundMethod;
    class ObjClass;
    class ObjFunction;
    class ObjString;
    class Object;

    enum class OpCode : u8;

    // 字节码执行与调用、异常派发；共享状态由 AriaVM 持有。
    class Eval {
    public:
        // 驱动至返回或未捕获错误。成功时结果在栈顶，失败时载荷留在挂起错误寄存器。
        static bool dispatch_loop(AriaExeContext& ctx);

        // 检查并整理实参后进入 aria 函数帧。
        static bool call_aria_fn(AriaExeContext& ctx, ObjFunction* function, u8 argc);

        // 原生函数同步执行，aria 函数重入驱动；失败返回 nullopt，载荷留在 ctx。
        [[nodiscard]]
        static Opt<Value> execute_function(AriaExeContext& ctx, Value slot0, ObjFunction* function, ArgList args);

        // 对象经所属类的 __str__ 钩子渲染，调用方须保活 value。
        // 返回串须在下个 GC 点前发布或根化；nullptr 表示错误已挂起。
        [[nodiscard]]
        static ObjString* stringify(AriaExeContext& ctx, Value value);

        // 渲染并拼接各段，临时串由本函数保活，输入元素由调用方保活。
        // values 不得借用可能在渲染重入时搬迁的值栈。
        [[nodiscard]]
        static ObjString* concat_values(AriaExeContext& ctx, Span<const Value> values, StringView sep);

    private:
        // 调用区为 [callee, a1..aN]。call_* 失败时错误已挂起，成功时可能切换上下文。
        static bool call_value(AriaExeContext& ctx, Value callee, u8 argc);

        static bool call_function(AriaExeContext& ctx, ObjFunction* function, u8 argc);

        static bool call_class(AriaExeContext& ctx, ObjClass* klass, u8 argc);

        static bool call_bound_method(AriaExeContext& ctx, const ObjBoundMethod* bound, u8 argc);

        static bool call_native_fn(AriaExeContext& ctx, const ObjFunction* function, u8 argc);

        // 失败时嵌套帧已清理，错误留给外层驱动继续派发。
        [[nodiscard]]
        static Opt<Value> execute_aria_fn(AriaExeContext& ctx, Value slot0, ObjFunction* function, ArgList args);

        [[nodiscard]]
        static Opt<Value> execute_native_fn(AriaExeContext& ctx, Value slot0, const ObjFunction* function,
                                            ArgList args);

        [[nodiscard]]
        static Opt<Value> execute_callable(AriaExeContext& ctx, Value slot0, Value callee, ArgList args);

        [[nodiscard]]
        static ObjString* execute_str_hook(AriaExeContext& ctx, Value value, Value hook);

        template<OpCode Op>
        static bool run_binary_operator(AriaExeContext& ctx);

        template<OpCode Op>
        static bool run_unary_operator(AriaExeContext& ctx);

        template<OpCode Op>
        static bool run_binary_numeric(AriaExeContext& ctx);

        template<OpCode Op>
        static bool run_binary_bitwise(AriaExeContext& ctx);

        template<OpCode Op>
        static bool run_unary_numeric(AriaExeContext& ctx, Value operand);

        template<OpCode Op>
        static bool run_unary_bitwise(AriaExeContext& ctx, Value operand);

        // 访问协议可能分配，接收者须留在栈上保活。

        static bool run_load_field(AriaExeContext& ctx, ObjString* name);

        static bool run_store_field(AriaExeContext& ctx, ObjString* name);

        static bool run_load_this_field(AriaExeContext& ctx, ObjString* name);

        static bool run_store_this_field(AriaExeContext& ctx, ObjString* name);

        static bool run_prepare_method(AriaExeContext& ctx, ObjString* name);

        static bool run_call_method(AriaExeContext& ctx, u8 argc);

        static bool run_load_super_field(AriaExeContext& ctx, ObjString* name);

        // 返回 false 表示快路径不适用，须回退到对象协议；不代表访问失败。
        static bool try_list_load_fast_dispatch(AriaExeContext& ctx, Object* target, Value idx);

        static bool try_list_store_fast_dispatch(AriaExeContext& ctx, Object* target, Value idx, Value value);

        static bool run_load_index(AriaExeContext& ctx);

        static bool run_store_index(AriaExeContext& ctx);

        static bool run_make_range(AriaExeContext& ctx, u8 flags);

        static bool run_build_string(AriaExeContext& ctx, u8 count);

        static bool run_make_class(AriaExeContext& ctx, ObjString* name);

        static bool run_import(AriaExeContext& ctx, const ObjString* path);

        // 返回 true 表示本轮驱动结束，false 表示继续执行（包括协程切回）。
        static bool run_return(AriaExeContext& ctx);

        // 查找 handler，必要时沿 resume 链转投错误；可能切换当前上下文。
        // 返回 false 表示搜索边界内未捕获，错误仍挂起，交调用方处理。
        static bool unwind(AriaVM& vm);
    };

} // namespace aria

#endif // ARIA_EVAL_HPP
