#include "object/ObjList.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/EqualGuard.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjRange.hpp"
#include "object/PrintGuard.hpp"
#include "runtime/AriaVM.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
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
        // 环闭合:同对重遇已在比较链上,视为相等(余归纳,正则树同构);互环否则无限互递归。
        if (EqualGuard::is_cycle(this, other)) {
            return true;
        }
        const auto list = try_as<ObjList>(other);
        if (list == nullptr || elements_.size() != list->elements_.size()) {
            return false;
        }
        const EqualGuard guard{this, other};
        // 逐元素 value_equal:嵌套 list 经各自 equals 递归;value_equal 无 GC 分配,GC-pure 契约保持。
        for (usize index = 0; index < elements_.size(); ++index) {
            if (!value_equal(elements_[index], list->elements_[index])) {
                return false;
            }
        }
        return true;
    }

    String ObjList::debug_repr() const {
        // 环防护先查后挂:已在渲染路径上截断 "[...]",不截断则元素重遇无限递归。
        if (PrintGuard::is_cycle(this)) {
            return "[...]";
        }
        const PrintGuard guard{this};
        // [1, "ab"] 式:元素走 format_value_debug(嵌套字符串带引号;嵌套 list 递归)。
        return "[" + util::join(elements_, ", ", format_value_debug) + "]";
    }

    Opt<Value> ObjList::load_field(AriaVM& vm, ObjString* name) { return vm.list_class()->load_field(vm, name); }

    Opt<Value> ObjList::load_field_bound(AriaVM& vm, ObjString* name) {
        const auto hit = load_field(vm, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjList::load_index(AriaVM& vm, const Value key) {
        if (const auto range = try_obj<ObjRange>(key)) {
            return slice(vm, range);
        }
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "list index must be an integer, got {}", aria::type_name(key));
        }
        const i64 raw = key.as_int();
        if (const auto slot = util::resolve_index(raw, elements_.size())) {
            return elements_[*slot];
        }
        return vm.fail(ErrorCode::IndexOutOfBounds, "list index {} out of range", raw);
    }

    bool ObjList::store_index(AriaVM& vm, const Value key, const Value value) {
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "list index must be an integer, got {}", aria::type_name(key));
        }
        const i64 raw = key.as_int();
        if (const auto slot = util::resolve_index(raw, elements_.size())) {
            elements_[*slot] = value;
            return true;
        }
        return vm.fail(ErrorCode::IndexOutOfBounds, "list index {} out of range", raw);
    }

    Opt<Value> ObjList::op_add_impl(AriaVM& vm) { return vm.register_value(kListAddFnOffset); }

    Opt<Value> ObjList::op_mul_impl(AriaVM& vm) { return vm.register_value(kListMulFnOffset); }

    Opt<Value> ObjList::slice(AriaVM& vm, const ObjRange* range) {
        // 段解析收口 resolve_slice_bounds:nullopt = 无法形成合法区间,唯一失败点报错就地烘焙。
        const auto segment = resolve_slice_bounds(range, elements_.size());
        if (!segment) {
            return vm.fail(ErrorCode::IndexOutOfBounds, "slice range {} out of range", range->debug_repr());
        }
        // 指针用 data() + 起点:空段起点落在末元素之后,operator[] 越界断言不容它。GC 走查:receiver
        // 与 range 经调用方值栈为根,段拷 trivial 不触 GC,新 list 由 run_load_index 写回原槽根化。
        const auto list   = new_list(vm.gc());
        const auto source = Span<const Value>{elements_.data() + segment->start, segment->count};
        if (segment->is_reversed) {
            list->elements().copy_reversed_from(source); // 升序源段,由 Array 反转追加
        } else {
            list->elements().copy_from(source);
        }
        return Value::from_obj(list);
    }

    ObjList* new_list(GC& gc) { return gc.new_object<ObjList>(gc); }

} // namespace aria
