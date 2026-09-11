#include "object/ObjClass.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjClass::ObjClass(GC& gc, ObjString* name, ObjClass* super) :
        Object{ObjType::CLASS}, name_{name}, superclass_{super}, field_{&gc},
        init_{super != nullptr ? super->init() : Value::nil_val()} {
        // name_ 恒非空(ctor ASSERT,与 ObjModule::name_ 同模式);superclass_ 唯 Object 根为 nullptr。
        // init_ 构造期自 super 派生:super 非空即继承父 init_ 当前值(快照语义 -- 此后父 init
        // 变更不再传导),Object 根(super==nullptr)出厂 nil、由 bootstrap 经 set_field 设。
        // ctor 内读 super->init() 纯读无分配无 GC 点:值经调用方根化的 super 可达,new_object
        // 内部 maybe_collect 后 ctor 运行、其间无 GC 触发点。
        ASSERT(name != nullptr, "ObjClass name must not be null");
    }

    Opt<Value> ObjClass::find_field(ObjString* name) noexcept {
        for (auto cls = this; cls != nullptr; cls = cls->superclass_) {
            if (const auto entry = cls->field_.find(Value::from_obj(name))) {
                return entry->value; // 读穿透:链上首个命中值(拷出;写另经 set_field 落接收类)
            }
        }
        return std::nullopt;
    }

    void ObjClass::set_field(ObjString* name, Value value) {
        // 创建路径统一写入口(公开 API):落本类自身表(不沿链);"init" 同步 init_
        //(表槽/init_ 一致由本入口自维护)。
        field_.upsert(Value::from_obj(name))->value = value; // 继承名/新名新建键、本类已有原槽更新、父表不动
        if (name->view() == "init") {
            init_ = value;
        }
    }

    Opt<Value> ObjClass::load_field(AriaVM& vm, ObjString* name) {
        // 读穿透:静态值/静态方法闭包/原生原样直读,不绑定不缓存。查找路径纯查询无分配;
        // miss 的 fail 装箱(new_exception)是唯一分配点 --本类经调用方根化(VM:LOAD_FIELD
        // peek 在栈 / 实例组合路径经实例可达),name 经调用方(常量池/测试守卫)可达。
        if (const auto v = find_field(name)) {
            return v;
        }
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    bool ObjClass::store_field(AriaVM& vm, ObjString* name, Value value) {
        // 类上赋值落本类自身表,恒成功:原槽更新、继承名/新名新建键遮蔽、父表不动
        //(动态新增允许)。vm 为协议签名统一保留,本 override 无 fail 路径;upsert 走
        // trivial 分配不触 GC(GC 核心不变式),无 GC 点。
        set_field(name, value); // 继承名/新名新建键、本类已有原槽更新、父表不动;"init" 同步内聚于此
        return true;
    }

    void ObjClass::trace(GC& gc) const noexcept {
        gc.mark_object(name_);
        gc.mark_object(superclass_); // Object 根为 nullptr,mark_object 容 nullptr
        gc.mark_value(init_);        // 构造器方法值(闭包/原生装箱)
        field_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value);方法闭包的 defining_class 经其 trace 级联
    }

    String ObjClass::debug_repr() const {
        // name_ 恒非空(ctor ASSERT)。
        return std::format("<class {}>", name_->view());
    }

    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super) {
        // 工厂不替调用方守卫入参:只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 name 与 super(跨 new_object 顶 maybe_collect)。init_ 出厂值由 ObjClass 构造
        // 函数自 super 派生,工厂纯分配(详见头注释)。
        return gc.new_object<ObjClass>(gc, name, super);
    }

    ObjClass* new_class(GC& gc, const StringView name, ObjClass* super) {
        // StringView 名重载:name_str 经 intern 由本函数内部新建,工厂自行守卫跨下方 new_object;
        // super 的根化约定同显式名重载。委托显式名重载。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_class(gc, name_str, super);
    }

} // namespace aria
