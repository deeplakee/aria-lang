#include "object/ObjInstance.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/ObjMovement.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjInstance::ObjInstance(GC& gc, ObjClass* klass) : Object{ObjType::INSTANCE}, class_{klass}, fields_{&gc} {
        ASSERT(klass != nullptr, "class must not be null");
    }

    void ObjInstance::trace(GC& gc) const noexcept {
        gc.mark_object(class_);
        fields_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value);字段里的可调用值经值级联
    }

    String ObjInstance::debug_repr() const {
        // class_ 恒非空(ctor ASSERT)。
        return std::format("<{} instance>", class_->name()->view());
    }

    Opt<Value> ObjInstance::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        if (const auto v = class_->find_field(name)) { // 本类模板链起查(成员直取,不经映射)
            return v;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", class_->debug_repr(), name->view());
    }

    Opt<Value> ObjInstance::load_field(AriaExeContext& ctx, ObjString* name) {
        // 真字段优先(字段里存的可调用值原值直调);miss 缺席腿在本口(类措辞 fail 随其烘焙)。
        if (const auto entry = fields_.find(Value::from_obj(name))) {
            return entry->value;
        }
        return load_field_from_class(ctx, name);
    }

    Opt<Value> ObjInstance::load_field_bound(AriaExeContext& ctx, ObjString* name) {
        // fields 命中即真字段优先;缺席经裸读探类链,命中按 is_method 判:方法绑 this、不写回
        // fields;new_bound_method 唯一分配点,实例已根化。
        const auto hit = load_field(ctx, name);
        if (!hit) {
            return std::nullopt;
        }
        const auto member = *hit;
        if (!is_method(member)) {
            return member;
        }
        return Value::from_obj(new_bound_method(ctx.gc(), member, this));
    }

    bool ObjInstance::store_field(AriaExeContext& ctx, ObjString* name, const Value value) {
        // 实例字段动态(无预声明):set 即创建/更新、恒成功;set 走 trivial 分配不触 GC。
        fields_.set(Value::from_obj(name), value);
        return true;
    }

    ObjInstance* new_instance(GC& gc, ObjClass* klass) { return gc.new_object<ObjInstance>(gc, klass); }

} // namespace aria
