#include "object/ObjMap.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/EqualGuard.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/PrintGuard.hpp"
#include "runtime/ObjMovement.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjMap::ObjMap(GC& gc) : Object{ObjType::MAP}, table_{&gc} {}

    void ObjMap::trace(GC& gc) const noexcept {
        table_.trace(gc); // 遍历占用槽 mark_value key+value(nil/int/f64 无对象子节点)
    }

    bool ObjMap::equals(const Object* other) const noexcept {
        ASSERT(other != nullptr, "null object pointer");
        if (this == other) {
            return true;
        }
        // 环闭合防无限互递归。
        if (EqualGuard::is_cycle(this, other)) {
            return true;
        }
        const auto map = other->try_as<ObjMap>();
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

    String ObjMap::repr() const {
        // 环防护:自引用/互环时本 map 已在渲染路径上,截断 "{...}"(先查后挂,顺序反了自身即命中)。
        if (PrintGuard::is_cycle(this)) {
            return "{...}";
        }
        const PrintGuard guard{this};
        // {"a": 1} 式:键值均走 value_repr;渲染序随占用槽(unspecified,与迭代序同级不承诺)。
        const auto entry_repr = [](const AriaHashTable::Entry& entry) {
            return value_repr(entry.key) + ": " + value_repr(entry.value);
        };
        return "{" + util::join(table_, ", ", entry_repr) + "}";
    }

    Opt<Value> ObjMap::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        const auto klass = ctx.map_class();
        if (const auto v = klass->find_field(name)) {
            return v;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", klass->repr(), name->view());
    }

    Opt<Value> ObjMap::load_field(AriaExeContext& ctx, ObjString* name) { return load_field_from_class(ctx, name); }

    Opt<Value> ObjMap::load_field_bound(AriaExeContext& ctx, ObjString* name) {
        const auto hit = load_field(ctx, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(ctx.gc(), *hit, this));
    }

    Opt<Value> ObjMap::load_index(AriaExeContext& ctx, const Value key) {
        if (const auto entry = table_.find(key)) {
            return entry->value;
        }
        return ctx.fail(ErrorCode::KeyError, "map key not found: {}", value_repr(key));
    }

    bool ObjMap::store_index(AriaExeContext& ctx, const Value key, const Value value) {
        table_.set(key, value);
        return true;
    }

    ObjMap* new_map(GC& gc) { return gc.new_object<ObjMap>(gc); }

} // namespace aria
