#ifndef ARIA_ASTVISITOR_HPP
#define ARIA_ASTVISITOR_HPP

// AST 访问者（Visitor）接口：作为代码生成器（AST -> CodeUnit 的字节码编译器）等
// 遍历类（如语义分析器）的父类。
//
// 这里前置声明所有具体 AST 节点类型（完整定义见 compile/ast.hpp）。本头文件只用到
// 节点类型的引用，前置声明即可，与 ast.hpp 解耦；子类（代码生成器等）需自行 include
// ast.hpp 以访问节点成员。
//
// 每个具体节点对应一个 visitXxxNode(XxxNode&) 纯虚方法--子类必须逐一实现，编译器据此
// 强制覆盖全部节点类型，避免漏处理；双分派由节点的 accept(AstVisitor&) 完成
// （见 compile/ast.hpp），访问者自身不做按运行时类型的集中分派。
//
// 参数统一用非 const 引用：子类可在遍历中读写节点（如语义分析阶段注记解析结果）。
// 节点生命周期由 AST 的 UPtr 树持有者保证，访问者不拥有节点。

namespace aria {

    // --- 根节点 ---
    struct ProgramNode;

    // --- 语句节点（StmtNode 派生） ---
    struct BlockNode;
    struct ExprStmtNode;
    struct PrintStmtNode;
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

    // --- 表达式节点（ExprNode 派生） ---
    struct IntegerLiteralNode;
    struct FloatLiteralNode;
    struct StringLiteralNode;
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

    // --- 解构模式节点（PatternNode 派生） ---
    struct IdentifierPatternNode;
    struct WildcardPatternNode;
    struct ListPatternNode;

    class AstVisitor {
    public:
        virtual ~AstVisitor() = default;

        // --- 根节点 ---
        virtual void visitProgramNode(ProgramNode& node) = 0;

        // --- 语句节点 ---
        virtual void visitBlockNode(BlockNode& node)               = 0;
        virtual void visitExprStmtNode(ExprStmtNode& node)         = 0;
        virtual void visitPrintStmtNode(PrintStmtNode& node)       = 0;
        virtual void visitIfStmtNode(IfStmtNode& node)             = 0;
        virtual void visitWhileStmtNode(WhileStmtNode& node)       = 0;
        virtual void visitForStmtNode(ForStmtNode& node)           = 0;
        virtual void visitForInStmtNode(ForInStmtNode& node)       = 0;
        virtual void visitBreakStmtNode(BreakStmtNode& node)       = 0;
        virtual void visitContinueStmtNode(ContinueStmtNode& node) = 0;
        virtual void visitReturnStmtNode(ReturnStmtNode& node)     = 0;
        virtual void visitImportStmtNode(ImportStmtNode& node)     = 0;
        virtual void visitTryStmtNode(TryStmtNode& node)           = 0;
        virtual void visitThrowStmtNode(ThrowStmtNode& node)       = 0;
        virtual void visitMatchStmtNode(MatchStmtNode& node)       = 0;
        virtual void visitFunDeclNode(FunDeclNode& node)           = 0;
        virtual void visitDefDeclNode(DefDeclNode& node)           = 0;
        virtual void visitVarDeclNode(VarDeclNode& node)           = 0;

        // --- 表达式节点 ---
        virtual void visitIntegerLiteralNode(IntegerLiteralNode& node)               = 0;
        virtual void visitFloatLiteralNode(FloatLiteralNode& node)                   = 0;
        virtual void visitStringLiteralNode(StringLiteralNode& node)                 = 0;
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

        // --- 解构模式节点 ---
        virtual void visitIdentifierPatternNode(IdentifierPatternNode& node) = 0;
        virtual void visitWildcardPatternNode(WildcardPatternNode& node)     = 0;
        virtual void visitListPatternNode(ListPatternNode& node)             = 0;
    };

} // namespace aria

#endif // ARIA_ASTVISITOR_HPP
