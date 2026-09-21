// ObjString 协议面(下标字节语义/不可变写/load_field 绑定/算术与比较算子)的对象层测试;
// 错误白盒取件与 GC 守卫形态同 test_objlist/test_objmap。迭代器(ObjStringIterator)在
// test_objiterator.cpp。
#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::i32;
using aria::i64;
using aria::is_truthy;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjNativeFn;
using aria::ObjString;
using aria::Opt;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_obj;
using aria::usize;
using aria::Value;
using aria::value_identical;

namespace {

    // 建串并入根(建时 stress collect 在串诞生前完成,检视前无 GC 点)。
    ObjString* make_string(GC& gc, GC::Guard& guard, const StringView src) {
        auto s = new_string(gc, src);
        guard.push(s);
        return s;
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt / store 族 false ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.main_context().take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = try_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

    // 比较算子返回装箱 Bool:取「是否为真」(结果恒 Bool,故 is_truthy 即其值)。
    bool compared_true(const Opt<Value>& result) {
        EXPECT_TRUE(result.has_value());
        return result.has_value() && is_truthy(*result);
    }

} // namespace

// ---- 下标协议(load_index 字节域 / store_index 不可变) ----

// 字节域读:整数键产出单字节 1-char string(与 len 同域,计划 D5)。
TEST(ObjString, LoadIndexYieldsSingleByteString) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hello");

    auto first = s->load_index(vm, Value::from_int(0));
    ASSERT_TRUE(first.has_value());
    const auto h = aria::Object::try_as<ObjString>(first->as_obj());
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->view(), "h");

    auto last = s->load_index(vm, Value::from_int(4));
    ASSERT_TRUE(last.has_value());
    const auto o = aria::Object::try_as<ObjString>(last->as_obj());
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->view(), "o");
}

// 多字节序列的中间字节取该字节自身(字节契约的自然结果,非完整字符):
// "héllo" 的 é 是 2 字节(0xC3 0xA9),s[1]/s[2] 各取到其中一字节。
TEST(ObjString, LoadIndexMidSequenceByteYieldsItself) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "h\xC3\xA9llo");

    auto b1 = s->load_index(vm, Value::from_int(1));
    ASSERT_TRUE(b1.has_value());
    const auto first_byte = aria::Object::try_as<ObjString>(b1->as_obj());
    ASSERT_NE(first_byte, nullptr);
    const StringView mid_byte{"\xC3", 1}; // é 的首字节(花括号内逗号会劈 EXPECT_EQ 宏,先落局部)
    EXPECT_EQ(first_byte->view(), mid_byte);
    EXPECT_EQ(first_byte->length(), 1u); // 单字节,非完整字符
}

TEST(ObjString, LoadIndexNonIntKeyFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    for (const Value key: {Value::from_f64(0.5), Value::nil_val(), Value::from_bool(true)}) {
        EXPECT_FALSE(s->load_index(vm, key).has_value());
        const auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::TypeMismatch);
        EXPECT_EQ(message,
                  "Runtime: TypeMismatch string index must be an integer, got " + String{aria::type_name(key)});
    }
}

TEST(ObjString, LoadIndexOutOfBoundsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi"); // 长度 2
    for (const i64 index: {2, -3}) {
        EXPECT_FALSE(s->load_index(vm, Value::from_int(index)).has_value());
        const auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::IndexOutOfBounds);
        EXPECT_EQ(message, aria::String{std::format("Runtime: IndexOutOfBounds string index {} out of range", index)});
    }
}

// 负下标从尾计数(同 list):-1 = 末字节、-len = 首字节;-len-1 归一化后仍负即越界。
TEST(ObjString, LoadIndexNegativeReadsFromTail) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hello");
    auto   last  = s->load_index(vm, Value::from_int(-1));
    ASSERT_TRUE(last.has_value());
    const auto tail = aria::Object::try_as<ObjString>(last->as_obj());
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(tail->view(), "o");
    auto first = s->load_index(vm, Value::from_int(-5));
    ASSERT_TRUE(first.has_value());
    const auto head = aria::Object::try_as<ObjString>(first->as_obj());
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head->view(), "h");
    EXPECT_FALSE(s->load_index(vm, Value::from_int(-6)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
}

// string 不可变:下标写恒报错(定向文案,键值不检查)。
TEST(ObjString, StoreIndexAlwaysFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    EXPECT_FALSE(s->store_index(vm, Value::from_int(0), Value::from_obj(s)));
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch string does not support subscript assignment");
}

// ---- 命名成员协议(load_field → VM 的 String bootstrap 类) ----

// bootstrap 契约:类名与 type() 类型名一致、super 挂 Object 根(计划 D1)。
TEST(ObjString, BootstrapClassContract) {
    AriaVM vm;
    auto*  string_class = vm.string_class();
    ASSERT_NE(string_class, nullptr);
    EXPECT_EQ(string_class->name()->view(), "String");
    EXPECT_EQ(string_class->superclass(), vm.object_class());
}

// 命中恒绑定:bound 的 receiver 是本串、method 是类表内的原生函数 upper。
TEST(ObjString, LoadFieldBindsNativeToReceiver) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    auto   bound = s->load_field(vm, new_string(gc, "upper"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj()); // bound 白色,入临时根再检视
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(value_identical(method->receiver(), Value::from_obj(s)));
    const auto native = aria::Object::try_as<ObjNativeFn>(method->method().as_obj());
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "upper");
}

