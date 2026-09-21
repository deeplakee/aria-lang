#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::new_module;
using aria::new_string;
using aria::ObjException;
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::Pair;
using aria::Span;
using aria::String;
using aria::try_obj;
using aria::Value;

namespace {

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt / store 族 false ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.main_context().take_error();
        EXPECT_TRUE(payload.has_value()); // fail 契约:失败信号 ⟺ 寄存器必有载荷
        const auto ex = try_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

} // namespace

// try_as<T>(is+as 合一):动态类型匹配返回转型指针,否则 nullptr(含 null 入参)。

TEST(ObjectTryAs, MatchReturnsPointer) {
    GC   gc;
    auto s = new_string(gc, "hello");
    EXPECT_EQ(aria::Object::try_as<ObjString>(s), s);
}

TEST(ObjectTryAs, MismatchReturnsNull) {
    GC   gc;
    auto s = new_string(gc, "hello");
    EXPECT_EQ(aria::Object::try_as<ObjFunction>(s), nullptr);
}

TEST(ObjectTryAs, NullObjectReturnsNull) {
    EXPECT_EQ(aria::Object::try_as<ObjString>(static_cast<aria::Object*>(nullptr)), nullptr);
}

// const 重载:const Object* -> const T*。
TEST(ObjectTryAs, ConstOverload) {
    GC                  gc;
    auto                s = new_string(gc, "hello");
    const aria::Object* o = s;
    EXPECT_EQ(aria::Object::try_as<ObjString>(o), s);
    EXPECT_EQ(aria::Object::try_as<ObjFunction>(o), nullptr);
}

// 成员/下标访问/算术/比较/可调用协议的**基类默认**:未 override 的子类型对协议操作一律
// vm.fail 入寄存器后返失败信号 --load 族 nullopt、store 族 false。本测试钉住默认形态(码 +
// 文案子串)防将来基类签名漂移。探针类型随 override 落地而换:string 自批 6 起带下标/成员
// override,Module 自模块成员访问起 override load_field/store_field,string 又自字符串 `+`
// 与字符串比较起 override op_add 与四个比较算子,故各分组挑当下仍未 override 的类型探默认
//(string 探 store_field、Module 探下标注解与算术/比较)。

TEST(ObjectProtocolDefaults, MemberIndexAndOperatorDefaults) {
    AriaVM vm; // 报错经 vm.fail 入挂起寄存器
    auto&  gc    = vm.gc();
    auto   s     = new_string(gc, "hello");
    auto   guard = gc.make_guard(s);
    auto   m     = new_module(gc, "<test>");
    guard.push(m);
    auto k = new_string(gc, "len");
    guard.push(k);

    // 成员协议默认:load miss = "X has no member 'y'"(对象描述经 debug_repr)、
    // store = "type X does not support field access"。Module 已 override 两协议(load = 成员查
    // globals_、store = 只读拒),故写默认体用 string 探(ObjString 不 override store_field)。
    EXPECT_FALSE(m->load_field(vm, k).has_value());
    auto [code, msg] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("has no member 'len'"));

    EXPECT_FALSE(s->store_field(vm, k, Value::from_int(1)));
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("type String does not support field access"));

    // 下标协议默认(备置):"type X does not support subscript access"。
    EXPECT_FALSE(m->load_index(vm, Value::from_int(0)).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_TRUE(msg.contains("type Module does not support subscript access"));

    EXPECT_FALSE(m->store_index(vm, Value::from_int(0), Value::from_int(1)));
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_TRUE(msg.contains("type Module does not support subscript access"));

    // 算术协议默认体:"operator '+' requires numbers, got X and Y"(与 VM 原语路径
    // run_binary_numeric 的 TypeMismatch 文案一致)、一元 "negate requires a number, got X"。
    // op_add 已被 ObjString override(两侧 String 拼接,见 test_objstring),故默认体用 Module 探。
    EXPECT_FALSE(m->op_add(vm, Value::from_int(1)).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_TRUE(msg.contains("operator '+' requires numbers, got Module and Int"));

    EXPECT_FALSE(s->op_sub(vm, Value::from_int(1)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::TypeMismatch);
    EXPECT_FALSE(s->op_mul(vm, Value::from_int(1)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::TypeMismatch);
    EXPECT_FALSE(s->op_div(vm, Value::from_int(1)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::TypeMismatch);
    EXPECT_FALSE(s->op_mod(vm, Value::from_int(1)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::TypeMismatch);

    EXPECT_FALSE(s->op_negate(vm).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::InvalidOperand);
    EXPECT_TRUE(msg.contains("negate requires a number, got String"));

    // 比较算子默认体:同样"requires numbers, got X and Y",四算子各带自己的符号
    // (ObjString 已 override 成字节序比较,故用 Module 探默认体)。
    EXPECT_FALSE(m->op_less(vm, Value::from_int(1)).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_TRUE(msg.contains("operator '<' requires numbers, got Module and Int"));

    EXPECT_FALSE(m->op_less_equal(vm, Value::from_int(1)).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_TRUE(msg.contains("operator '<=' requires numbers"));

    EXPECT_FALSE(m->op_greater(vm, Value::from_int(1)).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_TRUE(msg.contains("operator '>' requires numbers"));

    EXPECT_FALSE(m->op_greater_equal(vm, Value::from_int(1)).has_value());
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_TRUE(msg.contains("operator '>=' requires numbers"));

    // 可调用协议默认(备置):本类型不可调用,CallNonCallable(文案与 call_value 原默认一致;
    // slots 契约同 NativeFn,调用区 Span 经 Span<Value>{&peek(argc), argc+1} 构造)。
    Value sv_box = Value::from_obj(s); // callee 占槽 0(与 VM 调用区同形)
    EXPECT_FALSE(s->op_call(vm, Span<Value>{&sv_box, 1}));
    std::tie(code, msg) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::CallNonCallable);
    EXPECT_TRUE(msg.contains("call non-callable String"));
}
