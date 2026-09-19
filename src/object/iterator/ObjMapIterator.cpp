#include "object/iterator/ObjMapIterator.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "runtime/AriaVM.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjMapIterator::ObjMapIterator(ObjMap* map) : ObjIterator{}, map_{map}, slot_{0} {
        // map_ 恒非空:铸造点(iter_fn)解出的即对象,无空态语义。
        ASSERT(map != nullptr, "ObjMapIterator map must not be null");
    }

    bool ObjMapIterator::has_next() const noexcept {
        return map_->table().next_occupied(slot_) != AriaHashTable::kNpos;
    }

    Opt<Value> ObjMapIterator::next(AriaVM& vm) {
        // 越界 fail-fast:forIn 靠 has_next 把关,仅绕过协议的手写滥用触此(同 ObjListIterator)。
        const usize slot = map_->table().next_occupied(slot_);
        if (slot == AriaHashTable::kNpos) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        slot_ = slot + 1;
        // 铸 [k, v] 二元 list 返回。GC 时序:迭代器在调用方 slots[0] 于栈根、map 经其
        // trace 可达(键值随之可达);new_list 顶部 maybe_collect 时 list 未诞生,建成
        // push 两元素(trivial 不触 GC)后随返回值写回槽发布,窗口内无 GC 点。
        const auto& [key, value] = map_->table().entry_at(slot);
        const auto list          = new_list(vm.gc());
        list->elements().push(key);
        list->elements().push(value);
        return Value::from_obj(list);
    }

    void ObjMapIterator::trace(GC& gc) const noexcept {
        gc.mark_object(map_); // 键值经 ObjMap::trace 级联
    }

    usize ObjMapIterator::size() const noexcept { return sizeof(ObjMapIterator); }

    ObjMapIterator* new_map_iterator(GC& gc, ObjMap* map) {
        // 工厂无守卫义务(见头注释)。
        return gc.new_object<ObjMapIterator>(map);
    }

} // namespace aria
