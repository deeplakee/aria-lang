#ifndef ARIA_FNKIND_HPP
#define ARIA_FNKIND_HPP

// 函数种类独立小头：AST 侧与编译执行侧共用，避免 ast.hpp <-> FunctionCtx.hpp 互相牵连。
#include "common.hpp"
#include "type.hpp"

namespace aria {

    // 函数种类:普通函数(语句位 fun 声明)/ lambda(匿名表达式,名字恒为 kAnonymousName)/
    // 静态方法(def 体内 fun 成员,无 this 绑定)/ 实例方法(def 体内裸方法,帧槽 0 = this)/
    // init 方法(名为 kInitName 的裸方法,构造角色--返回尾返回 this,实例化不变式 Foo() 得实例)/
    // 模块入口(主脚本 <main> 与导入模块 <module> 的顶层体,ModuleCtx 构造直接烙定):返回值恒为
    // 模块对象(IMPORT 栈效应的兑现),体顶层带值 return 编译期拒绝(见 visitReturnStmtNode)。
    // AST 侧:parser 按出现位置(及成员名)烙进 FunDeclNode,lambda 经 visitLambdaExprNode 直传
    // (入口不经 parser,由 ModuleCtx 构造);编译侧:随 FunctionCtx 挂 ctx 链--名字绑定(Function
    // 绑全局/局部,其余留栈不绑定)、隐式返回尾(InitMethod 返 this、ModuleEntry 返模块对象、其余
    // 返 nil)、slot 0 形态与 this/super 判据(实例方法族,见 is_method)都消费 kind。
    enum class FnKind : u8 { Function, Lambda, StaticMethod, Method, InitMethod, ModuleEntry };

    // 实例方法族:槽 0 为具名局部 this、承载 this/super 判据的帧形态。InitMethod 仅返回尾特判,
    // 其余编译语义(帧形/注册 MAKE_METHOD/super 合法性)与 Method 全同。
    constexpr bool is_method(const FnKind kind) noexcept {
        return kind == FnKind::Method || kind == FnKind::InitMethod;
    }

    // FnKind 可读名(如 "Function");switch 不加 default,新增枚举值由 -Wswitch 提示遗漏,
    // UNREACHABLE 收尾(同 Op::to_string/TokenType 先例)。
    [[nodiscard]]
    constexpr StringView to_string(const FnKind kind) noexcept {
        switch (kind) {
            case FnKind::Function:
                return "Function";
            case FnKind::Lambda:
                return "Lambda";
            case FnKind::StaticMethod:
                return "StaticMethod";
            case FnKind::Method:
                return "Method";
            case FnKind::InitMethod:
                return "InitMethod";
            case FnKind::ModuleEntry:
                return "ModuleEntry";
        }
        UNREACHABLE();
    }

} // namespace aria

#endif // ARIA_FNKIND_HPP
