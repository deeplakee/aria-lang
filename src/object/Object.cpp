#include "Object.hpp"

#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // op_* 族基类默认的共用 fail 体(二元):文案与 VM 原语路径 run_binary_numeric 的
        // TypeMismatch 报错逐字一致(对象在左、协议不支持的类型组合从此处报,两路同串)。
        // 返回 FailSignal 哨兵,各 op_* 默认体 `return op_binary_unsupported(...)` 一行出口
        //(转换为其 Opt<Value> 的 nullopt)。
        FailSignal op_binary_unsupported(AriaVM& vm, const StringView symbol, const Object* lhs, const Value rhs) {
            return vm.fail(ErrorCode::TypeMismatch, "operator '{}' requires numbers, got {} and {}", symbol,
                           lhs->type_name(), type_name(rhs));
        }

    } // namespace

    // 成员/下标访问协议与算术协议的基类默认体:定义移出头外 -- 默认体经 AriaVM::fail 模板
    // 报错,而 AriaVM.hpp 经 ObjException.hpp 依赖 Object.hpp、两头互不 include(环),虚函数
    // 默认实现只能落 .cpp。默认语义一律「本类型不支持」:vm.fail 就地烘焙文案入挂起错误寄存器,
    // 失败出口经 FailSignal 哨兵一行返回(契约见 Object.hpp 协议注释:load 族 nullopt ⟺ 已
    // fail / store 族 false ⟺ 已 fail)。

    Opt<Value> Object::load_field(AriaVM& vm, ObjString* name) {
        // 未 override 的类型无命名成员语义:文案统一为「X has no member 'y'」
        //(对象描述经 debug_repr,纯 C++ 惰性渲染契约)。
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    Opt<Value> Object::resolve_invoke(AriaVM& vm, ObjString* name) {
        // 基类默认 = load_field:未 override 的类型(类/模块等)照读路径取值,miss 文案随宿主就地
        // 烘焙。实例与内置容器/迭代器各自 override(调用路径不铸 ObjBoundMethod)。解析时机(实参
        // 求值之前)与调用区槽 0 由 VM 侧 PREPARE_METHOD / CALL_METHOD 保持(契约与理由见 Object.hpp)。
        return load_field(vm, name);
    }

    bool Object::store_field(AriaVM& vm, ObjString* name, const Value value) {
        return vm.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name());
    }

    Opt<Value> Object::load_index(AriaVM& vm, const Value key) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name());
    }

    bool Object::store_index(AriaVM& vm, const Value key, const Value value) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name());
    }

    Opt<Value> Object::op_add(AriaVM& vm, const Value rhs) const { return op_binary_unsupported(vm, "+", this, rhs); }

    Opt<Value> Object::op_sub(AriaVM& vm, const Value rhs) const { return op_binary_unsupported(vm, "-", this, rhs); }

    Opt<Value> Object::op_mul(AriaVM& vm, const Value rhs) const { return op_binary_unsupported(vm, "*", this, rhs); }

    Opt<Value> Object::op_div(AriaVM& vm, const Value rhs) const { return op_binary_unsupported(vm, "/", this, rhs); }

    Opt<Value> Object::op_mod(AriaVM& vm, const Value rhs) const { return op_binary_unsupported(vm, "%", this, rhs); }

    // 比较算子默认体:与数值原语路径同文案(比较指令的对象左值在此报,未 override 的类型一律
    // 「requires numbers」--String 已 override 成字节序比较,其余类型照此)。
    Opt<Value> Object::op_less(AriaVM& vm, const Value rhs) const { return op_binary_unsupported(vm, "<", this, rhs); }

    Opt<Value> Object::op_less_equal(AriaVM& vm, const Value rhs) const {
        return op_binary_unsupported(vm, "<=", this, rhs);
    }

    Opt<Value> Object::op_greater(AriaVM& vm, const Value rhs) const {
        return op_binary_unsupported(vm, ">", this, rhs);
    }

    Opt<Value> Object::op_greater_equal(AriaVM& vm, const Value rhs) const {
        return op_binary_unsupported(vm, ">=", this, rhs);
    }

    Opt<Value> Object::op_negate(AriaVM& vm) const {
        // 文案与 VM 的 NEGATE 指令报错一致(InvalidOperand)。
        return vm.fail(ErrorCode::InvalidOperand, "negate requires a number, got {}", type_name());
    }

    bool Object::op_call(AriaVM& vm, Span<Value> slots) {
        // 基类默认:本类型不可调用(消费方 = call_value 的 switch default);slots 本默认不读。
        return vm.fail(ErrorCode::CallNonCallable,
                       "call non-callable {} (supports closures / native functions / classes / bound "
                       "methods only)",
                       type_name());
    }

} // namespace aria
