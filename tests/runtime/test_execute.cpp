#include <gtest/gtest.h>

#include <limits>

#include "aria.hpp"
#include "compile/Compiler.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/Eval.hpp"
#include "type.hpp"
#include "util/source_file.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::Compiler;
using aria::Error;
using aria::ErrorCode;
using aria::Eval;
using aria::GC;
using aria::kModuleEntryName;
using aria::List;
using aria::NativeCallInfo;
using aria::NativeFn;
using aria::new_function;
using aria::new_module;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::Opt;
using aria::Pair;
using aria::Result;
using aria::SourceFile;
using aria::Span;
using aria::String;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;

namespace {

    // ---- 重入口驱动原生:脚本经全局名传入闭包/绑定方法值,原生同步执行之 ----

    // 拷出调用区实参(skip 个首部槽之后的部分)入 args 缓冲:重入实参不得指进值栈(execute_function
    // 别名契约),视图读数按值抄离,跨重入期间值栈扩容无牵连。returns u8 argc。
    u8 copy_call_args(const NativeCallInfo& info, const u8 skip, Value* args) {
        const u8 argc = static_cast<u8>(info.size() - skip);
        for (u8 i = 0; i < argc; ++i) {
            args[i] = info[skip + i];
        }
        return argc;
    }

    // 全局白盒取值(不存在返回空)。
    Opt<Value> find_global(ObjModule& module, GC& gc, const StringView name) {
        const auto found = module.globals().find(Value::from_obj(new_string(gc, name)));
        if (found == nullptr) {
            return std::nullopt;
        }
        return found->value;
    }

    // render(x) -> string:显示渲染探针(直驱 Eval::stringify:钩子命中/根默认/D2 违约全经此口),
    // 失败载荷在寄存器按 bool 契约透传。
    bool fn_render(const NativeCallInfo& info) {
        auto& ctx = info.ctx();
        if (const auto argc = info.size(); argc != 1) {
            return ctx.arity_error(argc, 1);
        }
        const auto s = Eval::stringify(ctx, info[0]);
        if (!s) {
            return false;
        }
        info.set_return_val(Value::from_obj(s));
        return true;
    }

    // 脚轮驱动出口:result 供断言,module 供 globals 检视(vm 内根,vm 存活期有效)。
    struct ScriptRun {
        Result<Value, Error> result;
        ObjModule*           module;
    };

    // 编译 + 原生播种 + run():先以 {名, fn} 把 natives 播进模块 globals(脚本 LOAD_GLOBAL 直接
    // 命中),再编译 src 为入口函数 run。编译失败属用例缺陷,ADD_FAILURE 后带错误 Result 返回。
    ScriptRun run_with_natives(AriaVM& vm, const StringView src, const List<Pair<const char*, NativeFn>>& natives) {
        auto& gc     = vm.gc();
        auto* module = new_module(gc, "<test-script>");
        auto  guard  = gc.make_guard(module);
        for (const auto& [name, fn]: natives) {
            const auto key    = new_string(gc, name);
            const auto kguard = gc.make_guard(key); // key 白色须跨 new_function 的 GC 点存活
            module->globals().set(Value::from_obj(key), Value::from_obj(new_function(gc, key, fn)));
        }
        SourceFile source{"<test-script>", "<test-script>.aria", String{src}};
        const auto entry = Compiler::compile(gc, source, module, kModuleEntryName);
        if (!entry.has_value()) {
            ADD_FAILURE() << "compile failed: " << entry.error().message();
            return {std::unexpected(entry.error()), module};
        }
        return {vm.run(*entry), module};
    }

} // namespace

namespace aria {

