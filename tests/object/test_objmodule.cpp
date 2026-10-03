#include <filesystem>
#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "value/AriaHashTable.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::new_function;
using aria::new_module;
using aria::new_string;
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_as_obj;
using aria::u8;
using aria::usize;
using aria::Value;

namespace {

    // 测试便利:委托 new_module 1 参 StringView 重载(name/dir 经工厂内部 intern 并自守)。
    // 返回的 m 未根,调用方跨 GC 点持有须自行守卫。默认名 "<script>"(临时模块)。
    ObjModule* make_module(GC& gc, const StringView name = "<script>") { return new_module(gc, name); }

    ObjModule* make_module(GC& gc, const StringView name, ObjString* dir) {
        auto guard = gc.make_guard(dir); // dir 先入根:下方 new_string(name) 可能 collect
        auto nm    = new_string(gc, name);
        guard.push(nm);
        return new_module(gc, nm, dir);
    }

    // 指定模块的具名函数:m 未根须自守(工厂内 intern name 与 new_object 均 GC 点,可能
    // collect 回收 m)。返回白色,调用方自守。
    ObjFunction* make_function(GC& gc, ObjModule* m, const StringView name, const u8 arity) {
        auto guard = gc.make_guard(m);
        return new_function(gc, m, name, arity, arity, false); // 无缺省,min_arity = arity
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    // (协议 fail 契约:load 族 nullopt / store 族 false ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.current_context()->take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = try_as_obj<aria::ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

} // namespace

TEST(ObjModule, Basics) {
    GC   gc;
    auto name  = new_string(gc, "lib/utils");
    auto guard = gc.make_guard(name); // 工厂不守卫入参:name 裸持跨 new_module 的 new_string(cwd)
    auto m     = new_module(gc, name);
    EXPECT_TRUE(aria::Object::is<ObjModule>(m));
    EXPECT_EQ(m->type(), aria::ObjType::MODULE);
    EXPECT_EQ(m->name(), name);     // intern 同指针
    EXPECT_EQ(m->entry(), nullptr); // 构造时无体
    EXPECT_EQ(m->globals().size(), 0u);
}

TEST(ObjModule, SetEntry) {
    GC   gc;
    auto m  = make_module(gc, "m");
    auto fn = make_function(gc, m, "<main>", 0); // body 属于 m(入口 <main> 名)
    m->set_entry(fn);
    EXPECT_EQ(m->entry(), fn);
}

TEST(ObjModule, ToString) {
    GC   gc;
    auto m = make_module(gc, "lib/utils");
    EXPECT_EQ(m->to_string(), "<module lib/utils>");
}

TEST(ObjModule, GlobalsSet) {
    GC   gc;
    auto m = make_module(gc, "m");
    auto k = new_string(gc, "x");
    auto v = new_string(gc, "a long enough value string!!!");
    m->globals().set(Value::from_obj(k), Value::from_obj(v));
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
    auto guard = gc.make_guard(name); // 工厂不守卫入参:name 裸持跨 new_module 的 new_string(cwd)
    auto m     = new_module(gc, name);
    guard.push(m); // 模块入临时根:collect -> mark_roots_ -> trace_gray_ -> m.trace

    auto body  = make_function(gc, m, "body", 0); // body 属于 m
    auto g_key = new_string(gc, "g");
    auto g_val = new_string(gc, "a long global value string!!!");
    m->set_entry(body);
    m->globals().set(Value::from_obj(g_key), Value::from_obj(g_val));

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
    make_module(gc, "orphan");
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// new_module 2 参重载时 dir_ 取当前工作目录(指针恒非空),与 std::filesystem::current_path 一致。
TEST(ObjModule, DirDefaultsToCwd) {
    GC   gc;
    auto m = make_module(gc, "lib/utils");
    ASSERT_NE(m->dir(), nullptr);
    EXPECT_EQ(m->dir()->view(), std::filesystem::current_path().string());
}

// abs_path = dir_ + "/" + name_ + ".aria"(= VM 模块表查重键形式)。
TEST(ObjModule, AbsPathComposesDirNameAria) {
    GC   gc;
    auto dir = new_string(gc, "/proj");
    auto m   = make_module(gc, "lib/utils", dir);
    EXPECT_EQ(m->abs_path(), "/proj/lib/utils.aria");
}

// name_ 为空(合成顶层)时 abs_path 仅返 dir_(无文件名)。
TEST(ObjModule, AbsPathForEmptyNameIsDir) {
    GC   gc;
    auto dir = new_string(gc, "/proj");
    auto m   = make_module(gc, "", dir);
    EXPECT_EQ(m->abs_path(), "/proj");
}

// 显式传 dir 时 dir_ 用所传值(不被 cwd 默认覆盖)。
TEST(ObjModule, ExplicitDirRespected) {
    GC   gc;
    auto dir = new_string(gc, "/stdlib");
    auto m   = make_module(gc, "math", dir);
    EXPECT_EQ(m->dir(), dir);
    EXPECT_EQ(m->abs_path(), "/stdlib/math.aria");
}

// ---- 位置串(未捕获堆栈跟踪 at 行):文件模块取 abs_path + 行号 ----

// 文件模块:有目录锚点且非合成名,位置串 = abs_path:line。
TEST(ObjModule, FormatLocationOfFileModuleIsAbsPath) {
    GC   gc;
    auto dir = new_string(gc, "/proj");
    auto m   = make_module(gc, "lib/utils", dir);
    EXPECT_EQ(m->format_location(12), "/proj/lib/utils.aria:12");
}

// 合成模块(名以 '<' 开头):abs_path 会拼出 dir_/<script>.aria 伪路径,位置串退化为模块名:line。
TEST(ObjModule, FormatLocationOfSyntheticModuleIsName) {
    GC   gc;
    auto dir = new_string(gc, "/proj");
    auto m   = make_module(gc, "<script>", dir);
    EXPECT_EQ(m->format_location(3), "<script>:3");
}

// 无目录锚点(abs_path 空,如 cwd 不可用):即使非合成名亦退化为模块名:line。
TEST(ObjModule, FormatLocationWithoutDirIsName) {
    GC   gc;
    auto dir = new_string(gc, "");
    auto m   = make_module(gc, "lib/utils", dir);
    EXPECT_TRUE(m->abs_path().empty());
    EXPECT_EQ(m->format_location(7), "lib/utils:7");
}

// ---- 命名成员协议(load_field/store_field = 模块全局绑定,只读) ----

// 命中:模块成员 = 模块全局绑定原值直读(不绑定 this、不铸包装物,返回值为原对象同指针)。
TEST(ObjModule, LoadFieldReadsGlobalBinding) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   m     = make_module(gc, "helper");
    guard.push(m);
    auto key = new_string(gc, "greet");
    guard.push(key);
    auto fn = make_function(gc, m, "greet", 1);
    guard.push(fn);
    m->globals().set(Value::from_obj(key), Value::from_obj(fn));

    const auto hit = m->load_field(vm, key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->as_obj(), fn); // 原值直读:同指针,无 bound/包裹
    EXPECT_FALSE(vm.current_context()->has_error());
}

// 绑定 nil 与「无此成员」的区分:键在表内、值为 nil 仍是命中(somed nil);仅 miss 才 fail。
// 这条是 find 空态(而非值判空)作命中判据的契约钉子。
TEST(ObjModule, LoadFieldNilBindingIsHit) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   m     = make_module(gc, "helper");
    guard.push(m);
    auto key = new_string(gc, "unset");
    guard.push(key);
    m->globals().set(Value::from_obj(key), Value::nil_val());

    const auto hit = m->load_field(vm, key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_TRUE(hit->is_nil());
    EXPECT_FALSE(vm.current_context()->has_error());
}

// miss:UndefinedProperty,文案与基类默认同形(模块描述经 debug_repr)。
TEST(ObjModule, LoadFieldMissFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   m     = make_module(gc, "helper");
    guard.push(m);
    auto key = new_string(gc, "nope");
    guard.push(key);

    EXPECT_FALSE(m->load_field(vm, key).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <module helper> has no member 'nope'");
}

// 写入:模块成员只读(定向文案)。越模块写会隐式创建未声明全局,违「赋值不隐式创建」。
TEST(ObjModule, StoreFieldRejectedAsReadOnly) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   m     = make_module(gc, "helper");
    guard.push(m);
    auto key = new_string(gc, "count");
    guard.push(key);

    EXPECT_FALSE(m->store_field(vm, key, Value::from_int(1)));
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch type Module does not support field assignment");
    EXPECT_EQ(m->globals().find(Value::from_obj(key)), nullptr); // 拒绝不留痕
}
