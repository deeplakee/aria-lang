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
        // class_ 恒非空(ctor ASSERT):实例必有类。
        ASSERT(klass != nullptr, "ObjInstance class must not be null");
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
        // 1) fields 命中:真字段优先,遮蔽类链同名成员。
        if (const auto entry = fields_.find(Value::from_obj(name))) {
            return entry->value;
        }

        // 2) 委托类协议 ObjClass::load_field:沿类链读穿透直读;miss 已按类措辞就地 fail --
        //    nullopt ⟺ 已 fail,本函数只透传信号(查找纯查询无分配;fail 装箱是分配点,本实例
        //    经调用方根化:LOAD_FIELD peek 在栈 / LOAD_THIS_FIELD 帧槽 0 在栈)。
        const auto hit = class_->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        // 3) 命中值解包 member:方法命中(经 is_method(Value) 一步判)现场绑定 this=本实例;其余
        //    (静态 fun/lambda/原生/静态值)原值直读。**不写回 fields**:读路径每次访问产出一个新
        //    bound(方法值是一等值,省不掉),换来类/父类改写对既有实例立即生效(monkey patch 完整)
        //    与 fields_ 回归纯字段。GC 走查:new_bound_method 是唯一分配点 -- 本实例经调用方根化
        //    (同上),方法对象经本实例->class_ 链类表可达,建成即经返回值写回原槽根化。
        const auto member = *hit;
        if (!is_method(member)) {
            return member;
        }
        return Value::from_obj(new_bound_method(vm.gc(), member, Value::from_obj(this)));
    }

    Opt<Value> ObjInstance::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 调用路径的成员解析:**不绑定**,返回字段/类链里的原值,调用区槽 0 由 CALL_METHOD 保持
        // receiver(方法戳闭包的方法体从槽 0 读 this)。零分配,且每次按当前类链解析(与读路径同
        // 一份可见性:改类/父类方法立即生效)。
        if (const auto entry = fields_.find(Value::from_obj(name))) {
            return entry->value; // 真字段优先(字段里存的可调用值原值直调)
        }
        return class_->load_field(vm, name); // 命中方法戳闭包也不绑定;miss 的类措辞随协议透传
    }

    bool ObjInstance::store_field(AriaVM& vm, ObjString* name, const Value value) {
        // 实例字段动态(无预声明):set 即创建/更新、恒成功;set 走 trivial 分配不触 GC。
        fields_.set(Value::from_obj(name), value);
        return true;
    }

    // 算子/调用协议实现:按名到本实例(实例 fields 优先,可遮蔽类链钩子)再类链取实现 --
    // 「实例上一个叫 `__add__` 的字段/方法就是它的 `+`」。名字取自 VM 常量串表(AriaVM::string_constant;
    // 注册表见 runtime/string_constant.hpp):表在 bootstrap 期驻留并随 VM 根恒久存活,故每次派发零取串开销、零分配。
    Opt<Value> ObjInstance::op_add_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpAdd));
    }

    Opt<Value> ObjInstance::op_sub_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpSub));
    }

    Opt<Value> ObjInstance::op_mul_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpMul));
    }

    Opt<Value> ObjInstance::op_div_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpDiv));
    }

    Opt<Value> ObjInstance::op_mod_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpMod));
    }

    Opt<Value> ObjInstance::op_less_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpLess));
    }

    Opt<Value> ObjInstance::op_less_equal_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpLessEqual));
    }

    Opt<Value> ObjInstance::op_greater_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpGreater));
    }

    Opt<Value> ObjInstance::op_greater_equal_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpGreaterEqual));
    }

    Opt<Value> ObjInstance::op_negate_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpNegate));
    }

    Opt<Value> ObjInstance::op_call_impl(AriaVM& vm) {
        return load_field_unbound(vm, vm.string_constant(StringConstant::OpCall));
    }

    ObjInstance* new_instance(GC& gc, ObjClass* klass) {
        // 守卫纪律见 Object.hpp;调用方须自行根化 klass;建成即写栈(值栈根)。
        return gc.new_object<ObjInstance>(gc, klass);
    }

} // namespace aria
