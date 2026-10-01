#include "object/iterator/ObjIterator.hpp"

#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjIterator::ObjIterator() : Object{ObjType::ITERATOR} {}

    String ObjIterator::debug_repr() const {
        // 源类型不进文案:语言层单数 Iterator。
        return "<iterator>";
    }

    Opt<Value> ObjIterator::load_field(AriaVM& vm, ObjString* name) {
        // 裸查找:命中直取 Iterator bootstrap 类表原生值,不铸 ObjBoundMethod(契约见 Object.hpp);本体是纯透传。
        return vm.iterator_class()->load_field(vm, name);
    }

    Opt<Value> ObjIterator::load_field_bound(AriaVM& vm, ObjString* name) {
        // 绑定读:同名裸查找命中即无条件绑定 this(类表条目全为原生恒为方法;权威注见 Object.hpp)。
        const auto hit = load_field(vm, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

} // namespace aria
