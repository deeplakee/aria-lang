#include "object/iterator/ObjRangeIterator.hpp"

#include "error/ErrorCode.hpp"
#include "object/ObjRange.hpp"
#include "runtime/AriaVM.hpp"

namespace aria {

    ObjRangeIterator::ObjRangeIterator(const i64 from, const Opt<i64> to, const bool is_exclusive) :
        ObjIterator{}, current_{from}, to_{to}, is_exclusive_{is_exclusive}, forward_{!to.has_value() || from <= *to} {}

    usize ObjRangeIterator::size() const noexcept { return sizeof(ObjRangeIterator); }

    bool ObjRangeIterator::has_next() const noexcept {
        // 五态方向矩阵:无上界恒真(无限序列,消费方自理边界);正/倒向与含/不含上界四种比较见 return;
        // 空区间仅剩 from==to 且不含上界(5...5),首问即 false、零迭代。
        if (!to_) {
            return true;
        }
        return forward_ ? (is_exclusive_ ? current_ < *to_ : current_ <= *to_)
                        : (is_exclusive_ ? current_ > *to_ : current_ >= *to_);
    }

    Opt<Value> ObjRangeIterator::next(AriaVM& vm) {
        // 耗尽 fail-fast:forIn 靠 has_next 把关,仅手写滥用触此。
        if (!has_next()) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        const auto value = Value::from_int(current_);
        // 推进永不溢出:i48 整数域(NaN-boxing payload + 字面量编译期上限)兜底;TagValue 下回绕推满端点不设防(不承诺)。
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
