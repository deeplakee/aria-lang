#ifndef ARIA_PARSER_HPP
#define ARIA_PARSER_HPP

#include "common.hpp"
#include "compile/Token.hpp"
#include "compile/ast.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"

namespace aria {

    // 递归下降语法分析器：把 Lexer 产出的 Token 流构造为 AST（ProgramNode）。文法来源：docs/grammar.txt。各解析函数与
    // 非终结符一一对应，命名一致（program/declaration/statement/expression/assignment/logic_or/.../primary/pattern
    // 等）。错误处理（与 Lexer 风格一致，见 AGENTS.md「四条错误通道」之 1 与 3）：
    //   - 内部用 AriaCompileException（C++ 异常）在递归下降深处传播语法错误--error()/expect() 抛出，沿 C++ 调用栈上抛。
    //   - 在 declaration() 层捕获：记入 errors_、做 panic-mode 同步（synchronize）后继续解析下一条声明/语句，从而像
    //     Lexer 一样收集多个错误。
    //   - 边界 parse() 返回 Result<UPtr<ProgramNode>, List<Error>>：有任何错误 -> 返回错误集合（丢弃部分 AST）；无错
    //     -> 返回完整程序。生命周期：Parser 不持有 SourceFile；源文件位置由各 Token 携带的 SourceLoc（含 SourceFile*）
    //     提供，故调用方须保证 SourceFile 在解析期间存活（同 Lexer 的生命周期约束）。
    class Parser {
    public:
        // 静态服务入口：解析 token 流为 Program AST，返回程序或错误集合。
        // 内部一次性构造（私有构造），无空态、无复用。
        static Result<UPtr<ProgramNode>, List<Error>> parse(List<Token> tokens);

    private:
        // 一次性实例：构造即注入 token 流，仅静态入口 parse 构造。
        explicit Parser(List<Token> tokens) noexcept;

        List<Token> tokens_;
        usize       pos_;
        List<Error> errors_;

        // token 游标辅助
        // 越界（ahead 超出末尾）返回末尾 Eof token，安全。
        [[nodiscard]]
        const Token& peek(usize ahead = 0) const noexcept;

        [[nodiscard]]
        bool check(TokenType t) const noexcept;

        [[nodiscard]]
        bool check_next(TokenType t) const noexcept;

        // 若当前 token 为 t 则消费它并返回 true；否则不动，返回 false（= check + advance）。
        bool match(TokenType t) noexcept;

        [[nodiscard]]
        bool is_at_end() const noexcept;

        // 推进游标并返回刚消费的 token（已在末尾时不推进）。
        const Token& advance() noexcept;

        // 刚消费的 token（pos_>0 时有效）。
        [[nodiscard]]
        const Token& previous() const noexcept;

        // 错误与期待
        // 以当前 token 位置构造 Error 并抛 AriaCompileException（[[noreturn]]），
        // 由 declaration() 捕获。EOF 时改报 UnexpectedEof。
        // 形态与 CodeGen::fail 同构（变参 std::format_string，格式化归报错入口）。
        template<typename... Args>
        [[noreturn]]
        void error(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{
                    Error::from_detail(code, peek().loc(), std::format(fmt, std::forward<Args>(args)...))};
        }

        // 期待特定 token：匹配则消费并返回；否则报 ExpectedToken/UnexpectedEof 抛出。
        const Token& expect(TokenType t, StringView what);

        // 期待标识符：返回其 lexeme 文本；否则报 ExpectedIdentifier/UnexpectedEof 抛出。
        [[nodiscard]]
        String expect_identifier();

        // panic-mode 同步：跳过当前 token 后推进到下一条语句/声明边界
        // （';' 之后，或 fun/def/var/if/while/for/.../'{' 等起首关键字）。
        void synchronize();

        // 顶层与声明
        [[nodiscard]]
        UPtr<ProgramNode> program();

        // 错误恢复点：try/catch AriaCompileException，记错 + synchronize + 返回 nullptr。
        [[nodiscard]]
        UPtr<StmtNode> declaration();

        // kind 按出现位置定：语句位 funDecl -> Function；def 体 funDecl -> StaticMethod（裸方法
        // 不经此，def_decl 直接构造 Method/InitMethod 节点）。
        [[nodiscard]]
        UPtr<FunDeclNode> fun_decl(FnKind kind);

        [[nodiscard]]
        List<Param> params();

        // def 声明/嵌套类成员：defDecl -> "def" identifier (":" identifier)? "{" member* "}"。
        // is_member 由调用位烙定：语句位声明(false)/def 体成员分派递归进入的嵌套类(true)；成员
        // 体再遇 def 同法递归，嵌套深度不限。
        [[nodiscard]]
        UPtr<DefDeclNode> def_decl(bool is_member);

