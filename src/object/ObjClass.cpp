#include "object/ObjClass.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjClass::ObjClass(GC& gc, ObjString* name, ObjClass* super) :
        Object{ObjType::CLASS}, name_{name}, superclass_{super}, field_{&gc}, init_{Value::nil_val()} {
        // name_ 恒非空(ctor ASSERT,与 ObjModule::name_ 同模式);superclass_ 唯 Object 根为 nullptr。
        // init_ 播 nil(Value 默认构造是不定值,须显式初始化):工厂只分配不 seed,由调用方
        // 写入(MAKE_CLASS 执行期继承父 init;Object 根由 VM bootstrap 设)。
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
        gc.mark_value(init_);        // 构造器方法值(闭包/原生装箱)
        field_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value);方法闭包的 defining_class 经其 trace 级联
    }

    String ObjClass::debug_repr() const {
        // name_ 恒非空(ctor ASSERT)。
        return std::format("<class {}>", name_->view());
    }

    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super) {
        // 工厂不替调用方守卫入参:只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 name 与 super(跨 new_object 顶 maybe_collect)。init_ 出厂恒 nil(纯分配工厂,
        // 与 new_function/new_closure 同纪律),seed 责任在调用方。
        return gc.new_object<ObjClass>(gc, name, super);
    }

} // namespace aria
