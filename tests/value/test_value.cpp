#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "memory/GC.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "value/AriaHashTable.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaHashTable;
using aria::GC;
using aria::new_module;
using aria::new_string;
using aria::ObjModule;
using aria::ObjString;
using aria::try_obj;
using aria::Value;
using aria::value_equal;
using aria::value_identical;

// 双相等体系见 Value.hpp:value_equal(== 内容相等)/ value_identical(=== 严格相等);
// 哈希表键用 ===(见末尾 HashTableKey*)。

// ===== value_identical (=== 严格相等) =====

TEST(ValueIdentical, NilBoolInt) {
    EXPECT_TRUE(value_identical(Value::nil_val(), Value::nil_val()));
    EXPECT_TRUE(value_identical(Value::from_bool(true), Value::from_bool(true)));
    EXPECT_FALSE(value_identical(Value::from_bool(true), Value::from_bool(false)));
    EXPECT_TRUE(value_identical(Value::from_i32(1), Value::from_i32(1)));
    EXPECT_FALSE(value_identical(Value::from_i32(1), Value::from_i32(2)));
}

TEST(ValueIdentical, TypeStrict) {
    // 类型严格:int 1 !== f64 1.0
    EXPECT_FALSE(value_identical(Value::from_i32(1), Value::from_f64(1.0)));
    // bool 不与 int 互比
    EXPECT_FALSE(value_identical(Value::from_bool(true), Value::from_i32(1)));
    // nil !== false
    EXPECT_FALSE(value_identical(Value::nil_val(), Value::from_bool(false)));
}

TEST(ValueIdentical, F64Bitwise) {
    // 按位:-0.0 !== +0.0(符号位不同,与 == 的 IEEE 数值不同)
    EXPECT_FALSE(value_identical(Value::from_f64(-0.0), Value::from_f64(0.0)));
    // NaN 规范化后:任意两个 NaN bit 相等 -> NaN === NaN true
    const auto nan_a = Value::from_f64(std::numeric_limits<double>::quiet_NaN());
    const auto nan_b = Value::from_f64(std::nan("0"));
    EXPECT_TRUE(value_identical(nan_a, nan_b));
}

TEST(ValueIdentical, ObjPointer) {
    GC   gc;
    auto lock = gc.make_lock(); // 持裸指针跨分配
    auto a    = new_string(gc, "hello");
    auto b    = new_string(gc, "hello"); // intern:同指针
    auto c    = new_string(gc, "world");
    EXPECT_TRUE(value_identical(Value::from_obj(a), Value::from_obj(b))); // intern 同指针
    EXPECT_FALSE(value_identical(Value::from_obj(a), Value::from_obj(c)));
}

// ===== value_equal (== 内容相等) =====

TEST(ValueEqual, NilBoolInt) {
    EXPECT_TRUE(value_equal(Value::nil_val(), Value::nil_val()));
    EXPECT_TRUE(value_equal(Value::from_bool(true), Value::from_bool(true)));
    EXPECT_FALSE(value_equal(Value::from_bool(true), Value::from_bool(false)));
    EXPECT_TRUE(value_equal(Value::from_i32(1), Value::from_i32(1)));
    EXPECT_FALSE(value_equal(Value::from_i32(1), Value::from_i32(2)));
}

TEST(ValueEqual, CrossTypeNumeric) {
    // 跨类型 IEEE 数值:1 == 1.0 true(双向)
    EXPECT_TRUE(value_equal(Value::from_i32(1), Value::from_f64(1.0)));
    EXPECT_TRUE(value_equal(Value::from_f64(1.0), Value::from_i32(1)));
    EXPECT_FALSE(value_equal(Value::from_i32(1), Value::from_f64(2.0)));
    // bool 不与数值互比(不做 JS 全套强制转换)
    EXPECT_FALSE(value_equal(Value::from_bool(true), Value::from_i32(1)));
    // nil 不与 bool/数值互比
    EXPECT_FALSE(value_equal(Value::nil_val(), Value::from_i32(0)));
    EXPECT_FALSE(value_equal(Value::nil_val(), Value::from_bool(false)));
}

