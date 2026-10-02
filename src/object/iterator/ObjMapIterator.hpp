#ifndef ARIA_OBJ_MAP_ITERATOR_HPP
#define ARIA_OBJ_MAP_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class ObjMap;

    // map 迭代器:持被遍历 map 与其哈希表的只读迭代器;next 每步产出一个 [k, v] 二元 list。
    class ObjMapIterator final : public ObjIterator {
    public:
        // map 恒非空(ctor ASSERT);游标出厂在 begin()。
        explicit ObjMapIterator(ObjMap* map);

        ~ObjMapIterator() override = default;

        ObjMapIterator(const ObjMapIterator&)            = delete;
        ObjMapIterator& operator=(const ObjMapIterator&) = delete;
        ObjMapIterator(ObjMapIterator&&)                 = delete;
        ObjMapIterator& operator=(ObjMapIterator&&)      = delete;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override;

        [[nodiscard]]
        bool has_next() const noexcept override;

        [[nodiscard]]
        Opt<Value> next(AriaVM& vm) override;

    private:
        ObjMap*                       map_;    // 被遍历者(恒非空;经 trace 标根)
        AriaHashTable::const_iterator cursor_; // 槽位扫描游标(占用槽序推进)
    };

    // 工厂:分配 ObjMapIterator(单次分配);调用方须已根化 map,返回对象白色无根,建成即写回槽发布。
    [[nodiscard]]
    ObjMapIterator* new_map_iterator(GC& gc, ObjMap* map);

} // namespace aria

#endif // ARIA_OBJ_MAP_ITERATOR_HPP
