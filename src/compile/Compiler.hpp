#ifndef ARIA_COMPILER_HPP
#define ARIA_COMPILER_HPP

// 编译编排器：把一个实际的 SourceFile 经 Lexer -> Parser -> CodeGen 编进模块入口 ObjFunction，
// 收口「源文件 -> 可执行 ObjFunction」的端到端编译链路。
//
// 本类不做词法/语法/代码生成--三者各归 Lexer/Parser/CodeGen，本类只负责按序串联，并把三阶段的
// Result<..., List<Error>>（词法/语法，取 errors_[0]）/ Result<..., Error>（CodeGen 已是单 Error）统一
// 翻译为单个 Result<ObjFunction*, Error>（首错即止）。
//
// 设计要点：
//   - 吃实际的 SourceFile，不捏造源：compile() 入参为 SourceFile&（由调用方加载/构造的真实源文件--
//     磁盘文件经 SourceFile::from_path 读盘、REPL/嵌入由调用方按需构造）。本类不拥有、不重建源文件，
//     只读它的 content() 做词法。故无 source_ 成员、无 source_name 参数--源的身份（name/path）就是
//     SourceFile 自带的，错误渲染 "path:line:col" 取自实际源文件，货真价实。
//   - SourceFile 生命期契约：AST 节点持 SourceLoc（内含 SourceFile*），但 AST 仅在 compile() 内部消费、
//     不外返；编译失败返回的 Error 在构造期已把位置烘进自有 message_ 串、不持 SourceFile*。故 source
//     只须存活到 compile() 返回（烘焙在此时完成），返回的 Error / ObjFunction 均不依赖 source，调用方
//     可立即释放或 move 它（Error 已不指向其 content）。
//   - 可复用：Lexer / Parser 均按「空态可复用」设计（tokenize / parse 扫完即清空成员），作为本类成员
//     跨多次 compile() 复用（如 REPL 逐行重编译）。CodeGen 一次性、状态局限单次编译（持 UPtr<ModuleCtx>），
//     故每次 compile() 内就地构造。
//   - GC 同源：构造取 GC&（与 VM 同一 GC），编译期分配的 ObjFunction / ObjString 归此 GC、与后续 run()
//     同源。编译期 GC 已启用--CodeGen::compile 入口 make_guard(&module) 自守 module，故调用方无需为编译期
//     再守模块（module 须是 GC 管理的合法 ObjModule）。
//
// 与 CodeGen 的分工：CodeGen 是「AST -> CodeUnit（包在 ObjFunction）」代码生成器；Compiler 是
// 「SourceFile -> AST」(Lexer/Parser) + 「调 CodeGen 产出 ObjFunction」编排层。不做磁盘 I/O--加载源文件
// 是调用方（解释器入口/REPL）的职责，本类只消费已就位的 SourceFile。

#include "common.hpp"
#include "compile/Lexer.hpp"
#include "compile/Parser.hpp"
#include "error/Error.hpp"
#include "util/source_file.hpp"

namespace aria {

    class GC;
    class ObjFunction;
    class ObjModule;

    class Compiler {
    public:
        Compiler() = delete;

        // 以 VM 的 GC 构造（编译期分配的 ObjFunction / ObjString 归此 GC，与后续 run() 同源）。
        explicit Compiler(GC& gc) noexcept;

        Compiler(const Compiler&)                = delete;
        Compiler& operator=(const Compiler&)     = delete;
        Compiler(Compiler&&) noexcept            = delete;
        Compiler& operator=(Compiler&&) noexcept = delete;

        // 编译源文件 source 到模块 module 的入口 ObjFunction（arity 0、名 entry_name，模块体包装，已
        // module.set_entry）。
        //   - source 为调用方拥有/加载的实际源文件；本类只读其 content()，不拥有、不重建。source 只须
        //     存活到 compile() 返回（位置串在构造期烘焙完成），返回的 Error / ObjFunction 均不依赖 source。
        //   - module 须为 GC 管理的合法 ObjModule（编译期由 CodeGen::compile 内部 make_guard 根化，调用方无需再守）。
        //   - entry_name：入口函数名。主入口模块传 <main>（默认）；运行期导入模块传 <module>（由 VM 加载层
        //     调用时显式传入，区别于主入口）。
        // 成功返回入口 ObjFunction*（归属 gc，须在 gc 存活期间使用）；失败返回首错 Error。
        [[nodiscard]]
        Result<ObjFunction*, Error> compile(SourceFile& source, ObjModule& module, StringView entry_name = "<main>");

    private:
        GC& gc_;

        // 可复用词法/语法分析器（跨多次 compile() 复用，tokenize/parse 扫完即清空成员）。
        Lexer  lexer_;
        Parser parser_;
    };

} // namespace aria

#endif // ARIA_COMPILER_HPP
