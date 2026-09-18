#include "object/iterator/ObjIterator.hpp"

#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjIterator::ObjIterator() : Object{ObjType::ITERATOR} {}

    Opt<Value> ObjIterator::load_field(AriaVM& vm, ObjString* name) {
        // 同 ObjList::load_field:先委托类协议查表、后自己绑定。GC 走查:new_bound_method
        // 是唯一分配点 --receiver(this)经调用方 peek 在栈(栈即根)、klass 经 VM 寄存器组
        // 根、命中值本体经类链 field_ 表可达(本地 hit 仅是值拷贝);bound 白色建成由
        // run_load_field 写回原槽根化。
        const auto hit = vm.iterator_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    String ObjIterator::debug_repr() const {
        // 源类型不进文案:语言层单数 Iterator(对标 <upvalue> 稳定短文案)。
        return "<iterator>";
    }

} // namespace aria
