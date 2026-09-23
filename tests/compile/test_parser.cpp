#include <gtest/gtest.h>

#include "compile/Lexer.hpp"
#include "compile/Parser.hpp"
#include "compile/ast.hpp"
#include "util/source_file.hpp"

// 不使用 using namespace aria：Windows SDK 全局符号（如 TokenType）会与 aria 冲突，
// 同 test_lexer.cpp / test_ast.cpp 的处理。按需显式引入。
using aria::AssignmentNode;
using aria::BinaryExprNode;
using aria::BlockNode;
using aria::CallNode;
using aria::DefDeclNode;
using aria::DestructureAssignmentNode;
using aria::Error;
using aria::ErrorCode;
using aria::ExprStmtNode;
using aria::FieldAccessNode;
using aria::ForInStmtNode;
using aria::ForStmtNode;
using aria::FunDeclNode;
using aria::IdentifierNode;
using aria::IdentifierPatternNode;
using aria::IfStmtNode;
using aria::IntegerLiteralNode;
using aria::LambdaExprNode;
using aria::Lexer;
using aria::List;
using aria::ListPatternNode;
using aria::MatchExprNode;
using aria::MatchStmtNode;
using aria::Param;
using aria::Parser;
using aria::ProgramNode;
using aria::RangeExprNode;
using aria::Result;
using aria::ReturnStmtNode;
using aria::SourceFile;
using aria::StmtNode;
using aria::String;
using aria::StringLiteralNode;
using aria::StringView;
using aria::TryStmtNode;
using aria::UnaryExprNode;
using aria::UPtr;
using aria::usize;
using aria::VarDeclNode;
using aria::WhileStmtNode;

namespace {
    // 持有 SourceFile 与解析结果：SourceFile 必须存活至 AST 使用结束
    // （AST 节点的 SourceLoc::src 指向它，同 Lexer 的生命周期约束）。
    struct Parsed {
        SourceFile                             sf;
        Result<UPtr<ProgramNode>, List<Error>> result;
    };

    // 词法 + 语法分析。SourceFile 就位后再 tokenize（避免 SSO 短串 move 后 view 悬空）。
    UPtr<Parsed> parse_src(const StringView src) {
        auto p   = std::make_unique<Parsed>();
        p->sf    = SourceFile{String{"t"}, String{"t"}, String{src}};
        auto lex = Lexer::tokenize(p->sf);
        if (!lex) {
            // 词法错误直接作为解析失败返回（测试用源码应词法合法）。
            p->result = std::unexpected(std::move(lex.error()));
            return p;
        }
        p->result = Parser::parse(std::move(*lex));
        return p;
    }

    // 断言解析成功，返回程序 dump 文本（独立 String，不依赖 SourceFile 存活）。
    String dump_ok(const StringView src) {
        auto p = parse_src(src);
        EXPECT_TRUE(p->result.has_value()) << "期望解析成功: " << src;
        if (!p->result) {
            return {};
        }
        return (*p->result)->dump(0);
    }

    void expect_has(const String& haystack, const StringView needle) {
        EXPECT_NE(haystack.find(needle), String::npos) << "missing: " << needle;
    }

    // 取程序的第一个顶层声明（已断言解析成功且有声明）。
    const StmtNode* first_decl(const UPtr<Parsed>& p) {
        EXPECT_TRUE(p->result.has_value());
        auto& decls = (*p->result)->declarations;
        EXPECT_FALSE(decls.empty());
        return decls.empty() ? nullptr : decls[0].get();
    }
} // namespace

// ---------------------------------------------------------------------------
// 空程序 / 字面量 / 基础表达式
// ---------------------------------------------------------------------------

TEST(ParserBasic, EmptyProgram) {
    auto p = parse_src("");
    ASSERT_TRUE(p->result.has_value());
    EXPECT_EQ((*p->result)->declarations.size(), 0u);
}

TEST(ParserBasic, EmptyProgramTrivia) {
    auto p = parse_src("// only a comment\n  \n");
    ASSERT_TRUE(p->result.has_value());
    EXPECT_EQ((*p->result)->declarations.size(), 0u);
}

