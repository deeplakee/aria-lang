#include <gtest/gtest.h>

#include "compile/ast.hpp"

// 不使用 `using namespace aria`：fs.hpp 在 Windows 下可能间接包含 windows.h，
// 其全局符号会与 aria 命名空间冲突（同 test_lexer.cpp 的处理）。按需显式引入。
// 节点类已加 Node 后缀；辅助值类型（Param/Match*/VarBinding/Map*）与枚举不加。
using aria::AssignmentNode;
using aria::ASTNode;
using aria::BinaryExprNode;
using aria::BlockNode;
using aria::BoolLiteralNode;
using aria::BreakStmtNode;
using aria::CallNode;
using aria::ContinueStmtNode;
using aria::DefDeclNode;
using aria::DefMember;
using aria::DestructureAssignmentNode;
using aria::ExprNode;
using aria::ExprStmtNode;
using aria::f64;
using aria::FieldAccessNode;
using aria::FloatLiteralNode;
using aria::ForInStmtNode;
using aria::ForStmtNode;
using aria::FunDeclNode;
using aria::i64;
using aria::IdentifierNode;
using aria::IdentifierPatternNode;
using aria::IfExprNode;
using aria::IfStmtNode;
using aria::ImportStmtNode;
using aria::IndexAccessNode;
using aria::IntegerLiteralNode;
using aria::LambdaExprNode;
using aria::List;
using aria::ListExprNode;
using aria::ListPatternNode;
using aria::MapEntry;
using aria::MapExprNode;
using aria::MatchArm;
using aria::MatchExprArm;
using aria::MatchExprNode;
using aria::MatchPattern;
using aria::MatchStmtNode;
using aria::NilLiteralNode;
using aria::Opt;
using aria::Param;
using aria::PatternNode;
using aria::PrintStmtNode;
using aria::ProgramNode;
using aria::ReturnStmtNode;
using aria::SourceLoc;
using aria::StmtNode;
using aria::String;
using aria::StringLiteralNode;
using aria::StringView;
using aria::SuperExprNode;
using aria::ThisExprNode;
using aria::ThrowStmtNode;
using aria::TryStmtNode;
using aria::UnaryExprNode;
using aria::UPtr;
using aria::usize;
using aria::VarBinding;
using aria::VarDeclNode;
using aria::WhileStmtNode;
using aria::WildcardPatternNode;

namespace {
    // 空位置：display 不依赖 loc，测试聚焦树形渲染与节点结构。
    const SourceLoc kLoc{};

    // 便利工厂。
    UPtr<IntegerLiteralNode>    i64lit(i64 v) { return std::make_unique<IntegerLiteralNode>(kLoc, v); }
    UPtr<IdentifierNode>        ident(String name) { return std::make_unique<IdentifierNode>(kLoc, std::move(name)); }
    UPtr<StringLiteralNode>     strlit(String v) { return std::make_unique<StringLiteralNode>(kLoc, std::move(v)); }
    UPtr<IdentifierPatternNode> id_pat(String name) {
        return std::make_unique<IdentifierPatternNode>(kLoc, std::move(name));
    }

    // 子串断言：避免引入 gmock（HasSubstr），用 String::find 手工检查。
    void expect_has(const String& haystack, StringView needle) {
        EXPECT_NE(haystack.find(needle), String::npos) << "missing: " << needle;
    }

    // 渲染整棵子树为字符串（dump(0) 的包装），便于对输出文本断言。
    // display() 是直接打印（无返回值），故测试用 dump_str 取字符串。
    String dump_str(const ASTNode& n) { return n.dump(0); }
} // namespace

// ---------------------------------------------------------------------------
// 运算符可读名
// ---------------------------------------------------------------------------