    // 重入口驱动探针宿主(全走公共面:ctx 服务面与重入口 execute_function/execute_method;定义
    // 全收于此)。
    struct TestProbe {
        // exec_closure(fn, a1..aN):同步执行函数 fn(execute_function 形态),fn 须函数值(原生/aria
        // 载荷皆可);失败载荷已在寄存器按 bool 契约返 false,成功返回值写槽 0。
        static bool exec_closure_native(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc < 1) {
                return ctx.arity_error(argc, 1);
            }
            if (!info[0].is_obj() || !info[0].as_obj()->is<ObjFunction>()) {
                return ctx.fail(ErrorCode::TypeMismatch, "exec_closure expects a function as its first argument");
            }
            Value      args[std::numeric_limits<u8>::max()];
            const u8   argc   = copy_call_args(info, 1, args);
            const auto result = ctx.execute_function(info[0].as_obj()->as<ObjFunction>(), {args, argc});
            if (!result.has_value()) {
                return false; // 载荷在寄存器,交外层续搜
            }
            info.set_return_val(*result);
            return true;
        }

        // exec_method(m, a1..aN):同步执行绑定方法 m(execute_method 形态,receiver 作 this),m 须
        // 绑定方法值(脚本 obj.name 的值即此形态)。余同 exec_closure。
        static bool exec_method_native(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc < 1) {
                return ctx.arity_error(argc, 1);
            }
            if (!info[0].is_obj() || !info[0].as_obj()->is<ObjBoundMethod>()) {
                return ctx.fail(ErrorCode::TypeMismatch, "exec_method expects a bound method as its first argument");
            }
            const auto bound = info[0].as_obj()->as<ObjBoundMethod>();
            Value      args[std::numeric_limits<u8>::max()];
            const u8   argc   = copy_call_args(info, 1, args);
            const auto method = bound->method();
            const auto result = ctx.execute_method(bound->receiver(), method, {args, argc});
            if (!result.has_value()) {
                return false; // 载荷在寄存器,交外层续搜
            }
            info.set_return_val(*result);
            return true;
        }
    };

} // namespace aria

// stress GC 默认开(同 AriaVMStress):锻炼重入口期值栈/帧根接线。
class NestedExecStress : public ::testing::Test {
protected:
    AriaVM vm;

    void SetUp() override { vm.gc().set_stress(true); }
};

TEST_F(NestedExecStress, RunClosureReturnsValue) {
    const auto src = R"(
        fun add(a, b) { return a + b; }
        var g = exec_closure(add, 3, 4);
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}});
    ASSERT_TRUE(r.result.has_value());
    const auto g = find_global(*r.module, vm.gc(), "g");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->as_int(), 7);
}

TEST_F(NestedExecStress, RunMethodBindsThis) {
    const auto src = R"(
        def Counter {
            init(start) { this.n = start; }
            bump(d) { this.n = this.n + d; return this.n; }
        }
        var c      = Counter(5);
        var r1     = exec_method(c.bump, 5);
        var r2     = exec_method(c.bump, 5);
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_method", &aria::TestProbe::exec_method_native}});
    ASSERT_TRUE(r.result.has_value());
    const auto r1 = find_global(*r.module, vm.gc(), "r1");
    const auto r2 = find_global(*r.module, vm.gc(), "r2");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->as_int(), 10);
    EXPECT_EQ(r2->as_int(), 15);
}

TEST_F(NestedExecStress, RunNativeFnReturnsValue) {
    const auto src = R"(
        var g = exec_closure(render, 123);
    )";
    const auto r =
            run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}, {"render", fn_render}});
    ASSERT_TRUE(r.result.has_value());
    const auto g = find_global(*r.module, vm.gc(), "g");
    ASSERT_TRUE(g.has_value());
    // 原生载荷经重入口同步执行(不进帧、无嵌套驱动):产出即返回值。
    const auto s = g->as_obj()->as<ObjString>();
    EXPECT_EQ(s->view(), "123");
}

