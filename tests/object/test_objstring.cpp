// ObjString 协议面(下标字节语义/不可变写/load_field_bound 绑定/算术与比较算子)的对象层测试;
// 错误白盒取件与 GC 守卫形态同 test_objlist/test_objmap。迭代器(ObjStringIterator)在
// test_objiterator.cpp。
#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjRange.hpp"
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
using aria::new_range;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjNativeFn;
using aria::ObjString;
using aria::Opt;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_as_obj;
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
        auto payload = vm.current_context()->take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = try_as_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

    // 比较算子返回装箱 Bool:取「是否为真」(结果恒 Bool,故 is_truthy 即其值)。
    bool compared_true(const Opt<Value>& result) {
        EXPECT_TRUE(result.has_value());
        return result.has_value() && is_truthy(*result);
    }

    // 算子的实现是 String 类表里的原生方法(钩子键经 new_string 驻留命中取串,皆注册表条目零分配,
    // 故本函数无需自守):取槽内裸原生值(不绑定;与 VM 侧 op_*_impl 读的实现格是同一批值,bootstrap
    // 期拷入并断言一致)按原生契约调用 -- slots[0] = receiver 兼返回槽、slots[1] = rhs。返回结果
    // Value;失败返 nullopt(载荷已在挂起寄存器)。调用方负责让两侧存活(receiver 与 rhs 由
    // make_string 入根)。端到端路径(算子指令 -> 取钩子 -> 调用)由语料覆盖(13_strings 的拼接/比较
    // 各例,语料开 stress GC)。
    Opt<Value> invoke_string_hook(AriaVM& vm, const StringView hook_key, const Value lhs, const Value rhs) {
        const auto hook = vm.string_class()->load_field(vm, new_string(vm.gc(), hook_key));
        if (!hook) {
            return std::nullopt;
        }
        Value      slots[]{lhs, rhs};
        const auto fn = hook->as_obj()->as<ObjNativeFn>();
        if (!fn->fn()(vm, aria::Span<Value>{slots, 2})) {
            return std::nullopt;
        }
        return slots[0];
    }

    // 取切片结果并逐字符比对(切片恒产出 string;比较就在本函数内完成,结果串不需要额外根)。
    void expect_slice(AriaVM& vm, ObjString* s, aria::ObjRange* range, const StringView expected) {
        const auto result = s->load_index(vm, Value::from_obj(range));
        ASSERT_TRUE(result.has_value());
        const auto out = result->as_obj()->try_as<ObjString>();
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(out->view(), expected);
    }

} // namespace

// ---- 下标协议(load_index 字节域 / store_index 不可变) ----

// 字节域读:整数键产出单字节 1-char string(与 len 同域)。
TEST(ObjString, LoadIndexYieldsSingleByteString) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hello");

    auto first = s->load_index(vm, Value::from_int(0));
    ASSERT_TRUE(first.has_value());
    const auto h = first->as_obj()->try_as<ObjString>();
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->view(), "h");

    auto last = s->load_index(vm, Value::from_int(4));
    ASSERT_TRUE(last.has_value());
    const auto o = last->as_obj()->try_as<ObjString>();
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
    const auto first_byte = b1->as_obj()->try_as<ObjString>();
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
    const auto tail = last->as_obj()->try_as<ObjString>();
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(tail->view(), "o");
    auto first = s->load_index(vm, Value::from_int(-5));
    ASSERT_TRUE(first.has_value());
    const auto head = first->as_obj()->try_as<ObjString>();
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head->view(), "h");
    EXPECT_FALSE(s->load_index(vm, Value::from_int(-6)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
}

// ---- 切片协议(Range 键,字节域) ----

// 切片与 list 同口径(段解析共用 resolve_slice_bounds):含/不含上界、无上界后缀、末尾之后的空段。
TEST(ObjString, SliceYieldsNewString) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hello");
    auto   r1    = new_range(gc, 1, 3, false);
    guard.push(r1);
    expect_slice(vm, s, r1, "ell"); // s[1..3] 闭区间
    auto r2 = new_range(gc, 1, 3, true);
    guard.push(r2);
    expect_slice(vm, s, r2, "el"); // s[1...3] 半开
    auto r3 = new_range(gc, 3);
    guard.push(r3);
    expect_slice(vm, s, r3, "lo"); // s[3..] 无上界后缀
    auto r4 = new_range(gc, 0);
    guard.push(r4);
    expect_slice(vm, s, r4, "hello"); // s[0..] 全串
    auto r5 = new_range(gc, 5);
    guard.push(r5);
    expect_slice(vm, s, r5, ""); // s[size..] 末尾之后取剩余 = 空段(解构 rest 空尾)
}

