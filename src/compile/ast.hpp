#ifndef ARIA_AST_HPP
#define ARIA_AST_HPP

#include "common.hpp"
#include "compile/FnKind.hpp"
#include "compile/Token.hpp"
#include "util/source_file.hpp"

namespace aria {

    using src::SourceLoc;

    // 前置声明 AstVisitor：节点经 accept 参与双分派，这里只需声明不需完整类型。
    class AstVisitor;

    // 窄指针成员引用的后段节点：指针仅借地址，前置声明即可，定义仍按段序展开。
    struct StringLiteralNode;
    struct ListPatternNode;

    // AST 根基类：所有节点持 SourceLoc（parser 取构造首 token，供语义/代码生成报错定位；空态 = 合成节点）。
    // 节点全部由 AstArena 分配、裸指针互指（arena 拥有内存，指针仅借用），存活期由所属 arena 覆盖
    // （整个编译期）；析构函数仅为多态保留，节点从不单独析构。
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
    // 节点段序即依赖序，文件头的前置声明仅供窄指针成员引用后段节点。
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

    // 共享辅助值类型（非 ASTNode 派生，提供 dump 供父节点统一渲染；子节点指针借用 arena 内存）。

    // 函数参数：plainParams / defaultParam / varargs 统一为 Param；文法保证 is_varargs 与 default_value
    // 互斥（varargs 无默认值）。
    struct Param {
        StringView name;
        ExprNode*  default_value = nullptr;
        bool       is_varargs    = false;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 分支模式：expression 或 "_" 二选一（value 为 nullptr）；模式是相等比较，不绑定。
    struct MatchPattern {
        ExprNode* value = nullptr; // nullptr -> "_" 通配

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 语句分支：matchPattern "=>" statement。
    struct MatchArm {
        MatchPattern pattern;
        StmtNode*    body = nullptr;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 表达式分支：matchPattern "=>" expression。
    struct MatchExprArm {
        MatchPattern pattern;
        ExprNode*    body = nullptr; // 结果表达式

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // var 的单绑定：target + 可选初始化器。
    struct VarBinding {
        PatternNode* target      = nullptr;
        ExprNode*    initializer = nullptr;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // map 字面量的键值对，键为任意 expression（运行时求值）。
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

        // 子项渲染按形态分派：裸指针空安全（null 跳过）、值、Span 逐元素分派。
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
        ProgramNode(const SourceLoc loc, const Span<StmtNode*> declarations) :
            ASTNode{loc}, declarations{declarations} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<StmtNode*> declarations;
    };

    // 语句节点（StmtNode）

    // BlockNode：语句块（block -> "{" declaration* "}"），持 declaration 列表（统一为 StmtNode）。
    struct BlockNode : StmtNode {
        BlockNode(const SourceLoc loc, const Span<StmtNode*> statements) : StmtNode{loc}, statements{statements} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<StmtNode*> statements;
    };

    // 表达式语句：expression ";"。
    struct ExprStmtNode : StmtNode {
        ExprStmtNode(const SourceLoc loc, ExprNode* expr) : StmtNode{loc}, expr{expr} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* expr;
    };

    // if 语句：if (cond) stmt (else stmt)?。
    struct IfStmtNode : StmtNode {
        IfStmtNode(const SourceLoc loc, ExprNode* cond, StmtNode* then_branch, StmtNode* else_branch) :
            StmtNode{loc}, condition{cond}, then_branch{then_branch}, else_branch{else_branch} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* condition;
        StmtNode* then_branch;
        StmtNode* else_branch;
    };

    // while 语句：while (cond) stmt。
    struct WhileStmtNode : StmtNode {
        WhileStmtNode(const SourceLoc loc, ExprNode* cond, StmtNode* body) :
            StmtNode{loc}, condition{cond}, body{body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* condition;
        StmtNode* body;
    };

    // for 语句（C 风格）：for (init; cond; incr) stmt，init / condition / increment 均可省。
    struct ForStmtNode : StmtNode {
        ForStmtNode(const SourceLoc loc, StmtNode* init, ExprNode* condition, ExprNode* increment, StmtNode* body) :
            StmtNode{loc}, init{init}, condition{condition}, increment{increment}, body{body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StmtNode* init;
        ExprNode* condition;
        ExprNode* increment;
        StmtNode* body;
    };

    // for-in 语句：for (pattern in expr) stmt，pattern 绑 next() 的值；listPattern 按位置解构。
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
        ReturnStmtNode(const SourceLoc loc, ExprNode* value) : StmtNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* value;
    };

    // import 语句：import string as identifier ";"。path 文法钉死字符串字面量，解码在代码生成端；
    // alias 为绑定的本地名。
    struct ImportStmtNode : StmtNode {
        ImportStmtNode(const SourceLoc loc, StringLiteralNode* path, const StringView alias) :
            StmtNode{loc}, path{path}, alias{alias} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringLiteralNode* path;
        StringView         alias;
    };

    // try 语句：try block (catch (id) block)?；ename/catch_body 成对缺省（parser 保证；
    // ename 空视图即无 catch，identifier 恒非空），语义阶段强制 catch 必有（TryWithoutHandler）。
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

    // throw 语句：throw expression ";"。
    struct ThrowStmtNode : StmtNode {
        ThrowStmtNode(const SourceLoc loc, ExprNode* expr) : StmtNode{loc}, expr{expr} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* expr;
    };

    // match 语句：match (expr) { matchArm+ }。分支为 statement（MatchArm.body）。
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

    // 函数声明：fun identifier params block；kind 由 parser 烙定。
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

    // def 声明："def" identifier (":" expression)? "{" member* "}"（成员含嵌套 defDecl）。
    // members 按源序保留，静态变量初始化即此序（前一静态变量可被后续初始化器引用）。
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

