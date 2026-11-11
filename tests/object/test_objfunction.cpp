#include <gtest/gtest.h>

#include <tuple>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::new_function;
using aria::new_string;
using aria::new_upvalue;
using aria::ObjFnProto;
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::ObjUpvalue;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;
using aria::value_equal;
using aria::value_identical;

namespace {

    // 测试便利:委托 new_module 1 参 StringView 重载(name/dir 经工厂内部 intern 并自守)。
    // 返回的 m 未根,调用方跨 GC 点持有须自行守卫。默认名 "<script>"(临时模块)。
    ObjModule* make_module(GC& gc, const StringView name = "<script>") { return aria::new_module(gc, name); }

    // 指定模块的具名函数原型:m 未根须自守(工厂内 intern name 与 new_object 均 GC 点,可能
    // collect 回收 m)。返回白色,调用方自守。
    ObjFnProto* make_proto(GC& gc, ObjModule* m, const StringView name, const u8 arity) {
        auto guard = gc.make_guard(m);
        return aria::new_fn_proto(gc, m, name, arity, arity, false); // 无缺省,min_arity = arity
    }

    // 3 参便利重载:造临时模块 + 委托工厂 StringView 重载。屏蔽全局 aria::new_fn_proto。
    // m 未根须自守:make_module 与工厂内部分配在 stress GC 下会 collect,裸局部指针无根会被
    // 扫掉;name 串由工厂内部 intern 并自守。
    ObjFnProto* new_fn_proto(GC& gc, const StringView name, const u8 arity) {
        auto m     = make_module(gc);
        auto guard = gc.make_guard(m);
        return aria::new_fn_proto(gc, m, name, arity, arity, false); // 无缺省,min_arity = arity
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt ⟺ 寄存器必有载荷)。
    aria::Pair<aria::ErrorCode, aria::String> take_pending_error(aria::AriaVM& vm) {
        auto payload = vm.current_context()->take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = aria::try_as_obj<aria::ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), aria::String{ex->message()->view()}};
    }

} // namespace

// 基础:包装原型,捕获数组初始为空(CLOSURE 执行时填入)。
TEST(ObjFunction, WrapsPrototype) {
    GC   gc;
    auto proto    = new_fn_proto(gc, "add", 2);
    auto function = new_function(gc, proto);
    EXPECT_TRUE(function->is<ObjFunction>());
    EXPECT_FALSE(function->is_native());
    EXPECT_EQ(function->type(), aria::ObjType::FUNCTION);
    EXPECT_EQ(function->type_name(), "Function");
    EXPECT_EQ(function->proto(), proto);
    EXPECT_TRUE(function->upvalues().empty());
}

// 捕获数组:逐个后填,按下标可取。
TEST(ObjFunction, AddUpvaluesInOrder) {
    GC    gc;
    auto  proto   = new_fn_proto(gc, "f", 0);
    auto  closure = new_function(gc, proto);
    auto  guard   = gc.make_guard(closure); // 习惯性守卫(非 stress 下无 GC 触发)
    Value a       = Value::from_i32(1);
    Value b       = Value::from_i32(2);
    auto  u1      = new_upvalue(gc, &a);
    auto  u2      = new_upvalue(gc, &b);
    closure->add_upvalue(u1);
    closure->add_upvalue(u2);
    EXPECT_EQ(closure->upvalues().size(), usize{2});
    EXPECT_EQ(closure->upvalues()[0], u1);
    EXPECT_EQ(closure->upvalues()[1], u2);
}

// repr 按原型名称渲染 `<fn name>`。
TEST(ObjFunction, ReprUsesPrototypeName) {
    GC   gc;
    auto proto    = new_fn_proto(gc, "add", 0);
    auto function = new_function(gc, proto);
    EXPECT_EQ(function->repr(), "<fn add>");
    // 调试渲染同文案(value_repr 经 repr 虚分派)。
    EXPECT_EQ(aria::value_repr(Value::from_obj(function)), "<fn add>");
}

// 身份语义:同一原型创建两次得到不同闭包;equals/=== 均按地址。
TEST(ObjFunction, IdentitySemantics) {
    GC   gc;
    auto proto          = new_fn_proto(gc, "f", 0);
    auto first_closure  = new_function(gc, proto);
    auto second_closure = new_function(gc, proto);
    EXPECT_NE(first_closure, second_closure);
    EXPECT_TRUE(value_identical(Value::from_obj(first_closure), Value::from_obj(first_closure)));
    EXPECT_FALSE(value_identical(Value::from_obj(first_closure), Value::from_obj(second_closure)));
    EXPECT_FALSE(value_equal(Value::from_obj(first_closure), Value::from_obj(second_closure))); // equals 默认地址相等
}

