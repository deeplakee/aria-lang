#ifndef ARIA_ARIA_HPP
#define ARIA_ARIA_HPP

#include "common.hpp"

namespace aria {

    // 项目级定义头:收拢语言/产品层的固有命名事实,作为单一事实源防各层字面量漂移。
    // 分工:基础设施配置归 common.hpp(宏/NaN-boxing 开关),通用辅助工具归 util/,
    // 领域对象模型归各模块头(如 object/ObjModule.hpp);跨层共享的项目级命名落本头。
    // 仅依赖 common.hpp,任何层(含 util)均可无分层顾虑引用。

    // 模块源文件扩展名:「一个源文件 = 一个模块」(见 ObjModule)。
    // AriaVM resolve_module 剥 import spec 末段可选后缀、查找时统一补回;
    // ObjModule::abs_path 以之合成模块表键(dir_ + "/" + name_ + 本后缀)。
    constexpr StringView kAriaExtension = ".aria";

    // VM 合成实体保留名(尖括号家族):「<」/「>」不是合法标识符字符,用户代码无法产生含它们的
    // 函数名/模块名,故尖括号名可安全标记 VM 合成实体。家族:kModuleEntryName(模块体入口函数)、
    // kMainEntryName(主入口函数)、kScriptModuleName(求值串合成模块)、kReplModuleName(REPL 合成
    // 模块)、kAnonymousName(匿名函数)。

    // 模块体入口函数名:IMPORT 加载的模块体 ObjFunction 之名。runtime 经 Compiler 以此名
    // 编译模块体,RETURN 处按名回查识别模块体帧(弹弃返回值、改压模块对象)--
    // 以名字这一函数固有属性取代帧上 is_module_body 标志位,无进帧置位/复位与槽复用残留之虞。
    constexpr StringView kModuleEntryName = "<module>";

    // 主入口函数名:源文件/求值串顶层代码编进的 ObjFunction 之名(Compiler/CodeGen 的默认参数);
    // RETURN 顶层帧返回即程序结果。
    constexpr StringView kMainEntryName = "<main>";

    // 求值串合成模块名(--eval / interpret_from_src):无文件身份的源,模块与 SourceFile 名共用。
    constexpr StringView kScriptModuleName = "<script>";

    // REPL 合成模块名:逐行复用单模块使顶层 var 跨行持久;SourceFile 名同步用之。
    constexpr StringView kReplModuleName = "<repl>";

    // 匿名函数名:lambda(ObjFunction)与匿名原生函数(ObjNativeFn)共用;
    // compile_function 据名判定「lambda 留栈作表达式值不绑定」,具名/匿名发射分岔依赖此名。
    constexpr StringView kAnonymousName = "<anonymous>";

    // ---- 产品标识与部署约定 ----

    // 产品名:CLI 程序名(util::Cli)与 REPL 提示符句柄(isocline)共用。
    constexpr StringView kProductName = "aria";

    // 语义化版本分量:版本的单一事实源,**不设字符串常量副本**(派生串在 --version 消费点
    // 就地 format,字符串与分量两处维护必漂移);代码内版本判定直接比较分量。
    constexpr i32 kVersionMajor = 0;
    constexpr i32 kVersionMinor = 1;
    constexpr i32 kVersionPatch = 0;

    // 内建 stdlib 源根的安装约定:相对可执行文件目录的路径(<exe_dir>/../share/aria/lib,确切路径
    // 待定),运行时经 fs::program_dir 推导、weakly_canonical 规范化后播种 source_roots_[1]。
    constexpr StringView kStdlibRelPath = "../share/aria/lib";

} // namespace aria

#endif // ARIA_ARIA_HPP