TEST(ParserBasic, Literals) {
    expect_has(dump_ok("42;"), "IntegerLiteral 42");
    expect_has(dump_ok("3.14;"), "FloatLiteral");
    expect_has(dump_ok("\"hi\";"), "StringLiteral \"hi\"");
    expect_has(dump_ok("true;"), "BoolLiteral true");
    expect_has(dump_ok("false;"), "BoolLiteral false");
    expect_has(dump_ok("nil;"), "NilLiteral");
    expect_has(dump_ok("x;"), "Identifier x");
    expect_has(dump_ok("this;"), "ThisExpr");
}

TEST(ParserBasic, ParenDoesNotMakeNode) {
    // parenExpr 不设独立节点，AST 保留内层表达式
    const String out = dump_ok("(42);");
    EXPECT_NE(out.find("IntegerLiteral 42"), String::npos);
    EXPECT_EQ(out.find("Paren", 0), String::npos);
}

// ---------------------------------------------------------------------------
// 二元运算：优先级与结合性
// ---------------------------------------------------------------------------

TEST(ParserBinary, Precedence) {
    // 1 + 2 * 3 -> 1 + (2 * 3)
    auto p = parse_src("1 + 2 * 3;");
    ASSERT_TRUE(p->result.has_value());
    auto stmt = first_decl(p);
    ASSERT_NE(stmt, nullptr);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    auto top = dynamic_cast<const BinaryExprNode*>(expr_stmt->expr.get());
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->op, aria::Op::Binary::Plus); // 顶层为 +
    auto rhs = dynamic_cast<const BinaryExprNode*>(top->rhs.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->op, aria::Op::Binary::Star); // 右子为 *
}

TEST(ParserBinary, LeftAssociative) {
    // 1 - 2 - 3 -> (1 - 2) - 3
    auto p         = parse_src("1 - 2 - 3;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    auto top       = dynamic_cast<const BinaryExprNode*>(expr_stmt->expr.get());
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->op, aria::Op::Binary::Minus);
    auto lhs = dynamic_cast<const BinaryExprNode*>(top->lhs.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->op, aria::Op::Binary::Minus); // 左子亦为 -（左结合）
}

TEST(ParserBinary, Logic) {
    // a && b || c -> (a && b) || c（&& 优先级高于 ||）
    auto p         = parse_src("a && b || c;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    auto top = dynamic_cast<const BinaryExprNode*>(expr_stmt->expr.get());
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->op, aria::Op::Binary::Or); // 外层 ||
    auto lhs = dynamic_cast<const BinaryExprNode*>(top->lhs.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->op, aria::Op::Binary::And); // 内层 &&
}

TEST(ParserBinary, ComparisonAndEquality) {
    expect_has(dump_ok("a == b;"), "op===");
    expect_has(dump_ok("a != b;"), "op=!=");
    expect_has(dump_ok("a >= b;"), "op=>=");
    expect_has(dump_ok("a < b;"), "op=<");
    // === / !== 严格相等:dump 为 "op=" + 运算符(==="===" -> "op====";!=="!==" -> "op=!==")
    expect_has(dump_ok("a === b;"), "op====");
    expect_has(dump_ok("a !== b;"), "op=!==");
}

// ---------------------------------------------------------------------------
// 区间
// ---------------------------------------------------------------------------

