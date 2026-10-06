#include "compile/Compiler.hpp"

#include "compile/CodeGen.hpp"
#include "compile/Lexer.hpp"
#include "compile/Parser.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"

namespace aria {

    Result<ObjFunction*, Error> Compiler::compile(GC& gc, SourceFile& source, ObjModule* module,
                                                  const StringView entry_name) {
        auto tokens = Lexer::tokenize(source);
        if (!tokens) {
            return std::unexpected(std::move(tokens.error()));
        }

        // token 表存活至 parse 返回：token 的 lexeme/内层视图借自源缓冲，Parser 只在解析期读
        // （AST 节点持视图，存活至编译结束，仍在 source 的存活期内）。
        auto ast = Parser::parse(*tokens, source);
        if (!ast) {
            return std::unexpected(std::move(ast.error()[0]));
        }

        tokens->clear();

        return CodeGen::compile(gc, **ast, module, entry_name);
    }

} // namespace aria
