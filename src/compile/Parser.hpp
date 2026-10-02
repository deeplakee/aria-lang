#ifndef ARIA_PARSER_HPP
#define ARIA_PARSER_HPP

#include "common.hpp"
#include "compile/Token.hpp"
#include "compile/ast.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"

namespace aria {

    // 递归下降语法分析器：Token 流 -> AST（ProgramNode），解析函数与非终结符一一对应。
    // 语法错误在递归深处经 error()/expect() 抛 AriaCompileException，declaration() 捕获记账后
    // synchronize 继续，多错收集；SourceFile 不持有，存活须覆盖解析期（SourceLoc 内嵌于 Token）。
    class Parser {
    public:
        static Result<UPtr<ProgramNode>, List<Error>> parse(List<Token> tokens);

    private:
        explicit Parser(List<Token> tokens) noexcept;

        List<Token> tokens_;
        usize       pos_;
        List<Error> errors_;

        // token 游标辅助；越界（ahead 超出末尾）返回末尾 Eof token。
        [[nodiscard]]
        const Token& peek(usize ahead = 0) const noexcept;

        [[nodiscard]]
        bool check(TokenType t) const noexcept;

        [[nodiscard]]
        bool check_next(TokenType t) const noexcept;

        bool match(TokenType t) noexcept;

        [[nodiscard]]
        bool is_at_end() const noexcept;

        // 推进游标并返回刚消费的 token（已在末尾时不推进）。
        const Token& advance() noexcept;

        // 刚消费的 token（pos_>0 时有效）。
        [[nodiscard]]
        const Token& previous() const noexcept;

        // 以当前 token 位置报错并抛 AriaCompileException（声明层捕获）；EOF 改报 UnexpectedEof。
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

        // panic-mode 同步：跳过当前 token 后推进到下一条语句/声明边界。
        void synchronize();

        // 顶层与声明
        [[nodiscard]]
        UPtr<ProgramNode> program();

        // 错误恢复点：try/catch AriaCompileException，记错 + synchronize + 返回 nullptr。
        [[nodiscard]]
        UPtr<StmtNode> declaration();

        // kind 由调用位定：语句位 funDecl -> Function，def 体 funDecl -> StaticMethod（裸方法
        // 不经此，def_decl 直接构造 Method/InitMethod 节点）。
        [[nodiscard]]
        UPtr<FunDeclNode> fun_decl(FnKind kind);

        [[nodiscard]]
        List<Param> params();

        // def 声明或嵌套类成员，is_member 由调用位烙定；成员体再遇 def 同法递归，嵌套深度不限。
        [[nodiscard]]
        UPtr<DefDeclNode> def_decl(bool is_member);

        // def 体静态变量成员：单 identifier 绑定，多绑定/解构 pattern 在成员位不收。
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

        // 表达式解析链（优先级自低向高）
        [[nodiscard]]
        UPtr<ExprNode> expression();

        // 序列层：expression ("," expression)*。逗号最低优先级，单元素透明（直接返回内层节点），
        // 仅用于其后不紧跟逗号分隔符的文法位。
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

        // pattern -> identifier | "_" | listPattern。
        [[nodiscard]]
        UPtr<PatternNode> pattern();

        [[nodiscard]]
        UPtr<ListPatternNode> list_pattern();

        // rest 模式："..." identifier，返回绑名模式节点（拒绝 "..._"）。
        [[nodiscard]]
        UPtr<IdentifierPatternNode> rest_pattern();

        // for / for-in 消歧：pos_ 位于 '(' 后首个 token，判 <pattern> "in"
        // （identifier/"_" 或 [...] 后跟 in）。in 非表达式运算符，故此形唯一标识 forIn。
        [[nodiscard]]
        bool looks_like_for_in() const noexcept;

        [[nodiscard]]
        UPtr<StmtNode> finish_for_in_stmt(SourceLoc loc);

        [[nodiscard]]
        UPtr<StmtNode> finish_for_stmt(SourceLoc loc, UPtr<StmtNode> init);
    };

} // namespace aria

#endif // ARIA_PARSER_HPP
