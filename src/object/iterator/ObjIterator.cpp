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

    Opt<Value> ObjIterator::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 方法调用解析(PREPARE_METHOD):与 load_field 同一趟类表查找,命中直取类表原生值交 VM 调用
        // -- **不铸 ObjBoundMethod** 正是本 override 存在的理由(load_field 那条读路径要绑定;方法调用
        // 若改走读路径,每取一次方法白铸一个 bound -- 迭代协议每迭代两次,见集合计划 §4.4);
        // 调用区槽 0 保持 receiver 原样,正是原生要的 this。查找纯查询无分配,故本体是 load_field
        // 结果的纯透传(miss 的 fail 装箱在 ObjClass::load_field 内就地完成,receiver 与 name 由调用
        // 方根化:VM 侧 receiver peek 在栈、name 经常量池)。
        return vm.iterator_class()->load_field(vm, name);
    }

    String ObjIterator::debug_repr() const {
        // 源类型不进文案:语言层单数 Iterator(对标 <upvalue> 稳定短文案)。
        return "<iterator>";
    }

} // namespace aria
