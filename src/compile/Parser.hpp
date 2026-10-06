#ifndef ARIA_PARSER_HPP
#define ARIA_PARSER_HPP

#include "common.hpp"
#include "compile/Token.hpp"
#include "compile/ast.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"
#include "memory/AstArena.hpp"
#include "util/source_file.hpp"

namespace aria {

    using src::SourceFile;
    using src::SourceLoc;

    // 递归下降语法分析器：Token 流 -> AST（ProgramNode），解析函数与非终结符一一对应。
    // 语法错误在递归深处经 error()/expect() 抛 AriaCompileException，declaration() 捕获记账后
    // synchronize 继续，多错收集；token 表借用不持有（存活须覆盖 parse 调用），SourceFile 同样
    // 不持有、存活须覆盖解析期（token 不携带位置，SourceLoc 由 lexeme 指针对源缓冲的偏移派生）。
    // 节点经调用方提供的 AstArena 分配、裸指针借用（arena 拥有内存，存活须覆盖整棵 AST 的
    // 消费期）；解析失败路径弃掉的节点不单独回收，随 arena 整批释放。
    class Parser {
    public:
        static Result<ProgramNode*, List<Error>> parse(AstArena& arena, List<Token>& tokens, SourceFile& source);

    private:
        explicit Parser(AstArena& arena, List<Token>& tokens, SourceFile& source) noexcept;

        AstArena&    arena_;
        List<Token>& tokens_;
        SourceFile&  source_;
        usize        pos_;
        List<Error>  errors_;

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

        // token 位置：lexeme 指针对源缓冲起点的偏移（EOF 锚在末尾哨兵位，偏移 = size()）。
        [[nodiscard]]
        SourceLoc loc_of(const Token& token) const noexcept {
            const auto base = source_.content().data();
            const auto data = token.lexeme().data();
            ASSERT(data >= base && data <= base + source_.content().size(), "lexeme not within source buffer");
            return SourceLoc{&source_, static_cast<u32>(data - base)};
        }

        // 以当前 token 位置报错并抛 AriaCompileException（声明层捕获）；EOF 改报 UnexpectedEof。
        template<typename... Args>
        [[noreturn]]
        void error(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{
                    Error::from_detail(code, loc_of(peek()), std::format(fmt, std::forward<Args>(args)...))};
        }

        // 期待特定 token：匹配则消费并返回；否则报 ExpectedToken/UnexpectedEof 抛出。
        const Token& expect(TokenType t, StringView what);

        // 期待标识符：返回其 lexeme 视图（借自源缓冲）；否则报 ExpectedIdentifier/UnexpectedEof 抛出。
        [[nodiscard]]
        StringView expect_identifier();

        // panic-mode 同步：跳过当前 token 后推进到下一条语句/声明边界。
        void synchronize();

        // 字符串族 token -> 字面节点：内层原文视图 + 消费形态随 token 走，primary / import /
        // 插值字面段三处共用。
        [[nodiscard]]
        StringLiteralNode* make_string_literal(const Token& token) const;

        // 顶层与声明
        [[nodiscard]]
        ProgramNode* program();

        // 错误恢复点：try/catch AriaCompileException，记错 + synchronize + 返回 nullptr。
        [[nodiscard]]
        StmtNode* declaration();

        // kind 由调用位定：语句位 funDecl -> Function，def 体 funDecl -> StaticMethod（裸方法
        // 不经此，def_decl 直接构造 Method/InitMethod 节点）。
        [[nodiscard]]
        FunDeclNode* fun_decl(FnKind kind);

        [[nodiscard]]
        List<Param> params();

        // def 声明或嵌套类成员，is_member 由调用位烙定；成员体再遇 def 同法递归，嵌套深度不限。
        [[nodiscard]]
        DefDeclNode* def_decl(bool is_member);

        // def 体静态变量成员：单 identifier 绑定，多绑定/解构 pattern 在成员位不收。
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

        // for / for-in 消歧入口。
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

        // expression ";"（statement 默认分支，亦用于 forStmt 的 exprStmt init）。
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

        [[nodiscard]]
        ExprNode* logic_or();

        [[nodiscard]]
        ExprNode* logic_and();

        [[nodiscard]]
        ExprNode* equality();

        [[nodiscard]]
        ExprNode* comparison();

        [[nodiscard]]
        ExprNode* range();

        [[nodiscard]]
        ExprNode* term();

        [[nodiscard]]
        ExprNode* factor();

        [[nodiscard]]
        ExprNode* unary();

        // 后缀链：primary ( args | "." identifier | "[" expression "]" )*。
        [[nodiscard]]
        ExprNode* value();

        [[nodiscard]]
        ExprNode* primary();

        // 插值串：InterpStart (表达式 InterpMiddle)* 表达式 InterpEnd 组段；空字面段不入列。
        [[nodiscard]]
        ExprNode* interp_string();

        [[nodiscard]]
        List<ExprNode*> args();

        [[nodiscard]]
        ExprNode* list_expr();

        [[nodiscard]]
        ExprNode* map_expr();

        // mapExpr 的单个键值对：expression ":" expression。
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

        // pattern -> identifier | "_" | listPattern。
        [[nodiscard]]
        PatternNode* pattern();

        [[nodiscard]]
        ListPatternNode* list_pattern();

        // rest 模式："..." identifier，返回绑名模式节点（拒绝 "..._"）。
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
