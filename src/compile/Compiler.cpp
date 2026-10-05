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
        auto stream = Lexer::tokenize(source);
        if (!stream) {
            return std::unexpected(std::move(stream.error()));
        }

        // token 流存活至 parse 返回：token 的 lexeme/内层视图借自源缓冲，Parser 只在解析期读
        // （AST 节点持视图，存活至编译结束，仍在 source 的存活期内）。
        auto ast = Parser::parse(std::move(stream->tokens));
        if (!ast) {
            return std::unexpected(std::move(ast.error()[0]));
        }

        return CodeGen::compile(gc, **ast, module, entry_name);
    }

} // namespace aria