    // var 声明：var varTarget ("=" expr)? ("," ...)* ";"。
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

    // 字符串字面量。value 为字面内层原文视图（转义未展开，借自源缓冲，存活至编译结束）；shape
    // 为字面段消费形态（词法期记账），解码在代码生成端进行。
    struct StringLiteralNode : ExprNode {
        StringLiteralNode(const SourceLoc loc, const StringView value, const StringShape shape) :
            ExprNode{loc}, value{value}, shape{shape} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView  value;
        StringShape shape;
    };

    // 插值串字面量。段 = 表达式节点序列（字面段为 StringLiteralNode，与 ListExprNode 的
    // 元素列表同构）；空字面段不入列，纯字面（含 \$ 转义形态）在词法层已退化为普通串。
    struct InterpolatedStringNode : ExprNode {
        InterpolatedStringNode(const SourceLoc loc, const Span<ExprNode*> parts) : ExprNode{loc}, parts{parts} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<ExprNode*> parts;
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
        IdentifierNode(const SourceLoc loc, const StringView name) : ExprNode{loc}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView name;
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
        SuperExprNode(const SourceLoc loc, const StringView name) : ExprNode{loc}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView name;
    };

    // 运算符表达式

    // 二元运算表达式：各文法层级统一为一个节点，左结合由 parser 构建左倾树。
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

    // 一元（前缀）表达式：- / ! / ++ / -- 作用于后续 unary。
    struct UnaryExprNode : ExprNode {
        UnaryExprNode(const SourceLoc loc, const Op::Unary op, ExprNode* operand) :
            ExprNode{loc}, op{op}, operand{operand} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Unary op;
        ExprNode* operand;
    };

    // 赋值（含复合赋值）：target op= value，target 合法性在语义阶段查。
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

    // 解构赋值：listPattern = expression。
    struct DestructureAssignmentNode : ExprNode {
        DestructureAssignmentNode(const SourceLoc loc, ListPatternNode* target, ExprNode* value) :
            ExprNode{loc}, target{target}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ListPatternNode* target;
        ExprNode*        value;
    };

    // 函数调用：callee(args)。
    struct CallNode : ExprNode {
        CallNode(const SourceLoc loc, ExprNode* callee, const Span<ExprNode*> args) :
            ExprNode{loc}, callee{callee}, args{args} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode*       callee;
        Span<ExprNode*> args;
    };

    // 字段访问：object.name。
    struct FieldAccessNode : ExprNode {
        FieldAccessNode(const SourceLoc loc, ExprNode* object, const StringView name) :
            ExprNode{loc}, object{object}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode*  object;
        StringView name;
    };

    // 下标访问：object[index]。
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

    // 列表字面量：[e, e, ...]。
    struct ListExprNode : ExprNode {
        ListExprNode(const SourceLoc loc, const Span<ExprNode*> elements) : ExprNode{loc}, elements{elements} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<ExprNode*> elements;
    };

    // map 字面量：{ k: v, ... }。键为任意 expression（运行时求值）。
    struct MapExprNode : ExprNode {
        MapExprNode(const SourceLoc loc, const Span<MapEntry> entries) : ExprNode{loc}, entries{entries} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<MapEntry> entries;
    };

    // 区间表达式：a..b（含上界）/ a...b（不含上界）/ a.. 或 a...（无上界，upper 为空，两者同义）。
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

    // if 表达式：if (cond) { then } else { else }，分支为单表达式块。
    struct IfExprNode : ExprNode {
        IfExprNode(const SourceLoc loc, ExprNode* cond, ExprNode* then_branch, ExprNode* else_branch) :
            ExprNode{loc}, condition{cond}, then_branch{then_branch}, else_branch{else_branch} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode* condition;
        ExprNode* then_branch;
        ExprNode* else_branch;
    };

    // lambda 表达式：fun params block。
    struct LambdaExprNode : ExprNode {
        LambdaExprNode(const SourceLoc loc, const Span<Param> params, BlockNode* body) :
            ExprNode{loc}, params{params}, body{body} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<Param> params;
        BlockNode*  body;
    };

    // match 表达式：match (subject) { pat => expr ... }。
    struct MatchExprNode : ExprNode {
        MatchExprNode(const SourceLoc loc, ExprNode* subject, const Span<MatchExprArm> arms) :
            ExprNode{loc}, subject{subject}, arms{arms} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        ExprNode*          subject;
        Span<MatchExprArm> arms;
    };

    // 序列表达式：e1, e2, ...，逐个求值、值为最后一个、不短路。
    // 单元素由 parser 透明化不产本节点（"(a)" 保持纯分组，"(a) = v" 左值行为不回归）。
    struct SequenceExprNode : ExprNode {
        SequenceExprNode(const SourceLoc loc, const Span<ExprNode*> expressions) :
            ExprNode{loc}, expressions{expressions} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<ExprNode*> expressions;
    };

    // 解构模式节点（PatternNode）：用于 var 声明的 varTarget、for-in 目标与解构赋值左侧目标。
    // listPattern 映射为下标访问（多余忽略、不足越界报错）；rest 仅 listPattern。

    // 标识符模式：绑定该名字。
    struct IdentifierPatternNode : PatternNode {
        IdentifierPatternNode(const SourceLoc loc, const StringView name) : PatternNode{loc}, name{name} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        StringView name;
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
        ListPatternNode(const SourceLoc loc, const Span<PatternNode*> elements, IdentifierPatternNode* rest) :
            PatternNode{loc}, elements{elements}, rest{rest} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Span<PatternNode*>     elements;
        IdentifierPatternNode* rest;
    };

} // namespace aria

#endif // ARIA_AST_HPP
