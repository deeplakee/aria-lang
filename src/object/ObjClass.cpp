#include "object/ObjClass.hpp"

#include <format>

#include "aria.hpp"
#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjClass::ObjClass(GC& gc, ObjString* name, ObjClass* super) :
        Object{ObjType::CLASS}, name_{name}, superclass_{super}, field_{&gc},
        init_{super != nullptr ? super->init() : Value::nil_val()} {
        // name_ 恒非空;superclass_ 唯 Object 根为 nullptr。init_ 构造期自 super 派生(快照语义:
        // 此后父 init 变更不传导;Object 根出厂 nil,由 bootstrap 经 set_field 设)。ctor 内读
        // super->init() 纯读无分配,new_object 后 ctor 运行其间无 GC 点。
        ASSERT(name != nullptr, "ObjClass name must not be null");
    }

    void ObjClass::set_field(ObjString* name, const Value value) {
        // 创建路径唯一公开写入口:落本类自身表(不沿链);"init" 命中同步 init_。
        field_.set(Value::from_obj(name), value); // 继承名/新名新建键、本类已有原槽更新、父表不动
        if (name->view() == kInitName) {
            init_ = value;
        }
    }

    void ObjClass::trace(GC& gc) const noexcept {
        gc.mark_object(name_);
        gc.mark_object(superclass_); // Object 根为 nullptr,mark_object 容 nullptr
        gc.mark_value(init_);        // 构造器方法值(闭包/原生装箱)
        field_.trace(gc);            // 遍历占用槽 mark_value(key+value);方法闭包的 defining_class 经其 trace 级联
    }

    String ObjClass::debug_repr() const {
        // name_ 恒非空(ctor ASSERT)。
        return std::format("<class {}>", name_->view());
    }

    Opt<Value> ObjClass::load_field(AriaVM& vm, ObjString* name) {
        // 读穿透:静态值/方法闭包/原生原样直读,不绑定不缓存。查找纯查询无分配;miss 的 fail
        // 装箱(new_exception)是唯一分配点 -- 本类与 name 皆经调用方根化(VM:LOAD_FIELD peek 在栈)。
        if (const auto v = find_field(name)) {
            return v;
        }
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    bool ObjClass::store_field(AriaVM& vm, ObjString* name, const Value value) {
        // 类上赋值落本类自身表恒成功(动态新增允许);set 走 trivial 分配不触 GC(GC 核心不变式)。
        set_field(name, value); // 继承名/新名新建键、本类已有原槽更新、父表不动;"init" 同步内聚于此
        return true;
    }

    Opt<Value> ObjClass::find_field(ObjString* name) noexcept {
        for (auto klass = this; klass != nullptr; klass = klass->superclass_) {
            if (const auto entry = klass->field_.find(Value::from_obj(name))) {
                return entry->value; // 读穿透:链上首个命中值(拷出;写另经 set_field 落接收类)
            }
        }
        return std::nullopt;
    }

    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super) {
        // 守卫纪律见 Object.hpp;调用方须自行根化 name 与 super。init_ 由构造函数自 super 派生。
        return gc.new_object<ObjClass>(gc, name, super);
    }

    ObjClass* new_class(GC& gc, const StringView name, ObjClass* super) {
        // name_str 由本函数内部 intern 并自守跨下方 new_object;super 根化约定同显式名重载。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_class(gc, name_str, super);
    }

} // namespace aria