// miss:类措辞 fail 随协议透传(与 list/map 同文案形)。
TEST(ObjString, LoadFieldMissFailsWithClassWording) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    EXPECT_FALSE(s->load_field(vm, new_string(gc, "nope")).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class String> has no member 'nope'");
}

// stress collect 后类链仍解析:bootstrap 类经寄存器组根、方法原生经类链 field_ 表级联标根。
TEST(ObjString, BootstrapSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc      = vm.gc();
    auto       guard   = gc.make_guard();
    auto       s       = make_string(gc, guard, "hi");
    const auto trigger = new_string(gc, "trigger"); // stress:分配即 collect
    guard.push(trigger);
    auto bound = s->load_field(vm, new_string(gc, "upper"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj());
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name()->view(), "upper"); // name() 经 bound 的原生取名,存活即链完好
}

// ---- 算术协议(op_add = 拼接;算术族当前唯一接线者) ----

// 两侧皆 String 即拼接:内容为两串相接,结果为长串时行走 long_chars_ 路径。
TEST(ObjString, OpAddConcatenates) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   lhs   = make_string(gc, guard, "0123456789abcdefghij"); // 20 字节:SSO 外
    auto   rhs   = make_string(gc, guard, "KLM");
    auto   sum   = lhs->op_add(vm, Value::from_obj(rhs));
    ASSERT_TRUE(sum.has_value());
    guard.push(sum->as_obj());
    const auto sum_str = try_obj<ObjString>(*sum);
    ASSERT_NE(sum_str, nullptr);
    EXPECT_EQ(sum_str->debug_repr(), "\"0123456789abcdefghijKLM\"");
    EXPECT_EQ(sum_str->length(), 23u);
}

// 结果为驻留串:与同内容字面量同指针(=== 与 == 同真)。拼接走 new_string 驻留是契约,
// 故此处直接钉指针同一。
TEST(ObjString, OpAddResultIsInterned) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   lhs   = make_string(gc, guard, "in");
    auto   rhs   = make_string(gc, guard, "tern");
    auto   sum   = lhs->op_add(vm, Value::from_obj(rhs));
    ASSERT_TRUE(sum.has_value());
    EXPECT_TRUE(value_identical(*sum, Value::from_obj(new_string(gc, "intern"))));
}

// 空串参与:两侧皆空得空串;一侧空得另一侧内容。
TEST(ObjString, OpAddHandlesEmptyOperands) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   empty = make_string(gc, guard, "");
    auto   text  = make_string(gc, guard, "x");
    auto   both  = empty->op_add(vm, Value::from_obj(empty));
    ASSERT_TRUE(both.has_value());
    guard.push(both->as_obj());
    const auto both_str = try_obj<ObjString>(*both);
    ASSERT_NE(both_str, nullptr);
    EXPECT_EQ(both_str->length(), 0u);
    auto one = text->op_add(vm, Value::from_obj(empty));
    ASSERT_TRUE(one.has_value());
    guard.push(one->as_obj());
    const auto one_str = try_obj<ObjString>(*one);
    ASSERT_NE(one_str, nullptr);
    EXPECT_EQ(one_str->debug_repr(), "\"x\"");
}

// rhs 非 String:TypeMismatch 定向文案(不做隐式转字符串,显式转换走内置 str())。
TEST(ObjString, OpAddNonStringRhsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "a");
    EXPECT_FALSE(s->op_add(vm, Value::from_int(1)).has_value());
    auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch operator '+' requires two strings, got String and Int");

    EXPECT_FALSE(s->op_add(vm, Value::nil_val()).has_value());
    std::tie(code, message) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch operator '+' requires two strings, got String and Nil");
}

// stress collect 下的拼接:分配点(gc.new_object 顶部 maybe_collect)两侧经调用方值栈为根
// --调用方 peek 不弹是接线纪律,此处按 VM 调用区形态把两侧入栈再调,复现真实根形态。
TEST(ObjString, OpAddSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto& gc    = vm.gc();
    auto  guard = gc.make_guard();
    auto  lhs   = make_string(gc, guard, "stress");
    auto  rhs   = make_string(gc, guard, "collect");
    auto* ctx   = &vm.main_context();
    ctx->push(Value::from_obj(lhs)); // 值栈即根:与 run_binary_add 的 peek 形态一致
    ctx->push(Value::from_obj(rhs));
    auto sum = ctx->peek(1).as_obj()->op_add(vm, ctx->peek(0));
    ASSERT_TRUE(sum.has_value());
    ctx->drop(2);
    ctx->push(*sum);
    EXPECT_EQ(ctx->peek(0).as_obj()->debug_repr(), "\"stresscollect\"");
}

