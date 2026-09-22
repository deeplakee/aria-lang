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

    Opt<Value> ObjInstance::load_field(AriaVM& vm, ObjString* name) {
        // 1) fields 命中:真字段(含用户存进字段的可调用值)优先,遮蔽类链同名成员。
        if (const auto entry = fields_.find(Value::from_obj(name))) {
            return entry->value;
        }

        // 2) 委托类协议(ObjClass::load_field):沿类链读穿透直读,命中原样回传(类协议不绑定
        //    不缓存);miss 已按类措辞就地 fail --nullopt ⟺ 已 fail,本函数只透传信号,不再
        //    自持实例措辞、不重复烘焙(成员表在类链上,miss 文案随宿主)。查找纯查询无分配;
        //    fail 装箱是分配点 --本实例经调用方根化(VM:LOAD_FIELD peek 在栈 / LOAD_THIS_FIELD
        //    帧槽 0 在栈)。
        const auto hit = class_->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        // 3) 命中值解包 member(类表成员值):方法命中(defining class 戳定的方法闭包,经
        //    is_method(Value) 一步判)现场绑定(this=本实例);其余(静态方法 fun/lambda/原生/静态值)
        //    原值直读。**不写回 fields**(bound 缓存已取消,契约与理由见 ObjInstance.hpp):
        //    读路径每次访问产出一个新 bound -- 方法值是一等值,这一步省不掉;换来的是类/父类
        //    方法改写对既有实例立即生效(monkey patch 完整),以及 fields_ 回归纯字段。
        //
        //    GC 走查:new_bound_method 是唯一分配点 --本实例经调用方根化(VM:LOAD_FIELD peek
        //    在栈 / LOAD_THIS_FIELD 帧槽 0 在栈),方法对象本体经本实例->class_ 链类表可达(本地
        //    member 仅是值拷贝),name 经调用方(常量池/测试守卫)可达;建成即经返回值写回原槽根化。
        const auto member = *hit;
        if (!is_method(member)) {
            return member;
        }
        return Value::from_obj(new_bound_method(vm.gc(), member, Value::from_obj(this)));
    }

    Opt<Value> ObjInstance::resolve_invoke(AriaVM& vm, ObjString* name) {
        // 调用路径的成员解析:与内置类型同一条规则 -- **不绑定**,返回字段/类链里的原值,调用区
        // 槽 0 由 CALL_METHOD 保持 receiver(方法戳闭包的方法体从槽 0 读 this,原生以槽 0 为 this
        // 兼返回槽)。
        // 故每次调用零分配,且每次按当前类链解析(与读路径同一份可见性:改类/父类方法立即生效)。
        if (const auto entry = fields_.find(Value::from_obj(name))) {
            return entry->value; // 真字段优先(字段里存的可调用值原值直调)
        }
        return class_->load_field(vm, name); // 命中方法戳闭包也不绑定;miss 的类措辞随协议透传
    }

    Opt<Value> ObjInstance::op_call_impl(AriaVM& vm) { return resolve_invoke(vm, new_string(vm.gc(), kOpCallName)); }

    bool ObjInstance::store_field(AriaVM& vm, ObjString* name, const Value value) {
        // 实例字段动态(无预声明):set 即创建/更新,永不失败(恒 true;false ⟺ 已 fail)。
        // set 走 trivial 分配不触 GC(GC 核心不变式)。
        fields_.set(Value::from_obj(name), value);
        return true;
    }

    void ObjInstance::trace(GC& gc) const noexcept {
        gc.mark_object(class_);
        fields_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value);缓存 bound 经此级联
    }

    String ObjInstance::debug_repr() const {
        // class_ 恒非空(ctor ASSERT)。
        return std::format("<{} instance>", class_->name()->view());
    }

    ObjInstance* new_instance(GC& gc, ObjClass* klass) {
        // 工厂不替调用方守卫入参:调用方须在调用前自行根化 klass(实例化路径 klass 在栈根化);
        // 返回对象白色无根,建成即写栈(值栈根)。
        return gc.new_object<ObjInstance>(gc, klass);
    }

} // namespace aria
