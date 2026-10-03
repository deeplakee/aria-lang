#ifndef ARIA_AST_HPP
#define ARIA_AST_HPP

#include "common.hpp"
#include "compile/FnKind.hpp"
#include "util/source_file.hpp"

namespace aria {

    using src::SourceLoc;

    // 前置声明 AstVisitor：节点经 accept 参与双分派，这里只需声明不需完整类型。
    class AstVisitor;

    // AST 根基类：所有节点持 SourceLoc（parser 取构造首 token，供语义/代码生成报错定位；空态 = 合成节点）。
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

        // 行号（1-based；空态为 0，不兜底直接取 loc 原值）。
        [[nodiscard]]
        u32 line() const noexcept {
            return loc_.line();
        }

        [[nodiscard]]
        virtual String dump(usize indent) const = 0;

        void display() const;

        // 双分派入口。
        virtual void accept(AstVisitor& visitor) = 0;

    protected:
        SourceLoc loc_; // 源码位置（空态表合成节点）；src 非拥有，不得越过 SourceFile 存活期
    };

    // 分类基类（仅作分类标记、无额外数据，继承 ASTNode 构造）：StmtNode 语句（声明 fun/def/var 亦属之）、
    // ExprNode 表达式、PatternNode 解构模式（var 目标 / for-in 目标 / 解构赋值左侧）。
    // 节点段序即依赖序，无需任何前置声明。
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
            Or,              // ||
            And,             // &&
            EqualEqual,      // ==
            EqualEqualEqual, // ===
            BangEqual,       // !=
            BangEqualEqual,  // !==
            Greater,         // >
            GreaterEqual,    // >=
            Less,            // <
            LessEqual,       // <=
            Plus,            // +
            Minus,           // -
            Star,            // *
            Slash,           // /
            Percent,         // %
        };

        enum class Unary : u8 {
            Minus,  // -
            Not,    // !
            PreInc, // ++（前置）
            PreDec, // --（前置）
        };

        enum class Assignment : u8 {
            Assign,        // =
            PlusAssign,    // +=
            MinusAssign,   // -=
            StarAssign,    // *=
            SlashAssign,   // /=
            PercentAssign, // %=
        };

        // 运算符可读名。
        [[nodiscard]]
        StringView to_string(Binary op) noexcept;

        [[nodiscard]]
        StringView to_string(Unary op) noexcept;

        [[nodiscard]]
        StringView to_string(Assignment op) noexcept;
    } // namespace Op

    // 共享辅助值类型（非 ASTNode 派生，move-only，提供 dump 供父节点统一渲染）。

    // 函数参数：plainParams / defaultParam / varargs 统一为 Param；文法保证 is_varargs 与 default_value
    // 互斥（varargs 无默认值）。
    struct Param {
        String         name;
        UPtr<ExprNode> default_value = nullptr;
        bool           is_varargs    = false;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 分支模式：expression 或 "_" 二选一（value 为 nullptr）；模式是相等比较，不绑定。
    struct MatchPattern {
        UPtr<ExprNode> value; // nullptr -> "_" 通配

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 语句分支：matchPattern "=>" statement。
    struct MatchArm {
        MatchPattern   pattern;
        UPtr<StmtNode> body;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 表达式分支：matchPattern "=>" expression。
    struct MatchExprArm {
        MatchPattern   pattern;
        UPtr<ExprNode> body; // 结果表达式

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // var 的单绑定：target + 可选初始化器。
    struct VarBinding {
        UPtr<PatternNode> target;
        UPtr<ExprNode>    initializer;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // map 字面量的键值对，键为任意 expression（运行时求值）。
    struct MapEntry {
        UPtr<ExprNode> key;
        UPtr<ExprNode> value;

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

        // 子项渲染按形态分派：UPtr 空安全（null 跳过）、值、UPtr 列表逐元素判空、值列表。
        template<Dumpable T>
        String dump_child(const usize indent, const UPtr<T>& node) {
            return node ? node->dump(indent) : "";
        }

        template<Dumpable T>
        String dump_child(const usize indent, const T& value) {
            return value.dump(indent);
        }

        template<Dumpable T>
        String dump_child(const usize indent, const List<UPtr<T>>& nodes) {
            String out;
            for (const auto& node: nodes) {
                if (node) {
                    out += node->dump(indent);
                }
            }
            return out;
        }

        template<Dumpable T>
        String dump_child(const usize indent, const List<T>& values) {
            String out;
            for (const auto& value: values) {
                out += value.dump(indent);
            }
            return out;
        }

        // 节点 dump 收口：写自身头行后逐子项渲染（子项缩进 = 节点缩进 + 1）。
        template<typename... Kids>
        String dump_node(const usize indent, const StringView header, const Kids&... kids) {
            String out;
            write_line(out, indent, header);
            ((out += dump_child(indent + 1, kids)), ...);
            return out;
        }

    } // namespace detail::ast

    // ProgramNode：program -> declaration*。整个编译单元的根，持顶层声明（StmtNode）列表。
    struct ProgramNode : ASTNode {
        ProgramNode(const SourceLoc loc, List<UPtr<StmtNode>> declarations) :
            ASTNode{loc}, declarations{std::move(declarations)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<StmtNode>> declarations;
    };

    // 语句节点（StmtNode）

    // BlockNode：语句块（block -> "{" declaration* "}"），持 declaration 列表（统一为 StmtNode）。
    struct BlockNode : StmtNode {
        BlockNode(const SourceLoc loc, List<UPtr<StmtNode>> statements) :
            StmtNode{loc}, statements{std::move(statements)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<StmtNode>> statements;
    };

    // 表达式语句：expression ";"。
    struct ExprStmtNode : StmtNode {
        ExprStmtNode(const SourceLoc loc, UPtr<ExprNode> expr) : StmtNode{loc}, expr{std::move(expr)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> expr;
    };

    // if 语句：if (cond) stmt (else stmt)?。
    struct IfStmtNode : StmtNode {
        IfStmtNode(const SourceLoc loc, UPtr<ExprNode> cond, UPtr<StmtNode> then_branch, UPtr<StmtNode> else_branch) :
            StmtNode{loc}, condition{std::move(cond)}, then_branch{std::move(then_branch)},
            else_branch{std::move(else_branch)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> condition;
        UPtr<StmtNode> then_branch;
        UPtr<StmtNode> else_branch;
    };

    // while 语句：while (cond) stmt。
    struct WhileStmtNode : StmtNode {
        WhileStmtNode(const SourceLoc loc, UPtr<ExprNode> cond, UPtr<StmtNode> body) :
            StmtNode{loc}, condition{std::move(cond)}, body{std::move(body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> condition;
        UPtr<StmtNode> body;
    };

    // for 语句（C 风格）：for (init; cond; incr) stmt，init / condition / increment 均可省。
    struct ForStmtNode : StmtNode {
        ForStmtNode(const SourceLoc loc, UPtr<StmtNode> init, UPtr<ExprNode> condition, UPtr<ExprNode> increment,
                    UPtr<StmtNode> body) :
            StmtNode{loc}, init{std::move(init)}, condition{std::move(condition)}, increment{std::move(increment)},
            body{std::move(body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<StmtNode> init;
        UPtr<ExprNode> condition;
        UPtr<ExprNode> increment;
        UPtr<StmtNode> body;
    };

    // for-in 语句：for (pattern in expr) stmt，pattern 绑 next() 的值；listPattern 按位置解构。
    struct ForInStmtNode : StmtNode {
        ForInStmtNode(const SourceLoc loc, UPtr<PatternNode> pattern, UPtr<ExprNode> iterable, UPtr<StmtNode> body) :
            StmtNode{loc}, pattern{std::move(pattern)}, iterable{std::move(iterable)}, body{std::move(body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<PatternNode> pattern;
        UPtr<ExprNode>    iterable;
        UPtr<StmtNode>    body;
    };

    // break 语句。
    struct BreakStmtNode : StmtNode {
        using StmtNode::StmtNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // continue 语句。
    struct ContinueStmtNode : StmtNode {
        using StmtNode::StmtNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // return 语句：return expression? ";"。value 缺省表无返回值。
    struct ReturnStmtNode : StmtNode {
        ReturnStmtNode(const SourceLoc loc, UPtr<ExprNode> value) : StmtNode{loc}, value{std::move(value)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> value;
    };

    // import 语句：import string as identifier ";"。path 为字面量解析后的内容。
    struct ImportStmtNode : StmtNode {
        ImportStmtNode(const SourceLoc loc, String path, String alias) :
            StmtNode{loc}, path{std::move(path)}, alias{std::move(alias)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String path;
        String alias;
    };

    // try 语句：try block (catch (id) block)?；catch_param/catch_body 成对缺省（parser 保证），
    // 语义阶段强制 catch 必有（TryWithoutHandler）。
    struct TryStmtNode : StmtNode {
        TryStmtNode(const SourceLoc loc, UPtr<BlockNode> body, Opt<String> catch_param, UPtr<BlockNode> catch_body) :
            StmtNode{loc}, body{std::move(body)}, catch_param{std::move(catch_param)},
            catch_body{std::move(catch_body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<BlockNode> body;
        Opt<String>     catch_param;
        UPtr<BlockNode> catch_body;
    };

    // throw 语句：throw expression ";"。
    struct ThrowStmtNode : StmtNode {
        ThrowStmtNode(const SourceLoc loc, UPtr<ExprNode> expr) : StmtNode{loc}, expr{std::move(expr)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> expr;
    };

    // match 语句：match (expr) { matchArm+ }。分支为 statement（MatchArm.body）。
    struct MatchStmtNode : StmtNode {
        MatchStmtNode(const SourceLoc loc, UPtr<ExprNode> subject, List<MatchArm> arms) :
            StmtNode{loc}, subject{std::move(subject)}, arms{std::move(arms)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> subject;
        List<MatchArm> arms;
    };

    // 声明节点（StmtNode 派生：声明即「可出现在 program/block 顶层的语句」）

    // 函数声明：fun identifier params block；kind 由 parser 烙定。
    struct FunDeclNode : StmtNode {
        FunDeclNode(const SourceLoc loc, String name, List<Param> params, UPtr<BlockNode> body, const FnKind kind) :
            StmtNode{loc}, name{std::move(name)}, params{std::move(params)}, body{std::move(body)}, kind{kind} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String          name;
        List<Param>     params;
        UPtr<BlockNode> body;
        FnKind          kind;
    };

    // def 声明："def" identifier (":" identifier)? "{" member* "}"（成员含嵌套 defDecl）。
    // members 按源序保留，静态变量初始化即此序（前一静态变量可被后续初始化器引用）。
    struct DefDeclNode : StmtNode {
        DefDeclNode(const SourceLoc loc, String name, Opt<String> superclass, List<UPtr<StmtNode>> members,
                    bool is_member) :
            StmtNode{loc}, name{std::move(name)}, superclass{std::move(superclass)}, members{std::move(members)},
            is_member{is_member} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String               name;
        Opt<String>          superclass;
        List<UPtr<StmtNode>> members;
        bool                 is_member; // parser 烙定:语句位声明(false)/类体成员位嵌套类(true)
    };

    // var 声明：var varTarget ("=" expr)? ("," ...)* ";"。
    struct VarDeclNode : StmtNode {
        VarDeclNode(const SourceLoc loc, List<VarBinding> bindings) : StmtNode{loc}, bindings{std::move(bindings)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<VarBinding> bindings;
    };

    // def 体静态变量成员：单 identifier 绑定的窄节点，多绑定/解构 pattern 由文法在成员位拒绝。
    struct StaticVarMemberNode : StmtNode {
        StaticVarMemberNode(const SourceLoc loc, String name, UPtr<ExprNode> initializer) :
            StmtNode{loc}, name{std::move(name)}, initializer{std::move(initializer)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String         name;
        UPtr<ExprNode> initializer;
    };

    // 表达式节点（ExprNode）

    // 字面量与基础表达式（primary）
    // 注：parenExpr -> "(" expression ")" 不设独立节点，AST 直接保留内层表达式。

    // 整数字面量。
    struct IntegerLiteralNode : ExprNode {
        IntegerLiteralNode(const SourceLoc loc, const i64 value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        i64 value;
    };

    // 浮点字面量（f64）。
    struct FloatLiteralNode : ExprNode {
        FloatLiteralNode(const SourceLoc loc, const f64 value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        f64 value;
    };

    // 字符串字面量。value 为 lexer 解析转义后的内容（非源码原文）。
    struct StringLiteralNode : ExprNode {
        StringLiteralNode(const SourceLoc loc, String value) : ExprNode{loc}, value{std::move(value)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String value;
    };

    // 插值串字面量。段 = 表达式节点序列（字面段为 StringLiteralNode，与 ListExprNode 的
    // 元素列表同构）；空字面段不入列，纯字面（含 \{ 转义形态）在词法层已退化为普通串。
    struct InterpolatedStringNode : ExprNode {
        InterpolatedStringNode(const SourceLoc loc, List<UPtr<ExprNode>> parts) :
            ExprNode{loc}, parts{std::move(parts)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<ExprNode>> parts;
    };

    // 布尔字面量：true / false。
    struct BoolLiteralNode : ExprNode {
        BoolLiteralNode(const SourceLoc loc, const bool value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        bool value;
    };

    // nil 字面量。
    struct NilLiteralNode : ExprNode {
        using ExprNode::ExprNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // 标识符引用。name 为源码 identifier 文本。
    struct IdentifierNode : ExprNode {
        IdentifierNode(const SourceLoc loc, String name) : ExprNode{loc}, name{std::move(name)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String name;
    };

    // this 表达式。
    struct ThisExprNode : ExprNode {
        using ExprNode::ExprNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // super 成员表达式：super "." identifier（文法单形，裸 super 解析期不收）；写形态非左值。
    struct SuperExprNode : ExprNode {
        SuperExprNode(const SourceLoc loc, String name) : ExprNode{loc}, name{std::move(name)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String name;
    };

    // 运算符表达式

    // 二元运算表达式：各文法层级统一为一个节点，左结合由 parser 构建左倾树。
    struct BinaryExprNode : ExprNode {
        BinaryExprNode(const SourceLoc loc, const Op::Binary op, UPtr<ExprNode> lhs, UPtr<ExprNode> rhs) :
            ExprNode{loc}, op{op}, lhs{std::move(lhs)}, rhs{std::move(rhs)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Binary     op;
        UPtr<ExprNode> lhs;
        UPtr<ExprNode> rhs;
    };

    // 一元（前缀）表达式：- / ! / ++ / -- 作用于后续 unary。
    struct UnaryExprNode : ExprNode {
        UnaryExprNode(const SourceLoc loc, const Op::Unary op, UPtr<ExprNode> operand) :
            ExprNode{loc}, op{op}, operand{std::move(operand)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Unary      op;
        UPtr<ExprNode> operand;
    };

    // 赋值（含复合赋值）：target op= value，target 合法性在语义阶段查。
    struct AssignmentNode : ExprNode {
        AssignmentNode(const SourceLoc loc, const Op::Assignment op, UPtr<ExprNode> target, UPtr<ExprNode> value) :
            ExprNode{loc}, op{op}, target{std::move(target)}, value{std::move(value)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Assignment op;
        UPtr<ExprNode> target;
        UPtr<ExprNode> value;
    };

    // 解构赋值：listPattern = expression。
    struct DestructureAssignmentNode : ExprNode {
        DestructureAssignmentNode(const SourceLoc loc, UPtr<PatternNode> target, UPtr<ExprNode> value) :
            ExprNode{loc}, target{std::move(target)}, value{std::move(value)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<PatternNode> target;
        UPtr<ExprNode>    value;
    };

    // 函数调用：callee(args)。
    struct CallNode : ExprNode {
        CallNode(const SourceLoc loc, UPtr<ExprNode> callee, List<UPtr<ExprNode>> args) :
            ExprNode{loc}, callee{std::move(callee)}, args{std::move(args)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode>       callee;
        List<UPtr<ExprNode>> args;
    };

    // 字段访问：object.name。
    struct FieldAccessNode : ExprNode {
        FieldAccessNode(const SourceLoc loc, UPtr<ExprNode> object, String name) :
            ExprNode{loc}, object{std::move(object)}, name{std::move(name)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> object;
        String         name;
    };

    // 下标访问：object[index]。
    struct IndexAccessNode : ExprNode {
        IndexAccessNode(const SourceLoc loc, UPtr<ExprNode> object, UPtr<ExprNode> index) :
            ExprNode{loc}, object{std::move(object)}, index{std::move(index)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> object;
        UPtr<ExprNode> index;
    };

    // 复合表达式（primary 的复合部分）

    // 列表字面量：[e, e, ...]。
    struct ListExprNode : ExprNode {
        ListExprNode(const SourceLoc loc, List<UPtr<ExprNode>> elements) :
            ExprNode{loc}, elements{std::move(elements)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<ExprNode>> elements;
    };

    // map 字面量：{ k: v, ... }。键为任意 expression（运行时求值）。
    struct MapExprNode : ExprNode {
        MapExprNode(const SourceLoc loc, List<MapEntry> entries) : ExprNode{loc}, entries{std::move(entries)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<MapEntry> entries;
    };

    // 区间表达式：a..b（含上界）/ a...b（不含上界）/ a.. 或 a...（无上界，upper 为空，两者同义）。
    struct RangeExprNode : ExprNode {
        RangeExprNode(const SourceLoc loc, const bool is_exclusive, UPtr<ExprNode> lower, UPtr<ExprNode> upper) :
            ExprNode{loc}, is_exclusive{is_exclusive}, lower{std::move(lower)}, upper{std::move(upper)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        bool           is_exclusive; // true: a...b（不含上界）；false: a..b（含上界）
        UPtr<ExprNode> lower;
        UPtr<ExprNode> upper;
    };

    // if 表达式：if (cond) { then } else { else }，分支为单表达式块。
    struct IfExprNode : ExprNode {
        IfExprNode(const SourceLoc loc, UPtr<ExprNode> cond, UPtr<ExprNode> then_branch, UPtr<ExprNode> else_branch) :
            ExprNode{loc}, condition{std::move(cond)}, then_branch{std::move(then_branch)},
            else_branch{std::move(else_branch)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> condition;
        UPtr<ExprNode> then_branch;
        UPtr<ExprNode> else_branch;
    };

    // lambda 表达式：fun params block。
    struct LambdaExprNode : ExprNode {
        LambdaExprNode(const SourceLoc loc, List<Param> params, UPtr<BlockNode> body) :
            ExprNode{loc}, params{std::move(params)}, body{std::move(body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<Param>     params;
        UPtr<BlockNode> body;
    };

    // match 表达式：match (subject) { pat => expr ... }。
    struct MatchExprNode : ExprNode {
        MatchExprNode(const SourceLoc loc, UPtr<ExprNode> subject, List<MatchExprArm> arms) :
            ExprNode{loc}, subject{std::move(subject)}, arms{std::move(arms)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode>     subject;
        List<MatchExprArm> arms;
    };

    // 序列表达式：e1, e2, ...，逐个求值、值为最后一个、不短路。
    // 单元素由 parser 透明化不产本节点（"(a)" 保持纯分组，"(a) = v" 左值行为不回归）。
    struct SequenceExprNode : ExprNode {
        SequenceExprNode(const SourceLoc loc, List<UPtr<ExprNode>> expressions) :
            ExprNode{loc}, expressions{std::move(expressions)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<ExprNode>> expressions;
    };

    // 解构模式节点（PatternNode）：用于 var 声明的 varTarget、for-in 目标与解构赋值左侧目标。
    // listPattern 映射为下标访问（多余忽略、不足越界报错）；rest 仅 listPattern。

    // 标识符模式：绑定该名字。
    struct IdentifierPatternNode : PatternNode {
        IdentifierPatternNode(const SourceLoc loc, String name) : PatternNode{loc}, name{std::move(name)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String name;
    };

    // 通配符模式 "_"：匹配/忽略该位置不绑定（用于跳过不关心的元素）。
    struct WildcardPatternNode : PatternNode {
        using PatternNode::PatternNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // 列表模式：[p, p, ..., ...rest?]。rest 收集剩余为新 list（只接受绑名，文法禁 "..._"），
    // nullptr 表无 rest 忽略剩余。
    struct ListPatternNode : PatternNode {
        ListPatternNode(const SourceLoc loc, List<UPtr<PatternNode>> elements, UPtr<IdentifierPatternNode> rest) :
            PatternNode{loc}, elements{std::move(elements)}, rest{std::move(rest)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<PatternNode>>     elements;
        UPtr<IdentifierPatternNode> rest;
    };

} // namespace aria

#endif // ARIA_AST_HPP