// 端点负数从尾计数(与 list 切片同口径;负值只在下标位置出现,方法面不收)。
TEST(ObjString, SliceNegativeEndpoints) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hello");
    auto   r1    = new_range(gc, -3);
    guard.push(r1);
    expect_slice(vm, s, r1, "llo"); // s[-3..] 尾三
    auto r2 = new_range(gc, 1, -1, false);
    guard.push(r2);
    expect_slice(vm, s, r2, "ello"); // s[1..-1] 第二个到最后一个(正起点配负终点不倒序)
    auto r3 = new_range(gc, 0, -1, true);
    guard.push(r3);
    expect_slice(vm, s, r3, "hell"); // s[0...-1] 末字节不含
}

// 倒序段在字节域反转:多字节串被按字节倒排,产出不是合法 UTF-8(同 s[i] 能取到续接字节的
// 字节域契约;按码点反转不在切片口径内)。
TEST(ObjString, SliceReversedRangeYieldsByteReversed) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hello");
    auto   r1    = new_range(gc, 3, 1, false);
    guard.push(r1);
    expect_slice(vm, s, r1, "lle"); // s[3..1] 倒序闭区间
    auto r2 = new_range(gc, 4, 0, false);
    guard.push(r2);
    expect_slice(vm, s, r2, "olleh");                 // 全串倒序
    auto mb = make_string(gc, guard, "h\xC3\xA9llo"); // 字节 1/2 是 é(0xC3 0xA9)
    auto r3 = new_range(gc, 2, 0, false);
    guard.push(r3);
    const StringView byte_reversed{"\xA9\xC3h", 3}; // 字节倒排:0xA9 0xC3 'h'
    expect_slice(vm, mb, r3, byte_reversed);
}

// 越界/空串:nullopt + IndexOutOfBounds,文案与 list 切片同串(list 的失败形态见 test_objlist)。
TEST(ObjString, SliceOutOfBoundsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi"); // 长度 2
    auto   r1    = new_range(gc, 0, 9, false);
    guard.push(r1);
    EXPECT_FALSE(s->load_index(vm, Value::from_obj(r1)).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::IndexOutOfBounds);
    EXPECT_EQ(message, "Runtime: IndexOutOfBounds slice range 0..9 out of range");
    auto r2 = new_range(gc, -3);
    guard.push(r2);
    EXPECT_FALSE(s->load_index(vm, Value::from_obj(r2)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);

    // 空串:有上界形态任何端点都解析不出;无上界 0.. 得空段(空容器单下标同样报错,口径一致)。
    auto empty = make_string(gc, guard, "");
    auto r3    = new_range(gc, 0, 1, false);
    guard.push(r3);
    EXPECT_FALSE(empty->load_index(vm, Value::from_obj(r3)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
    auto r4 = new_range(gc, 0);
    guard.push(r4);
    expect_slice(vm, empty, r4, "");
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
    EXPECT_EQ(message, "Runtime: TypeMismatch type String does not support subscript assignment");
}

// ---- 命名成员协议(load_field_bound → VM 的 String bootstrap 类) ----

// bootstrap 契约:类名与 type() 类型名一致、super 挂 Object 根。
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
    auto   bound = s->load_field_bound(vm, new_string(gc, "upper"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj()); // bound 白色,入临时根再检视
    const auto method = try_as_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(value_identical(method->receiver(), Value::from_obj(s)));
    const auto native = method->method().as_obj()->try_as<ObjNativeFn>();
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "upper");
}

// miss:类措辞 fail 随协议透传(与 list/map 同文案形)。
TEST(ObjString, LoadFieldMissFailsWithClassWording) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    EXPECT_FALSE(s->load_field_bound(vm, new_string(gc, "nope")).has_value());
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
    auto bound = s->load_field_bound(vm, new_string(gc, "upper"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj());
    const auto method = try_as_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name()->view(), "upper"); // name() 经 bound 的原生取名,存活即链完好
}

// ---- 拼接钩子(String 类表里的 __add__ 原生;算子实现即方法) ----

// 两侧皆 String 即拼接:内容为两串相接,结果为长串时行走 long_chars_ 路径。
TEST(ObjString, OpAddConcatenates) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   lhs   = make_string(gc, guard, "0123456789abcdefghij"); // 20 字节:SSO 外
    auto   rhs   = make_string(gc, guard, "KLM");
    auto   sum   = invoke_string_hook(vm, "__add__", Value::from_obj(lhs), Value::from_obj(rhs));
    ASSERT_TRUE(sum.has_value());
    guard.push(sum->as_obj());
    const auto sum_str = try_as_obj<ObjString>(*sum);
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
    auto   sum   = invoke_string_hook(vm, "__add__", Value::from_obj(lhs), Value::from_obj(rhs));
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
    auto   both  = invoke_string_hook(vm, "__add__", Value::from_obj(empty), Value::from_obj(empty));
    ASSERT_TRUE(both.has_value());
    guard.push(both->as_obj());
    const auto both_str = try_as_obj<ObjString>(*both);
    ASSERT_NE(both_str, nullptr);
    EXPECT_EQ(both_str->length(), 0u);
    auto one = invoke_string_hook(vm, "__add__", Value::from_obj(text), Value::from_obj(empty));
    ASSERT_TRUE(one.has_value());
    guard.push(one->as_obj());
    const auto one_str = try_as_obj<ObjString>(*one);
    ASSERT_NE(one_str, nullptr);
    EXPECT_EQ(one_str->debug_repr(), "\"x\"");
}

