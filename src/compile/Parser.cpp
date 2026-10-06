#include "Parser.hpp"

#include <format>
#include <utility>

#include "aria.hpp"

namespace aria {

    namespace {
        // TokenType -> Op 映射；调用方须先 match 对应 token，非法 token 触发 UNREACHABLE。
        Op::Assignment assignment_op(const TokenType t) noexcept {
            switch (t) {
                case TokenType::Equal:
                    return Op::Assignment::Assign;
                case TokenType::PlusEqual:
                    return Op::Assignment::PlusAssign;
                case TokenType::MinusEqual:
                    return Op::Assignment::MinusAssign;
                case TokenType::StarEqual:
                    return Op::Assignment::StarAssign;
                case TokenType::SlashEqual:
                    return Op::Assignment::SlashAssign;
                case TokenType::PercentEqual:
                    return Op::Assignment::PercentAssign;
                default:
                    UNREACHABLE();
            }
        }

        Op::Binary binary_op(const TokenType t) noexcept {
            switch (t) {
                case TokenType::OrOr:
                    return Op::Binary::Or;
                case TokenType::AndAnd:
                    return Op::Binary::And;
                case TokenType::EqualEqual:
                    return Op::Binary::EqualEqual;
                case TokenType::EqualEqualEqual:
                    return Op::Binary::EqualEqualEqual;
                case TokenType::BangEqual:
                    return Op::Binary::BangEqual;
                case TokenType::BangEqualEqual:
                    return Op::Binary::BangEqualEqual;
                case TokenType::Greater:
                    return Op::Binary::Greater;
                case TokenType::GreaterEqual:
                    return Op::Binary::GreaterEqual;
                case TokenType::Less:
                    return Op::Binary::Less;
                case TokenType::LessEqual:
                    return Op::Binary::LessEqual;
                case TokenType::Plus:
                    return Op::Binary::Plus;
                case TokenType::Minus:
                    return Op::Binary::Minus;
                case TokenType::Star:
                    return Op::Binary::Star;
                case TokenType::Slash:
                    return Op::Binary::Slash;
                case TokenType::Percent:
                    return Op::Binary::Percent;
                default:
                    UNREACHABLE();
            }
        }

        Op::Unary unary_op(const TokenType t) noexcept {
            switch (t) {
                case TokenType::Minus:
                    return Op::Unary::Minus;
                case TokenType::Bang:
                    return Op::Unary::Not;
                case TokenType::PlusPlus:
                    return Op::Unary::PreInc;
                case TokenType::MinusMinus:
                    return Op::Unary::PreDec;
                default:
                    UNREACHABLE();
            }
        }

        // 字符串族 token -> 字面节点：内层原文视图 + 消费形态随 token 走，primary / import /
        // 插值字面段三处共用。
        UPtr<StringLiteralNode> make_string_literal(const Token& token) {
            return std::make_unique<StringLiteralNode>(token.loc(), token.string_value(), token.shape());
        }

        // Interp 系段 token 的非空字面段包成 StringLiteralNode 入列；空段（展开后零字节）对值无贡献，
        // 不入列。
        void maybe_add_string(List<UPtr<ExprNode>>& parts, const Token& token) {
            if (token.shape().decoded_len != 0) {
                parts.push_back(make_string_literal(token));
            }
        }

    } // namespace

    Parser::Parser(List<Token>& tokens) noexcept : tokens_{tokens}, pos_{0}, errors_{} {}

    Result<UPtr<ProgramNode>, List<Error>> Parser::parse(List<Token>& tokens) {
        Parser parser{tokens};

        UPtr<ProgramNode> prog;
        try {
            prog = parser.program();
        } catch (const AriaCompileException& e) {
            // 兜底：program() 内 declaration() 已捕获常规错误；此处仅防御异常逃逸。
            parser.errors_.push_back(e.error());
            prog = nullptr;
        }

        if (!parser.errors_.empty()) {
            return std::unexpected(std::move(parser.errors_));
        }
        return prog;
    }

    const Token& Parser::peek(const usize ahead) const noexcept {
        const usize size     = tokens_.size();
        const usize peek_pos = pos_ + ahead;
        const usize idx      = (peek_pos < size) ? peek_pos : size - 1;
        return tokens_[idx];
    }

