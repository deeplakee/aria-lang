#include "Parser.hpp"

#include <format>
#include <utility>

namespace aria {

    namespace {
        // TokenType -> Op 映射。parser 负责 token->op 映射（Op 与 TokenType 解耦，见 ast.hpp）。
        // 调用方须先 match 对应 token，故入参必为合法运算符 token；非运算符 token 触发 UNREACHABLE。
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
    } // namespace

    // ============================================================
    // 构造与入口
    // ============================================================

    Parser::Parser() noexcept : tokens_{}, pos_{0}, errors_{} {}

    Result<UPtr<ProgramNode>, List<Error>> Parser::parse(List<Token> tokens) {
        tokens_ = std::move(tokens);
        pos_    = 0;
        errors_.clear();

        UPtr<ProgramNode> prog;
        try {
            prog = program();
        } catch (const AriaCompileException& e) {
            // 兜底：program() 内 declaration() 已捕获常规错误；此处仅防御异常逃逸。
            errors_.push_back(e.error());
            prog = nullptr;
        }

        // 清空成员，回到空态，供 Parser 复用。
        List<Error> out_errors = std::move(errors_);
        tokens_.clear();
        pos_ = 0;

        if (!out_errors.empty()) {
            return std::unexpected(std::move(out_errors));
        }
        return prog;
    }

    // ============================================================
    // token 游标辅助
    // ============================================================

    const Token& Parser::peek(const usize ahead) const noexcept {
        const usize size     = tokens_.size();
        const usize peek_pos = pos_ + ahead;
        // size 至少为 1（末尾 Eof）；越界钳到末尾 Eof，安全。
        const usize idx = (peek_pos < size) ? peek_pos : size - 1;
        return tokens_[idx];
    }

    TokenType Parser::peek_type(const usize ahead) const noexcept { return peek(ahead).type(); }

    bool Parser::check(const TokenType t) const noexcept { return peek_type() == t; }

