#include "Object.hpp"

#include "runtime/ObjMovement.hpp"
#include "value/Value.hpp"

namespace aria {

    // 协议默认体在此定义:报错需要执行上下文的完整类型,避免 Object.hpp 与上下文头互相 include。
    Opt<Value> Object::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->repr(), name->view());
    }

    // 没有独立成员规则时,普通查询默认采用类型成员查询。
    Opt<Value> Object::load_field(AriaExeContext& ctx, ObjString* name) { return load_field_from_class(ctx, name); }

    // 基类默认:直接委托裸查找 load_field(读取本就不绑定的类型照取原值);会绑定的类型各自 override。
    Opt<Value> Object::load_field_bound(AriaExeContext& ctx, ObjString* name) { return load_field(ctx, name); }

    bool Object::store_field(AriaExeContext& ctx, ObjString* name, const Value value) {
        return ctx.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name());
    }

    Opt<Value> Object::load_index(AriaExeContext& ctx, const Value key) {
        return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name());
    }

    bool Object::store_index(AriaExeContext& ctx, const Value key, const Value value) {
        return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name());
    }

} // namespace aria
