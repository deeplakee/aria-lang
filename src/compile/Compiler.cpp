#include "compile/Compiler.hpp"

#include "compile/CodeGen.hpp"
#include "compile/Lexer.hpp"
#include "compile/Parser.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"

namespace aria {

    // 端到端：tokenize -> parse -> CodeGen::compile，各阶段首错即止（契约见 Compiler.hpp compile 注）。
    Result<ObjFunction*, Error> Compiler::compile(GC& gc, SourceFile& source, ObjModule* module,
                                                  const StringView entry_name) {
        // 1) 词法：SourceFile -> token 流（或 List<Error>，取首错）。
        auto tokens = Lexer::tokenize(source);
        if (!tokens) {
            return std::unexpected(std::move(tokens.error()[0]));
        }

        // 2) 语法：token 流 -> ProgramNode（或 List<Error>，取首错）。
        auto ast = Parser::parse(std::move(*tokens));
        if (!ast) {
            return std::unexpected(std::move(ast.error()[0]));
        }

        // 3) 代码生成：AST -> 入口 ObjFunction（或单 Error）。
        return CodeGen::compile(gc, **ast, module, entry_name);
    }

} // namespace aria