    bool Parser::check(const TokenType t) const noexcept { return peek().is(t); }

    bool Parser::check_next(const TokenType t) const noexcept { return peek(1).is(t); }

    bool Parser::match(const TokenType t) noexcept {
        if (check(t)) {
            advance();
            return true;
        }
        return false;
    }

    bool Parser::is_at_end() const noexcept { return peek().is_eof(); }

    const Token& Parser::advance() noexcept {
        if (!is_at_end()) {
            ++pos_;
        }
        return previous();
    }

    const Token& Parser::previous() const noexcept {
        ASSERT(pos_ > 0, "no token consumed yet");
        return tokens_[pos_ - 1];
    }

    const Token& Parser::expect(const TokenType t, const StringView what) {
        if (check(t)) {
            return advance();
        }
        // 违规片段取 peek 的 lexeme（源码原片段）；to_string 的 CamelCase 类型名不面向用户。
        if (is_at_end()) {
            error(ErrorCode::UnexpectedEof, "expected {}, got end of file", what);
        }
        error(ErrorCode::ExpectedToken, "expected {}, got '{}'", what, peek().lexeme());
    }

    StringView Parser::expect_identifier() {
        if (check(TokenType::Identifier)) {
            return advance().lexeme();
        }
        if (is_at_end()) {
            error(ErrorCode::UnexpectedEof, "expected identifier, got end of file");
        }
        error(ErrorCode::ExpectedIdentifier, "expected identifier, got '{}'", peek().lexeme());
    }

    void Parser::synchronize() {
        // 跳过引发错误的 token（确保前进，避免死循环）。
        if (!is_at_end()) {
            advance();
        }
        while (!is_at_end()) {
            // previous 判 ';'：分号本身被丢弃，从下一 token 续扫。
            if (pos_ > 0 && previous().is(TokenType::Semicolon)) {
                return;
            }
            // 同步点集合 = statement 分派集 + Fun/Def/Var 三个声明起首关键字，与 declaration() 分派对应。
            switch (peek().type()) {
                case TokenType::Fun:
                case TokenType::Def:
                case TokenType::Var:
                case TokenType::If:
                case TokenType::While:
                case TokenType::For:
                case TokenType::Break:
                case TokenType::Continue:
                case TokenType::Return:
                case TokenType::Import:
                case TokenType::Try:
                case TokenType::Throw:
                case TokenType::Match:
                case TokenType::LeftBrace:
                    return;
                default:
                    advance();
            }
        }
    }

    UPtr<ProgramNode> Parser::program() {
        const SourceLoc      loc = peek().loc();
        List<UPtr<StmtNode>> decls;
        while (!is_at_end()) {
            if (UPtr<StmtNode> d = declaration()) {
                decls.push_back(std::move(d));
            }
        }
        return std::make_unique<ProgramNode>(loc, std::move(decls));
    }

    UPtr<StmtNode> Parser::declaration() {
        try {
            // fun + 标识符 -> 函数声明；fun + '(' -> lambda 表达式语句（走 statement）。
            if (check(TokenType::Fun) && check_next(TokenType::Identifier)) {
                return fun_decl(FnKind::Function);
            }
            if (check(TokenType::Def)) {
                return def_decl(false);
            }
            if (check(TokenType::Var)) {
                return var_decl();
            }
            return statement();
        } catch (const AriaCompileException& e) {
            errors_.push_back(e.error());
            synchronize();
            return nullptr;
        }
    }

