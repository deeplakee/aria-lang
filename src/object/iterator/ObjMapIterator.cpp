#include "object/iterator/ObjMapIterator.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "runtime/AriaVM.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjMapIterator::ObjMapIterator(ObjMap* map) : ObjIterator{}, map_{map}, cursor_{map->table().begin()} {
        // map_ 恒非空:铸造点(fn_iter)解出的即对象,无空态语义。
        ASSERT(map != nullptr, "map must not be null");
    }

    void ObjMapIterator::trace(GC& gc) const noexcept {
        gc.mark_object(map_); // 键值经 ObjMap::trace 级联
    }

    usize ObjMapIterator::size() const noexcept { return sizeof(ObjMapIterator); }

    bool ObjMapIterator::has_next() const noexcept { return cursor_ != map_->table().end(); }

    Opt<Value> ObjMapIterator::next(AriaVM& vm) {
        // 耗尽 fail-fast:forIn 靠 has_next 把关,仅绕过协议的手写滥用触此(同 ObjListIterator)。
        if (!has_next()) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        const auto& [key, value] = *cursor_;
        ++cursor_;
        // 铸 [k, v] 二元 list 返回。GC 时序:迭代器在调用方 slots[0] 于栈根、map 经其
        // trace 可达(键值随之可达);new_list 顶部 maybe_collect 时 list 未诞生,建成
        // push 两元素(trivial 不触 GC)后随返回值写回槽发布,窗口内无 GC 点。
        const auto list = new_list(vm.gc());
        list->elements().push(key);
        list->elements().push(value);
        return Value::from_obj(list);
    }

    ObjMapIterator* new_map_iterator(GC& gc, ObjMap* map) {
        // 工厂无守卫义务(见头注释)。
        return gc.new_object<ObjMapIterator>(map);
    }

} // namespace aria