TEST_F(NestedExecStress, RunNativeMethodBindsReceiver) {
    const auto src = R"(
        var m = "abc".upper;
        var u = exec_method(m);
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_method", &aria::TestProbe::exec_method_native}});
    ASSERT_TRUE(r.result.has_value());
    const auto u = find_global(*r.module, vm.gc(), "u");
    ASSERT_TRUE(u.has_value());
    // 绑定原生实例方法经重入口执行:receiver 作槽 0,原生 receiver<T>() 语义照常成立。
    const auto s = u->as_obj()->as<ObjString>();
    EXPECT_EQ(s->view(), "ABC");
}

TEST_F(NestedExecStress, UncaughtSurfacesAtTopLevel) {
    const auto src = R"(
        fun boom() { throw "deep boom"; }
        var x = exec_closure(boom);
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}});
    ASSERT_FALSE(r.result.has_value());
    const Error& err = r.result.error();
    EXPECT_EQ(err.code(), ErrorCode::UncaughtException);
    // 抛出原值渲染进消息;嵌套段帧随善后截净,跟踪只含最外段,"at boom" 不见。
    EXPECT_NE(err.message().find("deep boom"), String::npos);
    EXPECT_EQ(err.message().find("at boom"), String::npos);
}

TEST_F(NestedExecStress, UncaughtCaughtByOuterTry) {
    const auto src = R"(
        fun boom() { throw "outer caught"; }
        var caught = nil;
        try {
            var x = exec_closure(boom);
        } catch (e) {
            caught = e;
        }
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}});
    ASSERT_TRUE(r.result.has_value());
    const auto caught = find_global(*r.module, vm.gc(), "caught");
    ASSERT_TRUE(caught.has_value());
    // catch 绑原值:外层接住嵌套执行里抛的串。
    const auto s = caught->as_obj()->as<ObjString>();
    EXPECT_EQ(s->view(), "outer caught");
}

TEST_F(NestedExecStress, DepthCappedAtRecursionLimit) {
    const auto src = R"(
        fun rec() { return exec_closure(rec); }
        rec();
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}});
    ASSERT_FALSE(r.result.has_value());
    EXPECT_EQ(r.result.error().code(), ErrorCode::StackOverflow);
    EXPECT_NE(r.result.error().message().find("nested execution too deep"), String::npos);
}

TEST_F(NestedExecStress, YieldInsideNestedRefused) {
    const auto src = R"(
        fun yfn() { return Coroutine.yield(); }
        fun wrapper() { return exec_closure(yfn); }
        var co = Coroutine.create(wrapper);
        var caught = nil;
        try {
            Coroutine.resume(co);
        } catch (e) {
            caught = e;
        }
        var st = Coroutine.status(co);
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}});
    ASSERT_TRUE(r.result.has_value());
    const auto caught = find_global(*r.module, vm.gc(), "caught");
    ASSERT_TRUE(caught.has_value());
    const auto ex = caught->as_obj()->as<ObjException>();
    EXPECT_EQ(ex->code(), ErrorCode::YieldInNestedExecution);
    // yield 拒绝后沿 resume 链跳回主上下文:协程终态 failed。
    const auto st = find_global(*r.module, vm.gc(), "st");
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->as_obj()->as<ObjString>()->view(), "failed");
}

TEST_F(NestedExecStress, ResumeInsideNestedMultiplexes) {
    const auto src = R"(
        fun co_body() { return 42; }
        var inner = Coroutine.create(co_body);
        fun mult() { return Coroutine.resume(inner); }
        fun wrapper() { return exec_closure(mult); }
        var outer = Coroutine.create(wrapper);
        var v = Coroutine.resume(outer);
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}});
    ASSERT_TRUE(r.result.has_value());
    const auto v = find_global(*r.module, vm.gc(), "v");
    ASSERT_TRUE(v.has_value());
    // 嵌套执行内 resume 合法:inner 完成的 42 穿 exec_closure 回 wrapper 再回 outer。
    EXPECT_EQ(v->as_int(), 42);
}

// ---- Eval::stringify 显示渲染口(__str__ 咨询/D2 契约/重入) ----

