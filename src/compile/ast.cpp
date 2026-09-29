#include "ast.hpp"

#include <format>

#include "AstVisitor.hpp"
#include "util/io.hpp"

namespace aria {

    using detail::ast::dump_node;

    // ASTNode

    void ASTNode::display() const { io::print("{}", dump(0)); }

    // 运算符可读名：switch 不加 default（新增枚举值时 -Wswitch 提示遗漏），UNREACHABLE() 收尾。

    namespace Op {
        StringView to_string(const Binary op) noexcept {
            switch (op) {
                case Binary::Or:
                    return "||";
                case Binary::And:
                    return "&&";
                case Binary::EqualEqual:
                    return "==";
                case Binary::EqualEqualEqual:
                    return "===";
                case Binary::BangEqual:
                    return "!=";
                case Binary::BangEqualEqual:
                    return "!==";
                case Binary::Greater:
                    return ">";
                case Binary::GreaterEqual:
                    return ">=";
                case Binary::Less:
                    return "<";
                case Binary::LessEqual:
                    return "<=";
                case Binary::Plus:
                    return "+";
                case Binary::Minus:
                    return "-";
                case Binary::Star:
                    return "*";
                case Binary::Slash:
                    return "/";
                case Binary::Percent:
                    return "%";
            }
            UNREACHABLE();
        }

        StringView to_string(const Unary op) noexcept {
            switch (op) {
                case Unary::Minus:
                    return "-";
                case Unary::Not:
                    return "!";
                case Unary::PreInc:
                    return "++";
                case Unary::PreDec:
                    return "--";
            }
            UNREACHABLE();
        }

        StringView to_string(const Assignment op) noexcept {
            switch (op) {
                case Assignment::Assign:
                    return "=";
                case Assignment::PlusAssign:
                    return "+=";
                case Assignment::MinusAssign:
                    return "-=";
                case Assignment::StarAssign:
                    return "*=";
                case Assignment::SlashAssign:
                    return "/=";
                case Assignment::PercentAssign:
                    return "%=";
            }
            UNREACHABLE();
        }
    } // namespace Op

    // 共享辅助类型的 dump：统一经 dump_node 收口（写头行后逐子项渲染，UPtr 空安全 / 值 / 列表自动
    // 分派，子项缩进 +1），条件拼 header 在调用点拼好后传入（基础设施见 ast.hpp）。

    String Param::dump(const usize indent) const {
        String header = std::format("Param name={}", name);
        header        = is_varargs ? header + " (varargs)" : header;
        return dump_node(indent, header, default_value);
    }

    // "_" 通配（value=nullptr）渲染为无子项的专用头行；null 子项经 dump_child 落空串，两种
    // 形态同行收口。
    String MatchPattern::dump(const usize indent) const {
        return dump_node(indent, value ? "MatchPattern" : "MatchPattern _ (wildcard)", value);
    }

    String MatchArm::dump(const usize indent) const { return dump_node(indent, "MatchArm", pattern, body); }

    String MatchExprArm::dump(const usize indent) const { return dump_node(indent, "MatchExprArm", pattern, body); }

    String VarBinding::dump(const usize indent) const { return dump_node(indent, "VarBinding", target, initializer); }

    String MapEntry::dump(const usize indent) const { return dump_node(indent, "MapEntry", key, value); }

    // ProgramNode dump

    String ProgramNode::dump(const usize indent) const {
        return dump_node(indent, std::format("Program decls={}", declarations.size()), declarations);
    }

    // 语句节点 dump

    String BlockNode::dump(const usize indent) const {
        return dump_node(indent, std::format("Block stmts={}", statements.size()), statements);
    }

    String ExprStmtNode::dump(const usize indent) const { return dump_node(indent, "ExprStmt", expr); }


    String IfStmtNode::dump(const usize indent) const {
        return dump_node(indent, "IfStmt", condition, then_branch, else_branch);
    }

    String WhileStmtNode::dump(const usize indent) const { return dump_node(indent, "WhileStmt", condition, body); }

    String ForStmtNode::dump(const usize indent) const {
        return dump_node(indent, "ForStmt", init, condition, increment, body);
    }

    String ForInStmtNode::dump(const usize indent) const {
        return dump_node(indent, "ForInStmt", pattern, iterable, body);
    }

    String BreakStmtNode::dump(const usize indent) const { return dump_node(indent, "BreakStmt"); }

    String ContinueStmtNode::dump(const usize indent) const { return dump_node(indent, "ContinueStmt"); }

    String ReturnStmtNode::dump(const usize indent) const { return dump_node(indent, "ReturnStmt", value); }

    String ImportStmtNode::dump(const usize indent) const {
        return dump_node(indent, std::format("ImportStmt path={} as={}", path, alias));
    }

