#include "object/ObjInstance.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjInstance::ObjInstance(GC& gc, ObjClass* cls) : Object{ObjType::INSTANCE}, class_{cls}, fields_{&gc} {
        // class_ 恒非空(ctor ASSERT):实例必有类。
        ASSERT(cls != nullptr, "ObjInstance class must not be null");
    }

    Opt<Value> ObjInstance::load_field(AriaVM& vm, ObjString* name) {
        // 1) fields 命中:真字段与 bound-method 缓存同居同 keyspace(M5 决策 4),命中优先
        //    即真字段遮蔽同名方法与缓存项(铁则 3),无需区分直用。
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
        //    is_method(Value) 一步判)现场绑定(this=本实例)并回填 fields 缓存(铁则 1:
        //    只缓存绑定 --需要分配的--方法;快照语义,类上改写后新解析见新值);
        //    其余(静态方法 fun/lambda/原生/静态值)原值直读不缓存。
        //
        //    GC 走查:new_bound_method 是唯一分配点 --本实例经调用方根化,方法对象本体经本
        //    实例->class_ 链类表可达(本地 member 仅是值拷贝),name 经调用方(常量池/测试守卫)可达;
        //    绑定建成后回填 set 走 trivial 分配不触 GC(核心不变式),无守卫必要。
        const auto member = *hit;
        if (!is_method(member)) {
            return member; // 非方法槽值:直读不缓存(铁则 1 --静态槽可变,值缓存进实例会读陈旧)
        }
        const auto bound = new_bound_method(vm.gc(), member, Value::from_obj(this));
        fields_.set(Value::from_obj(name), Value::from_obj(bound));
        return Value::from_obj(bound);
    }

    bool ObjInstance::store_field(AriaVM& vm, ObjString* name, Value value) {
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

    ObjInstance* new_instance(GC& gc, ObjClass* cls) {
        // 工厂不替调用方守卫入参:调用方须在调用前自行根化 cls(实例化路径 cls 在栈根化);
        // 返回对象白色无根,建成即写栈(值栈根)。
        return gc.new_object<ObjInstance>(gc, cls);
    }

} // namespace aria
