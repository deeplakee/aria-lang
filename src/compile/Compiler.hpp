#ifndef ARIA_COMPILER_HPP
#define ARIA_COMPILER_HPP

// 编译编排器：把一个实际的 SourceFile 经 Lexer -> Parser -> CodeGen 编进模块入口 ObjFunction，收口「源文件 -> 可执行
// ObjFunction」的端到端链路（首错即止）。本类不做词法/语法/代码生成，只按序串联，把三阶段 Result<..., List<Error>> /
// Result<..., Error> 统一翻译为 Result<ObjFunction*, Error>。
//   - 吃实际的 SourceFile，不捏造源：只读调用方加载/构造的真实 SourceFile& 的 content() 做词法，不拥有不重建；错误渲染
//     "path:line:col" 取自实际源文件。
//   - 生命期：AST 仅 compile() 内部消费、不外返；失败返回的 Error 在构造期已把位置烘进自有 message_、不持 SourceFile*
//     ，故 source 只须存活到 compile() 返回，其后的 Error / ObjFunction 均不依赖它，调用方可立即释放或 move。
//   - 无状态：Lexer / Parser / CodeGen 与 compile 均为静态入口，无跨 compile() 复用；gc 参数是编译期分配的 ObjFunction
//     / ObjString 归属（与后续 run() 同源，CodeGen::compile 入口自守 module）。

#include "aria.hpp"
#include "common.hpp"
#include "error/Error.hpp"
#include "util/source_file.hpp"

namespace aria {

    class GC;
    class ObjFunction;
    class ObjModule;

    class Compiler {
    public:
        // 静态服务入口：编译源文件 source 到模块 module 的入口 ObjFunction（arity 0、名 entry_name，
        // 已 module.set_entry）。entry_name 无默认值（调用方意图显式）：主入口传 kMainEntryName
        // （<main>），运行期导入模块传 kModuleEntryName（<module>）。成功返回入口 ObjFunction*（归属
        // gc，须在 gc 存活期间使用）；失败返回首错 Error。
        static Result<ObjFunction*, Error> compile(GC& gc, SourceFile& source, ObjModule* module,
                                                   StringView entry_name);
    };

} // namespace aria

#endif // ARIA_COMPILER_HPP
