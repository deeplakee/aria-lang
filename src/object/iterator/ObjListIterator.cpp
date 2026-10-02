#include "object/iterator/ObjListIterator.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjList.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjListIterator::ObjListIterator(ObjList* list) : ObjIterator{}, list_{list}, cursor_{0} {
        ASSERT(list != nullptr, "list must not be null");
    }

    void ObjListIterator::trace(GC& gc) const noexcept {
        gc.mark_object(list_); // 元素经 ObjList::trace 级联
    }

    usize ObjListIterator::size() const noexcept { return sizeof(ObjListIterator); }

    bool ObjListIterator::has_next() const noexcept { return cursor_ < list_->elements().size(); }

    Opt<Value> ObjListIterator::next(AriaVM& vm) {
        // 越界 fail-fast:forIn 靠 has_next 把关,仅手写滥用触此(nil 哨兵与合法 nil 元素不可区分)。
        if (cursor_ >= list_->elements().size()) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        return list_->elements()[cursor_++];
    }

    ObjListIterator* new_list_iterator(GC& gc, ObjList* list) { return gc.new_object<ObjListIterator>(list); }

} // namespace aria
