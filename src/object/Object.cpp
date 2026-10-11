#include "Object.hpp"

#include "runtime/ObjMovement.hpp"
#include "value/Value.hpp"

namespace aria {

    // 协议默认体住本文件而不是 Object.hpp：默认实现要经上下文报错，而上下文头（ObjMovement.hpp）反向依赖本头，
    // 放头内会造成两个头互相 include。
    Opt<Value> Object::load_field(AriaExeContext& ctx, ObjString* name) {
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    Opt<Value> Object::load_field_bound(AriaExeContext& ctx, ObjString* name) {
        // 基类默认:直接委托裸查找 load_field(读取本就不绑定的类型照取原值);会绑定的类型各自 override。
        return load_field(ctx, name);
    }

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
