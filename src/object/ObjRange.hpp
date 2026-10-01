#ifndef ARIA_OBJ_RANGE_HPP
#define ARIA_OBJ_RANGE_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class AriaVM;
    class ObjString;

    // range 对象(ObjType::RANGE):`a..b`(含上界)/ `a...b`(不含上界)/ `a..`(无上界)的运行期载体,壳定长纯值(无外挂 buffer
    // 、无子对象)。语言方法面只 iter 一个:经 load_field_bound 委托 VM 的 Range bootstrap
    // 类表命中后恒绑定;迭代器不持源对象, 构造期把端点标量拷走自足(见 ObjRangeIterator)。无上界开区间 to_
    // 为空(类型即契约),含否上界无意义,ctor 内归一 is_exclusive_ = false(`5..` 与 `5...` 是同一个值)。
    //   - 内容哈希型不可变对象(Object ctor 注释名单):哈希构造期烘焙(两端点与含否上界折叠过 avalanche);equals 按内容(同
    //     为 range 且三字段全等),`===` 恒指针。无子对象,equals/debug_repr 无环防护义务。
    //   - 不做的面走基类默认:store_field(不可变,基类默认文案即正确行为)、下标与全部 op_* 运算符(无此需求;
    //     instruction-set §6.4 的解构 rest 切片目前不经 range+下标承载)。
    //   - debug_repr():`0..10` / `0...10` 式,与源码拼写一致;显示同文案(基类默认委托,无 string 式显示/调试分叉)。from >
    //     to 即倒序区间(10..1 迭代产出 10->1,方向由迭代器推断,本体字段原样存,10..1 != 1..10);空区间只剩 from == to 且
    //     不含上界,迭代零次。
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

        // 有上界(= 非无上界开区间 from..)。取上界值前先问它:无上界形态没有可取的端点值,
        // nullopt 只是 to_ 的空态、不是端点语义。
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

        // 裸读 override:同一趟类表查找但不铸 ObjBoundMethod,直取类表原生值(权威注见 Object.hpp)。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override:同一趟类表查找,命中自持 new_bound_method 恒绑 this(权威注见 Object.hpp)。
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

    // 切片段解析结果:消费端要的三件 -- 升序源段起点 + 元素数 + 是否反向写入。长度与方向的折算
    // (含否上界少走迭代序末元素、两端相等即空段、倒序段自低端起)全在解析口完成,消费端只按此段拷。
    struct SliceSegment {
        usize start;
        usize count;
        bool  is_reversed;
    };

    // 切片段解析(range + 容器 size -> SliceSegment)。两形态统一在此:
    //   - 有上界:两端点各经 resolve_index 从尾计数,均须落在实元素位置,方向由归一化端点大小关系自带(正序 from <= to,与
    //     range 迭代同一判据);端点越界与空容器为 nullopt。
    //   - 无上界(i..)= 后缀语义:起点从尾计数后允许 == size -- 「末尾之后取剩余」得空段(元素数 0,解构 rest 的空尾据此成
    //     立);越过长度仍为 nullopt。纯换算无分配无 fail。
    [[nodiscard]]
    Opt<SliceSegment> resolve_slice_bounds(const ObjRange* range, usize size) noexcept;

} // namespace aria

#endif // ARIA_OBJ_RANGE_HPP
