#include <gtest/gtest.h>

#include <limits>

#include "aria.hpp"
#include "compile/Compiler.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "type.hpp"
#include "util/source_file.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::Compiler;
using aria::Error;
using aria::ErrorCode;
using aria::GC;
using aria::kModuleEntryName;
using aria::List;
using aria::NativeFn;
using aria::new_module;
using aria::new_native_fn;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjClosure;
using aria::ObjException;
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

    // 拷出调用区实参(首参之后的全部)入 args 缓冲:调用区视角指进值栈,重入前先抄离 --
    // args 别名契约的消费侧示范形态,跨重入期间值栈扩容无牵连。returns u8 argc。
    u8 copy_call_args(Span<Value> rest, Value* args) {
        const u8 argc = static_cast<u8>(rest.size());
        for (usize i = 0; i < argc; ++i) {
            args[i] = rest[i];
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
            const auto kguard = gc.make_guard(key); // key 白色须跨 new_native_fn 的 GC 点存活
            module->globals().set(Value::from_obj(key), Value::from_obj(new_native_fn(gc, key, fn)));
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

    // 重入口驱动探针宿主(全走公共面:ctx 服务面与重入口 execute_closure/execute_method;定义
    // 全收于此)。
    struct TestProbe {
        // exec_closure(fn, a1..aN):同步执行闭包 fn(execute_closure 形态),fn 须闭包值;失败
        // 载荷已在寄存器按 bool 契约返 false,成功返回值写槽 0。
        static bool exec_closure_native(AriaVM& vm, Span<Value> slots) {
            if (slots.size() < 2) {
                return vm.arity_error(slots.size() - 1, 1);
            }
            if (!slots[1].is_obj() || !slots[1].as_obj()->is<ObjClosure>()) {
                return vm.fail(ErrorCode::TypeMismatch, "exec_closure expects a closure as its first argument");
            }
            Value      args[std::numeric_limits<u8>::max()];
            const u8   argc   = copy_call_args({slots.data() + 2, slots.size() - 2}, args);
            const auto ctx    = vm.current_context();
            const auto result = ctx->execute_closure(slots[1].as_obj()->as<ObjClosure>(), {args, argc});
            if (!result.has_value()) {
                return false; // 载荷在寄存器,交外层续搜
            }
            slots[0] = *result;
            return true;
        }

        // exec_method(m, a1..aN):同步执行绑定方法 m(execute_method 形态,receiver 作 this),m 须
        // 绑定方法值(脚本 obj.name 的值即此形态)。余同 exec_closure。
        static bool exec_method_native(AriaVM& vm, Span<Value> slots) {
            if (slots.size() < 2) {
                return vm.arity_error(slots.size() - 1, 1);
            }
            if (!slots[1].is_obj() || !slots[1].as_obj()->is<ObjBoundMethod>()) {
                return vm.fail(ErrorCode::TypeMismatch, "exec_method expects a bound method as its first argument");
            }
            const auto bound = slots[1].as_obj()->as<ObjBoundMethod>();
            Value      args[std::numeric_limits<u8>::max()];
            const u8   argc   = copy_call_args({slots.data() + 2, slots.size() - 2}, args);
            const auto method = bound->method().as_obj()->as<ObjClosure>();
            const auto ctx    = vm.current_context();
            const auto result = ctx->execute_method(bound->receiver(), method, {args, argc});
            if (!result.has_value()) {
                return false; // 载荷在寄存器,交外层续搜
            }
            slots[0] = *result;
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
        fun yfn() { return coroutine.yield(); }
        fun wrapper() { return exec_closure(yfn); }
        var co = coroutine.create(wrapper);
        var caught = nil;
        try {
            coroutine.resume(co);
        } catch (e) {
            caught = e;
        }
        var st = coroutine.status(co);
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
        var inner = coroutine.create(co_body);
        fun mult() { return coroutine.resume(inner); }
        fun wrapper() { return exec_closure(mult); }
        var outer = coroutine.create(wrapper);
        var v = coroutine.resume(outer);
    )";
    const auto r   = run_with_natives(vm, src, {{"exec_closure", &aria::TestProbe::exec_closure_native}});
    ASSERT_TRUE(r.result.has_value());
    const auto v = find_global(*r.module, vm.gc(), "v");
    ASSERT_TRUE(v.has_value());
    // 嵌套执行内 resume 合法:inner 完成的 42 穿 exec_closure 回 wrapper 再回 outer。
    EXPECT_EQ(v->as_int(), 42);
}