TEST(ParserRange, Inclusive) {
    // 1..10 -> RangeExpr inclusive，lower=1，upper=10
    auto p         = parse_src("1..10;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    auto r = dynamic_cast<const RangeExprNode*>(expr_stmt->expr.get());
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(r->is_exclusive);
    auto lo = dynamic_cast<const IntegerLiteralNode*>(r->lower.get());
    ASSERT_NE(lo, nullptr);
    EXPECT_EQ(lo->value, 1);
    auto hi = dynamic_cast<const IntegerLiteralNode*>(r->upper.get());
    ASSERT_NE(hi, nullptr);
    EXPECT_EQ(hi->value, 10);
}

TEST(ParserRange, Exclusive) {
    // 1...10 -> RangeExpr exclusive
    auto p         = parse_src("1...10;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    auto r = dynamic_cast<const RangeExprNode*>(expr_stmt->expr.get());
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->is_exclusive);
}

TEST(ParserRange, PrecedenceWithAdditive) {
    // 1..n+1 -> 1..(n+1)：加减比区间紧
    auto p         = parse_src("1..n+1;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    auto r = dynamic_cast<const RangeExprNode*>(expr_stmt->expr.get());
    ASSERT_NE(r, nullptr);
    // 上界应为 n+1（BinaryExpr Plus），非裸 n
    auto hi = dynamic_cast<const BinaryExprNode*>(r->upper.get());
    ASSERT_NE(hi, nullptr);
    EXPECT_EQ(hi->op, aria::Op::Binary::Plus);
}

TEST(ParserRange, NonAssociative) {
    // a..b..c 非法（非结合）
    auto p = parse_src("a..b..c;");
    EXPECT_FALSE(p->result.has_value());
}

// ---------------------------------------------------------------------------
// 一元 / 后缀
// ---------------------------------------------------------------------------

TEST(ParserUnary, Prefix) {
    expect_has(dump_ok("-x;"), "UnaryExpr op=-");
    expect_has(dump_ok("!x;"), "UnaryExpr op=!");
    expect_has(dump_ok("++x;"), "UnaryExpr op=++");
    expect_has(dump_ok("--x;"), "UnaryExpr op=--");
}

TEST(ParserUnary, RightAssociative) {
    // -- -x -> --(-x)
    auto p         = parse_src("-- -x;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    auto outer     = dynamic_cast<const UnaryExprNode*>(expr_stmt->expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, aria::Op::Unary::PreDec);
    auto inner = dynamic_cast<const UnaryExprNode*>(outer->operand.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, aria::Op::Unary::Minus);
}

TEST(ParserPostfix, Call) {
    const String out = dump_ok("f(1, 2);");
    expect_has(out, "Call args=2");
    expect_has(out, "Identifier f");
}

TEST(ParserPostfix, FieldAndIndex) {
    expect_has(dump_ok("o.field;"), "FieldAccess name=field");
    expect_has(dump_ok("a[0];"), "IndexAccess");
}

TEST(ParserPostfix, Chained) {
    // obj.method(1).field[2]
    const String out = dump_ok("obj.method(1).field[2];");
    expect_has(out, "FieldAccess name=method");
    expect_has(out, "Call args=1");
    expect_has(out, "FieldAccess name=field");
    expect_has(out, "IndexAccess");
}

TEST(ParserPostfix, SuperMethodCall) {
    // super.foo() -> SuperExpr（完整形态一处收口）-> Call（super.m(args) 调用无 FieldAccess 层）
    const String out = dump_ok("super.foo();");
    expect_has(out, "SuperExpr name=foo");
    expect_has(out, "Call args=0");
    EXPECT_EQ(out.find("FieldAccess"), String::npos);
}

// ---------------------------------------------------------------------------
// 赋值（普通 / 复合 / 解构）
// ---------------------------------------------------------------------------

TEST(ParserAssignment, Plain) {
    const String out = dump_ok("x = 5;");
    expect_has(out, "Assignment op==");
}

TEST(ParserAssignment, Compound) {
    expect_has(dump_ok("x += 1;"), "Assignment op=+=");
    expect_has(dump_ok("x -= 1;"), "Assignment op=-=");
    expect_has(dump_ok("x *= 1;"), "Assignment op=*=");
    expect_has(dump_ok("x /= 1;"), "Assignment op=/=");
    expect_has(dump_ok("x %= 1;"), "Assignment op=%=");
}

TEST(ParserAssignment, RightAssociative) {
    // a = b = 1
    const String out = dump_ok("a = b = 1;");
    expect_has(out, "Assignment");
}

TEST(ParserAssignment, ChainedCompoundRightAssoc) {
    // a += b += c  ==>  a += (b += c)：复合赋值与 = 同族，右结合，可链式。
    auto p         = parse_src("a += b += c;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    auto outer = dynamic_cast<const AssignmentNode*>(expr_stmt->expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, aria::Op::Assignment::PlusAssign);
    auto a = dynamic_cast<const IdentifierNode*>(outer->target.get());
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->name, "a");
    // 右结合：外层 value 为内层赋值节点
    auto inner = dynamic_cast<const AssignmentNode*>(outer->value.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, aria::Op::Assignment::PlusAssign);
    auto b = dynamic_cast<const IdentifierNode*>(inner->target.get());
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->name, "b");
    auto c = dynamic_cast<const IdentifierNode*>(inner->value.get());
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "c");
}

TEST(ParserAssignment, Destructure) {
    auto p         = parse_src("[a, b] = lst;");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    auto da = dynamic_cast<const DestructureAssignmentNode*>(expr_stmt->expr.get());
    ASSERT_NE(da, nullptr);
    expect_has(da->dump(0), "ListPattern elements=2");
    expect_has(da->dump(0), "Identifier lst");
}

TEST(ParserAssignment, DestructureWithRest) {
    const String out = dump_ok("[a, ...rest] = lst;");
    expect_has(out, "ListPattern elements=1 rest=rest");
}

TEST(ParserAssignment, ListExprNotDestructure) {
    // [1, 2] 作为列表表达式（非解构目标），不应产生 DestructureAssignment
    auto p         = parse_src("[1, 2];");
    auto stmt      = first_decl(p);
    auto expr_stmt = dynamic_cast<const ExprStmtNode*>(stmt);
    ASSERT_NE(expr_stmt, nullptr);
    EXPECT_EQ(dynamic_cast<const DestructureAssignmentNode*>(expr_stmt->expr.get()), nullptr);
    expect_has(expr_stmt->expr->dump(0), "ListExpr elements=2");
}

// ---------------------------------------------------------------------------
// 复合表达式
// ---------------------------------------------------------------------------

TEST(ParserCompound, ListExpr) {
    const String out = dump_ok("[1, 2, 3];");
    expect_has(out, "ListExpr elements=3");
}

TEST(ParserCompound, EmptyList) { expect_has(dump_ok("[];"), "ListExpr elements=0"); }

TEST(ParserCompound, MapExpr) {
    // map 字面量须处于表达式位置：语句位置的 '{' 一律作 block（文法上下文消歧）。
    const String out = dump_ok("var m = {\"k\": 1, \"v\": 2};");
    expect_has(out, "MapExpr entries=2");
    expect_has(out, "MapEntry");
}

TEST(ParserCompound, IfExpr) {
    const String out = dump_ok("var x = if (c) {1} else {2};");
    expect_has(out, "IfExpr");
    expect_has(out, "IntegerLiteral 1");
    expect_has(out, "IntegerLiteral 2");
}

TEST(ParserCompound, Lambda) {
    const String out = dump_ok("var f = fun(x) { return x; };");
    expect_has(out, "LambdaExpr params=1");
    expect_has(out, "Param name=x");
    expect_has(out, "ReturnStmt");
}

TEST(ParserCompound, MatchExpr) {
    const String out = dump_ok("var r = match (x) { 1 => 10, _ => 0 };");
    expect_has(out, "MatchExpr arms=2");
    expect_has(out, "MatchPattern _ (wildcard)");
}

// ---------------------------------------------------------------------------
// 声明
// ---------------------------------------------------------------------------

TEST(ParserDecl, VarSingle) {
    const String out = dump_ok("var x = 1;");
    EXPECT_EQ(out, "Program decls=1\n  VarDecl bindings=1\n    VarBinding\n      IdentifierPattern name=x\n      "
                   "IntegerLiteral 1\n");
}

TEST(ParserDecl, VarMultiple) {
    const String out = dump_ok("var a, b = 2, c;");
    expect_has(out, "VarDecl bindings=3");
}

TEST(ParserDecl, VarDestructure) {
    const String out = dump_ok("var [a, b] = lst;");
    expect_has(out, "ListPattern elements=2");
}

TEST(ParserDecl, VarWildcard) {
    const String out = dump_ok("var _ = expr;");
    expect_has(out, "WildcardPattern _");
}

TEST(ParserDecl, VarPatternWithRestAndHole) {
    // var [a, _, ...rest] = lst;
    const String out = dump_ok("var [a, _, ...rest] = lst;");
    expect_has(out, "ListPattern elements=2 rest=rest");
    expect_has(out, "WildcardPattern _");
}

TEST(ParserDecl, Fun) {
    const String out = dump_ok("fun id(x) { return x; }");
    expect_has(out, "FunDecl name=id params=1 kind=Function");
    expect_has(out, "ReturnStmt");
}

TEST(ParserDecl, FunNoParams) {
    const String out = dump_ok("fun f() { println(1); }");
    expect_has(out, "FunDecl name=f params=0 kind=Function");
}

TEST(ParserDecl, FunDefaultParams) {
    const String out = dump_ok("fun f(a, b = 2, c = 3) { }");
    expect_has(out, "FunDecl name=f params=3 kind=Function");
    // b、c 有默认值（dump 中 Param 下挂 default 子树）
    expect_has(out, "IntegerLiteral 2");
    expect_has(out, "IntegerLiteral 3");
}

TEST(ParserDecl, FunVarargs) {
    const String out = dump_ok("fun f(a, ...rest) { }");
    expect_has(out, "Param name=rest (varargs)");
}

TEST(ParserDecl, FunVarargsOnly) {
    const String out = dump_ok("fun f(...args) { }");
    expect_has(out, "Param name=args (varargs)");
}

TEST(ParserDecl, DefWithSuper) {
    const String out = dump_ok("def Dog : Animal { bark() { println(\"woof\"); } }");
    expect_has(out, "DefDecl name=Dog super=Animal");
    expect_has(out, "FunDecl name=bark params=0 kind=Method");
    expect_has(out, "StringLiteral \"woof\"");
}

TEST(ParserDecl, DefNoSuper) {
    const String out = dump_ok("def Empty { }");
    expect_has(out, "DefDecl name=Empty");
}

TEST(ParserDecl, DefMultipleInstanceMethods) {
    // 实例方法：裸 identifier（无 fun 关键字），按文法 function -> identifier params block
    const String out = dump_ok("def C { a() { } b(x) { } }");
    expect_has(out, "FunDecl name=a params=0 kind=Method");
    expect_has(out, "FunDecl name=b params=1 kind=Method");
}

TEST(ParserDecl, DefStaticVarAndStaticMethod) {
    // var -> 静态变量；fun name() -> 静态方法；裸 name() -> 实例方法
    const String out = dump_ok("def C { var x = 1; fun s() { } m() { } }");
    expect_has(out, "StaticVarMember name=x");
    expect_has(out, "FunDecl name=s params=0 kind=StaticMethod");
    expect_has(out, "FunDecl name=m params=0 kind=Method");
}

TEST(ParserDecl, DefInitMethodStamping) {
    // 裸方法名为 init -> InitMethod 构造角色（返回尾返回 this）；fun init() 仍为静态方法。
    const String out = dump_ok("def C { init() { } fun init() { } }");
    expect_has(out, "FunDecl name=init params=0 kind=InitMethod");
    expect_has(out, "FunDecl name=init params=0 kind=StaticMethod");
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

TEST(ParserStmt, IfElse) {
    const String out = dump_ok("if (c) println(1); else println(2);");
    expect_has(out, "IfStmt");
    expect_has(out, "ExprStmt");
}

TEST(ParserStmt, IfNoElse) {
    auto p    = parse_src("if (c) println(1);");
    auto stmt = first_decl(p);
    auto ifn  = dynamic_cast<const IfStmtNode*>(stmt);
    ASSERT_NE(ifn, nullptr);
    EXPECT_EQ(ifn->else_branch, nullptr);
}

TEST(ParserStmt, While) { expect_has(dump_ok("while (c) println(1);"), "WhileStmt"); }

TEST(ParserStmt, ForCStyle) {
    auto p    = parse_src("for (var i = 0; i < 10; i = i + 1) println(i);");
    auto stmt = first_decl(p);
    auto forn = dynamic_cast<const ForStmtNode*>(stmt);
    ASSERT_NE(forn, nullptr);
    EXPECT_NE(forn->init, nullptr);
    EXPECT_NE(forn->condition, nullptr);
    EXPECT_NE(forn->increment, nullptr);
}

TEST(ParserStmt, ForEmpty) {
    auto p    = parse_src("for (;;) println(1);");
    auto stmt = first_decl(p);
    auto forn = dynamic_cast<const ForStmtNode*>(stmt);
    ASSERT_NE(forn, nullptr);
    EXPECT_EQ(forn->init, nullptr);
    EXPECT_EQ(forn->condition, nullptr);
    EXPECT_EQ(forn->increment, nullptr);
}

TEST(ParserStmt, ForInSingle) {
    auto p    = parse_src("for (k in m) println(k);");
    auto stmt = first_decl(p);
    auto fin  = dynamic_cast<const ForInStmtNode*>(stmt);
    ASSERT_NE(fin, nullptr);
    ASSERT_NE(fin->pattern, nullptr);
    auto pat = dynamic_cast<const IdentifierPatternNode*>(fin->pattern.get());
    ASSERT_NE(pat, nullptr);
    EXPECT_EQ(pat->name, String{"k"});
}

TEST(ParserStmt, ForInMulti) {
    const String out = dump_ok("for ([k, v] in m) println(k);");
    expect_has(out, "ForInStmt");
    expect_has(out, "ListPattern elements=2");
    expect_has(out, "IdentifierPattern name=k");
    expect_has(out, "IdentifierPattern name=v");
}

TEST(ParserStmt, ForInPatternVariants) {
    // forIn 目标支持完整 pattern："_" 占位、listPattern + rest。
    const String w = dump_ok("for (_ in xs) println(1);");
    expect_has(w, "ForInStmt");
    expect_has(w, "WildcardPattern _");

    const String r = dump_ok("for ([a, ...rest] in pairs) println(1);");
    expect_has(r, "ForInStmt");
    expect_has(r, "ListPattern elements=1 rest=rest");
}

TEST(ParserStmt, ForCStyleExprInit) {
    // forStmt init 支持裸表达式：for (i = 0; ...) 走 exprStmt init。
    auto p    = parse_src("for (i = 0; i < 10; i = i + 1) println(i);");
    auto stmt = first_decl(p);
    auto forn = dynamic_cast<const ForStmtNode*>(stmt);
    ASSERT_NE(forn, nullptr);
    EXPECT_NE(forn->init, nullptr);
    EXPECT_NE(forn->condition, nullptr);
    EXPECT_NE(forn->increment, nullptr);
}

TEST(ParserStmt, ForCStyleDestructureInit) {
    // [...] 不跟 in -> forStmt 的解构赋值 exprStmt init（验证 [ 分流不走 forIn）。
    auto p    = parse_src("for ([a, b] = c; a < b; a = a + 1) println(a);");
    auto stmt = first_decl(p);
    auto forn = dynamic_cast<const ForStmtNode*>(stmt);
    ASSERT_NE(forn, nullptr);
    EXPECT_NE(forn->init, nullptr);
    EXPECT_NE(forn->condition, nullptr);
    EXPECT_NE(forn->increment, nullptr);
}

TEST(ParserStmt, BreakContinueReturn) {
    expect_has(dump_ok("break;"), "BreakStmt");
    expect_has(dump_ok("continue;"), "ContinueStmt");
    expect_has(dump_ok("return 1;"), "ReturnStmt");
    // 裸 return（无值）
    auto p    = parse_src("return;");
    auto stmt = first_decl(p);
    auto ret  = dynamic_cast<const ReturnStmtNode*>(stmt);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->value, nullptr);
}

TEST(ParserStmt, Import) {
    const String out = dump_ok("import \"math\" as m;");
    EXPECT_NE(out.find("ImportStmt path=math as=m"), String::npos);
}

TEST(ParserStmt, Throw) { expect_has(dump_ok("throw e;"), "ThrowStmt"); }

TEST(ParserStmt, TryCatch) {
    const String out = dump_ok("try { println(1); } catch (e) { println(e); }");
    expect_has(out, "TryStmt");
    expect_has(out, "Catch param=e");
    // finally 非关键字:解析器不产生 finally 子句节点
    EXPECT_EQ(out.find("Finally"), String::npos);
}

TEST(ParserStmt, TryWithoutCatchParses) {
    const String out = dump_ok("try { println(1); }");
    expect_has(out, "TryStmt");
    // parse 层允许无 catch（CodeGen 期才校验 TryWithoutHandler）
    EXPECT_EQ(out.find("Catch"), String::npos);
}

TEST(ParserStmt, FinallyIsPlainIdentifier) {
    // finally 是普通标识符，可作变量名
    const String out = dump_ok("var finally = 1; println(finally);");
    expect_has(out, "VarDecl bindings=1");
}

TEST(ParserStmt, MatchStmt) {
    const String out = dump_ok("match (x) { 1 => println(1); _ => println(0); }");
    expect_has(out, "MatchStmt arms=2");
    expect_has(out, "MatchPattern _ (wildcard)");
}

TEST(ParserStmt, Block) {
    const String out = dump_ok("{ var x = 1; println(x); }");
    expect_has(out, "Block stmts=2");
}

TEST(ParserStmt, NestedBlocksAndScope) {
    const String out = dump_ok("fun f() { if (c) { println(1); } }");
    expect_has(out, "FunDecl name=f params=0 kind=Function");
    expect_has(out, "IfStmt");
    expect_has(out, "Block stmts=1");
}

// ---------------------------------------------------------------------------
// 错误处理
// ---------------------------------------------------------------------------

TEST(ParserError, ExpectedIdentifier) {
    // var 后直接 '='（缺绑定目标）
    auto p = parse_src("var = 1;");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedIdentifier);
}

