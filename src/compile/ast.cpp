#include "ast.hpp"

#include <format>

#include "AstVisitor.hpp"
#include "util/io.hpp"

namespace aria {

    // =========================================================================
    // ASTNode
    // =========================================================================

    void ASTNode::display() const { io::print("{}", dump(0)); }

    // =========================================================================
    // 运算符可读名
    // =========================================================================
    //
    // switch 列举全部枚举值（不加 default），让编译器在新增枚举值时通过 -Wswitch
    // 给出遗漏告警；所有枚举值均已 return，函数末尾用 UNREACHABLE() 收尾（正常不可达）。

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

    // =========================================================================
    // 共享辅助类型的 dump
    // =========================================================================
    //
    // 每个 dump 构造本地 String out：先写自身头行（write_line），再 out += 子节点 dump(…)
    // 拼接子树文本，最后返回 out。

    String Param::dump(const usize indent) const {
        String       out;
        const String header =
                is_varargs ? std::format("Param name={} (varargs)", name) : std::format("Param name={}", name);
        detail::ast::write_line(out, indent, header);
        if (default_value) {
            out += default_value->dump(indent + 1);
        }
        return out;
    }

    String MatchPattern::dump(const usize indent) const {
        String out;
        if (!value) {
            detail::ast::write_line(out, indent, "MatchPattern _ (wildcard)");
            return out;
        }
        detail::ast::write_line(out, indent, "MatchPattern");
        out += value->dump(indent + 1);
        return out;
    }

