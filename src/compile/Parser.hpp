#ifndef ARIA_PARSER_HPP
#define ARIA_PARSER_HPP

#include "common.hpp"
#include "compile/Ast.hpp"
#include "compile/Token.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"
#include "memory/AstArena.hpp"
#include "util/source_file.hpp"

namespace aria {

    using src::SourceFile;
    using src::SourceLoc;

    // 递归下降并收集多个语法错误；token 和源缓冲须在解析期存活。
    // AST 借用调用方 arena，arena 与源缓冲须覆盖 AST 的消费期。
    class Parser {
    public:
        static Result<ProgramNode*, List<Error>> parse(AstArena& arena, SourceFile& source, List<Token>& tokens);

    private:
        explicit Parser(AstArena& arena, SourceFile& source, List<Token>& tokens) noexcept;

        AstArena&    arena_;
        SourceFile&  source_;
        usize        pos_;
        List<Token>& tokens_;
        List<Error>  errors_;

        // 越界前瞻返回末尾 Eof token。
        [[nodiscard]]
        const Token& peek(usize ahead = 0) const noexcept;

        [[nodiscard]]
        bool check(TokenType t) const noexcept;

        [[nodiscard]]
        bool check_next(TokenType t) const noexcept;

        bool match(TokenType t) noexcept;

        [[nodiscard]]
        bool is_at_end() const noexcept;

        const Token& advance() noexcept;

        [[nodiscard]]
        const Token& previous() const noexcept;

        [[nodiscard]]
        SourceLoc loc_of(const Token& token) const noexcept {
            const auto base = source_.content().data();
            const auto data = token.lexeme().data();
            ASSERT(data >= base && data <= base + source_.content().size(), "lexeme not within source buffer");
            return SourceLoc{&source_, static_cast<u32>(data - base)};
        }

        template<typename... Args>
        [[noreturn]]
        void error(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{
                    Error::from_detail(code, loc_of(peek()), std::format(fmt, std::forward<Args>(args)...))};
        }

        const Token& expect(TokenType t, StringView what);

        [[nodiscard]]
        StringView expect_identifier();

        // panic-mode 同步：跳过当前 token 后推进到下一条语句/声明边界。
        void synchronize();

        [[nodiscard]]
        StringLiteralNode* make_string_literal(const Token& token) const;

        // 顶层与声明
        [[nodiscard]]
        ProgramNode* program();

        // 声明层捕获语法错误并同步到下一条语句边界。
        [[nodiscard]]
        StmtNode* declaration();

        [[nodiscard]]
        FunDeclNode* fun_decl(FnKind kind);

        [[nodiscard]]
        Span<Param> params();

        [[nodiscard]]
        DefDeclNode* def_decl(bool is_member);

        [[nodiscard]]
        StaticVarMemberNode* member_var();

        [[nodiscard]]
        VarDeclNode* var_decl();

        [[nodiscard]]
        VarBinding var_binding();

        [[nodiscard]]
        StmtNode* statement();

        [[nodiscard]]
        StmtNode* if_stmt();

        [[nodiscard]]
        StmtNode* while_stmt();

        [[nodiscard]]
        StmtNode* for_or_for_in_stmt();

        [[nodiscard]]
        StmtNode* break_stmt();

        [[nodiscard]]
        StmtNode* continue_stmt();

        [[nodiscard]]
        StmtNode* return_stmt();

        [[nodiscard]]
        StmtNode* import_stmt();

        [[nodiscard]]
        StmtNode* try_stmt();

        [[nodiscard]]
        StmtNode* throw_stmt();

        [[nodiscard]]
        StmtNode* match_stmt();

        [[nodiscard]]
        StmtNode* expression_stmt();

        [[nodiscard]]
        BlockNode* block();

        // 表达式解析链（优先级自低向高）
        [[nodiscard]]
        ExprNode* expression();

        // 序列层：expression ("," expression)*。逗号最低优先级，单元素透明（直接返回内层节点），
        // 仅用于其后不紧跟逗号分隔符的文法位。
        [[nodiscard]]
        ExprNode* sequence();

        [[nodiscard]]
        ExprNode* assignment();

        // '[' 预扫：pos_ 停在 '[' 时判配对 ']' 之后是否紧跟 '='（解构赋值路由提示），只读不推进游标。
        [[nodiscard]]
        bool bracket_pair_followed_by_equal() const noexcept;

        [[nodiscard]]
        ExprNode* logic_or();

        [[nodiscard]]
        ExprNode* logic_and();

        [[nodiscard]]
        ExprNode* bit_or();

        [[nodiscard]]
        ExprNode* bit_xor();

        [[nodiscard]]
        ExprNode* bit_and();

        [[nodiscard]]
        ExprNode* equality();

        [[nodiscard]]
        ExprNode* comparison();

        [[nodiscard]]
        ExprNode* range();

        [[nodiscard]]
        ExprNode* term();

        [[nodiscard]]
        ExprNode* shift();

        [[nodiscard]]
        ExprNode* factor();

        [[nodiscard]]
        ExprNode* unary();

        // 后缀链：primary ( args | "." identifier | "[" expression "]" )*。
        [[nodiscard]]
        ExprNode* value();

        [[nodiscard]]
        ExprNode* primary();

        [[nodiscard]]
        ExprNode* interp_string();

        [[nodiscard]]
        Span<ExprNode*> args();

        [[nodiscard]]
        ExprNode* list_expr();

        [[nodiscard]]
        ExprNode* map_expr();

        [[nodiscard]]
        MapEntry parse_map_entry();

        [[nodiscard]]
        ExprNode* if_expr();

        [[nodiscard]]
        ExprNode* lambda_expr();

        [[nodiscard]]
        ExprNode* match_expr();

        [[nodiscard]]
        MatchPattern match_pattern();

        [[nodiscard]]
        MatchArm match_arm();

        [[nodiscard]]
        MatchExprArm match_expr_arm();

        [[nodiscard]]
        PatternNode* pattern();

        [[nodiscard]]
        ListPatternNode* list_pattern();

        [[nodiscard]]
        IdentifierPatternNode* rest_pattern();

        // for / for-in 消歧：pos_ 位于 '(' 后首个 token，判 <pattern> "in"
        // （identifier/"_" 或 [...] 后跟 in）。in 非表达式运算符，故此形唯一标识 forIn。
        [[nodiscard]]
        bool looks_like_for_in() const noexcept;

        [[nodiscard]]
        StmtNode* finish_for_in_stmt(SourceLoc loc);

        [[nodiscard]]
        StmtNode* finish_for_stmt(SourceLoc loc, StmtNode* init);
    };

} // namespace aria

#endif // ARIA_PARSER_HPP
