#ifndef ARIA_OBJ_RANGE_HPP
#define ARIA_OBJ_RANGE_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class ObjString;

    // 升序源段的起点、长度与输出方向，供切片拷贝使用。
    struct SliceSegment {
        usize start;
        usize count;
        bool  is_reversed;
    };

    class ObjRange final : public Object {
    public:
        ObjRange(i64 from, i64 to, bool is_exclusive);

        // 无上界时 is_exclusive 归一为 false。
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

        [[nodiscard]]
        Opt<i64> to() const noexcept {
            return to_;
        }

        [[nodiscard]]
        bool has_upper() const noexcept {
            return to_.has_value();
        }

        [[nodiscard]]
        bool is_exclusive() const noexcept {
            return is_exclusive_;
        }

        // 有上界时两端均须指向元素；无上界时起点可等于 size，表示空尾段。
        [[nodiscard]]
        Opt<SliceSegment> resolve_slice_bounds(usize size) const noexcept;

        void trace(GC&) const noexcept override {}

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjRange);
        }

        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

    private:
        i64      from_;         // 区间起点端点(恒有)
        Opt<i64> to_;           // 区间终点端点;nullopt = 无上界开区间
        bool     is_exclusive_; // true: from...to(不含上界);false: from..to(含上界);无上界恒 false
    };

    [[nodiscard]]
    ObjRange* new_range(GC& gc, i64 from, i64 to, bool is_exclusive);

    [[nodiscard]]
    ObjRange* new_range(GC& gc, i64 from);

} // namespace aria

#endif // ARIA_OBJ_RANGE_HPP
