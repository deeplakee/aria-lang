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

} // namespace aria
