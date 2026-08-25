#include <filesystem>
#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::new_function;
using aria::new_module;
using aria::new_string;
using aria::ObjModule;
using aria::usize;
using aria::Value;

TEST(ObjModule, Basics) {
    GC   gc;
    auto name = new_string(gc, "lib/utils");
    auto m    = new_module(gc, name);
    EXPECT_TRUE(aria::Object::is<ObjModule>(m));
    EXPECT_EQ(m->type(), aria::ObjType::MODULE);
    EXPECT_EQ(m->name(), name);                             // intern 同指针
    EXPECT_EQ(m->entry(), nullptr);                         // 构造时无体
    EXPECT_EQ(m->state(), ObjModule::ModuleState::Loading); // 构造即 Loading
    EXPECT_EQ(m->globals().size(), 0u);
}

TEST(ObjModule, SetEntryAndState) {
    GC   gc;
    auto m       = new_module(gc, new_string(gc, "m"));
    auto m_guard = gc.make_guard(m); // 护 m 跨下方 new_string("<script>"):m 未根,new_string 可能 collect
    auto fn      = new_function(gc, m, new_string(gc, "<script>"), 0); // body 属于 m(入口 <script> 名)
    m->set_entry(fn);
    EXPECT_EQ(m->entry(), fn);
    EXPECT_EQ(m->state(), ObjModule::ModuleState::Loading);
    m->set_state(ObjModule::ModuleState::Loaded);
    EXPECT_EQ(m->state(), ObjModule::ModuleState::Loaded);
}

TEST(ObjModule, ToString) {
    GC   gc;
    auto m = new_module(gc, new_string(gc, "lib/utils"));
    EXPECT_EQ(m->to_string(), "<module lib/utils>");
}

TEST(ObjModule, GlobalsUpsert) {
    GC   gc;
    auto m = new_module(gc, new_string(gc, "m"));
    auto k = new_string(gc, "x");
    auto v = new_string(gc, "a long enough value string!!!");
    auto e = m->globals().upsert(Value::from_obj(k));
    ASSERT_NE(e, nullptr);
    e->value   = Value::from_obj(v);
    auto found = m->globals().find(Value::from_obj(k));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value.as_obj(), v);
    EXPECT_EQ(m->globals().size(), 1u);
}

// 模块入临时根 -> collect 经 trace_gray_ 调 m.trace -> 标 name_/entry_/globals_,
// 三类子节点(name/body/g_key/g_val)存活。验证 trace 覆盖完整。
TEST(ObjModule, TraceKeepsNameEntryAndGlobals) {
    GC   gc;
    auto name  = new_string(gc, "lib/utils");
    auto m     = new_module(gc, name);
    auto guard = gc.make_guard(m); // 模块入临时根:collect -> mark_roots_ -> trace_gray_ -> m.trace

    auto body  = new_function(gc, m, new_string(gc, "body"), 0); // body 属于 m
    auto g_key = new_string(gc, "g");
    auto g_val = new_string(gc, "a long global value string!!!");
    m->set_entry(body);
    auto ge   = m->globals().upsert(Value::from_obj(g_key));
    ge->value = Value::from_obj(g_val);

    const usize before = gc.bytes_allocated();
    gc.collect(); // m 经 guard 标根 -> m.trace 标 name/body/g_key/g_val -> 全存活
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(name->view(), "lib/utils");
    EXPECT_EQ(m->entry(), body);
    EXPECT_EQ(g_val->view(), "a long global value string!!!");
}

// 未根模块被 sweep 回收。
TEST(ObjModule, UnrootedModuleSwept) {
    GC gc;
    (void) new_module(gc, new_string(gc, "orphan"));
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// new_module 2 参重载时 root_ 取当前工作目录(指针恒非空),与 std::filesystem::current_path 一致。
TEST(ObjModule, RootDefaultsToCwd) {
    GC   gc;
    auto m = new_module(gc, new_string(gc, "lib/utils"));
    ASSERT_NE(m->root(), nullptr);
    EXPECT_EQ(m->root()->view(), std::filesystem::current_path().string());
}

// abs_path = root_ + "/" + name_ + ".aria"(= VM 模块表查重键形式)。
TEST(ObjModule, AbsPathComposesRootNameAria) {
    GC   gc;
    auto root = new_string(gc, "/proj");
    auto m    = new_module(gc, new_string(gc, "lib/utils"), root);
    EXPECT_EQ(m->abs_path(), "/proj/lib/utils.aria");
}

// name_ 为空(合成顶层)时 abs_path 仅返 root_(无文件名)。
TEST(ObjModule, AbsPathForEmptyNameIsRoot) {
    GC   gc;
    auto root = new_string(gc, "/proj");
    auto m    = new_module(gc, new_string(gc, ""), root);
    EXPECT_EQ(m->abs_path(), "/proj");
}

// 显式传 root 时 root_ 用所传值(不被 cwd 默认覆盖)。
TEST(ObjModule, ExplicitRootRespected) {
    GC   gc;
    auto root = new_string(gc, "/stdlib");
    auto m    = new_module(gc, new_string(gc, "math"), root);
    EXPECT_EQ(m->root(), root);
    EXPECT_EQ(m->abs_path(), "/stdlib/math.aria");
}