TEST_F(NestedExecStress, StringifyHookOnInstance) {
    const auto src = R"src(
        def Point {
            init(x, y) { this.x = x; this.y = y; }
            __str__() { return "P(${this.x},${this.y})"; }
        }
        var s = render(Point(1, 2));
    )src";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_TRUE(r.result.has_value());
    const auto s = find_global(*r.module, vm.gc(), "s");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->as_obj()->as<ObjString>()->view(), "P(1,2)");
}

TEST_F(NestedExecStress, StringifyRootDefaultDebugForm) {
    const auto src = R"(
        def Point {
            init(x) { this.x = x; }
        }
        var s = render(Point(3));
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_TRUE(r.result.has_value());
    const auto s = find_global(*r.module, vm.gc(), "s");
    ASSERT_TRUE(s.has_value());
    // 链上无用户钩子:根类默认 __str__ 原生兜底,渲染实例 debug 形态。
    EXPECT_EQ(s->as_obj()->as<ObjString>()->view(), "<Point instance>");
}

TEST_F(NestedExecStress, StringifyInstanceFieldShadowExcluded) {
    const auto src = R"(
        def Box { }
        var b = Box();
        b.__str__ = fun() { return "shadow"; };
        var s = render(b);
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_TRUE(r.result.has_value());
    const auto s = find_global(*r.module, vm.gc(), "s");
    ASSERT_TRUE(s.has_value());
    // 咨询出局值本地状态:实例字段 __str__ 不接管渲染(成员读 b.__str__ 仍见字段值)。
    EXPECT_EQ(s->as_obj()->as<ObjString>()->view(), "<Box instance>");
}

TEST_F(NestedExecStress, StringifyClassAttrNonCallableRejected) {
    const auto src = R"(
        def Box { }
        Box.__str__ = 42;
        var s = render(Box());
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_FALSE(r.result.has_value());
    const Error& err = r.result.error();
    // 非可调解析值由外部调用口收口(CallNonCallable),原码透传附 at 行。
    EXPECT_EQ(err.code(), ErrorCode::CallNonCallable);
    EXPECT_NE(err.message().find("type Int is not callable"), String::npos);
}

TEST_F(NestedExecStress, StringifyHookMustReturnString) {
    const auto src = R"(
        def Box {
            __str__() { return 42; }
        }
        var s = render(Box());
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_FALSE(r.result.has_value());
    const Error& err = r.result.error();
    EXPECT_EQ(err.code(), ErrorCode::TypeMismatch);
    EXPECT_NE(err.message().find("'__str__' must return a string, got Int"), String::npos);
}

TEST_F(NestedExecStress, StringifyHookRaisePropagates) {
    const auto src = R"(
        def Box {
            __str__() { throw "render boom"; }
        }
        var s = render(Box());
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_FALSE(r.result.has_value());
    const Error& err = r.result.error();
    EXPECT_EQ(err.code(), ErrorCode::UncaughtException);
    EXPECT_NE(err.message().find("render boom"), String::npos);
}

TEST_F(NestedExecStress, StringifyRecursionCapped) {
    const auto src = R"(
        def Loop {
            __str__() { return render(this); }
        }
        var s = render(Loop());
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_FALSE(r.result.has_value());
    const Error& err = r.result.error();
    // 自指渲染在嵌套深度上限处折断(每圈两层:render 原生 + __str__ 闭包),VM 异常载荷保持原码。
    EXPECT_EQ(err.code(), ErrorCode::StackOverflow);
    EXPECT_NE(err.message().find("nested execution too deep"), String::npos);
}

