#ifndef ARIA_OBJ_RANGE_ITERATOR_HPP
#define ARIA_OBJ_RANGE_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"

namespace aria {

    class ObjRange;

    // range 迭代器:不持源对象,构造时把端点拷成标量;游标即区间当前值,构造期定向(from > to 为倒序),
    // 无上界区间恒有下一个元素。
    class ObjRangeIterator final : public ObjIterator {
    public:
        ObjRangeIterator(i64 from, Opt<i64> to, bool is_exclusive);

        ~ObjRangeIterator() override = default;

        ObjRangeIterator(const ObjRangeIterator&)            = delete;
        ObjRangeIterator& operator=(const ObjRangeIterator&) = delete;
        ObjRangeIterator(ObjRangeIterator&&)                 = delete;
        ObjRangeIterator& operator=(ObjRangeIterator&&)      = delete;

        void trace(GC&) const noexcept override {}

        [[nodiscard]]
        usize size() const noexcept override;

        [[nodiscard]]
        bool has_next() const noexcept override;

        [[nodiscard]]
        Opt<Value> next(AriaVM& vm) override;

    private:
        i64      current_;      // 下一个产出值(自 from 起步,按方向推进)
        Opt<i64> to_;           // 终点端点;nullopt = 无上界(has_next 恒真、方向恒正序)
        bool     is_exclusive_; // true: from...to(不含上界);false: from..to(含上界)
        bool     forward_;      // 迭代方向:from<=to 正序递增;from>to 倒序递减(构造期定向)
    };

    // 工厂:一次 new_object 无内部新建;调用方须已根化源 range(fn_iter 路径在 slots[0] 栈根),
    // 返回对象白色无根、建成即写回槽发布。不持源指针:写回后 range 不可达、随时可回收,迭代器标量自足。
    [[nodiscard]]
    ObjRangeIterator* new_range_iterator(GC& gc, const ObjRange* range);

} // namespace aria

#endif // ARIA_OBJ_RANGE_ITERATOR_HPP
