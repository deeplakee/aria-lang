#include "object/iterator/ObjListIterator.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjList.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjListIterator::ObjListIterator(ObjList* list) : ObjIterator{}, list_{list}, cursor_{0} {
        // list_ 恒非空:铸造点(fn_iter)解出的即对象,无空态语义。
        ASSERT(list != nullptr, "ObjListIterator list must not be null");
    }

    bool ObjListIterator::has_next() const noexcept { return cursor_ < list_->elements().size(); }

    Opt<Value> ObjListIterator::next(AriaVM& vm) {
        // 越界 fail-fast:forIn 靠 has_next 把关,仅绕过协议的手写滥用触此(nil 哨兵与合法
        // nil 元素不可区分)。
        if (cursor_ >= list_->elements().size()) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        return list_->elements()[cursor_++];
    }

    void ObjListIterator::trace(GC& gc) const noexcept {
        gc.mark_object(list_); // 元素经 ObjList::trace 级联
    }

    usize ObjListIterator::size() const noexcept { return sizeof(ObjListIterator); }

    ObjListIterator* new_list_iterator(GC& gc, ObjList* list) {
        // 工厂无守卫义务(见头注释)。
        return gc.new_object<ObjListIterator>(list);
    }

} // namespace aria
