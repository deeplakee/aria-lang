#ifndef ARIA_OBJ_RANGE_ITERATOR_HPP
#define ARIA_OBJ_RANGE_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"

namespace aria {

    class ObjRange;

    // range 迭代器(ObjIterator 引擎缝的 range 消费者):五对迭代器中唯一无源对象者 --
    // range 不可变,ctor 期把端点拷成标量自足,不持指针亦无 trace 义务(空体)。游标 =
    // 区间当前值,方向构造期定向(from>to 即倒序:含上界 10..1 产出 10->1,不含上界递减
    // 到 to+1、10...1 产出 10->2),has_next 即一次方向比较,next 产出 current 装箱整数
    // 并按方向推进。无上界开区间(to_ 为空)has_next 恒真、方向恒正序。空区间只剩
    // from==to 且不含上界(5...5),首问即 false、迭代零次;range 无「迭代中变更」问题
    //(不可变)。
    class ObjRangeIterator final : public ObjIterator {
    public:
        ObjRangeIterator(i64 from, Opt<i64> to, bool is_exclusive);

        ~ObjRangeIterator() override = default;

        ObjRangeIterator(const ObjRangeIterator&)            = delete;
        ObjRangeIterator& operator=(const ObjRangeIterator&) = delete;
        ObjRangeIterator(ObjRangeIterator&&)                 = delete;
        ObjRangeIterator& operator=(ObjRangeIterator&&)      = delete;

        [[nodiscard]]
        bool has_next() const noexcept override;

        [[nodiscard]]
        Opt<Value> next(AriaVM& vm) override;

        // 无源对象,标记空操作(对标 ObjString::trace)。
        void trace(GC&) const noexcept override {}

        [[nodiscard]]
        usize size() const noexcept override;

    private:
        i64      current_;      // 下一个产出值(自 from 起步,按方向推进)
        Opt<i64> to_;           // 终点端点;nullopt = 无上界(has_next 恒真、方向恒正序)
        bool     is_exclusive_; // true: from...to(不含上界);false: from..to(含上界)
        bool     forward_;      // 迭代方向:from<=to 正序递增;from>to 倒序递减(构造期定向)
    };

    // 工厂:分配 ObjRangeIterator。只做一次 new_object、无内部新建;调用方须已根化源 range
    //(fn_iter 路径 range 在 slots[0] 栈根),返回对象白色无根,建成即写回槽发布。不持源
    // 指针:分配点若 collect,range 仍由 slots[0] 保命;写回后 range 不可达、随时可回收,
    // 迭代器标量自足不受影响。
    [[nodiscard]]
    ObjRangeIterator* new_range_iterator(GC& gc, const ObjRange* range);

} // namespace aria

#endif // ARIA_OBJ_RANGE_ITERATOR_HPP
