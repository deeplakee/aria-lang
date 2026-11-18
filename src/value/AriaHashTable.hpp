#ifndef ARIA_ARIA_HASH_TABLE_HPP
#define ARIA_ARIA_HASH_TABLE_HPP

#include "common.hpp"
#include "memory/GC.hpp"
#include "memory/HashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    struct ValueHash {
        [[nodiscard]]
        u32 operator()(const Value value) const noexcept {
            return value_hash(value);
        }
    };

    struct ValueEq {
        // 哈希表键用 ===(value_identical),不用 value_equal(==)。
        [[nodiscard]]
        bool operator()(const Value lhs, const Value rhs) const noexcept {
            return value_identical(lhs, rhs);
        }
    };

    class AriaHashTable : public HashTable<Value, Value, ValueHash, ValueEq> {
    public:
        using HashTable<Value, Value, ValueHash, ValueEq>::HashTable;

        void trace(GC& gc) const noexcept {
            for (const auto& [key, value]: *this) {
                gc.mark_value(key);
                gc.mark_value(value);
            }
        }
    };

} // namespace aria

#endif // ARIA_ARIA_HASH_TABLE_HPP
