#include "object/ObjList.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "runtime/AriaVM.hpp"
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

    Opt<Value> ObjList::load_index(AriaVM& vm, const Value key) {
        // 整数键:非整数 TypeMismatch;越界/负数 IndexOutOfBounds(越界值与长度就地拼进文案)。
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "list index must be an integer, got {}", aria::type_name(key));
        }
        const i64 index = key.as_int();
        if (index < 0 || static_cast<u64>(index) >= elements_.size()) {
            return vm.fail(ErrorCode::IndexOutOfBounds, "list index {} out of range", index);
        }
        return elements_[static_cast<usize>(index)];
    }

    bool ObjList::store_index(AriaVM& vm, const Value key, const Value value) {
        // 键检查同读;不自动增长(越界即报,追加走 push 方法);写已存槽恒成功。
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "list index must be an integer, got {}", aria::type_name(key));
        }
        const i64 index = key.as_int();
        if (index < 0 || static_cast<u64>(index) >= elements_.size()) {
            return vm.fail(ErrorCode::IndexOutOfBounds, "list index {} out of range", index);
        }
        elements_[static_cast<usize>(index)] = value;
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
