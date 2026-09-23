#include "object/ObjRange.hpp"

#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
#include "util/util.hpp"

namespace aria {

    namespace {

        // 内容哈希:from 乘黄金比搅拌后与终点端点折叠,过 hash_num avalanche。终点折叠带
        // 标记位(有界 = 值<<2 | 含否<<1 | 1,无界 = 0),区分无界 from.. 与有界 from..0/
        // from...0;两端点交换或含否上界翻转必须可区分(from..to != to..from != from...to)。
        u32 content_hash(const i64 from, const Opt<i64>& to, const bool is_exclusive) {
            const u64 high  = to ? static_cast<u64>(*to) << 2 | (is_exclusive ? 2u : 0u) | 1u : 0u;
            const u64 mixed = static_cast<u64>(from) * 0x9E3779B97F4A7C15ull ^ high;
            return util::hash_num(mixed);
        }

    } // namespace

    ObjRange::ObjRange(const i64 from, const i64 to, const bool is_exclusive) :
        Object{content_hash(from, to, is_exclusive), ObjType::RANGE}, from_{from}, to_{to},
        is_exclusive_{is_exclusive} {}

    ObjRange::ObjRange(const i64 from) :
        Object{content_hash(from, Opt<i64>{}, false), ObjType::RANGE}, from_{from}, to_{std::nullopt},
        is_exclusive_{false} {} // 含否上界无意义,归一 false(5.. 与 5... 是同一个值)

    bool ObjRange::equals(const Object* other) const noexcept {
        if (this == other) {
            return true;
        }
        const auto range = try_as<ObjRange>(other);
        return range != nullptr && from_ == range->from_ && to_ == range->to_ && is_exclusive_ == range->is_exclusive_;
    }

    String ObjRange::debug_repr() const {
        // 与源码拼写一致:.. 含上界、... 不含;无上界渲染 from..。
        if (!to_) {
            return std::format("{}..", from_);
        }
        return std::format("{}{}{}", from_, is_exclusive_ ? "..." : "..", *to_);
    }

    Opt<Value> ObjRange::load_field(AriaVM& vm, ObjString* name) {
        // 两步形态与 GC 走查见 Object.hpp;命中自持 new_bound_method 恒绑 this。
        const auto hit = vm.range_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjRange::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 不铸 ObjBoundMethod,命中直取类表原生值(契约见 Object.hpp);本体是纯透传。
        return vm.range_class()->load_field(vm, name);
    }

    ObjRange* new_range(GC& gc, const i64 from, const i64 to, const bool is_exclusive) {
        // 工厂无入参对象可守(见头注释)。
        return gc.new_object<ObjRange>(from, to, is_exclusive);
    }

    ObjRange* new_range(GC& gc, const i64 from) { return gc.new_object<ObjRange>(from); }

    Opt<SliceSegment> resolve_slice_bounds(const ObjRange* range, const usize size) noexcept {
        const i64  raw_from     = range->from();
        const bool is_unbounded = !range->has_upper();

        // 起点归一化(从尾计数 + 越界判定)两形态共用;无上界形态另放行「末尾之后一位」= 空段
        //(「从末尾之后取剩余」即空,解构 rest 的空尾据此成立),先短路它。
        if (is_unbounded && raw_from == static_cast<i64>(size)) {
            return SliceSegment{.start = size, .count = 0, .is_reversed = false};
        }
        const auto from = util::resolve_index(raw_from, size);
        if (!from) {
            return std::nullopt;
        }
        // 无上界形态(后缀语义):自起点取到末尾。
        if (is_unbounded) {
            return SliceSegment{.start = *from, .count = size - *from, .is_reversed = false};
        }
        // 有上界:上界同须落在实元素位置(空容器切片自然落 nullopt)。方向由归一化端点的大小关系
        // 定(原始端点可因从尾计数翻转);不含上界少走末元素,两端相等即空段;倒序的升序源段自低端 to 起。
        const auto to = util::resolve_index(*range->to(), size);
        if (!to) {
            return std::nullopt;
        }
        const bool  is_exclusive = range->is_exclusive();
        const bool  is_reversed  = *from > *to;
        const usize start        = is_reversed ? *to + is_exclusive : *from;
        const usize count        = util::abs_diff(*from, *to) + (is_exclusive ? 0 : 1);
        return SliceSegment{.start = start, .count = count, .is_reversed = is_reversed};
    }

} // namespace aria
