#ifndef ARIA_OBJ_LIST_ITERATOR_HPP
#define ARIA_OBJ_LIST_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"

namespace aria {

    class ObjList;

    // list 迭代器(ObjIterator 引擎缝的 list 消费者):被遍历 list + 下一元素下标,游标
    // 单调 +1。迭代中变更容器不设防(v1 不承诺):push 的新尾会被遍历到,pop 缩界后越界
    // 由 next 的 fail-fast 拦住;下标式游标不悬垂(元素缓冲扩容搬迁,下标仍有效)。
    class ObjListIterator final : public ObjIterator {
    public:
        // list 恒非空(ctor ASSERT);cursor 出厂 0。
        explicit ObjListIterator(ObjList* list);

        ~ObjListIterator() override = default;

        ObjListIterator(const ObjListIterator&)            = delete;
        ObjListIterator& operator=(const ObjListIterator&) = delete;
        ObjListIterator(ObjListIterator&&)                 = delete;
        ObjListIterator& operator=(ObjListIterator&&)      = delete;

        [[nodiscard]]
        bool has_next() const noexcept override;

        [[nodiscard]]
        Opt<Value> next(AriaVM& vm) override;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override;

    private:
        ObjList* list_;   // 被遍历者(恒非空;经 trace 标根)
        usize    cursor_; // 下一元素下标(单调 +1)
    };

    // 工厂:分配 ObjListIterator。只做一次 new_object、无内部新建;调用方须已根化 list
    // (iter_fn 路径 list 在 slots[0] 栈根),返回对象白色无根,建成即写回槽发布。
    [[nodiscard]]
    ObjListIterator* new_list_iterator(GC& gc, ObjList* list);

} // namespace aria

#endif // ARIA_OBJ_LIST_ITERATOR_HPP