TEST(ParserError, DefBodyFunMustBeNamed) {
    // def 体内 fun 后须跟方法名；fun '('（lambda）不得作成员，由 fun_decl 的
    // expect_identifier 兜底报 ExpectedIdentifier（无须 def_decl 显式 check_next）。
    auto p = parse_src("def C { fun () { } }");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedIdentifier);
}

TEST(ParserError, DefBodyVarMultiBindingRejected) {
    // memberVar 窄形态：var 后仅单标识符绑定；语句级 varDecl 的多绑定在成员位不收，
    // member_var 就地报 ExpectedToken（解析期即拒）。
    auto p = parse_src("def C { var a = 1, b = 2; }");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedToken);
}

TEST(ParserError, DefBodyVarPatternRejected) {
    // 解构 pattern 不是合法成员：memberVar 只收标识符。
    auto p = parse_src("def C { var [a, b] = f(); }");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedIdentifier);
}

TEST(ParserError, SuperRequiresDotMember) {
    // superExpr 单形（super "." identifier）：裸 super 文法不收，解析期 ExpectedToken。
    auto p = parse_src("var x = super;");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedToken);
}

TEST(ParserError, ExpectedExpression) {
    auto p = parse_src("1 + ;");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedExpression);
}

TEST(ParserError, LowerUnboundedRangeRejected) {
    // range 只可省上界（term ( (".."|"...") term? )?）：无下界形态在表达式起点即拒。
    auto p = parse_src("var xs = [1, 2, 3];\nvar s = xs[..2];");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedExpression);
}