// rhs 非 String:TypeMismatch 定向文案(不做隐式转字符串,显式转换走内置 str())。
TEST(ObjString, OpAddNonStringRhsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "a");
    EXPECT_FALSE(invoke_string_hook(vm, "__add__", Value::from_obj(s), Value::from_int(1)).has_value());
    auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch __add__ requires two strings, got String and Int");

    EXPECT_FALSE(invoke_string_hook(vm, "__add__", Value::from_obj(s), Value::nil_val()).has_value());
    std::tie(code, message) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch __add__ requires two strings, got String and Nil");
}

// stress collect 下的拼接:分配点(gc.new_object 顶部 maybe_collect)两侧须为根 --运行期由算子
// 调用区槽 0/1 承担(receiver 与 rhs),此处两侧经 make_string 入根、结果取回后入根,复现同款根
// 形态;拼接结果经驻留池,故与同内容串同指针。
TEST(ObjString, OpAddSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto& gc    = vm.gc();
    auto  guard = gc.make_guard();
    auto  lhs   = make_string(gc, guard, "stress");
    auto  rhs   = make_string(gc, guard, "collect");
    auto  sum   = invoke_string_hook(vm, "__add__", Value::from_obj(lhs), Value::from_obj(rhs));
    ASSERT_TRUE(sum.has_value());
    guard.push(sum->as_obj());
    EXPECT_EQ(sum->as_obj()->debug_repr(), "\"stresscollect\"");
}

// ---- 重复钩子(String 类表里的 __mul__ 原生) ----

// 整次重复:count 次原样拼接自身(字节域整段复制,多字节序列原样成倍);0 次得空串、1 次得
// 同内容串。
TEST(ObjString, OpMulRepeats) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   lhs   = make_string(gc, guard, "0123456789abcdefghij"); // 20 字节:SSO 外
    auto   twice = invoke_string_hook(vm, "__mul__", Value::from_obj(lhs), Value::from_int(2));
    ASSERT_TRUE(twice.has_value());
    guard.push(twice->as_obj());
    const auto twice_str = try_as_obj<ObjString>(*twice);
    ASSERT_NE(twice_str, nullptr);
    EXPECT_EQ(twice_str->debug_repr(), "\"0123456789abcdefghij0123456789abcdefghij\"");
    EXPECT_EQ(twice_str->length(), 40u);

    auto ab  = make_string(gc, guard, "h\xC3\xA9"); // 多字节:é 两字节原样成倍
    auto one = invoke_string_hook(vm, "__mul__", Value::from_obj(ab), Value::from_int(1));
    ASSERT_TRUE(one.has_value());
    guard.push(one->as_obj());
    const auto one_str = try_as_obj<ObjString>(*one);
    ASSERT_NE(one_str, nullptr);
    EXPECT_EQ(one_str->view(), "h\xC3\xA9");
    auto zero = invoke_string_hook(vm, "__mul__", Value::from_obj(ab), Value::from_int(0));
    ASSERT_TRUE(zero.has_value());
    guard.push(zero->as_obj());
    const auto zero_str = try_as_obj<ObjString>(*zero);
    ASSERT_NE(zero_str, nullptr);
    EXPECT_EQ(zero_str->length(), 0u);
}

// 结果为驻留串:与同内容字面量同指针(=== 与 == 同真,与 __add__ 同契约)。
TEST(ObjString, OpMulResultIsInterned) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   lhs   = make_string(gc, guard, "ab");
    auto   rep   = invoke_string_hook(vm, "__mul__", Value::from_obj(lhs), Value::from_int(3));
    ASSERT_TRUE(rep.has_value());
    guard.push(rep->as_obj());
    EXPECT_TRUE(value_identical(*rep, Value::from_obj(new_string(gc, "ababab"))));
}

// 空串源:任意非负次数都得空串。
TEST(ObjString, OpMulEmptySourceYieldsEmpty) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   empty = make_string(gc, guard, "");
    auto   rep   = invoke_string_hook(vm, "__mul__", Value::from_obj(empty), Value::from_int(3));
    ASSERT_TRUE(rep.has_value());
    const auto rep_str = try_as_obj<ObjString>(*rep);
    ASSERT_NE(rep_str, nullptr);
    EXPECT_EQ(rep_str->length(), 0u);
}

