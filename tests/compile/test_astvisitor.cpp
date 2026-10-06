#include <gtest/gtest.h>

#include "compile/Ast.hpp"
#include "compile/AstVisitor.hpp"
#include "memory/AstArena.hpp"

// 同 test_ast.cpp：不使用 `using namespace aria`（fs.hpp 在 Windows 下可能间接包含
// windows.h，其全局符号与 aria 命名空间冲突）。按需显式引入。
using aria::AssignmentNode;
using aria::AstArena;
using aria::ASTNode;
using aria::AstVisitor;
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
using aria::InterpolatedStringNode;
using aria::LambdaExprNode;
using aria::List;
using aria::ListExprNode;
using aria::ListPatternNode;
using aria::MapEntry;
using aria::MapExprNode;
using aria::MatchArm;
using aria::MatchExprArm;
using aria::MatchExprNode;
using aria::MatchStmtNode;
using aria::NilLiteralNode;
using aria::Opt;
using aria::Param;
using aria::PatternNode;
using aria::ProgramNode;
using aria::RangeExprNode;
using aria::ReturnStmtNode;
using aria::SequenceExprNode;
using aria::SourceLoc;
using aria::Span;
using aria::StaticVarMemberNode;
using aria::StmtNode;
using aria::String;
using aria::StringLiteralNode;
using aria::StringShape;
using aria::StringView;
using aria::SuperExprNode;
using aria::ThisExprNode;
using aria::ThrowStmtNode;
using aria::TryStmtNode;
using aria::UnaryExprNode;
using aria::VarBinding;
using aria::VarDeclNode;
using aria::WhileStmtNode;
using aria::WildcardPatternNode;

namespace {
    // 空位置：访问者分派不依赖 loc。
    const SourceLoc kLoc{};

    // 便利工厂（同 test_ast.cpp）：节点分配自测试局部 arena，返回裸指针。
    IntegerLiteralNode* i64lit(AstArena& arena, const i64 v) { return arena.make<IntegerLiteralNode>(kLoc, v); }
    IdentifierNode*     ident(AstArena& arena, const StringView name) { return arena.make<IdentifierNode>(kLoc, name); }
    IdentifierPatternNode* id_pat(AstArena& arena, const StringView name) {
        return arena.make<IdentifierPatternNode>(kLoc, name);
    }
    BlockNode* empty_block(AstArena& arena) { return arena.make<BlockNode>(kLoc, Span<StmtNode*>{}); }

    // 记录型访问者：每个 visitXxxNode 把节点类型名压入 visited_，供断言分派结果。
    // 若节点 accept 分派到错误的 visitXxxNode，visited_ 内容会不符；若某节点类型未
    // 实现 accept（访问者模式不完整），编译期即报错。
    class RecordingVisitor final : public AstVisitor {
    public:
        [[nodiscard]]
        const List<StringView>& visited() const noexcept {
            return visited_;
        }

        // --- 根节点 ---
        void visitProgramNode(ProgramNode&) override { visited_.push_back("ProgramNode"); }

        // --- 语句节点 ---
        void visitBlockNode(BlockNode&) override { visited_.push_back("BlockNode"); }
        void visitExprStmtNode(ExprStmtNode&) override { visited_.push_back("ExprStmtNode"); }
        void visitIfStmtNode(IfStmtNode&) override { visited_.push_back("IfStmtNode"); }
        void visitWhileStmtNode(WhileStmtNode&) override { visited_.push_back("WhileStmtNode"); }
        void visitForStmtNode(ForStmtNode&) override { visited_.push_back("ForStmtNode"); }
        void visitForInStmtNode(ForInStmtNode&) override { visited_.push_back("ForInStmtNode"); }
        void visitBreakStmtNode(BreakStmtNode&) override { visited_.push_back("BreakStmtNode"); }
        void visitContinueStmtNode(ContinueStmtNode&) override { visited_.push_back("ContinueStmtNode"); }
        void visitReturnStmtNode(ReturnStmtNode&) override { visited_.push_back("ReturnStmtNode"); }
        void visitImportStmtNode(ImportStmtNode&) override { visited_.push_back("ImportStmtNode"); }
        void visitTryStmtNode(TryStmtNode&) override { visited_.push_back("TryStmtNode"); }
        void visitThrowStmtNode(ThrowStmtNode&) override { visited_.push_back("ThrowStmtNode"); }
        void visitMatchStmtNode(MatchStmtNode&) override { visited_.push_back("MatchStmtNode"); }
        void visitFunDeclNode(FunDeclNode&) override { visited_.push_back("FunDeclNode"); }
        void visitDefDeclNode(DefDeclNode&) override { visited_.push_back("DefDeclNode"); }
        void visitVarDeclNode(VarDeclNode&) override { visited_.push_back("VarDeclNode"); }
        void visitStaticVarMemberNode(StaticVarMemberNode&) override { visited_.push_back("StaticVarMemberNode"); }

