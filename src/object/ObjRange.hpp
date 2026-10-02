#ifndef ARIA_OBJ_RANGE_HPP
#define ARIA_OBJ_RANGE_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class AriaVM;
    class ObjString;

    // range 对象:a..b(含上界)/a...b(不含上界)/a..(无上界)的纯值载体;from > to 表示倒序区间,方向由迭代器推断。
    class ObjRange final : public Object {
    public:
        ObjRange(i64 from, i64 to, bool is_exclusive);

        // 无上界开区间(from..):is_exclusive_ 构造期归一为 false(含否上界无意义)。
        ObjRange(i64 from);

        ~ObjRange() override = default;

        ObjRange(const ObjRange&)            = delete;
        ObjRange& operator=(const ObjRange&) = delete;
        ObjRange(ObjRange&&)                 = delete;
        ObjRange& operator=(ObjRange&&)      = delete;

        [[nodiscard]]
        i64 from() const noexcept {
            return from_;
        }

        // 上界端点;nullopt = 无上界开区间(from..)。
        [[nodiscard]]
        Opt<i64> to() const noexcept {
            return to_;
        }

        // 有上界判定:取 to_ 值前先问它 -- 无上界的 nullopt 是空态、不是端点语义。
        [[nodiscard]]
        bool has_upper() const noexcept {
            return to_.has_value();
        }

        [[nodiscard]]
        bool is_exclusive() const noexcept {
            return is_exclusive_;
        }

        // 纯值无子对象,标记空操作(对标 ObjString)。
        void trace(GC&) const noexcept override {}

        // 壳定长(三标量字段,无外挂 buffer)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjRange);
        }

        // 内容相等:同为 range 且两端点与含否上界全等;其余(含跨类型)恒 false。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 调试渲染:0..10 / 0...10 / 0..(无上界)式。
        [[nodiscard]]
        String debug_repr() const override;

        // 裸读 override:委托 Range bootstrap 类表直取原生值。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override:同一查找命中恒绑 this。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaVM& vm, ObjString* name) override;

    private:
        i64      from_;         // 区间起点端点(恒有)
        Opt<i64> to_;           // 区间终点端点;nullopt = 无上界开区间
        bool     is_exclusive_; // true: from...to(不含上界);false: from..to(含上界);无上界恒 false
    };

    // 工厂:分配 ObjRange(纯值,无入参对象可守);返回对象白色无根,调用方建成即发布进根。
    [[nodiscard]]
    ObjRange* new_range(GC& gc, i64 from, i64 to, bool is_exclusive);

    // 无上界开区间重载(MAKE_RANGE unbounded 位)。
    [[nodiscard]]
    ObjRange* new_range(GC& gc, i64 from);

    // 切片段解析结果:升序源段起点 + 元素数 + 是否反向写入;长度与方向折算全在解析口完成,消费端只按此段拷。
    struct SliceSegment {
        usize start;
        usize count;
        bool  is_reversed;
    };

    // 切片段解析(range + 容器 size -> SliceSegment),有上界/无上界两形态统一在此:有上界两端点各经 resolve_index 从尾计数
    // 且均须落实元素位置;无上界为后缀语义,起点允许 == size 得空段(解构 rest 的空尾据此成立)。纯换算,无分配无 fail。
    [[nodiscard]]
    Opt<SliceSegment> resolve_slice_bounds(const ObjRange* range, usize size) noexcept;

} // namespace aria

#endif // ARIA_OBJ_RANGE_HPP