// ---- 比较算子(op_less/op_less_equal/op_greater/op_greater_equal = 字节序) ----

// 四算子的真值表(含空串、真前缀、相等四态)。
TEST(ObjString, OpCompareByteOrderTruthTable) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   a     = make_string(gc, guard, "apple");
    auto   b     = make_string(gc, guard, "banana");
    auto   same  = make_string(gc, guard, "apple");
    auto   empty = make_string(gc, guard, "");
    auto   pre   = make_string(gc, guard, "app"); // a 的真前缀

    EXPECT_TRUE(compared_true(a->op_less(vm, Value::from_obj(b))));
    EXPECT_FALSE(compared_true(b->op_less(vm, Value::from_obj(a))));
    EXPECT_TRUE(compared_true(b->op_greater(vm, Value::from_obj(a))));
    EXPECT_FALSE(compared_true(a->op_greater(vm, Value::from_obj(b))));

    EXPECT_TRUE(compared_true(same->op_less_equal(vm, Value::from_obj(a))));    // 相等:<= 真
    EXPECT_TRUE(compared_true(same->op_greater_equal(vm, Value::from_obj(a)))); // 相等:>= 真
    EXPECT_FALSE(compared_true(same->op_less(vm, Value::from_obj(a))));         // 相等:< 假
    EXPECT_FALSE(compared_true(same->op_greater(vm, Value::from_obj(a))));

    EXPECT_TRUE(compared_true(empty->op_less(vm, Value::from_obj(a)))); // 空串最小
    EXPECT_FALSE(compared_true(empty->op_less(vm, Value::from_obj(empty))));
    EXPECT_TRUE(compared_true(empty->op_less_equal(vm, Value::from_obj(empty))));
    EXPECT_TRUE(compared_true(pre->op_less(vm, Value::from_obj(a)))); // 真前缀更小
}

// 无符号字节序钉子:多字节 UTF-8 与孤立 continuation 字节串都按字节值比。
// 这两条同时是「不能手写逐 char 比较」的反证--有符号 char 下 0xC3 变负,两条都会翻转。
TEST(ObjString, OpCompareIsUnsignedBytewise) {
    AriaVM vm;
    auto&  gc     = vm.gc();
    auto   guard  = gc.make_guard();
    auto   accent = make_string(gc, guard, "é"); // 0xC3 0xA9
    auto   z      = make_string(gc, guard, "z"); // 0x7A
    // s[i] 可切出孤立 continuation 字节(字节域下标的既有结果,非法 UTF-8 串是一等值)。
    auto lone  = make_string(gc, guard, "\xC3");
    auto tilde = make_string(gc, guard, "~"); // 0x7E

    EXPECT_TRUE(compared_true(accent->op_greater(vm, Value::from_obj(z))));    // 0xC3 > 0x7A
    EXPECT_TRUE(compared_true(lone->op_greater(vm, Value::from_obj(tilde))));  // 0xC3 > 0x7E
    EXPECT_TRUE(compared_true(accent->op_greater(vm, Value::from_obj(lone)))); // 0xC3 0xA9 > 0xC3(前缀)
    EXPECT_FALSE(compared_true(accent->op_less(vm, Value::from_obj(accent)))); // 自反不成立
}

// rhs 非 String:TypeMismatch 定向文案,四个算子各自带符号(与算术族「override 自带符号」契约同形)。
TEST(ObjString, OpCompareNonStringRhsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "a");

    EXPECT_FALSE(s->op_less(vm, Value::from_int(1)).has_value());
    auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch operator '<' requires two strings, got String and Int");

    EXPECT_FALSE(s->op_greater_equal(vm, Value::nil_val()).has_value());
    std::tie(code, message) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch operator '>=' requires two strings, got String and Nil");
}

// 比较是 GC-pure:不分配、不触发回收(bytes_allocated 前后不变),故协议侧无 GC 点。
TEST(ObjString, OpCompareAllocatesNothing) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   a     = make_string(gc, guard, "aaa");
    auto   b     = make_string(gc, guard, "bbb");

    const usize before = gc.bytes_allocated();
    EXPECT_TRUE(compared_true(a->op_less(vm, Value::from_obj(b))));
    EXPECT_TRUE(compared_true(b->op_greater(vm, Value::from_obj(a))));
    EXPECT_EQ(gc.bytes_allocated(), before);
}