        // --- 表达式节点 ---
        void visitIntegerLiteralNode(IntegerLiteralNode&) override { visited_.push_back("IntegerLiteralNode"); }
        void visitFloatLiteralNode(FloatLiteralNode&) override { visited_.push_back("FloatLiteralNode"); }
        void visitStringLiteralNode(StringLiteralNode&) override { visited_.push_back("StringLiteralNode"); }
        void visitInterpolatedStringNode(InterpolatedStringNode&) override {
            visited_.push_back("InterpolatedStringNode");
        }
        void visitBoolLiteralNode(BoolLiteralNode&) override { visited_.push_back("BoolLiteralNode"); }
        void visitNilLiteralNode(NilLiteralNode&) override { visited_.push_back("NilLiteralNode"); }
        void visitIdentifierNode(IdentifierNode&) override { visited_.push_back("IdentifierNode"); }
        void visitThisExprNode(ThisExprNode&) override { visited_.push_back("ThisExprNode"); }
        void visitSuperExprNode(SuperExprNode&) override { visited_.push_back("SuperExprNode"); }
        void visitBinaryExprNode(BinaryExprNode&) override { visited_.push_back("BinaryExprNode"); }
        void visitUnaryExprNode(UnaryExprNode&) override { visited_.push_back("UnaryExprNode"); }
        void visitAssignmentNode(AssignmentNode&) override { visited_.push_back("AssignmentNode"); }
        void visitDestructureAssignmentNode(DestructureAssignmentNode&) override {
            visited_.push_back("DestructureAssignmentNode");
        }
        void visitCallNode(CallNode&) override { visited_.push_back("CallNode"); }
        void visitFieldAccessNode(FieldAccessNode&) override { visited_.push_back("FieldAccessNode"); }
        void visitIndexAccessNode(IndexAccessNode&) override { visited_.push_back("IndexAccessNode"); }
        void visitListExprNode(ListExprNode&) override { visited_.push_back("ListExprNode"); }
        void visitMapExprNode(MapExprNode&) override { visited_.push_back("MapExprNode"); }
        void visitRangeExprNode(RangeExprNode&) override { visited_.push_back("RangeExprNode"); }
        void visitIfExprNode(IfExprNode&) override { visited_.push_back("IfExprNode"); }
        void visitLambdaExprNode(LambdaExprNode&) override { visited_.push_back("LambdaExprNode"); }
        void visitMatchExprNode(MatchExprNode&) override { visited_.push_back("MatchExprNode"); }
        void visitSequenceExprNode(SequenceExprNode&) override { visited_.push_back("SequenceExprNode"); }

        // --- 解构模式节点 ---
        void visitIdentifierPatternNode(IdentifierPatternNode&) override {
            visited_.push_back("IdentifierPatternNode");
        }
        void visitWildcardPatternNode(WildcardPatternNode&) override { visited_.push_back("WildcardPatternNode"); }
        void visitListPatternNode(ListPatternNode&) override { visited_.push_back("ListPatternNode"); }

    private:
        List<StringView> visited_;
    };

    // 构造单个节点并经 node->accept(v) 分派，断言恰好命中对应 visitXxxNode。
    void expect_visit(ASTNode* node, const StringView tag) {
        RecordingVisitor v;
        node->accept(v);
        ASSERT_EQ(v.visited().size(), 1u);
        EXPECT_EQ(v.visited()[0], tag);
    }
} // namespace

// --- 分派：每个具体节点类型经 accept() 命中正确的 visitXxxNode ---
//
// 覆盖全部 43 个具体节点，验证节点 accept -> visitor.visitXxxNode 的双分派既正确（命中
// 对应方法）又完备（每个节点类型都实现 accept）。新增节点类型时若未实现 accept 与对应
// visitXxxNode，本测试编译期即报错。

TEST(AstVisitorDispatch, StatementsAndDeclarations) {
    AstArena arena;
    // 根节点
    expect_visit(arena.make<ProgramNode>(kLoc, Span<StmtNode*>{}), "ProgramNode");

    // 语句
    expect_visit(empty_block(arena), "BlockNode");
    expect_visit(arena.make<ExprStmtNode>(kLoc, i64lit(arena, 1)), "ExprStmtNode");
    expect_visit(arena.make<IfStmtNode>(kLoc, ident(arena, "c"), empty_block(arena), empty_block(arena)), "IfStmtNode");
    expect_visit(arena.make<WhileStmtNode>(kLoc, ident(arena, "c"), empty_block(arena)), "WhileStmtNode");
    expect_visit(arena.make<ForStmtNode>(kLoc, nullptr, ident(arena, "c"), nullptr, empty_block(arena)), "ForStmtNode");
    expect_visit(arena.make<ForInStmtNode>(kLoc, id_pat(arena, "x"), ident(arena, "xs"), empty_block(arena)),
                 "ForInStmtNode");
    expect_visit(arena.make<BreakStmtNode>(kLoc), "BreakStmtNode");
    expect_visit(arena.make<ContinueStmtNode>(kLoc), "ContinueStmtNode");
    expect_visit(arena.make<ReturnStmtNode>(kLoc, i64lit(arena, 1)), "ReturnStmtNode");
    expect_visit(arena.make<ImportStmtNode>(
                         kLoc, arena.make<StringLiteralNode>(kLoc, StringView{"math"}, StringShape{4, false}),
                         StringView{"m"}),
                 "ImportStmtNode");
    expect_visit(arena.make<TryStmtNode>(kLoc, empty_block(arena), StringView{"e"}, empty_block(arena)), "TryStmtNode");
    expect_visit(arena.make<ThrowStmtNode>(kLoc, i64lit(arena, 1)), "ThrowStmtNode");
    expect_visit(arena.make<MatchStmtNode>(kLoc, ident(arena, "s"), Span<MatchArm>{}), "MatchStmtNode");

    // 声明
    expect_visit(arena.make<FunDeclNode>(kLoc, StringView{"f"}, Span<Param>{}, empty_block(arena), FnKind::Function),
                 "FunDeclNode");
    expect_visit(arena.make<DefDeclNode>(kLoc, StringView{"C"}, nullptr, Span<StmtNode*>{}, false), "DefDeclNode");
    expect_visit(arena.make<VarDeclNode>(kLoc, Span<VarBinding>{}), "VarDeclNode");
    expect_visit(arena.make<StaticVarMemberNode>(kLoc, StringView{"x"}, i64lit(arena, 1)), "StaticVarMemberNode");
}