    // catch 子树是「Catch param=... 标签行 + 体」的标准 dump_node 形态（标签行在 +1、体在
    // +2）；catch_body 判空由 dump_child 的 UPtr 空安全承担，catch_param 缺省时无 catch 子树。
    String TryStmtNode::dump(const usize indent) const {
        String out = dump_node(indent, "TryStmt", body);
        if (catch_param) {
            out += dump_node(indent + 1, std::format("Catch param={}", *catch_param), catch_body);
        }
        return out;
    }

    String ThrowStmtNode::dump(const usize indent) const { return dump_node(indent, "ThrowStmt", expr); }

    String MatchStmtNode::dump(const usize indent) const {
        return dump_node(indent, std::format("MatchStmt arms={}", arms.size()), subject, arms);
    }

    // 声明节点 dump

    String FunDeclNode::dump(const usize indent) const {
        return dump_node(indent, std::format("FunDecl name={} params={} kind={}", name, params.size(), to_string(kind)),
                         params, body);
    }

    String DefDeclNode::dump(const usize indent) const {
        String header = std::format("DefDecl name={}", name);
        if (superclass) {
            header += std::format(" super={}", *superclass);
        }
        if (is_member) {
            header += " member";
        }
        return dump_node(indent, header, members);
    }

    String VarDeclNode::dump(const usize indent) const {
        return dump_node(indent, std::format("VarDecl bindings={}", bindings.size()), bindings);
    }

    String StaticVarMemberNode::dump(const usize indent) const {
        return dump_node(indent, std::format("StaticVarMember name={}", name), initializer);
    }

    // 表达式节点 dump

    String IntegerLiteralNode::dump(const usize indent) const {
        return dump_node(indent, std::format("IntegerLiteral {}", value));
    }

    String FloatLiteralNode::dump(const usize indent) const {
        return dump_node(indent, std::format("FloatLiteral {}", value));
    }

    String StringLiteralNode::dump(const usize indent) const {
        return dump_node(indent, std::format("StringLiteral \"{}\"", value));
    }

    String BoolLiteralNode::dump(const usize indent) const {
        return dump_node(indent, std::format("BoolLiteral {}", value ? "true" : "false"));
    }

    String NilLiteralNode::dump(const usize indent) const { return dump_node(indent, "NilLiteral"); }

    String IdentifierNode::dump(const usize indent) const {
        return dump_node(indent, std::format("Identifier {}", name));
    }

    String ThisExprNode::dump(const usize indent) const { return dump_node(indent, "ThisExpr"); }