// stress GC:仅根闭包,proto(含常量池长串)与 upvalue(含栈槽内捕获串)全链经 closure.trace 存活。
TEST(ObjFunction, TraceMarksPrototypeAndUpvalues) {
    GC gc;
    gc.set_stress(true);
    auto proto   = new_fn_proto(gc, "f", 0);
    auto guard   = gc.make_guard(proto);    // stress 下先根化 proto(closure 建好后另经 closure 可达)
    auto closure = new_function(gc, proto); // 建时 collect:proto 经 guard 存活
    guard.push(closure);
    auto constant = new_string(gc, "a long constant string beyond sso padding");
    proto->unit().add_constant(Value::from_obj(constant)); // push 走 trivial 分配不触 GC
    Value v = Value::nil_val();
    // 建时 collect:proto/closure 经 guard 存活
    auto uv = new_upvalue(gc, &v);
    closure->add_upvalue(uv); // 先入 closure(根可达)再跨 GC
    // 建时 collect:uv 经 closure 存活
    v           = Value::from_obj(new_string(gc, "captured long string beyond sso padding"));
    std::ignore = new_string(gc, "trigger"); // stress collect:全链经 closure.trace 存活
    EXPECT_EQ(closure->proto(), proto);
    EXPECT_EQ(constant->view(), "a long constant string beyond sso padding");
    ASSERT_EQ(closure->upvalues().size(), usize{1});
    EXPECT_EQ(closure->upvalues()[0], uv);
    EXPECT_TRUE(value_identical(*closure->upvalues()[0]->value_slot(), v));
}

// 无根闭包被 sweep(闭包壳 + proto 壳 + CodeUnit 内部 Array)。
TEST(ObjFunction, UnrootedClosureSwept) {
    GC   gc;
    auto proto         = new_fn_proto(gc, "f", 0);
    std::ignore        = new_function(gc, proto); // 两者皆无根
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// 类面接入:函数成员读经 Function 链命中根(__str__ 原生可及),miss 以类措辞报。
TEST(ObjFunction, ChainMembersResolvable) {
    aria::AriaVM vm;
    auto&        gc    = vm.gc();
    auto         guard = gc.make_guard();
    auto         proto = new_fn_proto(gc, "worker", 0);
    guard.push(proto);
    auto function = new_function(gc, proto);
    guard.push(function);

    const auto display_key = new_string(gc, "__str__");
    guard.push(display_key);
    const auto hit = function->load_field(*vm.current_context(), display_key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_TRUE(hit->as_obj()->is<aria::ObjFunction>()); // 沿 Function 链继承根原生

    const auto nope = new_string(gc, "nope");
    guard.push(nope);
    EXPECT_FALSE(function->load_field(*vm.current_context(), nope).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, aria::ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class Function> has no member 'nope'");
}

// 只根化函数对象:两种活动载荷各自保活名称、原型、常量和捕获值。
TEST(ObjFunction, PayloadGraphsSurviveWithOnlyFunctionsRooted) {
    GC gc;
    gc.set_stress(true);
    ObjFunction* function = nullptr;
    {
        const auto proto = new_fn_proto(gc, "script_root", 0);
        const auto guard = gc.make_guard(proto);
        function         = new_function(gc, proto);
    }
    auto       guard           = gc.make_guard(function);
    const auto native_function = new_function(
            gc, "native function name beyond sso padding", +[](const aria::NativeCallInfo& info) {
                info.set_return_val(Value::nil_val());
                return true;
            });
    guard.push(native_function);
    const auto constant = new_string(gc, "constant retained through function payload");
    function->proto()->unit().add_constant(Value::from_obj(constant));
    Value      captured = Value::nil_val();
    const auto upvalue  = new_upvalue(gc, &captured);
    function->add_upvalue(upvalue);
    captured = Value::from_obj(new_string(gc, "captured value retained through function payload"));
    gc.collect();
    EXPECT_EQ(function->name()->view(), "script_root");
    EXPECT_EQ(function->proto()->unit().constants[0].as_obj()->as<ObjString>()->view(),
              "constant retained through function payload");
    EXPECT_EQ(function->upvalues()[0]->value_slot()->as_obj()->as<ObjString>()->view(),
              "captured value retained through function payload");
    EXPECT_EQ(native_function->name()->view(), "native function name beyond sso padding");
    EXPECT_TRUE(native_function->is_native());
    EXPECT_FALSE(function->is_native());
}
