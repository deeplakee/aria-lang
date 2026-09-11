#ifndef ARIA_ARIA_HASH_TABLE_HPP
#define ARIA_ARIA_HASH_TABLE_HPP

#include "common.hpp"
#include "memory/GC.hpp"
#include "memory/HashTable.hpp"
#include "value/Value.hpp" // Value + value_hash/value_equal(定义在 Value.cpp)

namespace aria {

    // HashTable 模板参数用的仿函数:内联包装,转发到 value_hash/value_identical(定义在 Value.cpp)。
    // 仅 AriaHashTable 使用,故定义在此。保留仿函数形式是因为 HashTable<K,V,Hash,Eq> 以可调用
    // 类型作模板参数(Hash{}(key))。哈希键用 ===(见 ValueEq)。
    struct ValueHash {
        [[nodiscard]]
        u32 operator()(const Value value) const noexcept {
            return value_hash(value);
        }
    };

    struct ValueEq {
        // 哈希表键用 ===(value_identical,理由见 Value.hpp):int 1 与 f64 1.0 是不同键,
        // 不用 value_equal(== 内容相等)。
        [[nodiscard]]
        bool operator()(const Value lhs, const Value rhs) const noexcept {
            return value_identical(lhs, rhs);
        }
    };

    // 绑定 Value 的 aria 哈希表:继承 HashTable<Value,Value,ValueHash,ValueEq> 的
    //        Swiss Table 实现与接口(upsert/find/erase/for_each_occupied/size...),加 trace(GC&)
    //        (遍历占用槽 mark_value key+value)。
    //
    //        分层:src/memory/ 的 HashTable<K,V,Hash,Eq> 对 K/V 完全通用(不知 Value);本类
    //        绑成 Value 并补 GC trace,Phase 3 的 ObjMap 持其作成员、trace 委托 ht.trace(gc)。
    //        继承而非组合:直接复用底层全部公开接口;基类 dtor 非虚但本子类不作多态基,故安全。
    //        不可拷贝/不可移动(继承自 HashTable)。
    class AriaHashTable : public HashTable<Value, Value, ValueHash, ValueEq> {
    public:
        using HashTable<Value, Value, ValueHash, ValueEq>::HashTable; // 继承 explicit HashTable(GC*) ctor

        // GC 标记:遍历占用槽 mark_value(key)+(value)。由 owner(ObjMap)在 collect 的 trace
        // 阶段调用(「只看 ctrl、不加载非占用 Entry,垃圾槽安全」见 HashTable::for_each_occupied)。
        void trace(GC& gc) const noexcept {
            this->for_each_occupied([&gc](const Value& key, const Value& value) {
                gc.mark_value(key);
                gc.mark_value(value);
            });
        }
    };

} // namespace aria

#endif // ARIA_ARIA_HASH_TABLE_HPP
