#ifndef ARIA_ARIA_HASH_TABLE_HPP
#define ARIA_ARIA_HASH_TABLE_HPP

#include "common.hpp"
#include "memory/GC.hpp"
#include "memory/HashTable.hpp"
#include "value/Value.hpp" // Value + value_hash/value_equal(定义在 Value.cpp)

namespace aria {

    // HashTable 模板参数用的仿函数:内联包装,转发到 value_hash/value_identical(定义在
    // Value.cpp)。仅 AriaHashTable 使用,故定义在此;保留仿函数形式是因为 HashTable 以可调用
    // 类型作模板参数(Hash{}(key))。哈希键用 ===(见 ValueEq)。
    struct ValueHash {
        [[nodiscard]]
        u32 operator()(const Value value) const noexcept {
            return value_hash(value);
        }
    };

    struct ValueEq {
        // 哈希表键用 ===(value_identical),不用 value_equal(==);理由与例证见 Value.hpp 头注。
        [[nodiscard]]
        bool operator()(const Value lhs, const Value rhs) const noexcept {
            return value_identical(lhs, rhs);
        }
    };

    // 绑定 Value 的 aria 哈希表:继承 HashTable<Value,Value,ValueHash,ValueEq> 的 Swiss Table
    //        实现与接口(set/find/begin/end/size...),加 trace(GC&)(遍历占用槽 mark_value
    //        key+value)。ObjMap 持其作成员、trace 委托 ht.trace(gc)。
    //        继承而非组合(理由同 AriaArray 注:基类 dtor 非虚但本子类不作多态基);不可拷贝/不可移动。
    class AriaHashTable : public HashTable<Value, Value, ValueHash, ValueEq> {
    public:
        using HashTable<Value, Value, ValueHash, ValueEq>::HashTable; // 继承 explicit HashTable(GC*) ctor

        // 迭代器只看 ctrl、不加载非占用 Entry,故遍历占用槽 mark_value(key)+(value) 时
        // 垃圾槽安全(见 HashTable::const_iterator 注释)。
        void trace(GC& gc) const noexcept {
            for (const auto& [key, value]: *this) {
                gc.mark_value(key);
                gc.mark_value(value);
            }
        }
    };

} // namespace aria

#endif // ARIA_ARIA_HASH_TABLE_HPP
