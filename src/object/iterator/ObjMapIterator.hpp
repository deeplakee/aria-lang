#ifndef ARIA_OBJ_MAP_ITERATOR_HPP
#define ARIA_OBJ_MAP_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class ObjMap;

    // map 迭代器(ObjIterator 引擎缝的 map 消费者):被遍历 map + HashTable 只读迭代器作
    // 槽位游标(跳空槽/墓碑,推进即占用槽序)。next 每步产出 [k, v] 二元 list(每步一次
    // 小分配,拍板接受)。迭代中变更容器不设防(v1 不承诺):rehash/compact
    // 搬迁槽位使游标失效(HashTable::const_iterator 失效语义),可能跳元素或重复,由
    // 「不承诺」兜住,不做版本守卫。
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
