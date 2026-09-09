#include "object/ObjInstance.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjInstance::ObjInstance(GC& gc, ObjClass* cls) : Object{ObjType::INSTANCE}, class_{cls}, fields_{&gc} {
        // class_ 恒非空(ctor ASSERT):实例必有类。
        ASSERT(cls != nullptr, "ObjInstance class must not be null");
    }

    void ObjInstance::trace(GC& gc) const noexcept {
        gc.mark_object(class_);
        fields_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value);缓存 bound 经此级联
    }

    String ObjInstance::to_string() const {
        // class_ 恒非空(ctor ASSERT)。
        return std::format("<{} instance>", class_->name()->view());
    }

    ObjInstance* new_instance(GC& gc, ObjClass* cls) {
        // 工厂不替调用方守卫入参:只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 cls(实例化路径 cls 在栈根化)。
        return gc.new_object<ObjInstance>(gc, cls);
    }

} // namespace aria