TEST(ParserError, UnexpectedEof) {
    // fun foo() { 未闭合
    auto p = parse_src("fun foo() {");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::UnexpectedEof);
}

TEST(ParserError, EofReportsClosingDelimiter) {
    // 闭合符缺失遇 EOF 时，应报「期望 <闭合符>」而非「期望表达式/标识符」
    // （入口处的 !is_at_end() 守卫让控制流落到 expect(闭合符)）。
    // return; 合法 -> return 后 EOF 报「期望 ';'」而非「期望表达式」。
    {
        auto p = parse_src("return");
        ASSERT_FALSE(p->result.has_value());
        ASSERT_FALSE(p->result.error().empty());
        EXPECT_EQ(p->result.error()[0].code(), ErrorCode::UnexpectedEof);
        EXPECT_NE(p->result.error()[0].message().find("';'"), String::npos);
    }
    // fun f( 后 EOF（本意空参列表 ()）-> 报「期望 ')'」而非「期望标识符」。
    {
        auto p = parse_src("fun f(");
        ASSERT_FALSE(p->result.has_value());
        ASSERT_FALSE(p->result.error().empty());
        EXPECT_EQ(p->result.error()[0].code(), ErrorCode::UnexpectedEof);
        EXPECT_NE(p->result.error()[0].message().find("')'"), String::npos);
    }
    // var x = [ 后 EOF（本意空列表 []）-> 报「期望 ']'」而非「期望表达式」。
    {
        auto p = parse_src("var x = [");
        ASSERT_FALSE(p->result.has_value());
        ASSERT_FALSE(p->result.error().empty());
        EXPECT_EQ(p->result.error()[0].code(), ErrorCode::UnexpectedEof);
        EXPECT_NE(p->result.error()[0].message().find("']'"), String::npos);
    }
}

