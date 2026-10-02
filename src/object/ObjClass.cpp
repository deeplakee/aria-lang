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
        // init_ 快照语义:构造期自 super 派生,此后父 init 变更不传导(Object 根出厂 nil 由 bootstrap 设);
        // ctor 内读 super->init() 纯读无分配 -- new_object 后 ctor 运行其间无 GC 点。
        ASSERT(name != nullptr, "class name must not be null");
    }

    void ObjClass::set_field(ObjString* name, const Value value) {
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
        // 读穿透直读不绑定不缓存;miss 的 fail 装箱是唯一分配点 -- 本类与 name 皆经调用方根化(peek 在栈)。
        if (const auto v = find_field(name)) {
            return v;
        }
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    bool ObjClass::store_field(AriaVM& vm, ObjString* name, const Value value) {
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

    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super) { return gc.new_object<ObjClass>(gc, name, super); }

    ObjClass* new_class(GC& gc, const StringView name, ObjClass* super) {
        // name_str 由本函数内部 intern 并自守跨下方 new_object;super 根化约定同显式名重载。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_class(gc, name_str, super);
    }

} // namespace aria
