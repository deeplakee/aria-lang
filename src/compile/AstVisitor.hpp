#ifndef ARIA_ASTVISITOR_HPP
#define ARIA_ASTVISITOR_HPP

// AST 访问者接口：visitXxxNode(XxxNode&) 纯虚方法与节点一一对应，双分派经节点的 accept 完成。
// 前置声明全部节点类型，子类自行 include 访问节点成员。

namespace aria {

    struct ProgramNode;

    // 语句节点（StmtNode 派生）
    struct BlockNode;
    struct ExprStmtNode;
    struct IfStmtNode;
    struct WhileStmtNode;
    struct ForStmtNode;
    struct ForInStmtNode;
    struct BreakStmtNode;
    struct ContinueStmtNode;
    struct ReturnStmtNode;
    struct ImportStmtNode;
    struct TryStmtNode;
    struct ThrowStmtNode;
    struct MatchStmtNode;
    struct FunDeclNode;
    struct DefDeclNode;
    struct VarDeclNode;
    struct StaticVarMemberNode;

    // 表达式节点（ExprNode 派生）
    struct IntegerLiteralNode;
    struct FloatLiteralNode;
    struct StringLiteralNode;
    struct InterpolatedStringNode;
    struct BoolLiteralNode;
    struct NilLiteralNode;
    struct IdentifierNode;
    struct ThisExprNode;
    struct SuperExprNode;
    struct BinaryExprNode;
    struct UnaryExprNode;
    struct AssignmentNode;
    struct DestructureAssignmentNode;
    struct CallNode;
    struct FieldAccessNode;
    struct IndexAccessNode;
    struct ListExprNode;
    struct MapExprNode;
    struct RangeExprNode;
    struct IfExprNode;
    struct LambdaExprNode;
    struct MatchExprNode;
    struct SequenceExprNode;

    // 解构模式节点（PatternNode 派生）
    struct IdentifierPatternNode;
    struct WildcardPatternNode;
    struct ListPatternNode;

    class AstVisitor {
    public:
        virtual ~AstVisitor() = default;

        virtual void visitProgramNode(ProgramNode& node) = 0;

        // 语句节点
        virtual void visitBlockNode(BlockNode& node)                     = 0;
        virtual void visitExprStmtNode(ExprStmtNode& node)               = 0;
        virtual void visitIfStmtNode(IfStmtNode& node)                   = 0;
        virtual void visitWhileStmtNode(WhileStmtNode& node)             = 0;
        virtual void visitForStmtNode(ForStmtNode& node)                 = 0;
        virtual void visitForInStmtNode(ForInStmtNode& node)             = 0;
        virtual void visitBreakStmtNode(BreakStmtNode& node)             = 0;
        virtual void visitContinueStmtNode(ContinueStmtNode& node)       = 0;
        virtual void visitReturnStmtNode(ReturnStmtNode& node)           = 0;
        virtual void visitImportStmtNode(ImportStmtNode& node)           = 0;
        virtual void visitTryStmtNode(TryStmtNode& node)                 = 0;
        virtual void visitThrowStmtNode(ThrowStmtNode& node)             = 0;
        virtual void visitMatchStmtNode(MatchStmtNode& node)             = 0;
        virtual void visitFunDeclNode(FunDeclNode& node)                 = 0;
        virtual void visitDefDeclNode(DefDeclNode& node)                 = 0;
        virtual void visitVarDeclNode(VarDeclNode& node)                 = 0;
        virtual void visitStaticVarMemberNode(StaticVarMemberNode& node) = 0;

        // 表达式节点
        virtual void visitIntegerLiteralNode(IntegerLiteralNode& node)               = 0;
        virtual void visitFloatLiteralNode(FloatLiteralNode& node)                   = 0;
        virtual void visitStringLiteralNode(StringLiteralNode& node)                 = 0;
        virtual void visitInterpolatedStringNode(InterpolatedStringNode& node)       = 0;
        virtual void visitBoolLiteralNode(BoolLiteralNode& node)                     = 0;
        virtual void visitNilLiteralNode(NilLiteralNode& node)                       = 0;
        virtual void visitIdentifierNode(IdentifierNode& node)                       = 0;
        virtual void visitThisExprNode(ThisExprNode& node)                           = 0;
        virtual void visitSuperExprNode(SuperExprNode& node)                         = 0;
        virtual void visitBinaryExprNode(BinaryExprNode& node)                       = 0;
        virtual void visitUnaryExprNode(UnaryExprNode& node)                         = 0;
        virtual void visitAssignmentNode(AssignmentNode& node)                       = 0;
        virtual void visitDestructureAssignmentNode(DestructureAssignmentNode& node) = 0;
        virtual void visitCallNode(CallNode& node)                                   = 0;
        virtual void visitFieldAccessNode(FieldAccessNode& node)                     = 0;
        virtual void visitIndexAccessNode(IndexAccessNode& node)                     = 0;
        virtual void visitListExprNode(ListExprNode& node)                           = 0;
        virtual void visitMapExprNode(MapExprNode& node)                             = 0;
        virtual void visitRangeExprNode(RangeExprNode& node)                         = 0;
        virtual void visitIfExprNode(IfExprNode& node)                               = 0;
        virtual void visitLambdaExprNode(LambdaExprNode& node)                       = 0;
        virtual void visitMatchExprNode(MatchExprNode& node)                         = 0;
        virtual void visitSequenceExprNode(SequenceExprNode& node)                   = 0;

        // 解构模式节点
        virtual void visitIdentifierPatternNode(IdentifierPatternNode& node) = 0;
        virtual void visitWildcardPatternNode(WildcardPatternNode& node)     = 0;
        virtual void visitListPatternNode(ListPatternNode& node)             = 0;
    };

} // namespace aria

#endif // ARIA_ASTVISITOR_HPP