TEST(ParserError, ExpectedTokenSemicolon) {
    // println(1 后缺 ');'，下一个 token 是标识符 x（非 EOF）-> ExpectedToken。
    // （若在 EOF 处缺 ';'，expect 会报 UnexpectedEof，属另一码。）
    auto p = parse_src("println(1 x);");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedToken);
}

TEST(ParserError, MatchExprArmSeparator) {
    // matchExpr 臂间强制 ','：漏写报 ExpectedToken 且消息指向 ','。
    {
        auto p = parse_src("var x = match (1) { 1 => 10 _ => 0 };");
        ASSERT_FALSE(p->result.has_value());
        ASSERT_FALSE(p->result.error().empty());
        EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedToken);
        EXPECT_NE(p->result.error()[0].message().find("','"), String::npos);
    }
    // 尾逗号不允许：',' 后无臂可解析，报 ExpectedExpression。
    {
        auto p = parse_src("var x = match (1) { 1 => 10, };");
        ASSERT_FALSE(p->result.has_value());
        ASSERT_FALSE(p->result.error().empty());
        EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedExpression);
    }
}

TEST(ParserError, DefaultAfterPlain) {
    auto p = parse_src("fun f(a, b = 1, c) { }");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::DefaultAfterPlain);
}

TEST(ParserError, VarargsNotLast) {
    auto p = parse_src("fun f(...a, b) { }");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::VarargsNotLast);
}

