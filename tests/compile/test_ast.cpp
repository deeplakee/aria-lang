#include <gtest/gtest.h>

#include "compile/Ast.hpp"
#include "memory/AstArena.hpp"

// 不使用 `using namespace aria`：fs.hpp 在 Windows 下可能间接包含 windows.h，
// 其全局符号会与 aria 命名空间冲突（同 test_lexer.cpp 的处理）。按需显式引入。
using aria::AssignmentNode;
using aria::AstArena;
using aria::ASTNode;
using aria::BinaryExprNode;
using aria::BlockNode;
using aria::BoolLiteralNode;
using aria::BreakStmtNode;
using aria::CallNode;
using aria::ContinueStmtNode;
using aria::DefDeclNode;
using aria::DestructureAssignmentNode;
using aria::ExprNode;
using aria::ExprStmtNode;
using aria::f64;
using aria::FieldAccessNode;
using aria::FloatLiteralNode;
using aria::FnKind;
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
using aria::ProgramNode;
using aria::ReturnStmtNode;
using aria::SourceLoc;
using aria::Span;
using aria::StmtNode;
using aria::String;
using aria::StringLiteralNode;
using aria::StringShape;
using aria::StringView;
using aria::SuperExprNode;
using aria::ThisExprNode;
using aria::ThrowStmtNode;
using aria::TryStmtNode;
using aria::u32;
using aria::UnaryExprNode;
using aria::usize;
using aria::VarBinding;
using aria::VarDeclNode;
using aria::WhileStmtNode;
using aria::WildcardPatternNode;

namespace {
    // 空位置：display 不依赖 loc，测试聚焦树形渲染与节点结构。
    const SourceLoc kLoc{};

    // 便利工厂：节点分配自测试局部 arena，返回裸指针（arena 拥有内存）。
    IntegerLiteralNode* i64lit(AstArena& arena, const i64 v) { return arena.make<IntegerLiteralNode>(kLoc, v); }
    IdentifierNode*     ident(AstArena& arena, const StringView name) { return arena.make<IdentifierNode>(kLoc, name); }
    StringLiteralNode*  strlit(AstArena& arena, const StringView v) {
        return arena.make<StringLiteralNode>(kLoc, v, StringShape{static_cast<u32>(v.size()), false});
    }
    IdentifierPatternNode* id_pat(AstArena& arena, const StringView name) {
        return arena.make<IdentifierPatternNode>(kLoc, name);
    }