TEST_F(NestedExecStress, StringifyBoundMethodHook) {
    const auto src = R"(
        def Format {
            init(text) { this.text = text; }
            fmt() { return this.text; }
        }
        def Box { }
        Box.__str__ = Format("F").fmt;
        var aria_text = render(Box());
        Box.__str__ = [1, 2].__str__;
        var native_text = render(Box());
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_TRUE(r.result.has_value());
    // 绑定钩子经 execute_callable 分派:沿用其自身 receiver,argc = 0。
    const auto aria_text = find_global(*r.module, vm.gc(), "aria_text");
    ASSERT_TRUE(aria_text.has_value());
    EXPECT_EQ(aria_text->as_obj()->as<ObjString>()->view(), "F");
    const auto native_text = find_global(*r.module, vm.gc(), "native_text");
    ASSERT_TRUE(native_text.has_value());
    EXPECT_EQ(native_text->as_obj()->as<ObjString>()->view(), "[1, 2]");
}

TEST_F(NestedExecStress, StringifyPlainAndBuiltinValues) {
    const auto src = R"(
        def Point { }
        var a = render(42);
        var b = render(nil);
        var c = render(true);
        var d = render(1.5);
        var e = render("raw");
        var f = render([1, 2]);
        var g = render(Point);
    )";
    const auto r   = run_with_natives(vm, src, {{"render", fn_render}});
    ASSERT_TRUE(r.result.has_value());
    const auto check = [&](const char* name, const StringView want) {
        const auto v = find_global(*r.module, vm.gc(), name);
        ASSERT_TRUE(v.has_value()) << name;
        EXPECT_EQ(v->as_obj()->as<ObjString>()->view(), want) << name;
    };
    check("a", "42");
    check("b", "nil");
    check("c", "true");
    check("d", "1.5");
    check("e", "raw");
    check("f", "[1, 2]");
    // 类值沿元类链命中根默认,渲染 debug 形态。
    check("g", "<class Point>");
}

// ---- 显示位接线(str/println/String/join/插值全走 __str__ 咨询面) ----

TEST_F(NestedExecStress, DisplayFacesRouteThroughStringify) {
    const auto src = R"src(
        def P { __str__() { return "P"; } }
        var a = str(P());
        var b = "x${P()}x";
        var c = [P(), P()].join("-");
        var d = str([1, "s", P()]);
        var e = String(P());
    )src";
    const auto r   = run_with_natives(vm, src, {});
    ASSERT_TRUE(r.result.has_value());
    const auto check = [&](const char* name, const StringView want) {
        const auto v = find_global(*r.module, vm.gc(), name);
        ASSERT_TRUE(v.has_value()) << name;
        EXPECT_EQ(v->as_obj()->as<ObjString>()->view(), want) << name;
    };
    // str/插值/String( 逐对象咨询钩子。
    check("a", "P");
    check("b", "xPx");
    check("c", "P-P");
    check("e", "P");
    // 容器 debug 位经 value_repr 渲染,不跑用户钩子(显示/调试分叉钉)。
    check("d", "[1, \"s\", <P instance>]");
}

TEST_F(NestedExecStress, DisplayRenderFailureUnwindsCleanly) {
    const auto src = R"src(
        def Q { __str__() { throw "render boom"; } }
        var caught = "";
        try {
            var s = "x${Q()}";
        } catch (e) {
            caught = e;
        }
        var joined = "";
        try {
            var t = [Q()].join("-");
        } catch (e) {
            joined = e;
        }
    )src";
    const auto r   = run_with_natives(vm, src, {});
    ASSERT_TRUE(r.result.has_value());
    const auto caught = find_global(*r.module, vm.gc(), "caught");
    const auto joined = find_global(*r.module, vm.gc(), "joined");
    ASSERT_TRUE(caught.has_value());
    ASSERT_TRUE(joined.has_value());
    // BUILD_STRING 失败臂与 join 抄离渲染失败:载荷经 unwind 派发,catch 绑抛出原值。
    EXPECT_EQ(caught->as_obj()->as<ObjString>()->view(), "render boom");
    EXPECT_EQ(joined->as_obj()->as<ObjString>()->view(), "render boom");
}