TEST(AstOpName, All) {
    EXPECT_EQ(aria::Op::to_string(aria::Op::Binary::Plus), StringView{"+"});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Binary::And), StringView{"&&"});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Binary::EqualEqual), StringView{"=="});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Binary::EqualEqualEqual), StringView{"==="});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Binary::BangEqualEqual), StringView{"!=="});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Unary::Not), StringView{"!"});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Unary::PreInc), StringView{"++"});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Assignment::PlusAssign), StringView{"+="});
    EXPECT_EQ(aria::Op::to_string(aria::Op::Assignment::Assign), StringView{"="});
}

// ---------------------------------------------------------------------------
// 字面量与基础表达式 dump
// ---------------------------------------------------------------------------

TEST(AstDisplay, Literals) {
    EXPECT_EQ(dump_str(*i64lit(42)), "IntegerLiteral 42\n");
    EXPECT_EQ(dump_str(*strlit("hi")), "StringLiteral \"hi\"\n");
    EXPECT_EQ(dump_str(BoolLiteralNode{kLoc, true}), "BoolLiteral true\n");
    EXPECT_EQ(dump_str(BoolLiteralNode{kLoc, false}), "BoolLiteral false\n");
    EXPECT_EQ(dump_str(NilLiteralNode{kLoc}), "NilLiteral\n");
    EXPECT_EQ(dump_str(*ident("x")), "Identifier x\n");
    EXPECT_EQ(dump_str(ThisExprNode{kLoc}), "ThisExpr\n");
    EXPECT_EQ(dump_str(SuperExprNode{kLoc}), "SuperExpr\n");
}

TEST(AstDisplay, FloatLiteral) {
    const String f = dump_str(FloatLiteralNode{kLoc, 3.14});
    EXPECT_NE(f.find("FloatLiteral"), String::npos);
    EXPECT_NE(f.find("3.14"), String::npos);
}

// ---------------------------------------------------------------------------
// 二元表达式：dump + 结构
// ---------------------------------------------------------------------------

TEST(AstBinary, DisplayAndStruct) {
    BinaryExprNode node{kLoc, aria::Op::Binary::Plus, i64lit(1), i64lit(2)};
    EXPECT_EQ(node.op, aria::Op::Binary::Plus);

    const String out = dump_str(node);
    EXPECT_EQ(out, "BinaryExpr op=+\n  IntegerLiteral 1\n  IntegerLiteral 2\n");

    // 结构：左右子节点为 IntegerLiteralNode，值正确（dynamic_cast 依赖 ASTNode 虚析构）。
    auto lhs = dynamic_cast<IntegerLiteralNode*>(node.lhs.get());
    auto rhs = dynamic_cast<IntegerLiteralNode*>(node.rhs.get());
    ASSERT_NE(lhs, nullptr);
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(lhs->value, 1);
    EXPECT_EQ(rhs->value, 2);
}

TEST(AstBinary, LogicOpName) {
    BinaryExprNode node{kLoc, aria::Op::Binary::And, ident("a"), ident("b")};
    EXPECT_NE(dump_str(node).find("BinaryExpr op=&&"), String::npos);
}

// ---------------------------------------------------------------------------
// 一元 / 赋值 / 解构赋值
// ---------------------------------------------------------------------------

TEST(AstDisplay, Unary) {
    UnaryExprNode node{kLoc, aria::Op::Unary::Minus, ident("x")};
    EXPECT_EQ(dump_str(node), "UnaryExpr op=-\n  Identifier x\n");
}

TEST(AstDisplay, Assignment) {
    AssignmentNode node{kLoc, aria::Op::Assignment::Assign, ident("x"), i64lit(5)};
    const String   out = dump_str(node);
    // header "op=" + 赋值符号；Assign 符号恰为 "="，故 "op=="
    EXPECT_EQ(out, "Assignment op==\n  Identifier x\n  IntegerLiteral 5\n");
}

TEST(AstDisplay, CompoundAssign) {
    AssignmentNode node{kLoc, aria::Op::Assignment::PlusAssign, ident("x"), i64lit(5)};
    // header 形如 "Assignment op=+="（op 名 "+=" 拼到 "op=" 之后）
    EXPECT_NE(dump_str(node).find("Assignment op=+="), String::npos);
}