    UPtr<FunDeclNode> Parser::fun_decl(const FnKind kind) {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Fun, "'fun'");
        const auto      name = expect_identifier();
        List<Param>     ps   = params();
        UPtr<BlockNode> body = block();
        return std::make_unique<FunDeclNode>(loc, name, std::move(ps), std::move(body), kind);
    }

    List<Param> Parser::params() {
        expect(TokenType::LeftParen, "'('");
        List<Param> result;
        if (!check(TokenType::RightParen) && !is_at_end()) {
            bool seen_default = false;
            while (true) {
                if (match(TokenType::DotDotDot)) {
                    const auto name = expect_identifier();
                    result.push_back(Param{.name = name, .is_varargs = true});
                    break;
                }
                const auto name = expect_identifier();
                if (match(TokenType::Equal)) {
                    UPtr<ExprNode> dv = expression();
                    result.push_back(Param{.name = name, .default_value = std::move(dv)});
                    seen_default = true;
                } else {
                    if (seen_default) {
                        error(ErrorCode::DefaultAfterPlain, "non-default parameter after default parameter");
                    }
                    result.push_back(Param{.name = name});
                }
                if (!match(TokenType::Comma)) {
                    break;
                }
            }
            if (!result.empty() && result.back().is_varargs && !check(TokenType::RightParen)) {
                error(ErrorCode::VarargsNotLast, "varargs '...' must be the last parameter");
            }
        }
        expect(TokenType::RightParen, "')'");
        return result;
    }

    UPtr<DefDeclNode> Parser::def_decl(const bool is_member) {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Def, "'def'");
        const auto     name       = expect_identifier();
        UPtr<ExprNode> superclass = nullptr;
        if (match(TokenType::Colon)) {
            const auto super_loc = peek().loc();
            superclass           = std::make_unique<IdentifierNode>(super_loc, expect_identifier());
        }
        expect(TokenType::LeftBrace, "'{'");
        // def 体成员按首 token 分派：var -> 静态变量，fun -> 静态方法（无 this），裸 identifier ->
        // 实例方法（名为 init 烙 InitMethod 构造角色），def -> 嵌套类（递归烙 is_member）。其余报错。
        List<UPtr<StmtNode>> members;
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (check(TokenType::Var)) {
                members.push_back(member_var());
            } else if (check(TokenType::Fun)) {
                members.push_back(fun_decl(FnKind::StaticMethod));
            } else if (check(TokenType::Def)) {
                members.push_back(def_decl(true));
            } else if (check(TokenType::Identifier)) {
                const SourceLoc mloc  = peek().loc();
                const auto      mname = expect_identifier();
                List<Param>     mps   = params();
                UPtr<BlockNode> mbody = block();
                const auto      kind  = mname == kInitName ? FnKind::InitMethod : FnKind::Method;
                members.push_back(std::make_unique<FunDeclNode>(mloc, mname, std::move(mps), std::move(mbody), kind));
            } else {
                error(ErrorCode::ExpectedToken, "expected 'var', 'fun', 'def' or a method name in def body, got '{}'",
                      peek().lexeme());
            }
        }
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<DefDeclNode>(loc, name, std::move(superclass), std::move(members), is_member);
    }

    UPtr<StaticVarMemberNode> Parser::member_var() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Var, "'var'");
        const auto     name = expect_identifier();
        UPtr<ExprNode> init = match(TokenType::Equal) ? expression() : nullptr;
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<StaticVarMemberNode>(loc, String{name}, std::move(init));
    }

    UPtr<VarDeclNode> Parser::var_decl() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Var, "'var'");
        List<VarBinding> bindings;
        do {
            bindings.push_back(var_binding());
        } while (match(TokenType::Comma));
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<VarDeclNode>(loc, std::move(bindings));
    }

    VarBinding Parser::var_binding() {
        // varTarget 统一按 pattern 解析（identifier ⊂ pattern）。
        UPtr<PatternNode> target = pattern();
        UPtr<ExprNode>    init   = match(TokenType::Equal) ? expression() : nullptr;
        return VarBinding{.target = std::move(target), .initializer = std::move(init)};
    }

    UPtr<StmtNode> Parser::statement() {
        switch (peek().type()) {
            case TokenType::If:
                return if_stmt();
            case TokenType::While:
                return while_stmt();
            case TokenType::For:
                return for_or_for_in_stmt();
            case TokenType::Break:
                return break_stmt();
            case TokenType::Continue:
                return continue_stmt();
            case TokenType::Return:
                return return_stmt();
            case TokenType::Import:
                return import_stmt();
            case TokenType::Try:
                return try_stmt();
            case TokenType::Throw:
                return throw_stmt();
            case TokenType::Match:
                return match_stmt();
            case TokenType::LeftBrace:
                return block();
            default:
                return expression_stmt();
        }
    }

    UPtr<StmtNode> Parser::expression_stmt() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = sequence(); // 语句位收序列层：a = 1, b = 2;（for-init 复用本入口）
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ExprStmtNode>(loc, std::move(expr));
    }

    UPtr<StmtNode> Parser::if_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::If, "'if'");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> cond = sequence();
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> then_branch = statement();
        UPtr<StmtNode> else_branch = match(TokenType::Else) ? statement() : nullptr;
        return std::make_unique<IfStmtNode>(loc, std::move(cond), std::move(then_branch), std::move(else_branch));
    }

    UPtr<StmtNode> Parser::while_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::While, "'while'");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> cond = sequence();
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> body = statement();
        return std::make_unique<WhileStmtNode>(loc, std::move(cond), std::move(body));
    }

    UPtr<StmtNode> Parser::for_or_for_in_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::For, "'for'");
        expect(TokenType::LeftParen, "'('");

        if (check(TokenType::Semicolon)) {
            advance();
            return finish_for_stmt(loc, nullptr);
        }
        if (check(TokenType::Var)) {
            UPtr<StmtNode> init = var_decl();
            return finish_for_stmt(loc, std::move(init));
        }
        // forIn 消歧；否则按 forStmt。
        if (looks_like_for_in()) {
            return finish_for_in_stmt(loc);
        }
        return finish_for_stmt(loc, expression_stmt());
    }

    UPtr<StmtNode> Parser::finish_for_stmt(const SourceLoc loc, UPtr<StmtNode> init) {
        UPtr<ExprNode> condition = nullptr;
        if (!check(TokenType::Semicolon) && !is_at_end()) {
            condition = sequence();
        }
        expect(TokenType::Semicolon, "';'");
        UPtr<ExprNode> increment = nullptr;
        if (!check(TokenType::RightParen) && !is_at_end()) {
            increment = sequence(); // 增量位收序列层（++i, --j）
        }
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> body = statement();
        return std::make_unique<ForStmtNode>(loc, std::move(init), std::move(condition), std::move(increment),
                                             std::move(body));
    }

    UPtr<StmtNode> Parser::finish_for_in_stmt(const SourceLoc loc) {
        UPtr<PatternNode> target = pattern(); // forIn 目标为 pattern
        expect(TokenType::In, "'in'");
        UPtr<ExprNode> iterable = expression();
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> body = statement();
        return std::make_unique<ForInStmtNode>(loc, std::move(target), std::move(iterable), std::move(body));
    }

    bool Parser::looks_like_for_in() const noexcept {
        usize cursor = pos_;
        if (cursor >= tokens_.size()) {
            return false;
        }
        const TokenType t = tokens_[cursor].type();
        if (t == TokenType::Underscore || t == TokenType::Identifier) {
            return cursor + 1 < tokens_.size() && tokens_[cursor + 1].is(TokenType::In);
        }
        if (t == TokenType::LeftBracket) {
            usize depth = 0;
            for (; cursor < tokens_.size(); ++cursor) {
                if (tokens_[cursor].is(TokenType::LeftBracket)) {
                    ++depth;
                } else if (tokens_[cursor].is(TokenType::RightBracket)) {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
            }
            if (depth != 0) {
                return false; // 括号不配对，交由后续解析报错
            }
            return cursor + 1 < tokens_.size() && tokens_[cursor + 1].is(TokenType::In);
        }
        return false;
    }

    UPtr<StmtNode> Parser::break_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Break, "'break'");
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<BreakStmtNode>(loc);
    }

    UPtr<StmtNode> Parser::continue_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Continue, "'continue'");
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ContinueStmtNode>(loc);
    }

    UPtr<StmtNode> Parser::return_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Return, "'return'");
        UPtr<ExprNode> value = nullptr;
        if (!check(TokenType::Semicolon) && !is_at_end()) {
            value = expression();
        }
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ReturnStmtNode>(loc, std::move(value));
    }

    UPtr<StmtNode> Parser::import_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Import, "'import'");
        if (!check(TokenType::String)) {
            error(ErrorCode::ExpectedToken, "expected a string literal as module path, got '{}'", peek().lexeme());
        }
        auto path = make_string_literal(advance());
        expect(TokenType::As, "'as'");
        const auto alias = expect_identifier();
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ImportStmtNode>(loc, std::move(path), String{alias});
    }

    UPtr<StmtNode> Parser::try_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Try, "'try'");
        UPtr<BlockNode> body        = block();
        Opt<String>     catch_param = std::nullopt;
        UPtr<BlockNode> catch_body  = nullptr;
        if (match(TokenType::Catch)) {
            expect(TokenType::LeftParen, "'('");
            catch_param = Opt<String>{expect_identifier()};
            expect(TokenType::RightParen, "')'");
            catch_body = block();
        }
        return std::make_unique<TryStmtNode>(loc, std::move(body), std::move(catch_param), std::move(catch_body));
    }

    UPtr<StmtNode> Parser::throw_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Throw, "'throw'");
        UPtr<ExprNode> expr = expression();
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ThrowStmtNode>(loc, std::move(expr));
    }

    UPtr<StmtNode> Parser::match_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Match, "'match'");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> subject = sequence();
        expect(TokenType::RightParen, "')'");
        expect(TokenType::LeftBrace, "'{'");
        List<MatchArm> arms;
        arms.push_back(match_arm()); // matchArm+：至少一条
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            arms.push_back(match_arm());
        }
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<MatchStmtNode>(loc, std::move(subject), std::move(arms));
    }

    UPtr<BlockNode> Parser::block() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::LeftBrace, "'{'");
        List<UPtr<StmtNode>> stmts;
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (UPtr<StmtNode> d = declaration()) {
                stmts.push_back(std::move(d));
            }
        }
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<BlockNode>(loc, std::move(stmts));
    }

    UPtr<ExprNode> Parser::expression() { return assignment(); }

    UPtr<ExprNode> Parser::sequence() {
        const SourceLoc      loc = peek().loc();
        List<UPtr<ExprNode>> expressions;
        expressions.push_back(assignment());
        while (match(TokenType::Comma)) {
            expressions.push_back(assignment());
        }
        if (expressions.size() == 1) {
            return std::move(expressions[0]);
        }
        return std::make_unique<SequenceExprNode>(loc, std::move(expressions));
    }

    UPtr<ExprNode> Parser::assignment() {
        const SourceLoc loc = peek().loc();

        // '[' 歧义（listPattern 与 listExpr 同以 '[' 起头）：投机先按 listPattern 解析，其后非 '='
        // 或解析失败则回退 pos_ 按表达式重解析。
        if (check(TokenType::LeftBracket)) {
            const usize save = pos_;
            try {
                UPtr<PatternNode> pat = list_pattern();
                if (match(TokenType::Equal)) {
                    UPtr<ExprNode> rhs = assignment();
                    return std::make_unique<DestructureAssignmentNode>(loc, std::move(pat), std::move(rhs));
                }
            } catch (const AriaCompileException&) {
                // 吞掉异常统一回退按表达式重解析。rhs 的 assignment() 抛错也会进此 catch，正确性
                // 依赖回退 pos_ = save 后按 listExpr 重解析会在同一位置复现同一错误（错误仅由源内容
                // 决定，与解析路径无关）。
            }
            pos_ = save;
        }

        UPtr<ExprNode> lhs = logic_or();

        if (match(TokenType::Equal) || match(TokenType::PlusEqual) || match(TokenType::MinusEqual) ||
            match(TokenType::StarEqual) || match(TokenType::SlashEqual) || match(TokenType::PercentEqual)) {
            const Op::Assignment op  = assignment_op(previous().type());
            UPtr<ExprNode>       rhs = assignment();
            return std::make_unique<AssignmentNode>(loc, op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    UPtr<ExprNode> Parser::logic_or() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = logic_and();
        while (match(TokenType::OrOr)) {
            const Op::Binary op  = binary_op(previous().type());
            UPtr<ExprNode>   rhs = logic_and();
            expr                 = std::make_unique<BinaryExprNode>(loc, op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    UPtr<ExprNode> Parser::logic_and() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = equality();
        while (match(TokenType::AndAnd)) {
            const Op::Binary op  = binary_op(previous().type());
            UPtr<ExprNode>   rhs = equality();
            expr                 = std::make_unique<BinaryExprNode>(loc, op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    UPtr<ExprNode> Parser::equality() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = comparison();
        while (match(TokenType::EqualEqual) || match(TokenType::EqualEqualEqual) || match(TokenType::BangEqual) ||
               match(TokenType::BangEqualEqual)) {
            const Op::Binary op  = binary_op(previous().type());
            UPtr<ExprNode>   rhs = comparison();
            expr                 = std::make_unique<BinaryExprNode>(loc, op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    UPtr<ExprNode> Parser::comparison() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = range();
        while (match(TokenType::Greater) || match(TokenType::GreaterEqual) || match(TokenType::Less) ||
               match(TokenType::LessEqual)) {
            const Op::Binary op  = binary_op(previous().type());
            UPtr<ExprNode>   rhs = range();
            expr                 = std::make_unique<BinaryExprNode>(loc, op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    // 区间：a..b（含上界）/ a...b（不含上界）。非结合，rhs 调 term 不调 range；
    // DotDotDot 与 rest/varargs 前缀复用，按位置消歧；无上界时 .. 与 ... 语义同义。
    UPtr<ExprNode> Parser::range() {
        const SourceLoc loc   = peek().loc();
        UPtr<ExprNode>  lower = term();
        if (match(TokenType::DotDot) || match(TokenType::DotDotDot)) {
            const bool is_exclusive = previous().is(TokenType::DotDotDot);
            // 无上界走试探：term() 能解析则收 upper，失败即 .. / ... 后不跟表达式，回滚游标判无上界。
            // 试探期零副作用（error() 是纯抛出，errors_ 记账只在 declaration() 恢复点），回滚仅需游标；
            // upper 位表达式残缺也落此路，报错移到外层语法错，仍显性。
            const usize save = pos_;
            try {
                UPtr<ExprNode> upper = term();
                return std::make_unique<RangeExprNode>(loc, is_exclusive, std::move(lower), std::move(upper));
            } catch (const AriaCompileException&) {
                pos_ = save;
                return std::make_unique<RangeExprNode>(loc, is_exclusive, std::move(lower), nullptr);
            }
        }
        return lower;
    }


    UPtr<ExprNode> Parser::term() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = factor();
        while (match(TokenType::Plus) || match(TokenType::Minus)) {
            const Op::Binary op  = binary_op(previous().type());
            UPtr<ExprNode>   rhs = factor();
            expr                 = std::make_unique<BinaryExprNode>(loc, op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    UPtr<ExprNode> Parser::factor() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = unary();
        while (match(TokenType::Slash) || match(TokenType::Star) || match(TokenType::Percent)) {
            const Op::Binary op  = binary_op(previous().type());
            UPtr<ExprNode>   rhs = unary();
            expr                 = std::make_unique<BinaryExprNode>(loc, op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    UPtr<ExprNode> Parser::unary() {
        const SourceLoc loc = peek().loc();
        if (match(TokenType::Minus) || match(TokenType::Bang) || match(TokenType::PlusPlus) ||
            match(TokenType::MinusMinus)) {
            const Op::Unary op      = unary_op(previous().type());
            UPtr<ExprNode>  operand = unary(); // 右结合：允许 -- -x
            return std::make_unique<UnaryExprNode>(loc, op, std::move(operand));
        }
        return value();
    }

    UPtr<ExprNode> Parser::value() {
        const SourceLoc loc  = peek().loc();
        UPtr<ExprNode>  expr = primary();
        while (true) {
            if (check(TokenType::LeftParen)) {
                List<UPtr<ExprNode>> call_args = args();
                expr                           = std::make_unique<CallNode>(loc, std::move(expr), std::move(call_args));
            } else if (match(TokenType::Dot)) {
                expr = std::make_unique<FieldAccessNode>(loc, std::move(expr), expect_identifier());
            } else if (match(TokenType::LeftBracket)) {
                UPtr<ExprNode> index = expression();
                expect(TokenType::RightBracket, "']'");
                expr = std::make_unique<IndexAccessNode>(loc, std::move(expr), std::move(index));
            } else {
                break;
            }
        }
        return expr;
    }

    UPtr<ExprNode> Parser::interp_string() {
        const SourceLoc loc = peek().loc();

        List<UPtr<ExprNode>> parts;
        maybe_add_string(parts, advance()); // InterpStart，段值 = 首字面段
        while (true) {
            // 档内完整表达式；空档（"${}"）在此撞 Interp 系 token 走期望表达式错
            parts.push_back(expression());
            if (check(TokenType::InterpEnd)) {
                maybe_add_string(parts, advance()); // 尾字面段
                return std::make_unique<InterpolatedStringNode>(loc, std::move(parts));
            }
            maybe_add_string(parts, expect(TokenType::InterpMiddle, "'}' to close interpolation"));
        }
    }

    List<UPtr<ExprNode>> Parser::args() {
        expect(TokenType::LeftParen, "'('");
        List<UPtr<ExprNode>> args;
        if (!check(TokenType::RightParen) && !is_at_end()) {
            do {
                args.push_back(expression());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RightParen, "')'");
        return args;
    }

    UPtr<ExprNode> Parser::primary() {
        const SourceLoc loc = peek().loc();
        switch (peek().type()) {
            case TokenType::Integer: {
                const Token& t = advance();
                return std::make_unique<IntegerLiteralNode>(loc, t.int_value());
            }
            case TokenType::Float: {
                const Token& t = advance();
                return std::make_unique<FloatLiteralNode>(loc, t.float_value());
            }
            case TokenType::String: {
                return make_string_literal(advance());
            }
            case TokenType::InterpStart:
                return interp_string();
            case TokenType::True:
                advance();
                return std::make_unique<BoolLiteralNode>(loc, true);
            case TokenType::False:
                advance();
                return std::make_unique<BoolLiteralNode>(loc, false);
            case TokenType::Nil:
                advance();
                return std::make_unique<NilLiteralNode>(loc);
            case TokenType::Identifier: {
                return std::make_unique<IdentifierNode>(loc, advance().lexeme());
            }
            case TokenType::This:
                advance();
                return std::make_unique<ThisExprNode>(loc);
            case TokenType::Super: {
                advance();
                expect(TokenType::Dot, "'.'");
                return std::make_unique<SuperExprNode>(loc, expect_identifier());
            }
            case TokenType::LeftParen: {
                advance();
                UPtr<ExprNode> e = sequence();
                expect(TokenType::RightParen, "')'");
                return e;
            }
            case TokenType::LeftBracket:
                return list_expr();
            case TokenType::LeftBrace:
                return map_expr();
            case TokenType::If:
                return if_expr();
            case TokenType::Fun:
                return lambda_expr();
            case TokenType::Match:
                return match_expr();
            default:
                break;
        }
        if (is_at_end()) {
            error(ErrorCode::UnexpectedEof, "expected expression, got end of file");
        }
        error(ErrorCode::ExpectedExpression, "expected expression, got '{}'", peek().lexeme());
    }

    UPtr<ExprNode> Parser::list_expr() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::LeftBracket, "'['");
        List<UPtr<ExprNode>> elements;
        if (!check(TokenType::RightBracket) && !is_at_end()) {
            do {
                elements.push_back(expression());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RightBracket, "']'");
        return std::make_unique<ListExprNode>(loc, std::move(elements));
    }

    UPtr<ExprNode> Parser::map_expr() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::LeftBrace, "'{'");
        List<MapEntry> entries;
        if (!check(TokenType::RightBrace) && !is_at_end()) {
            do {
                entries.push_back(parse_map_entry());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<MapExprNode>(loc, std::move(entries));
    }

    MapEntry Parser::parse_map_entry() {
        UPtr<ExprNode> key = expression();
        expect(TokenType::Colon, "':'");
        UPtr<ExprNode> val = expression();
        return MapEntry{.key = std::move(key), .value = std::move(val)};
    }

    UPtr<ExprNode> Parser::if_expr() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::If, "'if'");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> cond = sequence();
        expect(TokenType::RightParen, "')'");
        expect(TokenType::LeftBrace, "'{'");
        UPtr<ExprNode> then_expr = sequence();
        expect(TokenType::RightBrace, "'}'");
        expect(TokenType::Else, "'else'");
        expect(TokenType::LeftBrace, "'{'");
        UPtr<ExprNode> else_expr = sequence();
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<IfExprNode>(loc, std::move(cond), std::move(then_expr), std::move(else_expr));
    }

    UPtr<ExprNode> Parser::lambda_expr() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Fun, "'fun'");
        List<Param>     ps   = params();
        UPtr<BlockNode> body = block();
        return std::make_unique<LambdaExprNode>(loc, std::move(ps), std::move(body));
    }

    UPtr<ExprNode> Parser::match_expr() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Match, "'match'");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> subject = sequence();
        expect(TokenType::RightParen, "')'");
        expect(TokenType::LeftBrace, "'{'");
        List<MatchExprArm> arms;
        arms.push_back(match_expr_arm()); // matchExprArm+：至少一条
        while (!check(TokenType::RightBrace)) {
            expect(TokenType::Comma, "','"); // 臂间以 ',' 分隔，不允许尾逗号
            arms.push_back(match_expr_arm());
        }
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<MatchExprNode>(loc, std::move(subject), std::move(arms));
    }

    MatchPattern Parser::match_pattern() {
        if (match(TokenType::Underscore)) {
            return MatchPattern{.value = nullptr}; // 通配兜底 "_"
        }
        return MatchPattern{.value = expression()};
    }

    MatchArm Parser::match_arm() {
        MatchPattern pat = match_pattern();
        expect(TokenType::FatArrow, "'=>'");
        UPtr<StmtNode> body = statement();
        return MatchArm{.pattern = std::move(pat), .body = std::move(body)};
    }

    MatchExprArm Parser::match_expr_arm() {
        MatchPattern pat = match_pattern();
        expect(TokenType::FatArrow, "'=>'");
        UPtr<ExprNode> body = expression();
        return MatchExprArm{.pattern = std::move(pat), .body = std::move(body)};
    }

    UPtr<PatternNode> Parser::pattern() {
        const SourceLoc loc = peek().loc();
        if (match(TokenType::Identifier)) {
            return std::make_unique<IdentifierPatternNode>(loc, previous().lexeme());
        }
        if (match(TokenType::Underscore)) {
            return std::make_unique<WildcardPatternNode>(loc);
        }
        if (check(TokenType::LeftBracket)) {
            return list_pattern();
        }
        if (is_at_end()) {
            error(ErrorCode::UnexpectedEof, "expected identifier or pattern, got end of file");
        }
        error(ErrorCode::ExpectedIdentifier, "expected identifier or pattern, got '{}'", peek().lexeme());
    }

    UPtr<ListPatternNode> Parser::list_pattern() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::LeftBracket, "'['");
        List<UPtr<PatternNode>>     elements;
        UPtr<IdentifierPatternNode> rest;
        if (!check(TokenType::RightBracket) && !is_at_end()) {
            do {
                if (check(TokenType::DotDotDot)) {
                    rest = rest_pattern();
                    break; // rest 必为末尾
                }
                elements.push_back(pattern());
            } while (match(TokenType::Comma));
            if (rest && !check(TokenType::RightBracket)) {
                error(ErrorCode::InvalidPattern, "rest pattern '...' must be last");
            }
        }
        expect(TokenType::RightBracket, "']'");
        return std::make_unique<ListPatternNode>(loc, std::move(elements), std::move(rest));
    }

    UPtr<IdentifierPatternNode> Parser::rest_pattern() {
        expect(TokenType::DotDotDot, "'...'");
        if (check(TokenType::Underscore)) {
            // ..._ 与不写 rest 等价，冗余非法。
            error(ErrorCode::InvalidPattern, "rest pattern cannot bind '_'");
        }
        const SourceLoc loc = peek().loc();
        return std::make_unique<IdentifierPatternNode>(loc, expect_identifier());
    }

} // namespace aria
