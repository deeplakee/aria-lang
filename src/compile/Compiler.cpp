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
            return std::unexpected(std::move(tokens.error()[0]));
        }

        auto ast = Parser::parse(std::move(*tokens));
        if (!ast) {
            return std::unexpected(std::move(ast.error()[0]));
        }

        return CodeGen::compile(gc, **ast, module, entry_name);
    }

} // namespace aria