    String SuperExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("SuperExpr name={}", name));
    }

    String BinaryExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("BinaryExpr op={}", Op::to_string(op)), lhs, rhs);
    }

    String UnaryExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("UnaryExpr op={}", Op::to_string(op)), operand);
    }

    String AssignmentNode::dump(const usize indent) const {
        return dump_node(indent, std::format("Assignment op={}", Op::to_string(op)), target, value);
    }

    String DestructureAssignmentNode::dump(const usize indent) const {
        return dump_node(indent, "DestructureAssignment", target, value);
    }

    String CallNode::dump(const usize indent) const {
        return dump_node(indent, std::format("Call args={}", args.size()), callee, args);
    }

    String FieldAccessNode::dump(const usize indent) const {
        return dump_node(indent, std::format("FieldAccess name={}", name), object);
    }

    String IndexAccessNode::dump(const usize indent) const { return dump_node(indent, "IndexAccess", object, index); }

    String ListExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("ListExpr elements={}", elements.size()), elements);
    }

    String MapExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("MapExpr entries={}", entries.size()), entries);
    }

    String RangeExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("RangeExpr {}", is_exclusive ? "exclusive" : "inclusive"), lower, upper);
    }

    String IfExprNode::dump(const usize indent) const {
        return dump_node(indent, "IfExpr", condition, then_branch, else_branch);
    }

    String LambdaExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("LambdaExpr params={}", params.size()), params, body);
    }

    String MatchExprNode::dump(const usize indent) const {
        return dump_node(indent, std::format("MatchExpr arms={}", arms.size()), subject, arms);
    }

    // 解构模式节点 dump

    String IdentifierPatternNode::dump(const usize indent) const {
        return dump_node(indent, std::format("IdentifierPattern name={}", name));
    }

    String WildcardPatternNode::dump(const usize indent) const { return dump_node(indent, "WildcardPattern _"); }

    String ListPatternNode::dump(const usize indent) const {
        String header = std::format("ListPattern elements={}", elements.size());
        if (rest) {
            header += std::format(" rest={}", rest->name);
        }
        return dump_node(indent, header, elements);
    }

    // accept 分发（访问者模式）
    // 双分派机制见 compile/AstVisitor.hpp 头注与 ast.hpp accept 注。

    void ProgramNode::accept(AstVisitor& visitor) { visitor.visitProgramNode(*this); }

    void BlockNode::accept(AstVisitor& visitor) { visitor.visitBlockNode(*this); }
    void ExprStmtNode::accept(AstVisitor& visitor) { visitor.visitExprStmtNode(*this); }
    void IfStmtNode::accept(AstVisitor& visitor) { visitor.visitIfStmtNode(*this); }
    void WhileStmtNode::accept(AstVisitor& visitor) { visitor.visitWhileStmtNode(*this); }
    void ForStmtNode::accept(AstVisitor& visitor) { visitor.visitForStmtNode(*this); }
    void ForInStmtNode::accept(AstVisitor& visitor) { visitor.visitForInStmtNode(*this); }
    void BreakStmtNode::accept(AstVisitor& visitor) { visitor.visitBreakStmtNode(*this); }
    void ContinueStmtNode::accept(AstVisitor& visitor) { visitor.visitContinueStmtNode(*this); }
    void ReturnStmtNode::accept(AstVisitor& visitor) { visitor.visitReturnStmtNode(*this); }
    void ImportStmtNode::accept(AstVisitor& visitor) { visitor.visitImportStmtNode(*this); }
    void TryStmtNode::accept(AstVisitor& visitor) { visitor.visitTryStmtNode(*this); }
    void ThrowStmtNode::accept(AstVisitor& visitor) { visitor.visitThrowStmtNode(*this); }
    void MatchStmtNode::accept(AstVisitor& visitor) { visitor.visitMatchStmtNode(*this); }
    void FunDeclNode::accept(AstVisitor& visitor) { visitor.visitFunDeclNode(*this); }
    void DefDeclNode::accept(AstVisitor& visitor) { visitor.visitDefDeclNode(*this); }
    void VarDeclNode::accept(AstVisitor& visitor) { visitor.visitVarDeclNode(*this); }
    void StaticVarMemberNode::accept(AstVisitor& visitor) { visitor.visitStaticVarMemberNode(*this); }

    void IntegerLiteralNode::accept(AstVisitor& visitor) { visitor.visitIntegerLiteralNode(*this); }
    void FloatLiteralNode::accept(AstVisitor& visitor) { visitor.visitFloatLiteralNode(*this); }
    void StringLiteralNode::accept(AstVisitor& visitor) { visitor.visitStringLiteralNode(*this); }
    void BoolLiteralNode::accept(AstVisitor& visitor) { visitor.visitBoolLiteralNode(*this); }
    void NilLiteralNode::accept(AstVisitor& visitor) { visitor.visitNilLiteralNode(*this); }
    void IdentifierNode::accept(AstVisitor& visitor) { visitor.visitIdentifierNode(*this); }
    void ThisExprNode::accept(AstVisitor& visitor) { visitor.visitThisExprNode(*this); }
    void SuperExprNode::accept(AstVisitor& visitor) { visitor.visitSuperExprNode(*this); }
    void BinaryExprNode::accept(AstVisitor& visitor) { visitor.visitBinaryExprNode(*this); }
    void UnaryExprNode::accept(AstVisitor& visitor) { visitor.visitUnaryExprNode(*this); }
    void AssignmentNode::accept(AstVisitor& visitor) { visitor.visitAssignmentNode(*this); }
    void DestructureAssignmentNode::accept(AstVisitor& visitor) { visitor.visitDestructureAssignmentNode(*this); }
    void CallNode::accept(AstVisitor& visitor) { visitor.visitCallNode(*this); }
    void FieldAccessNode::accept(AstVisitor& visitor) { visitor.visitFieldAccessNode(*this); }
    void IndexAccessNode::accept(AstVisitor& visitor) { visitor.visitIndexAccessNode(*this); }
    void ListExprNode::accept(AstVisitor& visitor) { visitor.visitListExprNode(*this); }
    void MapExprNode::accept(AstVisitor& visitor) { visitor.visitMapExprNode(*this); }
    void RangeExprNode::accept(AstVisitor& visitor) { visitor.visitRangeExprNode(*this); }
    void IfExprNode::accept(AstVisitor& visitor) { visitor.visitIfExprNode(*this); }
    void LambdaExprNode::accept(AstVisitor& visitor) { visitor.visitLambdaExprNode(*this); }
    void MatchExprNode::accept(AstVisitor& visitor) { visitor.visitMatchExprNode(*this); }

    void IdentifierPatternNode::accept(AstVisitor& visitor) { visitor.visitIdentifierPatternNode(*this); }
    void WildcardPatternNode::accept(AstVisitor& visitor) { visitor.visitWildcardPatternNode(*this); }
    void ListPatternNode::accept(AstVisitor& visitor) { visitor.visitListPatternNode(*this); }

} // namespace aria