TEST(AstDisplay, DestructureAssignment) {
    List<UPtr<PatternNode>> elems;
    elems.push_back(id_pat("a"));
    elems.push_back(id_pat("b"));
    auto                      target = std::make_unique<ListPatternNode>(kLoc, std::move(elems), Opt<String>{});
    DestructureAssignmentNode node{kLoc, std::move(target), ident("lst")};
    const String              out = dump_str(node);
    expect_has(out, "DestructureAssignment");
    expect_has(out, "ListPattern elements=2");
    expect_has(out, "Identifier lst");
}

// ---------------------------------------------------------------------------
// 调用 / 字段 / 下标
// ---------------------------------------------------------------------------

TEST(AstDisplay, Call) {
    List<UPtr<ExprNode>> args;
    args.push_back(i64lit(1));
    args.push_back(i64lit(2));
    CallNode     node{kLoc, ident("f"), std::move(args)};
    const String out = dump_str(node);
    EXPECT_EQ(out, "Call args=2\n  Identifier f\n  IntegerLiteral 1\n  IntegerLiteral 2\n");
}

TEST(AstDisplay, FieldAndIndex) {
    FieldAccessNode fa{kLoc, ident("o"), String{"field"}};
    EXPECT_EQ(dump_str(fa), "FieldAccess name=field\n  Identifier o\n");

    IndexAccessNode ia{kLoc, ident("a"), i64lit(0)};
    EXPECT_EQ(dump_str(ia), "IndexAccess\n  Identifier a\n  IntegerLiteral 0\n");
}

// ---------------------------------------------------------------------------
// 复合表达式
// ---------------------------------------------------------------------------

TEST(AstDisplay, ListExpr) {
    List<UPtr<ExprNode>> elems;
    elems.push_back(i64lit(1));
    elems.push_back(i64lit(2));
    ListExprNode node{kLoc, std::move(elems)};
    EXPECT_EQ(dump_str(node), "ListExpr elements=2\n  IntegerLiteral 1\n  IntegerLiteral 2\n");
}

TEST(AstDisplay, MapExpr) {
    List<MapEntry> entries;
    MapEntry       e;
    e.key   = ident("k");
    e.value = i64lit(1);
    entries.push_back(std::move(e));
    MapExprNode  node{kLoc, std::move(entries)};
    const String out = dump_str(node);
    expect_has(out, "MapExpr entries=1");
    expect_has(out, "MapEntry");
}

TEST(AstDisplay, IfExpr) {
    IfExprNode   node{kLoc, i64lit(1), i64lit(2), i64lit(3)};
    const String out = dump_str(node);
    EXPECT_EQ(out, "IfExpr\n  IntegerLiteral 1\n  IntegerLiteral 2\n  IntegerLiteral 3\n");
}

TEST(AstDisplay, LambdaExpr) {
    List<Param> params;
    Param       p;
    p.name = "x";
    params.push_back(std::move(p));

    List<UPtr<StmtNode>> body_stmts;
    body_stmts.push_back(std::make_unique<ReturnStmtNode>(kLoc, ident("x")));
    auto body = std::make_unique<BlockNode>(kLoc, std::move(body_stmts));

    LambdaExprNode node{kLoc, std::move(params), std::move(body)};
    const String   out = dump_str(node);
    expect_has(out, "LambdaExpr params=1");
    expect_has(out, "Param name=x");
    expect_has(out, "ReturnStmt");
    expect_has(out, "Identifier x");
}

