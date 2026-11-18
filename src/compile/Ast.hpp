#ifndef ARIA_AST_HPP
#define ARIA_AST_HPP

#include "common.hpp"
#include "compile/FnKind.hpp"
#include "compile/Token.hpp"
#include "util/source_file.hpp"

namespace aria {

    using src::SourceLoc;

    class AstVisitor;

    struct StringLiteralNode;
    struct ListPatternNode;

    // 节点和列表由 AstArena 整批持有，节点间指针仅借用，不单独析构。
    // SourceLoc 借用源文件；合成节点可无位置。
    struct ASTNode {
        ASTNode() noexcept = default;
        explicit ASTNode(const SourceLoc loc) noexcept : loc_{loc} {}
        virtual ~ASTNode() = default;

        ASTNode(const ASTNode&)                = delete;
        ASTNode& operator=(const ASTNode&)     = delete;
        ASTNode(ASTNode&&) noexcept            = delete;
        ASTNode& operator=(ASTNode&&) noexcept = delete;

        [[nodiscard]]
        SourceLoc loc() const noexcept {
            return loc_;
        }

        [[nodiscard]]
        u32 line() const noexcept {
            return loc_.line();
        }

        [[nodiscard]]
        virtual String dump(usize indent) const = 0;

        void display() const;

        virtual void accept(AstVisitor& visitor) = 0;

    protected:
        SourceLoc loc_; // 源码位置（空态表合成节点）；src 非拥有，不得越过 SourceFile 存活期
    };

    struct StmtNode : ASTNode {
        using ASTNode::ASTNode;
    };

    struct ExprNode : ASTNode {
        using ASTNode::ASTNode;
    };

    struct PatternNode : ASTNode {
        using ASTNode::ASTNode;
    };

    // 运算符枚举与 TokenType 解耦：同一词形按位置映射不同运算（如 Minus 分二元/一元），语义独立于词法拼写。

    namespace Op {
        enum class Binary : u8 {
            Or,
            And,
            EqualEqual,
            EqualEqualEqual,
            BangEqual,      // !=
            BangEqualEqual, // !==
            Greater,
            GreaterEqual,
            Less,
            LessEqual,
            Plus,
            Minus,
            Star,
            Slash,
            Percent,
            BitAnd, // &（按位与；逻辑 && 是上面的 And）
            BitOr,  // |（按位或；逻辑 || 是上面的 Or）
            BitXor, // ^
            Shl,    // <<
            Shr,    // >>（算术右移）
            Ushr,   // >>>（逻辑右移）
        };

        enum class Unary : u8 {
            Minus,
            Not,
            PreInc, // ++（前置）
            PreDec, // --（前置）
            BitNot, // ~（按位取反）
        };

        enum class Assignment : u8 {
            Assign,
            PlusAssign,
            MinusAssign,
            StarAssign,
            SlashAssign,
            PercentAssign,
            BitAndAssign,
            BitOrAssign,
            BitXorAssign,
            ShlAssign,
            ShrAssign,
            UshrAssign,
        };

        [[nodiscard]]
        StringView to_string(Binary op) noexcept;

        [[nodiscard]]
        StringView to_string(Unary op) noexcept;

        [[nodiscard]]
        StringView to_string(Assignment op) noexcept;
    } // namespace Op

    // varargs 与默认值互斥。
    struct Param {
        StringView name;
        ExprNode*  default_val = nullptr;
        bool       is_varargs  = false;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 分支模式：expression 或 "_" 二选一（value 为 nullptr）；模式是相等比较，不绑定。
    struct MatchPattern {
        ExprNode* value = nullptr; // nullptr -> "_" 通配

        [[nodiscard]]
        String dump(usize indent) const;
    };

    struct MatchArm {
        MatchPattern pattern;
        StmtNode*    body = nullptr;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    struct MatchExprArm {
        MatchPattern pattern;
        ExprNode*    body = nullptr; // 结果表达式

        [[nodiscard]]
        String dump(usize indent) const;
    };