TEST(ParserError, PatternRestNotLast) {
    // var 声明中 rest 不在末尾
    auto p = parse_src("var [a, ...r, b] = lst;");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::InvalidPattern);
}

TEST(ParserError, PatternRestUnderscore) {
    auto p = parse_src("var [..._] = lst;");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::InvalidPattern);
}

// ---------------------------------------------------------------------------
// 错误恢复：收集多个错误
// ---------------------------------------------------------------------------

TEST(ParserRecovery, MultipleErrorsCollected) {
    // 两条 var 声明均有错误，应收集到 >= 2 个错误
    auto p = parse_src("var = 1;\nvar = 2;\n");
    ASSERT_FALSE(p->result.has_value());
    EXPECT_GE(p->result.error().size(), 2u);
}

TEST(ParserRecovery, RecoversAndContinues) {
    // 第一条 var 缺目标（错），第二条合法：错误后同步应能继续解析第二条
    auto p = parse_src("var = 1;\nvar y = 2;\n");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_GE(p->result.error().size(), 1u);
    EXPECT_EQ(p->result.error()[0].code(), ErrorCode::ExpectedIdentifier);
}

// ---------------------------------------------------------------------------
// 错误位置：携带源码行列
// ---------------------------------------------------------------------------

TEST(ParserError, ErrorHasLocation) {
    // 第一行缺 ';'：解析器在第二行 'var' 处发现非预期 token，错误位置指向该 token（行 2）。
    auto p = parse_src("var x = 1\nvar y = 2;");
    ASSERT_FALSE(p->result.has_value());
    ASSERT_FALSE(p->result.error().empty());
    const Error& e = p->result.error()[0];
    // Error 构造期已把位置烘进完整消息串 message()（含 "path:line:col" 前缀），不再持 SourceFile*。
    // parse_src 用 "t" 作源名，错误指向行 2 -> 消息串含 "t:2:"。
    EXPECT_NE(e.message().find("t:2:"), String::npos);
}