    String MatchArm::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "MatchArm");
        out += pattern.dump(indent + 1);
        if (body) {
            out += body->dump(indent + 1);
        }
        return out;
    }

    String MatchExprArm::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "MatchExprArm");
        out += pattern.dump(indent + 1);
        if (body) {
            out += body->dump(indent + 1);
        }
        return out;
    }

    String VarBinding::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "VarBinding");
        if (target) {
            out += target->dump(indent + 1);
        }
        if (initializer) {
            out += initializer->dump(indent + 1);
        }
        return out;
    }

    String MapEntry::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "MapEntry");
        if (key) {
            out += key->dump(indent + 1);
        }
        if (value) {
            out += value->dump(indent + 1);
        }
        return out;
    }

    // =========================================================================
    // ProgramNode dump
    // =========================================================================

    String ProgramNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("Program decls={}", declarations.size()));
        for (const auto& d: declarations) {
            if (d) {
                out += d->dump(indent + 1);
            }
        }
        return out;
    }

    // =========================================================================
    // 语句节点 dump
    // =========================================================================

    String BlockNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("Block stmts={}", statements.size()));
        for (const auto& s: statements) {
            if (s) {
                out += s->dump(indent + 1);
            }
        }
        return out;
    }

    String ExprStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "ExprStmt");
        if (expr) {
            out += expr->dump(indent + 1);
        }
        return out;
    }

    String PrintStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "PrintStmt");
        if (expr) {
            out += expr->dump(indent + 1);
        }
        return out;
    }

    String IfStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "IfStmt");
        if (condition) {
            out += condition->dump(indent + 1);
        }
        if (then_branch) {
            out += then_branch->dump(indent + 1);
        }
        if (else_branch) {
            out += else_branch->dump(indent + 1);
        }
        return out;
    }

    String WhileStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "WhileStmt");
        if (condition) {
            out += condition->dump(indent + 1);
        }
        if (body) {
            out += body->dump(indent + 1);
        }
        return out;
    }

    String ForStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "ForStmt");
        if (init) {
            out += init->dump(indent + 1);
        }
        if (condition) {
            out += condition->dump(indent + 1);
        }
        if (increment) {
            out += increment->dump(indent + 1);
        }
        if (body) {
            out += body->dump(indent + 1);
        }
        return out;
    }

    String ForInStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "ForInStmt");
        if (pattern) {
            out += pattern->dump(indent + 1);
        }
        if (iterable) {
            out += iterable->dump(indent + 1);
        }
        if (body) {
            out += body->dump(indent + 1);
        }
        return out;
    }

    String BreakStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "BreakStmt");
        return out;
    }

    String ContinueStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "ContinueStmt");
        return out;
    }

    String ReturnStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "ReturnStmt");
        if (value) {
            out += value->dump(indent + 1);
        }
        return out;
    }

    String ImportStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("ImportStmt path={} as={}", path, alias));
        return out;
    }

    String TryStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "TryStmt");
        if (body) {
            out += body->dump(indent + 1);
        }
        if (catch_param) {
            detail::ast::write_line(out, indent + 1, std::format("Catch param={}", *catch_param));
            if (catch_body) {
                out += catch_body->dump(indent + 2);
            }
        }
        return out;
    }

    String ThrowStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "ThrowStmt");
        if (expr) {
            out += expr->dump(indent + 1);
        }
        return out;
    }

    String MatchStmtNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("MatchStmt arms={}", arms.size()));
        if (subject) {
            out += subject->dump(indent + 1);
        }
        for (const auto& arm: arms) {
            out += arm.dump(indent + 1);
        }
        return out;
    }

    // --- 声明节点 dump ---

    String FunDeclNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("FunDecl name={} params={}", name, params.size()));
        for (const auto& p: params) {
            out += p.dump(indent + 1);
        }
        if (body) {
            out += body->dump(indent + 1);
        }
        return out;
    }

    String DefMember::dump(const usize indent) const {
        String     out;
        StringView kind_name;
        switch (kind) {
            case Kind::StaticVar:
                kind_name = "StaticVar";
                break;
            case Kind::StaticMethod:
                kind_name = "StaticMethod";
                break;
            case Kind::InstanceMethod:
                kind_name = "InstanceMethod";
                break;
        }
        detail::ast::write_line(out, indent, std::format("DefMember kind={}", kind_name));
        if (node) {
            out += node->dump(indent + 1);
        }
        return out;
    }

    String DefDeclNode::dump(const usize indent) const {
        String out;
        String header = std::format("DefDecl name={}", name);
        if (superclass) {
            header += std::format(" super={}", *superclass);
        }
        detail::ast::write_line(out, indent, header);
        for (const auto& m: members) {
            out += m.dump(indent + 1);
        }
        return out;
    }

    String VarDeclNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("VarDecl bindings={}", bindings.size()));
        for (const auto& b: bindings) {
            out += b.dump(indent + 1);
        }
        return out;
    }

    // =========================================================================
    // 表达式节点 dump
    // =========================================================================

    String IntegerLiteralNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("IntegerLiteral {}", value));
        return out;
    }

    String FloatLiteralNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("FloatLiteral {}", value));
        return out;
    }

    String StringLiteralNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("StringLiteral \"{}\"", value));
        return out;
    }

    String BoolLiteralNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("BoolLiteral {}", value ? "true" : "false"));
        return out;
    }

    String NilLiteralNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "NilLiteral");
        return out;
    }

    String IdentifierNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("Identifier {}", name));
        return out;
    }

    String ThisExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "ThisExpr");
        return out;
    }

    String SuperExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "SuperExpr");
        return out;
    }

    String BinaryExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("BinaryExpr op={}", Op::to_string(op)));
        if (lhs) {
            out += lhs->dump(indent + 1);
        }
        if (rhs) {
            out += rhs->dump(indent + 1);
        }
        return out;
    }

    String UnaryExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("UnaryExpr op={}", Op::to_string(op)));
        if (operand) {
            out += operand->dump(indent + 1);
        }
        return out;
    }

    String AssignmentNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("Assignment op={}", Op::to_string(op)));
        if (target) {
            out += target->dump(indent + 1);
        }
        if (value) {
            out += value->dump(indent + 1);
        }
        return out;
    }

    String DestructureAssignmentNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "DestructureAssignment");
        if (target) {
            out += target->dump(indent + 1);
        }
        if (value) {
            out += value->dump(indent + 1);
        }
        return out;
    }

    String CallNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("Call args={}", args.size()));
        if (callee) {
            out += callee->dump(indent + 1);
        }
        for (const auto& arg: args) {
            if (arg) {
                out += arg->dump(indent + 1);
            }
        }
        return out;
    }

    String FieldAccessNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("FieldAccess name={}", name));
        if (object) {
            out += object->dump(indent + 1);
        }
        return out;
    }

    String IndexAccessNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "IndexAccess");
        if (object) {
            out += object->dump(indent + 1);
        }
        if (index) {
            out += index->dump(indent + 1);
        }
        return out;
    }

    String ListExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("ListExpr elements={}", elements.size()));
        for (const auto& e: elements) {
            if (e) {
                out += e->dump(indent + 1);
            }
        }
        return out;
    }

    String MapExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("MapExpr entries={}", entries.size()));
        for (const auto& entry: entries) {
            out += entry.dump(indent + 1);
        }
        return out;
    }

    String RangeExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("RangeExpr {}", is_exclusive ? "exclusive" : "inclusive"));
        if (lower) {
            out += lower->dump(indent + 1);
        }
        if (upper) {
            out += upper->dump(indent + 1);
        }
        return out;
    }

    String IfExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "IfExpr");
        if (condition) {
            out += condition->dump(indent + 1);
        }
        if (then_branch) {
            out += then_branch->dump(indent + 1);
        }
        if (else_branch) {
            out += else_branch->dump(indent + 1);
        }
        return out;
    }

    String LambdaExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("LambdaExpr params={}", params.size()));
        for (const auto& p: params) {
            out += p.dump(indent + 1);
        }
        if (body) {
            out += body->dump(indent + 1);
        }
        return out;
    }

    String MatchExprNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("MatchExpr arms={}", arms.size()));
        if (subject) {
            out += subject->dump(indent + 1);
        }
        for (const auto& arm: arms) {
            out += arm.dump(indent + 1);
        }
        return out;
    }

    // =========================================================================
    // 解构模式节点 dump
    // =========================================================================

    String IdentifierPatternNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, std::format("IdentifierPattern name={}", name));
        return out;
    }

    String WildcardPatternNode::dump(const usize indent) const {
        String out;
        detail::ast::write_line(out, indent, "WildcardPattern _");
        return out;
    }

    String ListPatternNode::dump(const usize indent) const {
        String out;
        String header = std::format("ListPattern elements={}", elements.size());
        if (rest) {
            header += std::format(" rest={}", *rest);
        }
        detail::ast::write_line(out, indent, header);
        for (const auto& e: elements) {
            if (e) {
                out += e->dump(indent + 1);
            }
        }
        return out;
    }

    // =========================================================================
    // accept 分发（访问者模式）
    // =========================================================================
    //
    // 各具体节点把实际类型交给访问者：visitor.visitXxxNode(this)。访问者基类 AstVisitor
    // 仅声明 visitXxxNode 纯虚接口（见 compile/AstVisitor.hpp），不做按运行时类型的集中
    // 分派；双分派由节点自身的 accept 完成。参数为非 const 指针，
    // 子类可读写节点。

    void ProgramNode::accept(AstVisitor& visitor) { visitor.visitProgramNode(this); }

    void BlockNode::accept(AstVisitor& visitor) { visitor.visitBlockNode(this); }
    void ExprStmtNode::accept(AstVisitor& visitor) { visitor.visitExprStmtNode(this); }
    void PrintStmtNode::accept(AstVisitor& visitor) { visitor.visitPrintStmtNode(this); }
    void IfStmtNode::accept(AstVisitor& visitor) { visitor.visitIfStmtNode(this); }
    void WhileStmtNode::accept(AstVisitor& visitor) { visitor.visitWhileStmtNode(this); }
    void ForStmtNode::accept(AstVisitor& visitor) { visitor.visitForStmtNode(this); }
    void ForInStmtNode::accept(AstVisitor& visitor) { visitor.visitForInStmtNode(this); }
    void BreakStmtNode::accept(AstVisitor& visitor) { visitor.visitBreakStmtNode(this); }
    void ContinueStmtNode::accept(AstVisitor& visitor) { visitor.visitContinueStmtNode(this); }
    void ReturnStmtNode::accept(AstVisitor& visitor) { visitor.visitReturnStmtNode(this); }
    void ImportStmtNode::accept(AstVisitor& visitor) { visitor.visitImportStmtNode(this); }
    void TryStmtNode::accept(AstVisitor& visitor) { visitor.visitTryStmtNode(this); }
    void ThrowStmtNode::accept(AstVisitor& visitor) { visitor.visitThrowStmtNode(this); }
    void MatchStmtNode::accept(AstVisitor& visitor) { visitor.visitMatchStmtNode(this); }
    void FunDeclNode::accept(AstVisitor& visitor) { visitor.visitFunDeclNode(this); }
    void DefDeclNode::accept(AstVisitor& visitor) { visitor.visitDefDeclNode(this); }
    void VarDeclNode::accept(AstVisitor& visitor) { visitor.visitVarDeclNode(this); }

    void IntegerLiteralNode::accept(AstVisitor& visitor) { visitor.visitIntegerLiteralNode(this); }
    void FloatLiteralNode::accept(AstVisitor& visitor) { visitor.visitFloatLiteralNode(this); }
    void StringLiteralNode::accept(AstVisitor& visitor) { visitor.visitStringLiteralNode(this); }
    void BoolLiteralNode::accept(AstVisitor& visitor) { visitor.visitBoolLiteralNode(this); }
    void NilLiteralNode::accept(AstVisitor& visitor) { visitor.visitNilLiteralNode(this); }
    void IdentifierNode::accept(AstVisitor& visitor) { visitor.visitIdentifierNode(this); }
    void ThisExprNode::accept(AstVisitor& visitor) { visitor.visitThisExprNode(this); }
    void SuperExprNode::accept(AstVisitor& visitor) { visitor.visitSuperExprNode(this); }
    void BinaryExprNode::accept(AstVisitor& visitor) { visitor.visitBinaryExprNode(this); }
    void UnaryExprNode::accept(AstVisitor& visitor) { visitor.visitUnaryExprNode(this); }
    void AssignmentNode::accept(AstVisitor& visitor) { visitor.visitAssignmentNode(this); }
    void DestructureAssignmentNode::accept(AstVisitor& visitor) { visitor.visitDestructureAssignmentNode(this); }
    void CallNode::accept(AstVisitor& visitor) { visitor.visitCallNode(this); }
    void FieldAccessNode::accept(AstVisitor& visitor) { visitor.visitFieldAccessNode(this); }
    void IndexAccessNode::accept(AstVisitor& visitor) { visitor.visitIndexAccessNode(this); }
    void ListExprNode::accept(AstVisitor& visitor) { visitor.visitListExprNode(this); }
    void MapExprNode::accept(AstVisitor& visitor) { visitor.visitMapExprNode(this); }
    void RangeExprNode::accept(AstVisitor& visitor) { visitor.visitRangeExprNode(this); }
    void IfExprNode::accept(AstVisitor& visitor) { visitor.visitIfExprNode(this); }
    void LambdaExprNode::accept(AstVisitor& visitor) { visitor.visitLambdaExprNode(this); }
    void MatchExprNode::accept(AstVisitor& visitor) { visitor.visitMatchExprNode(this); }

    void IdentifierPatternNode::accept(AstVisitor& visitor) { visitor.visitIdentifierPatternNode(this); }
    void WildcardPatternNode::accept(AstVisitor& visitor) { visitor.visitWildcardPatternNode(this); }
    void ListPatternNode::accept(AstVisitor& visitor) { visitor.visitListPatternNode(this); }

} // namespace aria
