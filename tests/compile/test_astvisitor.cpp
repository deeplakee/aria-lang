#include <gtest/gtest.h>

#include "compile/AstVisitor.hpp"
#include "compile/ast.hpp"

// 同 test_ast.cpp：不使用 `using namespace aria`（fs.hpp 在 Windows 下可能间接包含
// windows.h，其全局符号与 aria 命名空间冲突）。按需显式引入。
using aria::AssignmentNode;
using aria::ASTNode;
using aria::AstVisitor;
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
using aria::MatchStmtNode;
using aria::NilLiteralNode;
using aria::Opt;
using aria::Param;
using aria::PatternNode;
using aria::PrintStmtNode;
using aria::ProgramNode;
using aria::RangeExprNode;
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
using aria::VarBinding;
using aria::VarDeclNode;
using aria::WhileStmtNode;
using aria::WildcardPatternNode;

namespace {
    // 空位置：访问者分派不依赖 loc。
    const SourceLoc kLoc{};

    // 便利工厂（同 test_ast.cpp）。
    UPtr<IntegerLiteralNode>    i64lit(i64 v) { return std::make_unique<IntegerLiteralNode>(kLoc, v); }
    UPtr<IdentifierNode>        ident(String name) { return std::make_unique<IdentifierNode>(kLoc, std::move(name)); }
    UPtr<IdentifierPatternNode> id_pat(String name) {
        return std::make_unique<IdentifierPatternNode>(kLoc, std::move(name));
    }
    UPtr<BlockNode> empty_block() { return std::make_unique<BlockNode>(kLoc, List<UPtr<StmtNode>>{}); }

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
        void visitProgramNode(ProgramNode*) override { visited_.push_back("ProgramNode"); }

        // --- 语句节点 ---
        void visitBlockNode(BlockNode*) override { visited_.push_back("BlockNode"); }
        void visitExprStmtNode(ExprStmtNode*) override { visited_.push_back("ExprStmtNode"); }
        void visitPrintStmtNode(PrintStmtNode*) override { visited_.push_back("PrintStmtNode"); }
        void visitIfStmtNode(IfStmtNode*) override { visited_.push_back("IfStmtNode"); }
        void visitWhileStmtNode(WhileStmtNode*) override { visited_.push_back("WhileStmtNode"); }
        void visitForStmtNode(ForStmtNode*) override { visited_.push_back("ForStmtNode"); }
        void visitForInStmtNode(ForInStmtNode*) override { visited_.push_back("ForInStmtNode"); }
        void visitBreakStmtNode(BreakStmtNode*) override { visited_.push_back("BreakStmtNode"); }
        void visitContinueStmtNode(ContinueStmtNode*) override { visited_.push_back("ContinueStmtNode"); }
        void visitReturnStmtNode(ReturnStmtNode*) override { visited_.push_back("ReturnStmtNode"); }
        void visitImportStmtNode(ImportStmtNode*) override { visited_.push_back("ImportStmtNode"); }
        void visitTryStmtNode(TryStmtNode*) override { visited_.push_back("TryStmtNode"); }
        void visitThrowStmtNode(ThrowStmtNode*) override { visited_.push_back("ThrowStmtNode"); }
        void visitMatchStmtNode(MatchStmtNode*) override { visited_.push_back("MatchStmtNode"); }
        void visitFunDeclNode(FunDeclNode*) override { visited_.push_back("FunDeclNode"); }
        void visitDefDeclNode(DefDeclNode*) override { visited_.push_back("DefDeclNode"); }
        void visitVarDeclNode(VarDeclNode*) override { visited_.push_back("VarDeclNode"); }

