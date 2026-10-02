#include "object/ObjInstance.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
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

    Opt<Value> ObjInstance::load_field(AriaVM& vm, ObjString* name) {
        if (const auto entry = fields_.find(Value::from_obj(name))) {
            return entry->value; // 真字段优先(字段里存的可调用值原值直调)
        }
        return class_->load_field(vm, name); // 命中方法戳闭包也不绑定;miss 的类措辞随协议透传
    }

    Opt<Value> ObjInstance::load_field_bound(AriaVM& vm, ObjString* name) {
        // fields 命中即真字段优先;否则委托类协议沿链读穿透(miss 已就地 fail,只透传)。
        // 命中按 is_method 判:方法绑 this、不写回 fields;new_bound_method 唯一分配点,实例已根化。
        if (const auto entry = fields_.find(Value::from_obj(name))) {
            return entry->value;
        }

        const auto hit = class_->load_field(vm, name);
        if (!hit) {
            return std::nullopt;
        }
        const auto member = *hit;
        if (!is_method(member)) {
            return member;
        }
        return Value::from_obj(new_bound_method(vm.gc(), member, Value::from_obj(this)));
    }

    bool ObjInstance::store_field(AriaVM& vm, ObjString* name, const Value value) {
        // 实例字段动态(无预声明):set 即创建/更新、恒成功;set 走 trivial 分配不触 GC。
        fields_.set(Value::from_obj(name), value);
        return true;
    }

    // 算子/调用协议实现:按钩子名在本实例裸查找(实例 fields 可遮蔽类链钩子)再类链取;名字来自
    // bootstrap 期驻留的 VM 常量串表(随 VM 根恒久存活),每次派发零取串开销、零分配。
    Opt<Value> ObjInstance::op_add_impl(AriaVM& vm) { return load_field(vm, vm.str<"__add__">()); }

    Opt<Value> ObjInstance::op_sub_impl(AriaVM& vm) { return load_field(vm, vm.str<"__sub__">()); }

    Opt<Value> ObjInstance::op_mul_impl(AriaVM& vm) { return load_field(vm, vm.str<"__mul__">()); }

    Opt<Value> ObjInstance::op_div_impl(AriaVM& vm) { return load_field(vm, vm.str<"__div__">()); }

    Opt<Value> ObjInstance::op_mod_impl(AriaVM& vm) { return load_field(vm, vm.str<"__mod__">()); }

    Opt<Value> ObjInstance::op_less_impl(AriaVM& vm) { return load_field(vm, vm.str<"__lt__">()); }

    Opt<Value> ObjInstance::op_less_equal_impl(AriaVM& vm) { return load_field(vm, vm.str<"__le__">()); }

    Opt<Value> ObjInstance::op_greater_impl(AriaVM& vm) { return load_field(vm, vm.str<"__gt__">()); }

    Opt<Value> ObjInstance::op_greater_equal_impl(AriaVM& vm) { return load_field(vm, vm.str<"__ge__">()); }

    Opt<Value> ObjInstance::op_negate_impl(AriaVM& vm) { return load_field(vm, vm.str<"__neg__">()); }

    Opt<Value> ObjInstance::op_call_impl(AriaVM& vm) { return load_field(vm, vm.str<"__call__">()); }

    ObjInstance* new_instance(GC& gc, ObjClass* klass) { return gc.new_object<ObjInstance>(gc, klass); }

} // namespace aria
