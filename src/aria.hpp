#ifndef ARIA_ARIA_HPP
#define ARIA_ARIA_HPP

#include "common.hpp"

namespace aria {

    // 项目级定义头:收拢语言/产品层的固有命名,防止各层字面量漂移。
    // 仅依赖 common.hpp,任何层(含 util)均可无分层顾虑引用。

    // 语言值域

    // 整数(i48)值域:NaN-boxing 的 48 位尾部即语言 int 域;越域报错不静默截断。
    constexpr i64 kIntMin = -(static_cast<i64>(1) << 47);
    constexpr i64 kIntMax = (static_cast<i64>(1) << 47) - 1;

    // 模块源文件扩展名:一个源文件 = 一个模块。
    constexpr StringView kAriaExtension = ".aria";

    // VM 合成实体保留名:「<」/「>」非标识符字符,用户代码拼不出,故可安全标记 VM 合成实体。

    constexpr StringView kModuleEntryName  = "<module>";    // 模块体入口函数名。
    constexpr StringView kMainEntryName    = "<main>";      // 主入口函数名(源文件/求值串顶层代码)。
    constexpr StringView kScriptModuleName = "<script>";    // 求值串合成模块名。
    constexpr StringView kReplModuleName   = "<repl>";      // REPL 模块名(复用单模块,顶层 var 跨行持久)。
    constexpr StringView kAnonymousName    = "<anonymous>"; // 匿名函数名(lambda 与匿名原生函数共用)。
    constexpr StringView kInitName         = "init";        // 实例方法构造角色名(烙 FnKind::InitMethod)。

    // 产品标识与部署约定

    constexpr StringView kProductName = "aria"; // 产品名(CLI 程序名与 REPL 提示符)。

    // 语义化版本分量:不设字符串常量副本(两处维护必漂移,消费点就地 format)。
    constexpr i32 kVersionMajor = 0;
    constexpr i32 kVersionMinor = 1;
    constexpr i32 kVersionPatch = 0;

    // 内建 stdlib 源根:相对可执行文件目录的安装约定。
    constexpr StringView kStdlibRelPath = "../share/aria/lib";

} // namespace aria

#endif // ARIA_ARIA_HPP
