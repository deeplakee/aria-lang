#include "object/ObjMap.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/EqualGuard.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/PrintGuard.hpp"
#include "runtime/AriaVM.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjMap::ObjMap(GC& gc) : Object{ObjType::MAP}, table_{&gc} {}

    void ObjMap::trace(GC& gc) const noexcept {
        table_.trace(gc); // 遍历占用槽 mark_value key+value(nil/int/f64 无对象子节点)
    }

    bool ObjMap::equals(const Object* other) const noexcept {
        if (this == other) {
            return true;
        }
        if (other == nullptr) {
            return false; // null 恒不等(null 不是任何类型实例)
        }
        // 环闭合:同对重遇已在比较链上,视为相等(余归纳,正则树同构);互环否则无限互递归。
        if (EqualGuard::is_cycle(this, other)) {
            return true;
        }
        const auto map = try_as<ObjMap>(other);
        if (map == nullptr || table_.size() != map->table_.size()) {
            return false;
        }
        const EqualGuard guard{this, other};
        // 键命中按表内语义 ===(find 即 value_identical 匹配),值 value_equal(嵌套经各自 equals 递归);
        // find/value_equal 均无 GC 分配,GC-pure 契约保持。
        for (const auto& [key, value]: table_) {
            if (const auto entry = map->table_.find(key); entry == nullptr || !value_equal(value, entry->value)) {
                return false;
            }
        }
        return true;
    }

    String ObjMap::debug_repr() const {
        // 环防护:自引用/互环时本 map 已在渲染路径上,截断 "{...}"(先查后挂,顺序反了自身即命中)。
        if (PrintGuard::is_cycle(this)) {
            return "{...}";
        }
        const PrintGuard guard{this};
        // {"a": 1} 式:键值均走 format_value_debug;渲染序随占用槽(unspecified,与迭代序同级不承诺)。
        const auto entry_repr = [](const AriaHashTable::Entry& entry) {
            return format_value_debug(entry.key) + ": " + format_value_debug(entry.value);
        };
        return "{" + util::join(table_, ", ", entry_repr) + "}";
    }

    Opt<Value> ObjMap::load_field(AriaVM& vm, ObjString* name) { return vm.map_class()->load_field(vm, name); }

    Opt<Value> ObjMap::load_field_bound(AriaVM& vm, ObjString* name) {
        const auto hit = load_field(vm, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjMap::load_index(AriaVM& vm, const Value key) {
        if (const auto entry = table_.find(key)) {
            return entry->value;
        }
        return vm.fail(ErrorCode::KeyError, "map key not found: {}", format_value_debug(key));
    }

    bool ObjMap::store_index(AriaVM& vm, const Value key, const Value value) {
        table_.set(key, value);
        return true;
    }

    ObjMap* new_map(GC& gc) { return gc.new_object<ObjMap>(gc); }

} // namespace aria