        // --- 表达式节点 ---
        void visitIntegerLiteralNode(IntegerLiteralNode*) override { visited_.push_back("IntegerLiteralNode"); }
        void visitFloatLiteralNode(FloatLiteralNode*) override { visited_.push_back("FloatLiteralNode"); }
        void visitStringLiteralNode(StringLiteralNode*) override { visited_.push_back("StringLiteralNode"); }
        void visitBoolLiteralNode(BoolLiteralNode*) override { visited_.push_back("BoolLiteralNode"); }
        void visitNilLiteralNode(NilLiteralNode*) override { visited_.push_back("NilLiteralNode"); }
        void visitIdentifierNode(IdentifierNode*) override { visited_.push_back("IdentifierNode"); }
        void visitThisExprNode(ThisExprNode*) override { visited_.push_back("ThisExprNode"); }
        void visitSuperExprNode(SuperExprNode*) override { visited_.push_back("SuperExprNode"); }
        void visitBinaryExprNode(BinaryExprNode*) override { visited_.push_back("BinaryExprNode"); }
        void visitUnaryExprNode(UnaryExprNode*) override { visited_.push_back("UnaryExprNode"); }
        void visitAssignmentNode(AssignmentNode*) override { visited_.push_back("AssignmentNode"); }
        void visitDestructureAssignmentNode(DestructureAssignmentNode*) override {
            visited_.push_back("DestructureAssignmentNode");
        }
        void visitCallNode(CallNode*) override { visited_.push_back("CallNode"); }
        void visitFieldAccessNode(FieldAccessNode*) override { visited_.push_back("FieldAccessNode"); }
        void visitIndexAccessNode(IndexAccessNode*) override { visited_.push_back("IndexAccessNode"); }
        void visitListExprNode(ListExprNode*) override { visited_.push_back("ListExprNode"); }
        void visitMapExprNode(MapExprNode*) override { visited_.push_back("MapExprNode"); }
        void visitRangeExprNode(RangeExprNode*) override { visited_.push_back("RangeExprNode"); }
        void visitIfExprNode(IfExprNode*) override { visited_.push_back("IfExprNode"); }
        void visitLambdaExprNode(LambdaExprNode*) override { visited_.push_back("LambdaExprNode"); }
        void visitMatchExprNode(MatchExprNode*) override { visited_.push_back("MatchExprNode"); }

        // --- 解构模式节点 ---
        void visitIdentifierPatternNode(IdentifierPatternNode*) override {
            visited_.push_back("IdentifierPatternNode");
        }
        void visitWildcardPatternNode(WildcardPatternNode*) override { visited_.push_back("WildcardPatternNode"); }
        void visitListPatternNode(ListPatternNode*) override { visited_.push_back("ListPatternNode"); }

    private:
        List<StringView> visited_;
    };

    // 构造单个节点并经 node->accept(v) 分派，断言恰好命中对应 visitXxxNode。
    void expect_visit(UPtr<ASTNode> node, const StringView tag) {
        RecordingVisitor v;
        node->accept(v);
        ASSERT_EQ(v.visited().size(), 1u);
        EXPECT_EQ(v.visited()[0], tag);
    }
} // namespace

// ---------------------------------------------------------------------------
// 分派：每个具体节点类型经 accept() 命中正确的 visitXxxNode
// ---------------------------------------------------------------------------
//
// 覆盖全部 42 个具体节点，验证节点 accept -> visitor.visitXxxNode 的双分派既正确（命中
// 对应方法）又完备（每个节点类型都实现 accept）。新增节点类型时若未实现 accept 与对应
// visitXxxNode，本测试编译期即报错。

TEST(AstVisitorDispatch, StatementsAndDeclarations) {
    // 根节点
    expect_visit(std::make_unique<ProgramNode>(kLoc, List<UPtr<StmtNode>>{}), "ProgramNode");

    // 语句
    expect_visit(empty_block(), "BlockNode");
    expect_visit(std::make_unique<ExprStmtNode>(kLoc, i64lit(1)), "ExprStmtNode");
    expect_visit(std::make_unique<PrintStmtNode>(kLoc, i64lit(1)), "PrintStmtNode");
    expect_visit(std::make_unique<IfStmtNode>(kLoc, ident("c"), empty_block(), empty_block()), "IfStmtNode");
    expect_visit(std::make_unique<WhileStmtNode>(kLoc, ident("c"), empty_block()), "WhileStmtNode");
    expect_visit(std::make_unique<ForStmtNode>(kLoc, nullptr, ident("c"), nullptr, empty_block()), "ForStmtNode");
    expect_visit(std::make_unique<ForInStmtNode>(kLoc, id_pat("x"), ident("xs"), empty_block()), "ForInStmtNode");
    expect_visit(std::make_unique<BreakStmtNode>(kLoc), "BreakStmtNode");
    expect_visit(std::make_unique<ContinueStmtNode>(kLoc), "ContinueStmtNode");
    expect_visit(std::make_unique<ReturnStmtNode>(kLoc, i64lit(1)), "ReturnStmtNode");
    expect_visit(std::make_unique<ImportStmtNode>(kLoc, String{"math"}, String{"m"}), "ImportStmtNode");
    expect_visit(std::make_unique<TryStmtNode>(kLoc, empty_block(), Opt<String>{String{"e"}}, empty_block(), nullptr),
                 "TryStmtNode");
    expect_visit(std::make_unique<ThrowStmtNode>(kLoc, i64lit(1)), "ThrowStmtNode");
    expect_visit(std::make_unique<MatchStmtNode>(kLoc, ident("s"), List<MatchArm>{}), "MatchStmtNode");

    // 声明
    expect_visit(std::make_unique<FunDeclNode>(kLoc, String{"f"}, List<Param>{}, empty_block()), "FunDeclNode");
    expect_visit(std::make_unique<DefDeclNode>(kLoc, String{"C"}, Opt<String>{}, List<DefMember>{}), "DefDeclNode");
    expect_visit(std::make_unique<VarDeclNode>(kLoc, List<VarBinding>{}), "VarDeclNode");
}