TEST(ValueEqual, F64IEEE) {
    // IEEE 数值:-0.0 == +0.0 true(与 === 的按位不同)
    EXPECT_TRUE(value_equal(Value::from_f64(-0.0), Value::from_f64(0.0)));
    // NaN != NaN(IEEE 数值语义;即便规范化后 bit 相同,== 仍判 NaN 不等)
    const auto nan_a = Value::from_f64(std::nan("1"));
    const auto nan_b = Value::from_f64(std::nan("2"));
    EXPECT_FALSE(value_equal(nan_a, nan_b));
}

TEST(ValueEqual, ObjStringContent) {
    GC   gc;
    auto lock = gc.make_lock();
    // intern 路径:同指针 -> == 当然 true
    auto a = new_string(gc, "hello");
    auto b = new_string(gc, "hello");
    EXPECT_TRUE(value_equal(Value::from_obj(a), Value::from_obj(b)));
    // 非 intern 路径:不同指针、等价内容 -> == 仍 true(ObjString::equals 比内容)
    auto c = gc.new_object<ObjString>(gc, "hello");
    auto d = gc.new_object<ObjString>(gc, "hello");
    // 非 intern:不同指针
    EXPECT_NE(c, d);
    EXPECT_TRUE(value_equal(Value::from_obj(c), Value::from_obj(d)));      // 内容相等
    EXPECT_FALSE(value_identical(Value::from_obj(c), Value::from_obj(d))); // 指针不等
    // 不同内容
    auto e = new_string(gc, "world");
    EXPECT_FALSE(value_equal(Value::from_obj(a), Value::from_obj(e)));
    // 字符串不与数值互比
    EXPECT_FALSE(value_equal(Value::from_obj(a), Value::from_i32(1)));
}

// ===== 哈希键语义(===)=====

TEST(HashTableKey, IntAndFloatAreDifferentKeys) {
    GC            gc;
    AriaHashTable ht{&gc};
    ht.upsert(Value::from_i32(1))->value = Value::from_i32(100);
    // int 1 与 f64 1.0 是不同键(哈希键用 === value_identical:类型严格)
    EXPECT_EQ(ht.find(Value::from_f64(1.0)), nullptr);
    auto found = ht.find(Value::from_i32(1));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value.as_int(), 100);
}

TEST(HashTableKey, StringKeyByContentViaIntern) {
    GC            gc;
    auto          lock = gc.make_lock();
    AriaHashTable ht{&gc};
    ht.upsert(Value::from_obj(new_string(gc, "key")))->value = Value::from_i32(42);
    // 字符串键靠 intern 等价内容同指针 -> 按内容查到(=== 指针相等)
    auto found = ht.find(Value::from_obj(new_string(gc, "key")));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value.as_int(), 42);
}

// ===== try_obj (Value -> 对象子类型一步守卫) =====

TEST(ValueTryObj, ObjMatchReturnsPointer) {
    GC   gc;
    auto s = new_string(gc, "hello");
    EXPECT_EQ(try_obj<ObjString>(Value::from_obj(s)), s);
}

TEST(ValueTryObj, NonObjValueReturnsNull) {
    EXPECT_EQ(try_obj<ObjString>(Value::nil_val()), nullptr);
    EXPECT_EQ(try_obj<ObjString>(Value::from_bool(true)), nullptr);
    EXPECT_EQ(try_obj<ObjString>(Value::from_i32(1)), nullptr);
    EXPECT_EQ(try_obj<ObjString>(Value::from_f64(2.5)), nullptr);
}

TEST(ValueTryObj, ObjMismatchReturnsNull) {
    GC   gc;
    auto m = new_module(gc, "m"); // StringView 重载自守 name,本测试无 stress 无 GC 风险
    EXPECT_EQ(try_obj<ObjString>(Value::from_obj(m)), nullptr);
    EXPECT_EQ(try_obj<ObjModule>(Value::from_obj(m)), m);
}
