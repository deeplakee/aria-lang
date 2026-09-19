#ifndef ARIA_OBJ_MAP_ITERATOR_HPP
#define ARIA_OBJ_MAP_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"

namespace aria {

    class ObjMap;

    // map 迭代器(ObjIterator 引擎缝的 map 消费者):被遍历 map + 下一待扫槽位,游标沿
    // 占用槽单调推进(HashTable::next_occupied 扫描)。next 每步产出 [k, v] 二元 list
    //(每步一次小分配,计划 D4 拍板接受)。迭代中变更容器不设防(v1 不承诺,D4):rehash/
    // compact 搬迁槽位,游标可能跳元素或重复,由「不承诺」兜住,不做版本守卫。
    class ObjMapIterator final : public ObjIterator {
    public:
        // map 恒非空(ctor ASSERT);slot 出厂 0。
        explicit ObjMapIterator(ObjMap* map);

        ~ObjMapIterator() override = default;

        ObjMapIterator(const ObjMapIterator&)            = delete;
        ObjMapIterator& operator=(const ObjMapIterator&) = delete;
        ObjMapIterator(ObjMapIterator&&)                 = delete;
        ObjMapIterator& operator=(ObjMapIterator&&)      = delete;

        [[nodiscard]]
        bool has_next() const noexcept override;

        [[nodiscard]]
        Opt<Value> next(AriaVM& vm) override;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override;

    private:
        ObjMap* map_;  // 被遍历者(恒非空;经 trace 标根)
        usize   slot_; // 下一待扫槽位(单调推进;越过表容量即耗尽)
    };

    // 工厂:分配 ObjMapIterator。只做一次 new_object、无内部新建;调用方须已根化 map
    // (iter_fn 路径 map 在 slots[0] 栈根),返回对象白色无根,建成即写回槽发布。
    [[nodiscard]]
    ObjMapIterator* new_map_iterator(GC& gc, ObjMap* map);

} // namespace aria

#endif // ARIA_OBJ_MAP_ITERATOR_HPP
