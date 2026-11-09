#include <gtest/gtest.h>

#include <format>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::new_module;
using aria::new_string;
using aria::ObjException;
using aria::ObjFnProto;
using aria::ObjModule;
using aria::ObjString;
using aria::Pair;
using aria::Span;
using aria::String;
using aria::try_as_obj;
using aria::Value;

namespace {

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt / store 族 false ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.current_context()->take_error();
        EXPECT_TRUE(payload.has_value()); // fail 契约:失败信号 ⟺ 寄存器必有载荷
        const auto ex = try_as_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

} // namespace

// try_as<T>(is+as 合一):动态类型匹配返回转型指针,否则 nullptr。前置条件:this 非空
//(is_obj 门后的解码指针恒满足,from_obj 的 DEBUG 断言兜底),null 无测试形态。

TEST(ObjectTryAs, MatchReturnsPointer) {
    GC   gc;
    auto s = new_string(gc, "hello");
    EXPECT_EQ(s->try_as<ObjString>(), s);
}

TEST(ObjectTryAs, MismatchReturnsNull) {
    GC   gc;
    auto s = new_string(gc, "hello");
    EXPECT_EQ(s->try_as<ObjFnProto>(), nullptr);
}

// const 重载:const Object* -> const T*。
TEST(ObjectTryAs, ConstOverload) {
    GC                  gc;
    auto                s = new_string(gc, "hello");
    const aria::Object* o = s;
    EXPECT_EQ(o->try_as<ObjString>(), s);
    EXPECT_EQ(o->try_as<ObjFnProto>(), nullptr);
}

// 成员/下标访问协议的**基类默认**:未 override 的子类型对协议操作一律 ctx.fail 入
// 寄存器后返失败信号 --load 族 nullopt、store 族 false。本测试钉住默认形态(码 + 文案子串)
// 基类签名漂移即红。探针取当下仍未 override 的类型:string 探 store_field(ObjString 无
// store_field override)、Module 探下标(Module override 了 load/store_field)。算子与可调用
// 两侧随成员协议走(按钩子名 load_field,措辞由各类型成员协议承担),无独立的基类默认可钉。

TEST(ObjectProtocolDefaults, MemberIndexDefaults) {
    AriaVM vm; // 报错经 ctx.fail 入挂起寄存器
    auto&  gc    = vm.gc();
    auto   s     = new_string(gc, "hello");
    auto   guard = gc.make_guard(s);
    auto   m     = new_module(gc, "<test>");
    guard.push(m);
    auto k = new_string(gc, "len");
    guard.push(k);

    // 成员协议默认:load miss = "X has no member 'y'"(对象描述经 repr)、
    // store = "type X does not support field access"。Module 已 override 两协议(load = 成员查
    // globals_、store = 只读拒),故写默认体用 string 探(ObjString 不 override store_field)。
    EXPECT_FALSE(m->load_field(*vm.current_context(), k).has_value());
    auto [code, msg] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("has no member 'len'"));

    EXPECT_FALSE(s->store_field(*vm.current_context(), k, Value::from_int(1)));
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("type String does not support field access"));

    // 下标协议默认(备置):"type X does not support subscript access"。
    EXPECT_FALSE(m->load_index(*vm.current_context(), Value::from_int(0)).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_TRUE(msg.contains("type Module does not support subscript access"));

    EXPECT_FALSE(m->store_index(*vm.current_context(), Value::from_int(0), Value::from_int(1)));
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_TRUE(msg.contains("type Module does not support subscript access"));
}

// 内部对象没有类型成员表,普通读、绑定读与类型查询均返回可恢复的缺席错误。
TEST(ObjectProtocolDefaults, FunctionAndUpvalueMissingMembers) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc       = vm.gc();
    const auto module   = new_module(gc, "<test>");
    auto       guard    = gc.make_guard(module);
    const auto function = aria::new_fn_proto(gc, module, "internal", 0, 0, false);
    guard.push(function);
    auto       slot    = Value::nil_val();
    const auto upvalue = aria::new_upvalue(gc, &slot);
    guard.push(upvalue);
    const auto name = new_string(gc, "missing");
    guard.push(name);

    aria::Object* objects[] = {function, upvalue};
    for (const auto object: objects) {
        const auto expected = std::format("Runtime: UndefinedProperty {} has no member 'missing'", object->repr());
        EXPECT_FALSE(object->load_field_from_class(*vm.current_context(), name));
        auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::UndefinedProperty);
        EXPECT_EQ(message, expected);

        EXPECT_FALSE(object->load_field(*vm.current_context(), name));
        std::tie(code, message) = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::UndefinedProperty);
        EXPECT_EQ(message, expected);

        EXPECT_FALSE(object->load_field_bound(*vm.current_context(), name));
        std::tie(code, message) = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::UndefinedProperty);
        EXPECT_EQ(message, expected);
    }
}