TEST(AstVisitorDispatch, Expressions) {
    // 字面量与基础表达式
    expect_visit(i64lit(1), "IntegerLiteralNode");
    expect_visit(std::make_unique<FloatLiteralNode>(kLoc, 1.5), "FloatLiteralNode");
    expect_visit(std::make_unique<StringLiteralNode>(kLoc, String{"s"}), "StringLiteralNode");
    expect_visit(std::make_unique<BoolLiteralNode>(kLoc, true), "BoolLiteralNode");
    expect_visit(std::make_unique<NilLiteralNode>(kLoc), "NilLiteralNode");
    expect_visit(ident("x"), "IdentifierNode");
    expect_visit(std::make_unique<ThisExprNode>(kLoc), "ThisExprNode");
    expect_visit(std::make_unique<SuperExprNode>(kLoc), "SuperExprNode");

    // 运算符表达式
    expect_visit(std::make_unique<BinaryExprNode>(kLoc, aria::Op::Binary::Plus, i64lit(1), i64lit(2)),
                 "BinaryExprNode");
    expect_visit(std::make_unique<UnaryExprNode>(kLoc, aria::Op::Unary::Minus, ident("x")), "UnaryExprNode");
    expect_visit(std::make_unique<AssignmentNode>(kLoc, aria::Op::Assignment::Assign, ident("x"), i64lit(1)),
                 "AssignmentNode");
    expect_visit(std::make_unique<DestructureAssignmentNode>(kLoc, id_pat("x"), ident("l")),
                 "DestructureAssignmentNode");
    expect_visit(std::make_unique<CallNode>(kLoc, ident("f"), List<UPtr<ExprNode>>{}), "CallNode");
    expect_visit(std::make_unique<FieldAccessNode>(kLoc, ident("o"), String{"f"}), "FieldAccessNode");
    expect_visit(std::make_unique<IndexAccessNode>(kLoc, ident("a"), i64lit(0)), "IndexAccessNode");

    // 复合表达式
    expect_visit(std::make_unique<ListExprNode>(kLoc, List<UPtr<ExprNode>>{}), "ListExprNode");
    expect_visit(std::make_unique<MapExprNode>(kLoc, List<MapEntry>{}), "MapExprNode");
    expect_visit(std::make_unique<RangeExprNode>(kLoc, false, i64lit(1), i64lit(2)), "RangeExprNode");
    expect_visit(std::make_unique<IfExprNode>(kLoc, i64lit(1), i64lit(2), i64lit(3)), "IfExprNode");
    expect_visit(std::make_unique<LambdaExprNode>(kLoc, List<Param>{}, empty_block()), "LambdaExprNode");
    expect_visit(std::make_unique<MatchExprNode>(kLoc, ident("s"), List<MatchExprArm>{}), "MatchExprNode");
}

TEST(AstVisitorDispatch, Patterns) {
    expect_visit(id_pat("x"), "IdentifierPatternNode");
    expect_visit(std::make_unique<WildcardPatternNode>(kLoc), "WildcardPatternNode");
    expect_visit(std::make_unique<ListPatternNode>(kLoc, List<UPtr<PatternNode>>{}, Opt<String>{}), "ListPatternNode");
}
