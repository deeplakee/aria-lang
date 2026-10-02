#include "Object.hpp"

#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    // 协议默认体住本文件而不是 Object.hpp：默认实现要经 vm 报错，而 vm 的头已经反过来依赖本头，
    // 放头内会造成两个头互相 include。
    Opt<Value> Object::load_field(AriaVM& vm, ObjString* name) {
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    Opt<Value> Object::load_field_bound(AriaVM& vm, ObjString* name) {
        // 基类默认:直接委托裸查找 load_field(读取本就不绑定的类型照取原值);会绑定的类型各自 override。
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

    // 算子/调用协议基类默认:未实现该协议直接 fail(报文打钩子名)。
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
