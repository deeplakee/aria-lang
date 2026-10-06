#ifndef ARIA_FNKIND_HPP
#define ARIA_FNKIND_HPP

// 函数种类独立小头：AST 侧与编译执行侧共用，避免 Ast.hpp <-> FunctionCtx.hpp 互相牵连。
#include "common.hpp"
#include "type.hpp"

namespace aria {

    // 函数种类。实例方法族（Method/InitMethod）帧槽 0 = 具名局部 this；init 返回尾即 this（构造角色）；
    // ModuleEntry 返回尾即模块对象（IMPORT 栈效应的兑现），带值 return 编译期拒绝。
    enum class FnKind : u8 { Function, Lambda, StaticMethod, Method, InitMethod, ModuleEntry };

    // 实例方法族判定；InitMethod 仅隐式返回尾特判，其余编译语义与 Method 全同。
    constexpr bool is_method(const FnKind kind) noexcept {
        return kind == FnKind::Method || kind == FnKind::InitMethod;
    }

    // FnKind 可读名。
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
