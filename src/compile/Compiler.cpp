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

        // stream.strings（token 字符串值的存储）须存活至 parse 返回：token 持入其中的视图只在解析期被读，
        // Parser 将其拷贝进 AST 自有 String 后即无依赖。
        auto ast = Parser::parse(std::move(stream->tokens));
        if (!ast) {
            return std::unexpected(std::move(ast.error()[0]));
        }

        return CodeGen::compile(gc, **ast, module, entry_name);
    }

} // namespace aria
