#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::new_string;
using aria::new_upvalue;
using aria::ObjString;
using aria::ObjUpvalue;
using aria::usize;
using aria::Value;
using aria::value_identical;

// open 态:指向外部槽,value_slot() 即该槽;close 后值迁入内部 closed_ 自持,原槽不再可见。
TEST(ObjUpvalue, OpenPointsToSlot) {
    GC          gc;
    Value       v  = Value::from_i32(42);
    ObjUpvalue* uv = new_upvalue(gc, &v);
    EXPECT_TRUE(aria::Object::is<ObjUpvalue>(uv));
    EXPECT_EQ(uv->type(), aria::ObjType::UPVALUE);
    EXPECT_EQ(uv->type_name(), "Upvalue");
    EXPECT_TRUE(uv->is_open());
    EXPECT_EQ(uv->value_slot(), &v);
    EXPECT_TRUE(value_identical(*uv->value_slot(), v));
}

// 经 value_slot() 写穿透到原槽(内层经 upvalue 写外层局部的机制)。
TEST(ObjUpvalue, WriteThroughSlot) {
    GC    gc;
    Value v           = Value::nil_val();
    auto  uv          = new_upvalue(gc, &v);
    *uv->value_slot() = Value::from_i32(7);
    EXPECT_TRUE(value_identical(v, Value::from_i32(7)));
}

// close():值迁入自持 closed_,location_ 转指自身;原槽后续修改不再影响 upvalue。
TEST(ObjUpvalue, CloseMigratesValue) {
    GC    gc;
    Value v  = Value::from_i32(42);
    auto  uv = new_upvalue(gc, &v);
    uv->close();
    EXPECT_FALSE(uv->is_open());
    EXPECT_NE(uv->value_slot(), &v); // 已转指内部 closed_
    EXPECT_TRUE(value_identical(*uv->value_slot(), Value::from_i32(42)));
    v = Value::from_i32(99); // 原槽后续修改不再可见
    EXPECT_TRUE(value_identical(*uv->value_slot(), Value::from_i32(42)));
}

// set_location():grow_stack_ 重绑用,仅 open 态;重绑后读写走新槽。
TEST(ObjUpvalue, SetLocationRebinds) {
    GC    gc;
    Value a  = Value::from_i32(1);
    Value b  = Value::from_i32(2);
    auto  uv = new_upvalue(gc, &a);
    uv->set_location(&b);
    EXPECT_TRUE(uv->is_open());
    EXPECT_EQ(uv->value_slot(), &b);
    EXPECT_TRUE(value_identical(*uv->value_slot(), Value::from_i32(2)));
    uv->close(); // 关闭后不再可重绑(closed 态不持栈槽,debug 下 set_location 断言)
    EXPECT_FALSE(uv->is_open());
}

// 开链节点:next_open_ 侵入式链(不叫 next_--撞基类 Object::next_ 的 GC 对象链字段),
// VM 侧按槽址降序维护(此处只验访问器)。
TEST(ObjUpvalue, NextChain) {
    GC    gc;
    Value a  = Value::from_i32(1);
    Value b  = Value::from_i32(2);
    auto  u1 = new_upvalue(gc, &a);
    auto  u2 = new_upvalue(gc, &b);
    EXPECT_EQ(u1->next_open(), nullptr);
    u1->set_next_open(u2);
    EXPECT_EQ(u1->next_open(), u2);
    EXPECT_EQ(u2->next_open(), nullptr);
}

// stress GC(open 态):栈槽内的对象值须经 uv->trace(标 *value_slot())存活。
TEST(ObjUpvalue, TraceMarksSlotValueWhileOpen) {
    GC gc;
    gc.set_stress(true);
    Value v     = Value::nil_val();
    auto  uv    = new_upvalue(gc, &v);
    auto  guard = gc.make_guard(uv);                                         // upvalue 入临时根(跨后续任何分配)
    auto  s     = new_string(gc, "captured long string beyond sso padding"); // 建时 collect:uv 已根
    v           = Value::from_obj(s);
    (void) new_string(gc, "trigger"); // stress collect:串经 uv->trace 存活
    EXPECT_EQ(s->view(), "captured long string beyond sso padding");
    EXPECT_TRUE(value_identical(*uv->value_slot(), Value::from_obj(s)));
}

// stress GC(closed 态):原槽已改写,自持 closed_ 的对象值仅经 uv->trace 存活。
TEST(ObjUpvalue, TraceMarksClosedValue) {
    GC gc;
    gc.set_stress(true);
    ObjUpvalue* uv = nullptr;
    {
        auto  s     = new_string(gc, "closed payload long string beyond sso pad");
        auto  guard = gc.make_guard(s); // 串跨 uv 的 new_object(stress collect)须先根化
        Value v     = Value::from_obj(s);
        uv          = new_upvalue(gc, &v); // 建时 collect:s 经 guard 存活
        uv->close();                       // 值迁入 closed_
    } // guard 释放:s 此后仅经 uv.closed_ 可达
    auto guard = gc.make_guard(uv);
    (void) new_string(gc, "trigger"); // stress collect:closed 值经 uv->trace 存活
    EXPECT_FALSE(uv->is_open());
    EXPECT_NE(uv->value_slot(), nullptr);
    EXPECT_EQ(aria::Object::as<ObjString>(uv->value_slot()->as_obj())->view(),
              "closed payload long string beyond sso pad");
}

// 无根 upvalue 被 sweep(壳回收)。
TEST(ObjUpvalue, UnrootedUpvalueSwept) {
    GC    gc;
    Value v = Value::from_i32(1);
    (void) new_upvalue(gc, &v); // 无根
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// 调试渲染:语言层不可见的内部对象,渲染稳定短文案(非地址型)。
TEST(ObjUpvalue, DebugRenderIsStableText) {
    GC    gc;
    Value v  = Value::nil_val();
    auto  uv = new_upvalue(gc, &v);
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(uv)), "<upvalue>");
}