    struct VarBinding {
        PatternNode* target      = nullptr;
        ExprNode*    initializer = nullptr;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    struct MapEntry {
        ExprNode* key   = nullptr;
        ExprNode* value = nullptr;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // dump 基础设施
    namespace detail::ast {

        inline void write_indent(String& out, const usize indent) {
            for (usize i = 0; i < indent; ++i) {
                out.append("  ", 2);
            }
        }

        inline void write_line(String& out, const usize indent, const StringView content) {
            write_indent(out, indent);
            out.append(content);
            out.push_back('\n');
        }

        template<typename T>
        concept Dumpable = requires(const T& value, const usize indent) { value.dump(indent); };

        template<Dumpable T>
        String dump_child(const usize indent, const T* node) {
            return node != nullptr ? node->dump(indent) : "";
        }

        template<Dumpable T>
        String dump_child(const usize indent, const T& value) {
            return value.dump(indent);
        }

        template<Dumpable T>
        String dump_child(const usize indent, const Span<T*>& nodes) {
            String out;
            for (const T* node: nodes) {
                if (node != nullptr) {
                    out += node->dump(indent);
                }
            }
            return out;
        }

        template<Dumpable T>
        String dump_child(const usize indent, const Span<T>& values) {
            String out;
            for (const T& value: values) {
                out += value.dump(indent);
            }
            return out;
        }

        template<typename... Kids>
        String dump_node(const usize indent, const StringView header, const Kids&... kids) {
            String out;
            write_line(out, indent, header);
            ((out += dump_child(indent + 1, kids)), ...);
            return out;
        }

    } // namespace detail::ast

    struct ProgramNode : ASTNode {
        ProgramNode(const SourceLoc loc, const Span<StmtNode*> decls) : ASTNode{loc}, decls{decls} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<StmtNode*> decls;
    };

    // 语句节点（StmtNode）

    struct BlockNode : StmtNode {
        BlockNode(const SourceLoc loc, const Span<StmtNode*> stmts) : StmtNode{loc}, stmts{stmts} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<StmtNode*> stmts;
    };

    struct ExprStmtNode : StmtNode {
        ExprStmtNode(const SourceLoc loc, ExprNode* expr) : StmtNode{loc}, expr{expr} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* expr;
    };

    struct IfStmtNode : StmtNode {
        IfStmtNode(const SourceLoc loc, ExprNode* cond, StmtNode* then_br, StmtNode* else_br) :
            StmtNode{loc}, cond{cond}, then_br{then_br}, else_br{else_br} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* cond;
        StmtNode* then_br;
        StmtNode* else_br;
    };

    struct WhileStmtNode : StmtNode {
        WhileStmtNode(const SourceLoc loc, ExprNode* cond, StmtNode* body) : StmtNode{loc}, cond{cond}, body{body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* cond;
        StmtNode* body;
    };

    // for 语句（C 风格），init / cond / incr 均可省。
    struct ForStmtNode : StmtNode {
        ForStmtNode(const SourceLoc loc, StmtNode* init, ExprNode* cond, ExprNode* incr, StmtNode* body) :
            StmtNode{loc}, init{init}, cond{cond}, incr{incr}, body{body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StmtNode* init;
        ExprNode* cond;
        ExprNode* incr;
        StmtNode* body;
    };

    struct ForInStmtNode : StmtNode {
        ForInStmtNode(const SourceLoc loc, PatternNode* pattern, ExprNode* iterable, StmtNode* body) :
            StmtNode{loc}, pattern{pattern}, iterable{iterable}, body{body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        PatternNode* pattern;
        ExprNode*    iterable;
        StmtNode*    body;
    };

    struct BreakStmtNode : StmtNode {
        using StmtNode::StmtNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    struct ContinueStmtNode : StmtNode {
        using StmtNode::StmtNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    struct ReturnStmtNode : StmtNode {
        ReturnStmtNode(const SourceLoc loc, ExprNode* value) : StmtNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* value;
    };

    // path 保留字面量原文，在代码生成端解码。
    struct ImportStmtNode : StmtNode {
        ImportStmtNode(const SourceLoc loc, StringLiteralNode* path, const StringView alias) :
            StmtNode{loc}, path{path}, alias{alias} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringLiteralNode* path;
        StringView         alias;
    };

    // ename/catch_body 成对缺省；解析允许无 catch，由语义阶段拒绝。
    struct TryStmtNode : StmtNode {
        TryStmtNode(const SourceLoc loc, BlockNode* body, const StringView ename, BlockNode* catch_body) :
            StmtNode{loc}, body{body}, ename{ename}, catch_body{catch_body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        BlockNode* body;
        StringView ename;
        BlockNode* catch_body;
    };

    struct ThrowStmtNode : StmtNode {
        ThrowStmtNode(const SourceLoc loc, ExprNode* expr) : StmtNode{loc}, expr{expr} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* expr;
    };

    struct MatchStmtNode : StmtNode {
        MatchStmtNode(const SourceLoc loc, ExprNode* subject, const Span<MatchArm> arms) :
            StmtNode{loc}, subject{subject}, arms{arms} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode*      subject;
        Span<MatchArm> arms;
    };

    // 声明节点（StmtNode 派生：声明即「可出现在 program/block 顶层的语句」）

    struct FunDeclNode : StmtNode {
        FunDeclNode(const SourceLoc loc, const StringView name, const Span<Param> params, BlockNode* body,
                    const FnKind kind) : StmtNode{loc}, name{name}, params{params}, body{body}, kind{kind} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView  name;
        Span<Param> params;
        BlockNode*  body;
        FnKind      kind;
    };

    // 成员保留源码顺序，静态变量依此初始化。
    struct DefDeclNode : StmtNode {
        DefDeclNode(const SourceLoc loc, const StringView name, ExprNode* superclass, const Span<StmtNode*> members,
                    const bool is_member) :
            StmtNode{loc}, name{name}, super{superclass}, members{members}, is_member{is_member} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView      name;
        ExprNode*       super;
        Span<StmtNode*> members;
        bool            is_member; // 语句位声明(false)/类体成员位嵌套类(true)
    };

    struct VarDeclNode : StmtNode {
        VarDeclNode(const SourceLoc loc, const Span<VarBinding> bindings) : StmtNode{loc}, bindings{bindings} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<VarBinding> bindings;
    };

    // def 体静态变量成员：单 identifier 绑定的窄节点，多绑定/解构 pattern 由文法在成员位拒绝。
    struct StaticVarMemberNode : StmtNode {
        StaticVarMemberNode(const SourceLoc loc, const StringView name, ExprNode* initializer) :
            StmtNode{loc}, name{name}, initializer{initializer} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView name;
        ExprNode*  initializer;
    };

    // 表达式节点（ExprNode）

    // 字面量与基础表达式（primary）
    // 注：parenExpr -> "(" expression ")" 不设独立节点，AST 直接保留内层表达式。

    struct IntegerLiteralNode : ExprNode {
        IntegerLiteralNode(const SourceLoc loc, const i64 value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        i64 value;
    };

    struct FloatLiteralNode : ExprNode {
        FloatLiteralNode(const SourceLoc loc, const f64 value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        f64 value;
    };

    // value 借用源缓冲中的未展开原文，shape 用于消费端定容解码。
    struct StringLiteralNode : ExprNode {
        StringLiteralNode(const SourceLoc loc, const StringView value, const StringShape shape) :
            ExprNode{loc}, value{value}, shape{shape} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView  value;
        StringShape shape;
    };

    // 字面段和插值表达式按顺序存放，空字面段不入列。
    struct InterpolatedStringNode : ExprNode {
        InterpolatedStringNode(const SourceLoc loc, const Span<ExprNode*> parts) : ExprNode{loc}, parts{parts} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<ExprNode*> parts;
    };

    struct BoolLiteralNode : ExprNode {
        BoolLiteralNode(const SourceLoc loc, const bool value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        bool value;
    };

    struct NilLiteralNode : ExprNode {
        using ExprNode::ExprNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    struct IdentifierNode : ExprNode {
        IdentifierNode(const SourceLoc loc, const StringView name) : ExprNode{loc}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView name;
    };

    struct ThisExprNode : ExprNode {
        using ExprNode::ExprNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // 裸 super 不合法，super 成员不可作左值。
    struct SuperExprNode : ExprNode {
        SuperExprNode(const SourceLoc loc, const StringView name) : ExprNode{loc}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView name;
    };

    // 运算符表达式

    struct BinaryExprNode : ExprNode {
        BinaryExprNode(const SourceLoc loc, const Op::Binary op, ExprNode* lhs, ExprNode* rhs) :
            ExprNode{loc}, op{op}, lhs{lhs}, rhs{rhs} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Binary op;
        ExprNode*  lhs;
        ExprNode*  rhs;
    };

    struct UnaryExprNode : ExprNode {
        UnaryExprNode(const SourceLoc loc, const Op::Unary op, ExprNode* operand) :
            ExprNode{loc}, op{op}, operand{operand} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Unary op;
        ExprNode* operand;
    };

    // 目标合法性延至语义阶段检查。
    struct AssignmentNode : ExprNode {
        AssignmentNode(const SourceLoc loc, const Op::Assignment op, ExprNode* target, ExprNode* value) :
            ExprNode{loc}, op{op}, target{target}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Assignment op;
        ExprNode*      target;
        ExprNode*      value;
    };

    struct DestructureAssignmentNode : ExprNode {
        DestructureAssignmentNode(const SourceLoc loc, ListPatternNode* target, ExprNode* value) :
            ExprNode{loc}, target{target}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ListPatternNode* target;
        ExprNode*        value;
    };

    struct CallNode : ExprNode {
        CallNode(const SourceLoc loc, ExprNode* callee, const Span<ExprNode*> args) :
            ExprNode{loc}, callee{callee}, args{args} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode*       callee;
        Span<ExprNode*> args;
    };

    struct FieldAccessNode : ExprNode {
        FieldAccessNode(const SourceLoc loc, ExprNode* object, const StringView name) :
            ExprNode{loc}, object{object}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode*  object;
        StringView name;
    };

    struct IndexAccessNode : ExprNode {
        IndexAccessNode(const SourceLoc loc, ExprNode* object, ExprNode* index) :
            ExprNode{loc}, object{object}, index{index} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* object;
        ExprNode* index;
    };

    // 复合表达式（primary 的复合部分）

    struct ListExprNode : ExprNode {
        ListExprNode(const SourceLoc loc, const Span<ExprNode*> elems) : ExprNode{loc}, elems{elems} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<ExprNode*> elems;
    };

    struct MapExprNode : ExprNode {
        MapExprNode(const SourceLoc loc, const Span<MapEntry> entries) : ExprNode{loc}, entries{entries} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<MapEntry> entries;
    };

    // 区间表达式。无上界时 upper 为空（a.. 与 a... 同义），含否由 is_exclusive 区分。
    struct RangeExprNode : ExprNode {
        RangeExprNode(const SourceLoc loc, const bool is_exclusive, ExprNode* lower, ExprNode* upper) :
            ExprNode{loc}, is_exclusive{is_exclusive}, lower{lower}, upper{upper} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        bool      is_exclusive; // true: a...b（不含上界）；false: a..b（含上界）
        ExprNode* lower;
        ExprNode* upper;
    };

    struct IfExprNode : ExprNode {
        IfExprNode(const SourceLoc loc, ExprNode* cond, ExprNode* then_br, ExprNode* else_br) :
            ExprNode{loc}, cond{cond}, then_br{then_br}, else_br{else_br} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* cond;
        ExprNode* then_br;
        ExprNode* else_br;
    };

    struct LambdaExprNode : ExprNode {
        LambdaExprNode(const SourceLoc loc, const Span<Param> params, BlockNode* body) :
            ExprNode{loc}, params{params}, body{body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<Param> params;
        BlockNode*  body;
    };

    struct MatchExprNode : ExprNode {
        MatchExprNode(const SourceLoc loc, ExprNode* subject, const Span<MatchExprArm> arms) :
            ExprNode{loc}, subject{subject}, arms{arms} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode*          subject;
        Span<MatchExprArm> arms;
    };

    // 顺序求值，结果取末项；单元素分组不生成此节点，以保留左值性质。
    struct SequenceExprNode : ExprNode {
        SequenceExprNode(const SourceLoc loc, const Span<ExprNode*> exprs) : ExprNode{loc}, exprs{exprs} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<ExprNode*> exprs;
    };

    struct IdentifierPatternNode : PatternNode {
        IdentifierPatternNode(const SourceLoc loc, const StringView name) : PatternNode{loc}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView name;
    };

    struct WildcardPatternNode : PatternNode {
        using PatternNode::PatternNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // rest 收集为新 list，nullptr 表示无 rest；文法不允许 ..._。
    struct ListPatternNode : PatternNode {
        ListPatternNode(const SourceLoc loc, const Span<PatternNode*> elems, IdentifierPatternNode* rest) :
            PatternNode{loc}, elems{elems}, rest{rest} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<PatternNode*>     elems;
        IdentifierPatternNode* rest;
    };

} // namespace aria

#endif // ARIA_AST_HPP
