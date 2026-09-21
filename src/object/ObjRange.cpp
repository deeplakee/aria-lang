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

    Opt<Value> ObjRange::load_field(AriaVM& vm, ObjString* name) {
        // 内置侧两步,与实例路径同构(先委托类协议查表、后自己绑定,同 ObjMap::load_field
        // 形):VM 的 Range bootstrap 类经 ObjClass::load_field 沿链读穿透,miss 类措辞 fail
        // 随协议透传;命中即恒绑定 this --内置类表条目全为原生函数、恒为方法,判别无须戳。
        // GC 走查:new_bound_method 是唯一分配点 --receiver(this)经调用方 peek 在栈(栈即
        // 根)、klass 经 VM 寄存器组根、命中值本体经类链 field_ 表可达;bound 白色建成由
        // run_load_field 写回原槽根化。内置侧无 fields 缓存,每次取方法现场物化。
        const auto hit = vm.range_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjRange::resolve_invoke(AriaVM& vm, ObjString* name) {
        // 方法调用解析(PREPARE_METHOD):与 load_field 同一趟类表查找,命中直取类表原生值交 VM 调用
        // -- **不铸 ObjBoundMethod** 正是本 override 存在的理由(load_field 那条读路径要绑定;内置侧
        // 无 fields 缓存可回填,两步形态每取一次方法白铸一个,迭代协议每迭代两次,见集合计划 §4.4);
        // 调用区槽 0 保持 receiver 原样,正是原生要的 this。查找纯查询无分配,故本体是 load_field
        // 结果的纯透传(miss 的 fail 装箱在 ObjClass::load_field 内就地完成,receiver 与 name 由调用
        // 方根化:VM 侧 receiver peek 在栈、name 经常量池)。
        return vm.range_class()->load_field(vm, name);
    }

    String ObjRange::debug_repr() const {
        // 与源码拼写一致:.. 含上界、... 不含(空区间原样渲染两端点);无上界渲染 from..。
        if (!to_) {
            return std::format("{}..", from_);
        }
        return std::format("{}{}{}", from_, is_exclusive_ ? "..." : "..", *to_);
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
        // (「从末尾之后取剩余」即空,解构 rest 的空尾据此成立;负值经归一化只落 [0, size-1],
        // 故该格就是裸值 == size 这一种),先短路它。
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
        // 有上界形态:上界同须落在实元素位置(空容器任何端点都解析不出,与单下标空表行为一致,
        // 故空表切片自然落 nullopt)。方向由归一化端点的大小关系定(原始端点可因从尾计数翻转,
        // 故不按原始值判);不含上界少走迭代序末元素,两端相等即空段;倒序的升序源段自低端的 to
        // 起(被跳过的 to 在低端,须让开一位)。
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
