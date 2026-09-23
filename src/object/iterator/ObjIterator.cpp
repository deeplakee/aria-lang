#include "object/iterator/ObjIterator.hpp"

#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjIterator::ObjIterator() : Object{ObjType::ITERATOR} {}

    Opt<Value> ObjIterator::load_field(AriaVM& vm, ObjString* name) {
        // 两步形态与 GC 走查见 Object.hpp 协议契约;命中自持 new_bound_method 恒绑 this。
        const auto hit = vm.iterator_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjIterator::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 不铸 ObjBoundMethod,命中直取类表原生值交 VM 调用(契约见 Object.hpp);miss 的 fail
        // 装箱在 ObjClass::load_field 内就地完成,receiver 与 name 由调用方根化,本体是纯透传。
        return vm.iterator_class()->load_field(vm, name);
    }

    String ObjIterator::debug_repr() const {
        // 源类型不进文案:语言层单数 Iterator(对标 <upvalue> 稳定短文案)。
        return "<iterator>";
    }

} // namespace aria