// 乘数非 int(f64/nil/bool):TypeMismatch 定向文案(严格 int,同下标访问口径)。
TEST(ObjString, OpMulNonIntCountFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "a");
    for (const Value count: {Value::from_f64(2.5), Value::from_f64(2.0), Value::nil_val(), Value::from_bool(true)}) {
        EXPECT_FALSE(invoke_string_hook(vm, "__mul__", Value::from_obj(s), count).has_value());
        const auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::TypeMismatch);
        EXPECT_EQ(message, "Runtime: TypeMismatch __mul__ requires a string and an integer, got String and " +
                                   String{aria::type_name(count)});
    }
}

// 负数乘数:TypeMismatch(值域文案,不静默得空)。
TEST(ObjString, OpMulNegativeCountFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "a");
    EXPECT_FALSE(invoke_string_hook(vm, "__mul__", Value::from_obj(s), Value::from_int(-1)).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch __mul__ requires a non-negative integer, got -1");
}

// stress collect 下的重复:分配点两侧须为根(receiver 在调用区槽 0,此处经 make_string 入根)。
TEST(ObjString, OpMulSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto& gc    = vm.gc();
    auto  guard = gc.make_guard();
    auto  lhs   = make_string(gc, guard, "go");
    auto  rep   = invoke_string_hook(vm, "__mul__", Value::from_obj(lhs), Value::from_int(3));
    ASSERT_TRUE(rep.has_value());
    guard.push(rep->as_obj());
    EXPECT_EQ(rep->as_obj()->debug_repr(), "\"gogogo\"");
}

// ---- 比较钩子(__lt__/__le__/__gt__/__ge__ = 字节序) ----

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

    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(a), Value::from_obj(b))));
    EXPECT_FALSE(compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(b), Value::from_obj(a))));
    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__gt__", Value::from_obj(b), Value::from_obj(a))));
    EXPECT_FALSE(compared_true(invoke_string_hook(vm, "__gt__", Value::from_obj(a), Value::from_obj(b))));

    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__le__", Value::from_obj(same),
                                                 Value::from_obj(a)))); // 相等:<= 真
    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__ge__", Value::from_obj(same),
                                                 Value::from_obj(a)))); // 相等:>= 真
    EXPECT_FALSE(
            compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(same), Value::from_obj(a)))); // 相等:< 假
    EXPECT_FALSE(compared_true(invoke_string_hook(vm, "__gt__", Value::from_obj(same), Value::from_obj(a))));

    EXPECT_TRUE(
            compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(empty), Value::from_obj(a)))); // 空串最小
    EXPECT_FALSE(compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(empty), Value::from_obj(empty))));
    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__le__", Value::from_obj(empty), Value::from_obj(empty))));
    EXPECT_TRUE(
            compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(pre), Value::from_obj(a)))); // 真前缀更小
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

    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__gt__", Value::from_obj(accent),
                                                 Value::from_obj(z)))); // 0xC3 > 0x7A
    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__gt__", Value::from_obj(lone),
                                                 Value::from_obj(tilde)))); // 0xC3 > 0x7E
    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__gt__", Value::from_obj(accent),
                                                 Value::from_obj(lone)))); // 0xC3 0xA9 > 0xC3(前缀)
    EXPECT_FALSE(compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(accent),
                                                  Value::from_obj(accent)))); // 自反不成立
}

// rhs 非 String:TypeMismatch 定向文案,四个算子各自带符号(与算术族「override 自带符号」契约同形)。
TEST(ObjString, OpCompareNonStringRhsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "a");

    EXPECT_FALSE(invoke_string_hook(vm, "__lt__", Value::from_obj(s), Value::from_int(1)).has_value());
    auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch __lt__ requires two strings, got String and Int");

    EXPECT_FALSE(invoke_string_hook(vm, "__ge__", Value::from_obj(s), Value::nil_val()).has_value());
    std::tie(code, message) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch __ge__ requires two strings, got String and Nil");
}

// 比较是 GC-pure:不分配、不触发回收(bytes_allocated 前后不变),故协议侧无 GC 点。
TEST(ObjString, OpCompareAllocatesNothing) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   a     = make_string(gc, guard, "aaa");
    auto   b     = make_string(gc, guard, "bbb");

    const usize before = gc.bytes_allocated();
    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__lt__", Value::from_obj(a), Value::from_obj(b))));
    EXPECT_TRUE(compared_true(invoke_string_hook(vm, "__gt__", Value::from_obj(b), Value::from_obj(a))));
    EXPECT_EQ(gc.bytes_allocated(), before);
}
