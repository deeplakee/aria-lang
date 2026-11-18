#ifndef ARIA_COMPILER_HPP
#define ARIA_COMPILER_HPP

// 编排词法、语法和代码生成，返回首错；source 只须存活到 compile 返回。

#include "aria.hpp"
#include "common.hpp"
#include "error/Error.hpp"
#include "util/source_file.hpp"

namespace aria {

    class GC;
    class ObjFnProto;
    class ObjModule;

    class Compiler {
    public:
        // 成功后入口原型已挂到 module，归 gc 管理。
        static Result<ObjFnProto*, Error> compile(GC& gc, SourceFile& source, ObjModule* module, StringView entry_name);
    };

} // namespace aria

#endif // ARIA_COMPILER_HPP
