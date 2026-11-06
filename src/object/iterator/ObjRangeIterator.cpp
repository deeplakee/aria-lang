#include "object/iterator/ObjRangeIterator.hpp"

#include "error/ErrorCode.hpp"
#include "object/ObjRange.hpp"
#include "runtime/ObjMovement.hpp"

namespace aria {

    ObjRangeIterator::ObjRangeIterator(const i64 from, const Opt<i64> to, const bool is_exclusive) :
        ObjIterator{}, current_{from}, to_{to}, is_exclusive_{is_exclusive}, forward_{!to.has_value() || from <= *to} {}

    usize ObjRangeIterator::size() const noexcept { return sizeof(ObjRangeIterator); }

    bool ObjRangeIterator::has_next() const noexcept {
        // 五态方向矩阵:无上界恒真(域外请求由 next 报溢出);正/倒向与含/不含上界四种比较见 return;
        // 空区间仅剩 from==to 且不含上界(5...5),首问即 false、零迭代。
        if (!to_) {
            return true;
        }
        return forward_ ? (is_exclusive_ ? current_ < *to_ : current_ <= *to_)
                        : (is_exclusive_ ? current_ > *to_ : current_ >= *to_);
    }

    Opt<Value> ObjRangeIterator::next(AriaExeContext& ctx) {
        // 耗尽 fail-fast(仅手写滥用触此)。
        if (!has_next()) {
            return ctx.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        if (current_ < kIntMin || current_ > kIntMax) {
            return ctx.fail(ErrorCode::IntegerOverflow, "integer result out of range");
        }
        const auto value = Value::from_int(current_);
        // i64 游标允许推进到 i48 域外一位:有界区间此后耗尽,无上界区间下次 next 才报溢出。
        if (forward_) {
            ++current_;
        } else {
            --current_;
        }
        return value;
    }

    ObjRangeIterator* new_range_iterator(GC& gc, const ObjRange* range) {
        return gc.new_object<ObjRangeIterator>(range->from(), range->to(), range->is_exclusive());
    }

} // namespace aria
