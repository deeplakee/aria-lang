#ifndef ARIA_COMPILER_HPP
#define ARIA_COMPILER_HPP

// 编译编排器：SourceFile 经 Lexer -> Parser -> CodeGen 编为模块入口 ObjFunction，首错即止，各阶段
// Result 统一翻译为 Result<ObjFunction*, Error>。
// 生命期：source 只须存活到 compile() 返回 -- 失败 Error 构造期已烘焙 message_，不依赖 SourceFile。

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
        // 编译 source 到 module 的入口 ObjFunction（arity 0，名 entry_name 已 set_entry：主入口
        // kMainEntryName、导入模块 kModuleEntryName）。返回 ObjFunction* 归属 gc，须在 gc 存活期使用。
        static Result<ObjFunction*, Error> compile(GC& gc, SourceFile& source, ObjModule* module,
                                                   StringView entry_name);
    };

} // namespace aria

#endif // ARIA_COMPILER_HPP
