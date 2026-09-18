#include "object/ObjList.hpp"

#include "memory/GC.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjList::ObjList(GC& gc) : Object{ObjType::LIST}, elements_{&gc} {}

    void ObjList::trace(GC& gc) const noexcept {
        elements_.trace(gc); // 遍历元素 mark_value(nil/int/f64 无对象子节点)
    }

    bool ObjList::equals(const Object* other) const noexcept {
        if (this == other) {
            return true;
        }
        const auto list = try_as<ObjList>(other);
        if (list == nullptr || elements_.size() != list->elements_.size()) {
            return false;
        }
        // 逐元素 value_equal:嵌套 list 经各自 equals 递归;value_equal 无分配,GC-pure 契约保持。
        for (usize index = 0; index < elements_.size(); ++index) {
            if (!value_equal(elements_[index], list->elements_[index])) {
                return false;
            }
        }
        return true;
    }

    String ObjList::debug_repr() const {
        // [1, "ab"] 式:元素走 format_value_debug(嵌套字符串带引号;嵌套 list 递归 debug_repr)。
        String repr = "[";
        for (usize index = 0; index < elements_.size(); ++index) {
            if (index != 0) {
                repr += ", ";
            }
            repr += format_value_debug(elements_[index]);
        }
        repr += ']';
        return repr;
    }

    ObjList* new_list(GC& gc) {
        // 工厂无入参对象可守;调用方建成即发布进根(见头注释)。
        return gc.new_object<ObjList>(gc);
    }

} // namespace aria
