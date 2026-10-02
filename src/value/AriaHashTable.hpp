#ifndef ARIA_ARIA_HASH_TABLE_HPP
#define ARIA_ARIA_HASH_TABLE_HPP

#include "common.hpp"
#include "memory/GC.hpp"
#include "memory/HashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    // HashTable 模板参数仿函数:转发 value_hash;保留仿函数形式因 HashTable 以可调用类型作模板参数。
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

    // 绑定 Value 的 aria 哈希表:继承 HashTable<Value,Value,ValueHash,ValueEq>(Swiss Table)全部接口,
    // 加 trace(GC&)(遍历占用槽 mark_value key+value)。基类 dtor 非虚但本子类不作多态基;不可拷贝/移动。
    class AriaHashTable : public HashTable<Value, Value, ValueHash, ValueEq> {
    public:
        using HashTable<Value, Value, ValueHash, ValueEq>::HashTable; // 继承 explicit HashTable(GC*) ctor

        // 迭代器只看 ctrl、不加载非占用 Entry,mark 遍历对垃圾槽安全。
        void trace(GC& gc) const noexcept {
            for (const auto& [key, value]: *this) {
                gc.mark_value(key);
                gc.mark_value(value);
            }
        }
    };

} // namespace aria

#endif // ARIA_ARIA_HASH_TABLE_HPP
