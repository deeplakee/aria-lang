#include "object/iterator/ObjRangeIterator.hpp"

#include "error/ErrorCode.hpp"
#include "object/ObjRange.hpp"
#include "runtime/AriaVM.hpp"

namespace aria {

    ObjRangeIterator::ObjRangeIterator(const i64 low, const i64 high, const bool is_exclusive) :
        ObjIterator{}, current_{low}, high_{high}, is_exclusive_{is_exclusive} {}

    bool ObjRangeIterator::has_next() const noexcept {
        // 含上界 current<=high / 不含 current<high;low>high(或 low==high 且不含)首问即 false,零迭代。
        return is_exclusive_ ? current_ < high_ : current_ <= high_;
    }

    Opt<Value> ObjRangeIterator::next(AriaVM& vm) {
        // 耗尽 fail-fast:forIn 靠 has_next 把关,仅绕过协议的手写滥用触此(同 ObjMapIterator)。
        if (!has_next()) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        const auto value = Value::from_int(current_);
        // 整数域 i48(NaN-boxing payload + 字面量编译期上限)兜底,推进永不溢出;TagValue
        // 配置下算术回绕推满上界的理论边不设防(批 7 拍板,与 map 迭代中变更同级不承诺)。
        ++current_;
        return value;
    }

    usize ObjRangeIterator::size() const noexcept { return sizeof(ObjRangeIterator); }

    ObjRangeIterator* new_range_iterator(GC& gc, const ObjRange* range) {
        // 工厂只读源的三标量,不持有(见头注释)。
        return gc.new_object<ObjRangeIterator>(range->low(), range->high(), range->is_exclusive());
    }

} // namespace aria