        // def 体静态变量成员（memberVar -> "var" identifier ("=" expression)? ";"）：单标识符绑定，
        // 语句级 varDecl 的多绑定/解构 pattern 在成员位不收（成员是类对象上的具名槽，名字一等）。
        [[nodiscard]]
        UPtr<StaticVarMemberNode> member_var();

        [[nodiscard]]
        UPtr<VarDeclNode> var_decl();

        [[nodiscard]]
        VarBinding var_binding();

        [[nodiscard]]
        UPtr<StmtNode> statement();

        [[nodiscard]]
        UPtr<StmtNode> if_stmt();

        [[nodiscard]]
        UPtr<StmtNode> while_stmt();

        // for / for-in 消歧入口。
        [[nodiscard]]
        UPtr<StmtNode> for_or_for_in_stmt();

        [[nodiscard]]
        UPtr<StmtNode> break_stmt();

        [[nodiscard]]
        UPtr<StmtNode> continue_stmt();

        [[nodiscard]]
        UPtr<StmtNode> return_stmt();

        [[nodiscard]]
        UPtr<StmtNode> import_stmt();

        [[nodiscard]]
        UPtr<StmtNode> try_stmt();

        [[nodiscard]]
        UPtr<StmtNode> throw_stmt();

        [[nodiscard]]
        UPtr<StmtNode> match_stmt();

        // expression ";"（statement 默认分支，亦用于 forStmt 的 exprStmt init）。
        [[nodiscard]]
        UPtr<StmtNode> expression_stmt();

        [[nodiscard]]
        UPtr<BlockNode> block();

        // 表达式（优先级自低向高）
        [[nodiscard]]
        UPtr<ExprNode> expression();

        // 序列层：expression ("," expression)*，逗号最低优先级。单元素透明（直接返回内层节点）；
        // 仅用于其后不紧跟逗号分隔符的文法位，位次清单见 grammar.txt 说明区「序列表达式」。
        [[nodiscard]]
        UPtr<ExprNode> sequence();

        [[nodiscard]]
        UPtr<ExprNode> assignment();

        [[nodiscard]]
        UPtr<ExprNode> logic_or();

        [[nodiscard]]
        UPtr<ExprNode> logic_and();

        [[nodiscard]]
        UPtr<ExprNode> equality();

        [[nodiscard]]
        UPtr<ExprNode> comparison();

        [[nodiscard]]
        UPtr<ExprNode> range();

        [[nodiscard]]
        UPtr<ExprNode> term();

        [[nodiscard]]
        UPtr<ExprNode> factor();

        [[nodiscard]]
        UPtr<ExprNode> unary();

        // 后缀链：primary ( args | "." identifier | "[" expression "]" )*。
        [[nodiscard]]
        UPtr<ExprNode> value();

        [[nodiscard]]
        UPtr<ExprNode> primary();

        [[nodiscard]]
        List<UPtr<ExprNode>> args();

        [[nodiscard]]
        UPtr<ExprNode> list_expr();

        [[nodiscard]]
        UPtr<ExprNode> map_expr();

        // mapExpr 的单个键值对：expression ":" expression。
        [[nodiscard]]
        MapEntry parse_map_entry();

        [[nodiscard]]
        UPtr<ExprNode> if_expr();

        [[nodiscard]]
        UPtr<ExprNode> lambda_expr();

        [[nodiscard]]
        UPtr<ExprNode> match_expr();

        [[nodiscard]]
        MatchPattern match_pattern();

        [[nodiscard]]
        MatchArm match_arm();

        [[nodiscard]]
        MatchExprArm match_expr_arm();

        // 解构模式
        // pattern -> identifier | "_" | listPattern。
        [[nodiscard]]
        UPtr<PatternNode> pattern();

        [[nodiscard]]
        UPtr<ListPatternNode> list_pattern();

        // rest 模式："..." identifier，返回绑名模式节点（拒绝 ..._；与位置位同为模式节点，故绑定
        // 走同一 accept 路径）。
        [[nodiscard]]
        UPtr<IdentifierPatternNode> rest_pattern();

        // for / for-in 消歧与收尾
        // pos_ 位于 '(' 后首个 token；判定是否为 <pattern> "in"（identifier/"_" 紧跟 in，
        // 或 [...] 后跟 in）。in 非表达式运算符，故 <pattern> in 唯一标识 forIn。
        [[nodiscard]]
        bool looks_like_for_in() const noexcept;

        [[nodiscard]]
        UPtr<StmtNode> finish_for_in_stmt(SourceLoc loc);

        [[nodiscard]]
        UPtr<StmtNode> finish_for_stmt(SourceLoc loc, UPtr<StmtNode> init);
    };

} // namespace aria

#endif // ARIA_PARSER_HPP
