#include "compile/Compiler.hpp"

#include "compile/CodeGen.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"

namespace aria {

    Compiler::Compiler(GC& gc) noexcept : gc_{gc}, lexer_{}, parser_{} {}

    // 端到端：tokenize -> parse -> CodeGen::compile。三阶段首错即止--
    //   词法/语法阶段产 List<Error>（恢复式收集），取 errors_[0] 作首错；
    //   CodeGen 已是单 Error（首错抛 AriaCompileException，顶层 catch 翻译）。
    // source 是调用方传入的实际 SourceFile，本函数只经 Lexer 读其 content()，不拥有、不重建--
    // Token / AST 节点的 SourceLoc 指向 source（仅本函数内部消费）；Error 的位置在构造期烘焙、
    // 不持 source（见头注生命期契约）。
    // entry_name 透传给 CodeGen 作入口函数名（主入口 <main> / 导入 <module>）。
    Result<ObjFunction*, Error> Compiler::compile(SourceFile& source, ObjModule* module, const StringView entry_name) {
        // 1) 词法：SourceFile -> token 流（或 List<Error>，取首错）。
        auto lex = lexer_.tokenize(source);
        if (!lex.has_value()) {
            return std::unexpected(lex.error()[0]);
        }

        // 2) 语法：token 流 -> ProgramNode（或 List<Error>，取首错）。token 所有权移入 Parser。
        auto parse = parser_.parse(std::move(lex.value()));
        if (!parse.has_value()) {
            return std::unexpected(parse.error()[0]);
        }

        // 3) 代码生成：ProgramNode -> 入口 ObjFunction（或单 Error）。CodeGen 每次就地构造
        //    （一次性、状态局限单次编译），与 gc_ 同源；module 由 CodeGen::compile 内部 make_guard 根化。
        CodeGen codegen{gc_};
        return codegen.compile(*parse.value(), module, entry_name);
    }

} // namespace aria