    // 子串断言：避免引入 gmock（HasSubstr），用 String::find 手工检查。
    void expect_has(const String& haystack, const StringView needle) {
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
    AstArena arena;
    EXPECT_EQ(dump_str(*i64lit(arena, 42)), "IntegerLiteral 42\n");
    EXPECT_EQ(dump_str(*strlit(arena, "hi")), "StringLiteral \"hi\"\n");
    EXPECT_EQ(dump_str(BoolLiteralNode{kLoc, true}), "BoolLiteral true\n");
    EXPECT_EQ(dump_str(BoolLiteralNode{kLoc, false}), "BoolLiteral false\n");
    EXPECT_EQ(dump_str(NilLiteralNode{kLoc}), "NilLiteral\n");
    EXPECT_EQ(dump_str(*ident(arena, "x")), "Identifier x\n");
    EXPECT_EQ(dump_str(ThisExprNode{kLoc}), "ThisExpr\n");
    EXPECT_EQ(dump_str(SuperExprNode{kLoc, StringView{"m"}}), "SuperExpr name=m\n");
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
    AstArena       arena;
    BinaryExprNode node{kLoc, aria::Op::Binary::Plus, i64lit(arena, 1), i64lit(arena, 2)};
    EXPECT_EQ(node.op, aria::Op::Binary::Plus);

    const String out = dump_str(node);
    EXPECT_EQ(out, "BinaryExpr op=+\n  IntegerLiteral 1\n  IntegerLiteral 2\n");

    // 结构：左右子节点为 IntegerLiteralNode，值正确（dynamic_cast 依赖 ASTNode 虚析构）。
    auto lhs = dynamic_cast<IntegerLiteralNode*>(node.lhs);
    auto rhs = dynamic_cast<IntegerLiteralNode*>(node.rhs);
    ASSERT_NE(lhs, nullptr);
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(lhs->value, 1);
    EXPECT_EQ(rhs->value, 2);
}

TEST(AstBinary, LogicOpName) {
    AstArena       arena;
    BinaryExprNode node{kLoc, aria::Op::Binary::And, ident(arena, "a"), ident(arena, "b")};
    EXPECT_NE(dump_str(node).find("BinaryExpr op=&&"), String::npos);
}

// ---------------------------------------------------------------------------
// 一元 / 赋值 / 解构赋值
// ---------------------------------------------------------------------------

TEST(AstDisplay, Unary) {
    AstArena      arena;
    UnaryExprNode node{kLoc, aria::Op::Unary::Minus, ident(arena, "x")};
    EXPECT_EQ(dump_str(node), "UnaryExpr op=-\n  Identifier x\n");
}

TEST(AstDisplay, Assignment) {
    AstArena       arena;
    AssignmentNode node{kLoc, aria::Op::Assignment::Assign, ident(arena, "x"), i64lit(arena, 5)};
    const String   out = dump_str(node);
    // header "op=" + 赋值符号；Assign 符号恰为 "="，故 "op=="
    EXPECT_EQ(out, "Assignment op==\n  Identifier x\n  IntegerLiteral 5\n");
}

TEST(AstDisplay, CompoundAssign) {
    AstArena       arena;
    AssignmentNode node{kLoc, aria::Op::Assignment::PlusAssign, ident(arena, "x"), i64lit(arena, 5)};
    // header 形如 "Assignment op=+="（op 名 "+=" 拼到 "op=" 之后）
    EXPECT_NE(dump_str(node).find("Assignment op=+="), String::npos);
}

TEST(AstDisplay, DestructureAssignment) {
    AstArena           arena;
    List<PatternNode*> elems;
    elems.push_back(id_pat(arena, "a"));
    elems.push_back(id_pat(arena, "b"));
    auto                      target = arena.make<ListPatternNode>(kLoc, arena.make_list(elems), nullptr);
    DestructureAssignmentNode node{kLoc, target, ident(arena, "lst")};
    const String              out = dump_str(node);
    expect_has(out, "DestructureAssignment");
    expect_has(out, "ListPattern elements=2");
    expect_has(out, "Identifier lst");
}

// ---------------------------------------------------------------------------
// 调用 / 字段 / 下标
// ---------------------------------------------------------------------------

TEST(AstDisplay, Call) {
    AstArena        arena;
    List<ExprNode*> args;
    args.push_back(i64lit(arena, 1));
    args.push_back(i64lit(arena, 2));
    CallNode     node{kLoc, ident(arena, "f"), arena.make_list(args)};
    const String out = dump_str(node);
    EXPECT_EQ(out, "Call args=2\n  Identifier f\n  IntegerLiteral 1\n  IntegerLiteral 2\n");
}

TEST(AstDisplay, FieldAndIndex) {
    AstArena        arena;
    FieldAccessNode fa{kLoc, ident(arena, "o"), StringView{"field"}};
    EXPECT_EQ(dump_str(fa), "FieldAccess name=field\n  Identifier o\n");

    IndexAccessNode ia{kLoc, ident(arena, "a"), i64lit(arena, 0)};
    EXPECT_EQ(dump_str(ia), "IndexAccess\n  Identifier a\n  IntegerLiteral 0\n");
}

// ---------------------------------------------------------------------------
// 复合表达式
// ---------------------------------------------------------------------------

TEST(AstDisplay, ListExpr) {
    AstArena        arena;
    List<ExprNode*> elems;
    elems.push_back(i64lit(arena, 1));
    elems.push_back(i64lit(arena, 2));
    ListExprNode node{kLoc, arena.make_list(elems)};
    EXPECT_EQ(dump_str(node), "ListExpr elements=2\n  IntegerLiteral 1\n  IntegerLiteral 2\n");
}

TEST(AstDisplay, MapExpr) {
    AstArena       arena;
    List<MapEntry> entries;
    MapEntry       e;
    e.key   = ident(arena, "k");
    e.value = i64lit(arena, 1);
    entries.push_back(e);
    MapExprNode  node{kLoc, arena.make_list(entries)};
    const String out = dump_str(node);
    expect_has(out, "MapExpr entries=1");
    expect_has(out, "MapEntry");
}

TEST(AstDisplay, IfExpr) {
    AstArena     arena;
    IfExprNode   node{kLoc, i64lit(arena, 1), i64lit(arena, 2), i64lit(arena, 3)};
    const String out = dump_str(node);
    EXPECT_EQ(out, "IfExpr\n  IntegerLiteral 1\n  IntegerLiteral 2\n  IntegerLiteral 3\n");
}

TEST(AstDisplay, LambdaExpr) {
    AstArena    arena;
    List<Param> params;
    Param       p;
    p.name = "x";
    params.push_back(p);

    List<StmtNode*> body_stmts;
    body_stmts.push_back(arena.make<ReturnStmtNode>(kLoc, ident(arena, "x")));
    auto body = arena.make<BlockNode>(kLoc, arena.make_list(body_stmts));

    LambdaExprNode node{kLoc, arena.make_list(params), body};
    const String   out = dump_str(node);
    expect_has(out, "LambdaExpr params=1");
    expect_has(out, "Param name=x");
    expect_has(out, "ReturnStmt");
    expect_has(out, "Identifier x");
}

TEST(AstDisplay, MatchExpr) {
    AstArena           arena;
    List<MatchExprArm> arms;
    MatchExprArm       a1;
    a1.pattern.value = i64lit(arena, 1);
    a1.body          = i64lit(arena, 10);
    arms.push_back(a1);
    MatchExprArm a2; // 默认 value=nullptr -> "_" 通配
    a2.body = i64lit(arena, 99);
    arms.push_back(a2);

    MatchExprNode node{kLoc, ident(arena, "x"), arena.make_list(arms)};
    const String  out = dump_str(node);
    expect_has(out, "MatchExpr arms=2");
    expect_has(out, "MatchPattern");
    expect_has(out, "MatchPattern _ (wildcard)");
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

TEST(AstDisplay, BlockAndExprStmt) {
    AstArena        arena;
    List<StmtNode*> stmts;
    stmts.push_back(arena.make<ExprStmtNode>(kLoc, i64lit(arena, 1)));
    BlockNode    block{kLoc, arena.make_list(stmts)};
    const String out = dump_str(block);
    EXPECT_EQ(out, "Block stmts=1\n  ExprStmt\n    IntegerLiteral 1\n");
}

TEST(AstDisplay, IfStmt) {
    AstArena     arena;
    auto         then_b = arena.make<ExprStmtNode>(kLoc, i64lit(arena, 1));
    auto         else_b = arena.make<ExprStmtNode>(kLoc, i64lit(arena, 2));
    IfStmtNode   node{kLoc, ident(arena, "flag"), then_b, else_b};
    const String out = dump_str(node);
    expect_has(out, "IfStmt");
    expect_has(out, "Identifier flag");
}

TEST(AstDisplay, ReturnStmt) {
    AstArena       arena;
    ReturnStmtNode with_val{kLoc, i64lit(arena, 7)};
    expect_has(dump_str(with_val), "ReturnStmt");
    expect_has(dump_str(with_val), "IntegerLiteral 7");

    ReturnStmtNode no_val{kLoc, nullptr};
    EXPECT_EQ(dump_str(no_val), "ReturnStmt\n");
}

TEST(AstDisplay, ImportStmt) {
    AstArena       arena;
    ImportStmtNode node{kLoc, strlit(arena, "math"), StringView{"m"}};
    EXPECT_EQ(dump_str(node), "ImportStmt as=m\n  StringLiteral \"math\"\n");
}

TEST(AstDisplay, TryStmt) {
    AstArena        arena;
    List<StmtNode*> try_body_stmts;
    try_body_stmts.push_back(arena.make<ExprStmtNode>(kLoc, i64lit(arena, 1)));
    auto try_body = arena.make<BlockNode>(kLoc, arena.make_list(try_body_stmts));

    List<StmtNode*> catch_body_stmts;
    catch_body_stmts.push_back(arena.make<ExprStmtNode>(kLoc, ident(arena, "e")));
    auto catch_body = arena.make<BlockNode>(kLoc, arena.make_list(catch_body_stmts));

    TryStmtNode  node{kLoc, try_body, StringView{"e"}, catch_body};
    const String out = dump_str(node);
    expect_has(out, "TryStmt");
    expect_has(out, "Catch param=e");
    expect_has(out, "Identifier e");
}

TEST(AstDisplay, ForInStmt) {
    AstArena           arena;
    List<PatternNode*> elems;
    elems.push_back(id_pat(arena, "k"));
    elems.push_back(id_pat(arena, "v"));
    ForInStmtNode node{kLoc, arena.make<ListPatternNode>(kLoc, arena.make_list(elems), nullptr), ident(arena, "m"),
                       arena.make<BreakStmtNode>(kLoc)};
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
    AstArena         arena;
    List<VarBinding> bindings;
    VarBinding       b;
    b.target      = id_pat(arena, "x");
    b.initializer = i64lit(arena, 1);
    bindings.push_back(b);

    VarDeclNode  node{kLoc, arena.make_list(bindings)};
    const String out = dump_str(node);
    EXPECT_EQ(out, "VarDecl bindings=1\n  VarBinding\n    IdentifierPattern name=x\n    IntegerLiteral 1\n");
}

TEST(AstDisplay, FunDecl) {
    AstArena    arena;
    List<Param> params;
    Param       p;
    p.name = "x";
    params.push_back(p);

    List<StmtNode*> body_stmts;
    body_stmts.push_back(arena.make<ReturnStmtNode>(kLoc, ident(arena, "x")));
    auto body = arena.make<BlockNode>(kLoc, arena.make_list(body_stmts));

    FunDeclNode  node{kLoc, StringView{"id"}, arena.make_list(params), body, FnKind::Function};
    const String out = dump_str(node);
    expect_has(out, "FunDecl name=id params=1 kind=Function");
    expect_has(out, "ReturnStmt");
}

TEST(AstDisplay, DefDecl) {
    AstArena        arena;
    List<StmtNode*> members;
    // bark() { println("woof"); }  -- 实例方法（Method）
    List<StmtNode*> bark_body_stmts;
    bark_body_stmts.push_back(arena.make<ExprStmtNode>(kLoc, strlit(arena, "woof")));
    auto bark_body = arena.make<BlockNode>(kLoc, arena.make_list(bark_body_stmts));
    members.push_back(arena.make<FunDeclNode>(kLoc, StringView{"bark"}, Span<Param>{}, bark_body, FnKind::Method));

    DefDeclNode  node{kLoc, StringView{"Dog"}, arena.make<IdentifierNode>(kLoc, StringView{"Animal"}),
                      arena.make_list(members), false};
    const String out = dump_str(node);
    expect_has(out, "DefDecl name=Dog");
    expect_has(out, "Identifier Animal");
    expect_has(out, "FunDecl name=bark params=0 kind=Method");
    expect_has(out, "StringLiteral \"woof\"");
}

TEST(AstDisplay, Program) {
    AstArena arena;
    // var x = 1; println(x);
    List<StmtNode*> decls;

    List<VarBinding> bindings;
    VarBinding       b;
    b.target      = id_pat(arena, "x");
    b.initializer = i64lit(arena, 1);
    bindings.push_back(b);
    decls.push_back(arena.make<VarDeclNode>(kLoc, arena.make_list(bindings)));

    decls.push_back(arena.make<ExprStmtNode>(kLoc, ident(arena, "x")));

    ProgramNode  prog{kLoc, arena.make_list(decls)};
    const String out = dump_str(prog);
    expect_has(out, "Program decls=2");
    expect_has(out, "VarDecl bindings=1");
    expect_has(out, "ExprStmt");
}

// ---------------------------------------------------------------------------
// 解构模式
// ---------------------------------------------------------------------------

TEST(AstDisplay, Patterns) {
    AstArena arena;
    EXPECT_EQ(dump_str(*id_pat(arena, "a")), "IdentifierPattern name=a\n");
    EXPECT_EQ(dump_str(WildcardPatternNode{kLoc}), "WildcardPattern _\n");

    // [a, _, ...rest]
    List<PatternNode*> elems;
    elems.push_back(id_pat(arena, "a"));
    elems.push_back(arena.make<WildcardPatternNode>(kLoc));
    ListPatternNode lp{kLoc, arena.make_list(elems), id_pat(arena, "rest")};
    const String    lp_out = dump_str(lp);
    expect_has(lp_out, "ListPattern elements=2 rest=rest");
    expect_has(lp_out, "IdentifierPattern name=a");
    expect_has(lp_out, "WildcardPattern _");
}

// ---------------------------------------------------------------------------
// 多态：通过基类指针调用 dump
// ---------------------------------------------------------------------------

TEST(AstPolymorphism, BasePointerDump) {
    AstArena  arena;
    ExprNode* expr = i64lit(arena, 42); // IntegerLiteralNode* -> ExprNode*（裸指针隐式上行）
    ASTNode*  node = expr;
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(dump_str(*node), "IntegerLiteral 42\n");
}