    bool Parser::check_next(const TokenType t) const noexcept { return peek_type(1) == t; }

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
        ASSERT(pos_ > 0, "Parser::previous 在未消费任何 token 时调用");
        return tokens_[pos_ - 1];
    }

    // ============================================================
    // 错误与期待
    // ============================================================

    void Parser::error(const ErrorCode code, String msg) const {
        throw AriaCompileException{Error::from_detail(code, peek().loc(), msg)};
    }

    const Token& Parser::expect(const TokenType t, const StringView what) {
        if (check(t)) {
            return advance();
        }
        if (is_at_end()) {
            error(ErrorCode::UnexpectedEof, std::format("期望 {} 但遇到文件结束", what));
        }
        error(ErrorCode::ExpectedToken, std::format("期望 {} 但遇到 '{}'", what, token_type_name(peek_type())));
    }

    String Parser::expect_identifier() {
        if (check(TokenType::Identifier)) {
            const Token& t = advance();
            return String{t.lexeme()};
        }
        if (is_at_end()) {
            error(ErrorCode::UnexpectedEof, "期望标识符但遇到文件结束");
        }
        error(ErrorCode::ExpectedIdentifier, std::format("期望标识符但遇到 '{}'", token_type_name(peek_type())));
    }

    void Parser::synchronize() {
        // 跳过引发错误的 token（确保前进，避免死循环）。
        if (!is_at_end()) {
            advance();
        }
        while (!is_at_end()) {
            // 恰在消费 ';' 之后判定（previous 而非 peek）： ';' 本身被丢弃，从下一 token 续扫。
            if (pos_ > 0 && previous().type() == TokenType::Semicolon) {
                return;
            }
            // 同步点集合须与 statement() 的分派集保持一致：statement 认哪些语句起首关键字，
            // 这里就恢复到哪些（漏一个即少一个恢复点）。
            switch (peek_type()) {
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
                case TokenType::Print:
                case TokenType::LeftBrace:
                    return;
                default:
                    advance();
            }
        }
    }

    // ============================================================
    // 顶层与声明
    // ============================================================

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
                return fun_decl();
            }
            if (check(TokenType::Def)) {
                return def_decl();
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

    UPtr<FunDeclNode> Parser::fun_decl() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Fun, "\"fun\"");
        String          name = expect_identifier();
        List<Param>     ps   = params();
        UPtr<BlockNode> body = block();
        return std::make_unique<FunDeclNode>(loc, std::move(name), std::move(ps), std::move(body));
    }

    List<Param> Parser::params() {
        expect(TokenType::LeftParen, "'('");
        List<Param> result;
        if (!check(TokenType::RightParen) && !is_at_end()) {
            bool seen_default = false;
            while (true) {
                if (match(TokenType::DotDotDot)) {
                    String name = expect_identifier();
                    result.push_back(Param{.name = std::move(name), .is_varargs = true});
                    break;
                }
                String name = expect_identifier();
                if (match(TokenType::Equal)) {
                    UPtr<ExprNode> dv = expression();
                    result.push_back(Param{.name = std::move(name), .default_value = std::move(dv)});
                    seen_default = true;
                } else {
                    if (seen_default) {
                        error(ErrorCode::DefaultAfterPlain, "默认参数之后不得再有无默认参数");
                    }
                    result.push_back(Param{.name = std::move(name)});
                }
                if (!match(TokenType::Comma)) {
                    break;
                }
            }
            if (!result.empty() && result.back().is_varargs && !check(TokenType::RightParen)) {
                error(ErrorCode::VarargsNotLast, "varargs '...' 必须位于参数列表末尾");
            }
        }
        expect(TokenType::RightParen, "')'");
        return result;
    }

    UPtr<DefDeclNode> Parser::def_decl() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Def, "\"def\"");
        String      name       = expect_identifier();
        Opt<String> superclass = match(TokenType::Colon) ? Opt{expect_identifier()} : std::nullopt;
        expect(TokenType::LeftBrace, "'{'");
        // def 体：(varDecl | funDecl | function)*。按首 token 分派成员种类：
        //   - var 声明 -> 静态变量（StaticVar）；
        //   - fun 声明 -> 静态方法（StaticMethod，无 this）；
        //   - 裸 identifier（identifier params block）-> 实例方法（InstanceMethod，有 this）。
        // 其余 token 走 else 报错。
        List<DefMember> members;
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (check(TokenType::Var)) {
                members.push_back(DefMember{.kind = DefMember::Kind::StaticVar, .node = var_decl()});
            } else if (check(TokenType::Fun)) {
                members.push_back(DefMember{.kind = DefMember::Kind::StaticMethod, .node = fun_decl()});
            } else if (check(TokenType::Identifier)) {
                const SourceLoc mloc  = peek().loc();
                String          mname = expect_identifier();
                List<Param>     mps   = params();
                UPtr<BlockNode> mbody = block();
                auto fn = std::make_unique<FunDeclNode>(mloc, std::move(mname), std::move(mps), std::move(mbody));
                members.push_back(DefMember{.kind = DefMember::Kind::InstanceMethod, .node = std::move(fn)});
            } else {
                error(ErrorCode::ExpectedToken,
                      std::format("def 体内只允许 var/fun/方法，但遇到 '{}'", token_type_name(peek_type())));
            }
        }
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<DefDeclNode>(loc, std::move(name), std::move(superclass), std::move(members));
    }

    UPtr<VarDeclNode> Parser::var_decl() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Var, "\"var\"");
        List<VarBinding> bindings;
        do {
            bindings.push_back(var_binding());
        } while (match(TokenType::Comma));
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<VarDeclNode>(loc, std::move(bindings));
    }

    VarBinding Parser::var_binding() {
        // varTarget -> identifier | pattern（identifier ⊂ pattern，统一按 pattern 解析）。
        UPtr<PatternNode> target = pattern();
        UPtr<ExprNode>    init   = match(TokenType::Equal) ? expression() : nullptr;
        return VarBinding{.target = std::move(target), .initializer = std::move(init)};
    }

    // ============================================================
    // 语句
    // ============================================================

    UPtr<StmtNode> Parser::statement() {
        switch (peek_type()) {
            case TokenType::Print:
                return print_stmt();
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
        UPtr<ExprNode>  expr = expression();
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ExprStmtNode>(loc, std::move(expr));
    }

    UPtr<StmtNode> Parser::print_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Print, "\"print\"");
        UPtr<ExprNode> expr = expression();
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<PrintStmtNode>(loc, std::move(expr));
    }

    UPtr<StmtNode> Parser::if_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::If, "\"if\"");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> cond = expression();
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> then_branch = statement();
        UPtr<StmtNode> else_branch = match(TokenType::Else) ? statement() : nullptr;
        return std::make_unique<IfStmtNode>(loc, std::move(cond), std::move(then_branch), std::move(else_branch));
    }

    UPtr<StmtNode> Parser::while_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::While, "\"while\"");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> cond = expression();
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> body = statement();
        return std::make_unique<WhileStmtNode>(loc, std::move(cond), std::move(body));
    }

    UPtr<StmtNode> Parser::for_or_for_in_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::For, "\"for\"");
        expect(TokenType::LeftParen, "'('");

        // forStmt 空 init：'(' 后即 ';'。
        if (check(TokenType::Semicolon)) {
            advance();
            return finish_for_stmt(loc, nullptr);
        }
        // forStmt varDecl init：'var ... ;'。
        if (check(TokenType::Var)) {
            UPtr<StmtNode> init = var_decl(); // var_decl 消费末尾 ';'
            return finish_for_stmt(loc, std::move(init));
        }
        // forIn：<pattern> "in"。in 非表达式运算符，故 <pattern> in 唯一标识 forIn
        // （identifier/"_" 紧跟 in，或 [...] 后跟 in）；否则按 forStmt 的 exprStmt init。
        if (looks_like_for_in()) {
            return finish_for_in_stmt(loc);
        }
        // forStmt exprStmt init：复用 expression_stmt()（expression ";"，返回 ExprStmtNode）。
        return finish_for_stmt(loc, expression_stmt());
    }

    UPtr<StmtNode> Parser::finish_for_stmt(const SourceLoc loc, UPtr<StmtNode> init) {
        UPtr<ExprNode> condition = nullptr;
        if (!check(TokenType::Semicolon) && !is_at_end()) {
            condition = expression();
        }
        expect(TokenType::Semicolon, "';'");
        UPtr<ExprNode> increment = nullptr;
        if (!check(TokenType::RightParen) && !is_at_end()) {
            increment = expression();
        }
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> body = statement();
        return std::make_unique<ForStmtNode>(loc, std::move(init), std::move(condition), std::move(increment),
                                             std::move(body));
    }

    UPtr<StmtNode> Parser::finish_for_in_stmt(const SourceLoc loc) {
        UPtr<PatternNode> target = pattern(); // forIn 目标为 pattern
        expect(TokenType::In, "\"in\"");
        UPtr<ExprNode> iterable = expression();
        expect(TokenType::RightParen, "')'");
        UPtr<StmtNode> body = statement();
        return std::make_unique<ForInStmtNode>(loc, std::move(target), std::move(iterable), std::move(body));
    }

    bool Parser::looks_like_for_in() const noexcept {
        // pos_ 位于 '(' 后首个 token；判定 <pattern> "in"：identifier/"_" 紧跟 in，或
        // [...]（扫到匹配 ']'）后跟 in。in 非表达式运算符，故命中即 forIn。
        usize cursor = pos_;
        if (cursor >= tokens_.size()) {
            return false;
        }
        const TokenType t = tokens_[cursor].type();
        if (t == TokenType::Underscore || t == TokenType::Identifier) {
            return cursor + 1 < tokens_.size() && tokens_[cursor + 1].type() == TokenType::In;
        }
        if (t == TokenType::LeftBracket) {
            usize depth = 0;
            for (; cursor < tokens_.size(); ++cursor) {
                if (tokens_[cursor].type() == TokenType::LeftBracket) {
                    ++depth;
                } else if (tokens_[cursor].type() == TokenType::RightBracket) {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
            }
            if (depth != 0) {
                return false; // 括号不配对，交由后续解析报错
            }
            return cursor + 1 < tokens_.size() && tokens_[cursor + 1].type() == TokenType::In;
        }
        return false;
    }

    UPtr<StmtNode> Parser::break_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Break, "\"break\"");
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<BreakStmtNode>(loc);
    }

    UPtr<StmtNode> Parser::continue_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Continue, "\"continue\"");
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ContinueStmtNode>(loc);
    }

    UPtr<StmtNode> Parser::return_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Return, "\"return\"");
        UPtr<ExprNode> value = nullptr;
        if (!check(TokenType::Semicolon) && !is_at_end()) {
            value = expression();
        }
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ReturnStmtNode>(loc, std::move(value));
    }

    UPtr<StmtNode> Parser::import_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Import, "\"import\"");
        if (!check(TokenType::String)) {
            error(ErrorCode::ExpectedToken, "期望字符串字面量作为模块路径");
        }
        const StringView path = advance().string_value();
        expect(TokenType::As, "\"as\"");
        String alias = expect_identifier();
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ImportStmtNode>(loc, String{path}, std::move(alias));
    }

    UPtr<StmtNode> Parser::try_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Try, "\"try\"");
        UPtr<BlockNode> body        = block();
        Opt<String>     catch_param = std::nullopt;
        UPtr<BlockNode> catch_body  = nullptr;
        if (match(TokenType::Catch)) {
            expect(TokenType::LeftParen, "'('");
            catch_param = Opt<String>{expect_identifier()};
            expect(TokenType::RightParen, "')'");
            catch_body = block();
        }
        // parse 层允许无 catch（try 单独成块），语义阶段保证必有（TryWithoutHandler）。
        return std::make_unique<TryStmtNode>(loc, std::move(body), std::move(catch_param), std::move(catch_body));
    }

    UPtr<StmtNode> Parser::throw_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Throw, "\"throw\"");
        UPtr<ExprNode> expr = expression();
        expect(TokenType::Semicolon, "';'");
        return std::make_unique<ThrowStmtNode>(loc, std::move(expr));
    }

    UPtr<StmtNode> Parser::match_stmt() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Match, "\"match\"");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> subject = expression();
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

    // ============================================================
    // 表达式
    // ============================================================

    UPtr<ExprNode> Parser::expression() { return assignment(); }

    UPtr<ExprNode> Parser::assignment() {
        const SourceLoc loc = peek().loc();

        // 解构赋值候选：listPattern "=" assignment。listPattern 与 listExpr 均以 '[' 起头，
        // 故见 '[' 时投机地先按 listPattern 解析；若其后非 '='（或 listPattern 解析失败）
        // 则回退 pos_，按表达式（listExpr）重解析。
        if (check(TokenType::LeftBracket)) {
            const usize save = pos_;
            try {
                UPtr<PatternNode> pat = list_pattern();
                if (match(TokenType::Equal)) {
                    UPtr<ExprNode> rhs = assignment();
                    return std::make_unique<DestructureAssignmentNode>(loc, std::move(pat), std::move(rhs));
                }
            } catch (const AriaCompileException&) {
                // 吞掉异常统一回退按表达式重解析。注意 match(Equal) 之后 rhs 的 assignment()
                // 抛错也会进此 catch--正确性依赖回退 pos_ = save 后按 listExpr 重解析会在
                // 同一位置复现同一错误（错误仅由源内容决定，与解析路径无关）。
            }
            pos_ = save; // 非解构赋值或 listPattern 失败，回退 pos_
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

    // 区间：a..b（含上界）/ a...b（不含上界）。非结合（单层），rhs 调 term 不调 range。
    // ... 与 rest/varargs 前缀复用 DotDotDot，按位置消歧（表达式中缀 vs 模式/参数前缀）。
    UPtr<ExprNode> Parser::range() {
        const SourceLoc loc   = peek().loc();
        UPtr<ExprNode>  lower = term();
        if (match(TokenType::DotDot) || match(TokenType::DotDotDot)) {
            const bool     is_exclusive = previous().is(TokenType::DotDotDot);
            UPtr<ExprNode> upper        = term();
            return std::make_unique<RangeExprNode>(loc, is_exclusive, std::move(lower), std::move(upper));
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
                // '(' 由 args() 自行消费，此处用 check 而非 match。
                List<UPtr<ExprNode>> call_args = args();
                expr                           = std::make_unique<CallNode>(loc, std::move(expr), std::move(call_args));
            } else if (match(TokenType::Dot)) {
                String name = expect_identifier();
                expr        = std::make_unique<FieldAccessNode>(loc, std::move(expr), std::move(name));
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
        switch (peek_type()) {
            case TokenType::Integer: {
                const Token& t = advance();
                return std::make_unique<IntegerLiteralNode>(loc, t.int_value());
            }
            case TokenType::Float: {
                const Token& t = advance();
                return std::make_unique<FloatLiteralNode>(loc, t.float_value());
            }
            case TokenType::String: {
                const Token& t = advance();
                return std::make_unique<StringLiteralNode>(loc, String{t.string_value()});
            }
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
                const Token& t = advance();
                return std::make_unique<IdentifierNode>(loc, String{t.lexeme()});
            }
            case TokenType::This:
                advance();
                return std::make_unique<ThisExprNode>(loc);
            case TokenType::Super:
                advance();
                return std::make_unique<SuperExprNode>(loc);
            case TokenType::LeftParen: {
                advance();
                UPtr<ExprNode> e = expression(); // parenExpr 不设独立节点
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
            error(ErrorCode::UnexpectedEof, "期望表达式却遇到文件结束");
        }
        error(ErrorCode::ExpectedExpression, std::format("期望表达式却遇到 '{}'", token_type_name(peek_type())));
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
        expect(TokenType::If, "\"if\"");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> cond = expression();
        expect(TokenType::RightParen, "')'");
        expect(TokenType::LeftBrace, "'{'");
        UPtr<ExprNode> then_expr = expression();
        expect(TokenType::RightBrace, "'}'");
        expect(TokenType::Else, "\"else\"");
        expect(TokenType::LeftBrace, "'{'");
        UPtr<ExprNode> else_expr = expression();
        expect(TokenType::RightBrace, "'}'");
        return std::make_unique<IfExprNode>(loc, std::move(cond), std::move(then_expr), std::move(else_expr));
    }

    UPtr<ExprNode> Parser::lambda_expr() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Fun, "\"fun\"");
        List<Param>     ps   = params();
        UPtr<BlockNode> body = block();
        return std::make_unique<LambdaExprNode>(loc, std::move(ps), std::move(body));
    }

    UPtr<ExprNode> Parser::match_expr() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::Match, "\"match\"");
        expect(TokenType::LeftParen, "'('");
        UPtr<ExprNode> subject = expression();
        expect(TokenType::RightParen, "')'");
        expect(TokenType::LeftBrace, "'{'");
        List<MatchExprArm> arms;
        arms.push_back(match_expr_arm()); // matchExprArm+：至少一条
        while (!check(TokenType::RightBrace) && !is_at_end()) {
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

    // ============================================================
    // 解构模式
    // ============================================================

    UPtr<PatternNode> Parser::pattern() {
        const SourceLoc loc = peek().loc();
        if (match(TokenType::Identifier)) {
            return std::make_unique<IdentifierPatternNode>(loc, String{previous().lexeme()});
        }
        if (match(TokenType::Underscore)) {
            return std::make_unique<WildcardPatternNode>(loc);
        }
        // '[' 由 list_pattern() 自行消费，此处用 check。
        if (check(TokenType::LeftBracket)) {
            return list_pattern();
        }
        if (is_at_end()) {
            error(ErrorCode::UnexpectedEof, "期望标识符或模式却遇到文件结束");
        }
        error(ErrorCode::ExpectedIdentifier, std::format("期望标识符或模式却遇到 '{}'", token_type_name(peek_type())));
    }

    UPtr<ListPatternNode> Parser::list_pattern() {
        const SourceLoc loc = peek().loc();
        expect(TokenType::LeftBracket, "'['");
        List<UPtr<PatternNode>> elements;
        Opt<String>             rest = std::nullopt;
        if (!check(TokenType::RightBracket) && !is_at_end()) {
            do {
                if (check(TokenType::DotDotDot)) {
                    rest = Opt<String>{rest_pattern()};
                    break; // rest 必为末尾
                }
                elements.push_back(pattern());
            } while (match(TokenType::Comma));
            // rest 之后必须紧跟 ']'，否则报 InvalidPattern。
            if (rest.has_value() && !check(TokenType::RightBracket)) {
                error(ErrorCode::InvalidPattern, "rest 模式 '...' 必须位于列表末尾");
            }
        }
        expect(TokenType::RightBracket, "']'");
        return std::make_unique<ListPatternNode>(loc, std::move(elements), std::move(rest));
    }

    String Parser::rest_pattern() {
        expect(TokenType::DotDotDot, "'...'");
        if (check(TokenType::Underscore)) {
            // ..._ 与不写 rest 等价，冗余非法。
            error(ErrorCode::InvalidPattern, "rest 模式不接受 '_'（..._ 等价于不写 rest）");
        }
        return expect_identifier();
    }

} // namespace aria