TEST(AstDisplay, MatchExpr) {
    List<MatchExprArm> arms;
    MatchExprArm       a1;
    a1.pattern.value = i64lit(1);
    a1.body          = i64lit(10);
    arms.push_back(std::move(a1));
    MatchExprArm a2; // 默认 value=nullptr -> "_" 通配
    a2.body = i64lit(99);
    arms.push_back(std::move(a2));

    MatchExprNode node{kLoc, ident("x"), std::move(arms)};
    const String  out = dump_str(node);
    expect_has(out, "MatchExpr arms=2");
    expect_has(out, "MatchPattern");
    expect_has(out, "MatchPattern _ (wildcard)");
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

TEST(AstDisplay, BlockAndExprStmt) {
    List<UPtr<StmtNode>> stmts;
    stmts.push_back(std::make_unique<PrintStmtNode>(kLoc, i64lit(1)));
    BlockNode    block{kLoc, std::move(stmts)};
    const String out = dump_str(block);
    EXPECT_EQ(out, "Block stmts=1\n  PrintStmt\n    IntegerLiteral 1\n");
}

TEST(AstDisplay, IfStmt) {
    auto         then_b = std::make_unique<PrintStmtNode>(kLoc, i64lit(1));
    auto         else_b = std::make_unique<PrintStmtNode>(kLoc, i64lit(2));
    IfStmtNode   node{kLoc, ident("flag"), std::move(then_b), std::move(else_b)};
    const String out = dump_str(node);
    expect_has(out, "IfStmt");
    expect_has(out, "Identifier flag");
}

TEST(AstDisplay, ReturnStmt) {
    ReturnStmtNode with_val{kLoc, i64lit(7)};
    expect_has(dump_str(with_val), "ReturnStmt");
    expect_has(dump_str(with_val), "IntegerLiteral 7");

    ReturnStmtNode no_val{kLoc, nullptr};
    EXPECT_EQ(dump_str(no_val), "ReturnStmt\n");
}

TEST(AstDisplay, ImportStmt) {
    ImportStmtNode node{kLoc, String{"math"}, String{"m"}};
    EXPECT_EQ(dump_str(node), "ImportStmt path=math as=m\n");
}

TEST(AstDisplay, TryStmt) {
    List<UPtr<StmtNode>> try_body_stmts;
    try_body_stmts.push_back(std::make_unique<PrintStmtNode>(kLoc, i64lit(1)));
    auto try_body = std::make_unique<BlockNode>(kLoc, std::move(try_body_stmts));

    List<UPtr<StmtNode>> catch_body_stmts;
    catch_body_stmts.push_back(std::make_unique<PrintStmtNode>(kLoc, ident("e")));
    auto catch_body = std::make_unique<BlockNode>(kLoc, std::move(catch_body_stmts));

    TryStmtNode  node{kLoc, std::move(try_body), Opt<String>{String{"e"}}, std::move(catch_body), nullptr};
    const String out = dump_str(node);
    expect_has(out, "TryStmt");
    expect_has(out, "Catch param=e");
    expect_has(out, "Identifier e");
}

TEST(AstDisplay, ForInStmt) {
    List<UPtr<PatternNode>> elems;
    elems.push_back(id_pat("k"));
    elems.push_back(id_pat("v"));
    ForInStmtNode node{kLoc, std::make_unique<ListPatternNode>(kLoc, std::move(elems), Opt<String>{}), ident("m"),
                       std::make_unique<BreakStmtNode>(kLoc)};
    const String  out = dump_str(node);
    expect_has(out, "ForInStmt");
    expect_has(out, "ListPattern elements=2");
    expect_has(out, "BreakStmt");
}

TEST(AstDisplay, BreakContinue) {
    EXPECT_EQ(dump_str(BreakStmtNode{kLoc}), "BreakStmt\n");
    EXPECT_EQ(dump_str(ContinueStmtNode{kLoc}), "ContinueStmt\n");
}

// ---------------------------------------------------------------------------
// 声明 + Program
// ---------------------------------------------------------------------------

TEST(AstDisplay, VarDecl) {
    List<VarBinding> bindings;
    VarBinding       b;
    b.target      = id_pat("x");
    b.initializer = i64lit(1);
    bindings.push_back(std::move(b));

    VarDeclNode  node{kLoc, std::move(bindings)};
    const String out = dump_str(node);
    EXPECT_EQ(out, "VarDecl bindings=1\n  VarBinding\n    IdentifierPattern name=x\n    IntegerLiteral 1\n");
}

TEST(AstDisplay, FunDecl) {
    List<Param> params;
    Param       p;
    p.name = "x";
    params.push_back(std::move(p));

    List<UPtr<StmtNode>> body_stmts;
    body_stmts.push_back(std::make_unique<ReturnStmtNode>(kLoc, ident("x")));
    auto body = std::make_unique<BlockNode>(kLoc, std::move(body_stmts));

    FunDeclNode  node{kLoc, String{"id"}, std::move(params), std::move(body)};
    const String out = dump_str(node);
    expect_has(out, "FunDecl name=id params=1");
    expect_has(out, "ReturnStmt");
}

TEST(AstDisplay, DefDecl) {
    List<DefMember> members;
    // bark() { print "woof"; }  -- 实例方法（InstanceMethod）
    List<UPtr<StmtNode>> bark_body_stmts;
    bark_body_stmts.push_back(std::make_unique<PrintStmtNode>(kLoc, strlit("woof")));
    auto bark_body = std::make_unique<BlockNode>(kLoc, std::move(bark_body_stmts));
    members.push_back(DefMember{
            .kind = DefMember::Kind::InstanceMethod,
            .node = std::make_unique<FunDeclNode>(kLoc, String{"bark"}, List<Param>{}, std::move(bark_body))});

    DefDeclNode  node{kLoc, String{"Dog"}, Opt<String>{String{"Animal"}}, std::move(members)};
    const String out = dump_str(node);
    expect_has(out, "DefDecl name=Dog super=Animal");
    expect_has(out, "DefMember kind=InstanceMethod");
    expect_has(out, "FunDecl name=bark params=0");
    expect_has(out, "StringLiteral \"woof\"");
}

TEST(AstDisplay, Program) {
    // var x = 1; print x;
    List<UPtr<StmtNode>> decls;

    List<VarBinding> bindings;
    VarBinding       b;
    b.target      = id_pat("x");
    b.initializer = i64lit(1);
    bindings.push_back(std::move(b));
    decls.push_back(std::make_unique<VarDeclNode>(kLoc, std::move(bindings)));

    decls.push_back(std::make_unique<PrintStmtNode>(kLoc, ident("x")));

    ProgramNode  prog{kLoc, std::move(decls)};
    const String out = dump_str(prog);
    expect_has(out, "Program decls=2");
    expect_has(out, "VarDecl bindings=1");
    expect_has(out, "PrintStmt");
}

// ---------------------------------------------------------------------------
// 解构模式
// ---------------------------------------------------------------------------

TEST(AstDisplay, Patterns) {
    EXPECT_EQ(dump_str(*id_pat("a")), "IdentifierPattern name=a\n");
    EXPECT_EQ(dump_str(WildcardPatternNode{kLoc}), "WildcardPattern _\n");

    // [a, _, ...rest]
    List<UPtr<PatternNode>> elems;
    elems.push_back(id_pat("a"));
    elems.push_back(std::make_unique<WildcardPatternNode>(kLoc));
    ListPatternNode lp{kLoc, std::move(elems), Opt<String>{String{"rest"}}};
    const String    lp_out = dump_str(lp);
    expect_has(lp_out, "ListPattern elements=2 rest=rest");
    expect_has(lp_out, "IdentifierPattern name=a");
    expect_has(lp_out, "WildcardPattern _");
}

// ---------------------------------------------------------------------------
// 多态：通过基类指针调用 dump
// ---------------------------------------------------------------------------

TEST(AstPolymorphism, BasePointerDump) {
    UPtr<ExprNode> expr = i64lit(42); // UPtr<IntegerLiteralNode> -> UPtr<ExprNode>
    ASTNode*       node = expr.get();
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(dump_str(*node), "IntegerLiteral 42\n");
}
