#include "object/ObjClass.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjClass::ObjClass(GC& gc, ObjString* name, ObjClass* super) :
        Object{ObjType::CLASS}, name_{name}, superclass_{super}, field_{&gc}, init_{nullptr} {
        // name_ 恒非空(ctor ASSERT,与 ObjModule::name_ 同模式);superclass_ 唯 Object 根为 nullptr。
        ASSERT(name != nullptr, "ObjClass name must not be null");
    }

    Value* ObjClass::find_field(ObjString* name) noexcept {
        for (auto cls = this; cls != nullptr; cls = cls->superclass_) {
            if (const auto entry = cls->field_.find(Value::from_obj(name))) {
                return &entry->value; // 读穿透:链上首个命中槽(只读/存在性检查,写另落接收类 upsert_field)
            }
        }
        return nullptr;
    }

    Value* ObjClass::upsert_field(ObjString* name) { return &field_.upsert(Value::from_obj(name))->value; }

    void ObjClass::trace(GC& gc) const noexcept {
        gc.mark_object(name_);
        gc.mark_object(superclass_); // Object 根为 nullptr,mark_object 容 nullptr
        gc.mark_object(init_);       // ctor nullptr 态(seed 前)容 nullptr
        field_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value);方法闭包的 defining_class 经其 trace 级联
    }

    String ObjClass::to_string() const {
        // name_ 恒非空(ctor ASSERT)。
        return std::format("<class {}>", name_->view());
    }

    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super) {
        // 工厂不替调用方守卫入参:只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 name 与 super(跨 new_object 顶 maybe_collect)。
        return gc.new_object<ObjClass>(gc, name, super);
    }

} // namespace aria
