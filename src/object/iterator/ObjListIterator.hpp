#ifndef ARIA_OBJ_LIST_ITERATOR_HPP
#define ARIA_OBJ_LIST_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"

namespace aria {

    class ObjList;

    // list 迭代器:持被遍历 list 与下一元素下标;游标用下标而非指针,元素缓冲扩容搬迁后仍然有效。
    class ObjListIterator final : public ObjIterator {
    public:
        // list 恒非空(ctor ASSERT);cursor 出厂 0。
        explicit ObjListIterator(ObjList* list);

        ~ObjListIterator() override = default;

        ObjListIterator(const ObjListIterator&)            = delete;
        ObjListIterator& operator=(const ObjListIterator&) = delete;
        ObjListIterator(ObjListIterator&&)                 = delete;
        ObjListIterator& operator=(ObjListIterator&&)      = delete;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override;

        [[nodiscard]]
        bool has_next() const noexcept override;

        [[nodiscard]]
        Opt<Value> next(AriaVM& vm) override;

    private:
        ObjList* list_;   // 被遍历者(恒非空;经 trace 标根)
        usize    cursor_; // 下一元素下标(单调 +1)
    };

    // 工厂:分配 ObjListIterator(单次分配);调用方须已根化 list,返回对象白色无根,建成即写回槽发布。
    [[nodiscard]]
    ObjListIterator* new_list_iterator(GC& gc, ObjList* list);

} // namespace aria

#endif // ARIA_OBJ_LIST_ITERATOR_HPP
