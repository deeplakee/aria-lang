#include "Object.hpp"

#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    // 协议基类默认体移出头外:默认体经 AriaVM::fail 模板报错,而 AriaVM.hpp 经 ObjException.hpp
    // 依赖 Object.hpp(两头互不 include),故只能落 .cpp。默认语义一律「本类型不支持」,失败出口
    // 经 FailSignal 哨兵一行返回(契约见 Object.hpp)。

    Opt<Value> Object::load_field(AriaVM& vm, ObjString* name) {
        // 未 override 的类型无命名成员语义;对象描述经 debug_repr(纯 C++ 惰性渲染契约)。
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    Opt<Value> Object::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 基类默认 = load_field;实例与内置容器/迭代器各自 override(不铸 ObjBoundMethod,契约见
        // Object.hpp)。
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

    // 算子/调用协议基类默认:未实现该协议直接 fail(报文打钩子名);实现者见 Object.hpp。
    Opt<Value> Object::op_add_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__add__'", type_name());
    }

    Opt<Value> Object::op_sub_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__sub__'", type_name());
    }

    Opt<Value> Object::op_mul_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__mul__'", type_name());
    }

    Opt<Value> Object::op_div_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__div__'", type_name());
    }

    Opt<Value> Object::op_mod_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__mod__'", type_name());
    }

    Opt<Value> Object::op_less_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__lt__'", type_name());
    }

    Opt<Value> Object::op_less_equal_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__le__'", type_name());
    }

    Opt<Value> Object::op_greater_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__gt__'", type_name());
    }

    Opt<Value> Object::op_greater_equal_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__ge__'", type_name());
    }

    Opt<Value> Object::op_negate_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support '__neg__'", type_name());
    }

    Opt<Value> Object::op_call_impl(AriaVM& vm) {
        return vm.fail(ErrorCode::CallNonCallable, "type {} does not support '__call__'", type_name());
    }

} // namespace aria
