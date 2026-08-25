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
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;

namespace {

    // intern + 守卫 name,再调 new_module。工厂不再替调用方守卫入参,故本助手显式守卫 name 跨
    // new_module 内部 new_string(cwd)/new_object。返回的 m 未根(守卫随函数退出释放)。
    // 2 参(无 root):root 取 cwd。3 参:root 须由调用方传入(本助手先守 root 再 new_string(name))。
    ObjModule* make_module(GC& gc, StringView name = "<script>") {
        auto nm    = new_string(gc, name);
        auto guard = gc.make_guard(nm);
        return new_module(gc, nm);
    }

    ObjModule* make_module(GC& gc, StringView name, ObjString* root) {
        auto guard = gc.make_guard(root); // root 先入根:下方 new_string(name) 可能 collect
        auto nm    = new_string(gc, name);
        guard.push(nm);
        return new_module(gc, nm, root);
    }

    // 指定模块的具名函数:intern + 守卫 name,守卫 m,调 new_function(aria::)。工厂不再守卫入参,
    // 故本助手显式守卫 m 与 name。m 须在 new_string(name) 之前入根。
    ObjFunction* make_function(GC& gc, ObjModule* m, StringView name, u8 arity) {
        auto guard = gc.make_guard(m);
        auto nm    = new_string(gc, name);
        guard.push(nm);
        return new_function(gc, m, nm, arity);
    }

} // namespace

TEST(ObjModule, Basics) {
    GC   gc;
    auto name       = new_string(gc, "lib/utils");
    auto name_guard = gc.make_guard(name); // 工厂不再守卫入参:name 裸持跨 new_module 的 new_string(cwd)
    auto m          = new_module(gc, name);
    EXPECT_TRUE(aria::Object::is<ObjModule>(m));
    EXPECT_EQ(m->type(), aria::ObjType::MODULE);
    EXPECT_EQ(m->name(), name);                             // intern 同指针
    EXPECT_EQ(m->entry(), nullptr);                         // 构造时无体
    EXPECT_EQ(m->state(), ObjModule::ModuleState::Loading); // 构造即 Loading
    EXPECT_EQ(m->globals().size(), 0u);
}

TEST(ObjModule, SetEntryAndState) {
    GC   gc;
    auto m  = make_module(gc, "m");
    auto fn = make_function(gc, m, "<script>", 0); // body 属于 m(入口 <script> 名)
    m->set_entry(fn);
    EXPECT_EQ(m->entry(), fn);
    EXPECT_EQ(m->state(), ObjModule::ModuleState::Loading);
    m->set_state(ObjModule::ModuleState::Loaded);
    EXPECT_EQ(m->state(), ObjModule::ModuleState::Loaded);
}

TEST(ObjModule, ToString) {
    GC   gc;
    auto m = make_module(gc, "lib/utils");
    EXPECT_EQ(m->to_string(), "<module lib/utils>");
}

TEST(ObjModule, GlobalsUpsert) {
    GC   gc;
    auto m = make_module(gc, "m");
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
    auto name       = new_string(gc, "lib/utils");
    auto name_guard = gc.make_guard(name); // 工厂不再守卫入参:name 裸持跨 new_module 的 new_string(cwd)
    auto m          = new_module(gc, name);
    auto guard      = gc.make_guard(m); // 模块入临时根:collect -> mark_roots_ -> trace_gray_ -> m.trace

    auto body  = make_function(gc, m, "body", 0); // body 属于 m
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
    (void) make_module(gc, "orphan");
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// new_module 2 参重载时 root_ 取当前工作目录(指针恒非空),与 std::filesystem::current_path 一致。
TEST(ObjModule, RootDefaultsToCwd) {
    GC   gc;
    auto m = make_module(gc, "lib/utils");
    ASSERT_NE(m->root(), nullptr);
    EXPECT_EQ(m->root()->view(), std::filesystem::current_path().string());
}

// abs_path = root_ + "/" + name_ + ".aria"(= VM 模块表查重键形式)。
TEST(ObjModule, AbsPathComposesRootNameAria) {
    GC   gc;
    auto root = new_string(gc, "/proj");
    auto m    = make_module(gc, "lib/utils", root);
    EXPECT_EQ(m->abs_path(), "/proj/lib/utils.aria");
}

// name_ 为空(合成顶层)时 abs_path 仅返 root_(无文件名)。
TEST(ObjModule, AbsPathForEmptyNameIsRoot) {
    GC   gc;
    auto root = new_string(gc, "/proj");
    auto m    = make_module(gc, "", root);
    EXPECT_EQ(m->abs_path(), "/proj");
}

// 显式传 root 时 root_ 用所传值(不被 cwd 默认覆盖)。
TEST(ObjModule, ExplicitRootRespected) {
    GC   gc;
    auto root = new_string(gc, "/stdlib");
    auto m    = make_module(gc, "math", root);
    EXPECT_EQ(m->root(), root);
    EXPECT_EQ(m->abs_path(), "/stdlib/math.aria");
}