TEST(AstVisitorDispatch, Expressions) {
    AstArena arena;
    // 字面量与基础表达式
    expect_visit(i64lit(arena, 1), "IntegerLiteralNode");
    expect_visit(arena.make<FloatLiteralNode>(kLoc, 1.5), "FloatLiteralNode");
    expect_visit(arena.make<StringLiteralNode>(kLoc, StringView{"s"}, StringShape{1, false}), "StringLiteralNode");
    expect_visit(arena.make<BoolLiteralNode>(kLoc, true), "BoolLiteralNode");
    expect_visit(arena.make<NilLiteralNode>(kLoc), "NilLiteralNode");
    expect_visit(ident(arena, "x"), "IdentifierNode");
    expect_visit(arena.make<ThisExprNode>(kLoc), "ThisExprNode");
    expect_visit(arena.make<SuperExprNode>(kLoc, StringView{"m"}), "SuperExprNode");

    // 运算符表达式
    expect_visit(arena.make<BinaryExprNode>(kLoc, aria::Op::Binary::Plus, i64lit(arena, 1), i64lit(arena, 2)),
                 "BinaryExprNode");
    expect_visit(arena.make<UnaryExprNode>(kLoc, aria::Op::Unary::Minus, ident(arena, "x")), "UnaryExprNode");
    expect_visit(arena.make<AssignmentNode>(kLoc, aria::Op::Assignment::Assign, ident(arena, "x"), i64lit(arena, 1)),
                 "AssignmentNode");
    List<PatternNode*> destructure_elems;
    destructure_elems.push_back(id_pat(arena, "x"));
    expect_visit(arena.make<DestructureAssignmentNode>(
                         kLoc,
                         arena.make<ListPatternNode>(kLoc, arena.make_list(std::move(destructure_elems)), nullptr),
                         ident(arena, "l")),
                 "DestructureAssignmentNode");
    expect_visit(arena.make<CallNode>(kLoc, ident(arena, "f"), Span<ExprNode*>{}), "CallNode");
    expect_visit(arena.make<FieldAccessNode>(kLoc, ident(arena, "o"), StringView{"f"}), "FieldAccessNode");
    expect_visit(arena.make<IndexAccessNode>(kLoc, ident(arena, "a"), i64lit(arena, 0)), "IndexAccessNode");

    // 复合表达式
    expect_visit(arena.make<ListExprNode>(kLoc, Span<ExprNode*>{}), "ListExprNode");
    expect_visit(arena.make<MapExprNode>(kLoc, Span<MapEntry>{}), "MapExprNode");
    expect_visit(arena.make<RangeExprNode>(kLoc, false, i64lit(arena, 1), i64lit(arena, 2)), "RangeExprNode");
    expect_visit(arena.make<IfExprNode>(kLoc, i64lit(arena, 1), i64lit(arena, 2), i64lit(arena, 3)), "IfExprNode");
    expect_visit(arena.make<LambdaExprNode>(kLoc, Span<Param>{}, empty_block(arena)), "LambdaExprNode");
    expect_visit(arena.make<MatchExprNode>(kLoc, ident(arena, "s"), Span<MatchExprArm>{}), "MatchExprNode");
    expect_visit(arena.make<SequenceExprNode>(kLoc, Span<ExprNode*>{}), "SequenceExprNode");
}

TEST(AstVisitorDispatch, Patterns) {
    AstArena arena;
    expect_visit(id_pat(arena, "x"), "IdentifierPatternNode");
    expect_visit(arena.make<WildcardPatternNode>(kLoc), "WildcardPatternNode");
    expect_visit(arena.make<ListPatternNode>(kLoc, Span<PatternNode*>{}, nullptr), "ListPatternNode");
}
