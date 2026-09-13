#include "compile/Compiler.hpp"

#include "compile/CodeGen.hpp"
#include "compile/Lexer.hpp"
#include "compile/Parser.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"

namespace aria {

    Compiler::Compiler(GC& gc) noexcept : gc_{gc} {}

    // 端到端：tokenize -> parse -> CodeGen::compile，各阶段首错即止（契约见 Compiler.hpp compile 注）。
    Result<ObjFunction*, Error> Compiler::compile(SourceFile& source, ObjModule* module, const StringView entry_name) {
        // 1) 词法：SourceFile -> token 流（或 List<Error>，取首错）。
        Lexer lexer;
        auto  lex = lexer.tokenize(source);
        if (!lex) {
            return std::unexpected(std::move(lex.error()[0]));
        }

        // 2) 语法：token 流 -> ProgramNode（或 List<Error>，取首错）。
        Parser parser;
        auto   parse = parser.parse(std::move(*lex));
        if (!parse) {
            return std::unexpected(std::move(parse.error()[0]));
        }

        // 3) 代码生成：ProgramNode -> 入口 ObjFunction（或单 Error）。
        auto    program = std::move(*parse);
        CodeGen codegen{gc_};
        return codegen.compile(*program, module, entry_name);
    }

} // namespace aria
