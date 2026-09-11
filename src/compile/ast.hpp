#ifndef ARIA_AST_HPP
#define ARIA_AST_HPP

#include "common.hpp"
#include "util/source_file.hpp"

namespace aria {

    // 将 SourceLoc 引入 aria 命名空间（source_file 相关类型位于 aria::src 下，
    // 引用需分别 using，见 CLAUDE.md）。
    using src::SourceLoc;

    // 前置声明 AstVisitor：AST 节点经 accept(AstVisitor&) 参与访问者模式（双分派）。
    // 完整定义见 compile/AstVisitor.hpp；此处仅需声明，accept 的实现（ast.cpp）才
    // include 该头以调用 visitXxxNode。参数/返回值只用指针与引用，无需完整类型。
    class AstVisitor;

    // =========================================================================
    // AST 根基类
    // =========================================================================
    //
    // 所有语法树节点的公共基类。设计要点：
    //   - 持有 SourceLoc（源码位置）：parser 从构造的首个 token 拷入，供语义分析 /
    //     代码生成阶段报错定位。默认构造为空态（src=nullptr），合成节点可保持空态。
    //   - dump(indent) 把本节点（含子树）渲染为带缩进的树形文本并返回（存储到字符串）；
    //     indent 为当前缩进层数（每层 2 空格）。所有派生节点必须实现（纯虚）。
    //     标 [[nodiscard]]：返回的字符串不应被丢弃--丢弃通常意味着忘了拼接子树文本。
    //   - display() 直接打印本节点（含子树）到终端：经 dump 构造文本后 io::print 输出。
    //   - 多态通过指针使用（UPtr<ExprNode>/UPtr<StmtNode> 等）；ASTNode 因
    //     dump 为纯虚而不可直接实例化。
    //
    // 命名约定：所有 ASTNode 派生类（含抽象分类基类）均以 Node 后缀命名，ASTNode 自身
    // 已含 Node 故不变。辅助值类型（Param/MatchPattern/MatchArm/MatchExprArm/VarBinding/
    // MapEntry）非 ASTNode 派生，不加后缀。
    //
    // 生命周期：节点持有的 UPtr 子节点随节点一起销毁；SourceLoc::src 为非拥有指针，
    // 不得比所引用的 SourceFile 活得更久（同 Token::lexeme_ 约束）。
    struct ASTNode {
        ASTNode() noexcept = default;
        explicit ASTNode(SourceLoc loc) noexcept : loc_{loc} {}
        virtual ~ASTNode() = default;

        // 节点不可拷贝、不可移动：节点总是经 UPtr 在堆上分配，移动的只是指针
        ASTNode(const ASTNode&)                = delete;
        ASTNode& operator=(const ASTNode&)     = delete;
        ASTNode(ASTNode&&) noexcept            = delete;
        ASTNode& operator=(ASTNode&&) noexcept = delete;

        // 源码位置（空态表合成节点）。
        [[nodiscard]]
        SourceLoc loc() const noexcept {
            return loc_;
        }

        void set_loc(SourceLoc loc) noexcept { loc_ = loc; }

        // 行号（1-based；空态 / 无效为 0，不做兜底，直接取 loc 原值）。
        [[nodiscard]]
        u32 loc_line() const noexcept {
            return static_cast<u32>(loc_.line());
        }

        [[nodiscard]]
        virtual String dump(usize indent) const = 0;

        void display() const;

        // 访问者模式入口（双分派，机制见 compile/AstVisitor.hpp 头注）。
        virtual void accept(AstVisitor& visitor) = 0;

    protected:
        SourceLoc loc_;
    };

    // =========================================================================
    // 分类基类
    // =========================================================================
    //
    // StmtNode：语句基类。文法 declaration 产生式为 funDecl|defDecl|varDecl|statement--
    //   声明即「可出现在 program/block 顶层的语句」，故 FunDeclNode/DefDeclNode/
    //   VarDeclNode 亦为 StmtNode 的派生。ProgramNode 与 BlockNode 持 List<UPtr<StmtNode>>。
    // ExprNode：表达式基类。
    // PatternNode：解构模式基类（var 声明的 varTarget 与解构赋值右侧候选）。
    //
    // 三者仅作分类标记、无额外数据，继承 ASTNode 的构造函数。
    //
    // 具体节点段定义顺序：ProgramNode -> StmtNode -> ExprNode -> PatternNode。依赖关系自洽，
    // 无需任何前置声明：
    //   - ProgramNode 持 List<UPtr<StmtNode>>（基，已完整）。
    //   - StmtNode 段依赖 ExprNode 基（条件/返回等持 UPtr<ExprNode>）；段内 BlockNode
    //     置首--TryStmtNode/FunDeclNode 持 UPtr<BlockNode>。
    //   - ExprNode 段依赖 PatternNode 基（DestructureAssignmentNode）与 StmtNode 段
    //     （LambdaExprNode 持 UPtr<BlockNode>--StmtNode 段在前，BlockNode 已完整，
    //     构造函数可 inline，无需移至 .cpp）。
    //   - PatternNode 段仅依赖 PatternNode 基与辅助类型。
    struct StmtNode : ASTNode {
        using ASTNode::ASTNode;
    };

    struct ExprNode : ASTNode {
        using ASTNode::ASTNode;
    };

    struct PatternNode : ASTNode {
        using ASTNode::ASTNode;
    };

    // =========================================================================
    // 运算符枚举（与 TokenType 解耦，AST 自持语义标识）
    // =========================================================================
    //
    // 不直接复用 TokenType：AST 是词法之上的语义结构，运算符语义独立于词法拼写
    // （如 "and"/"&&" 同为 Op::Binary::And）。parser 负责 TokenType->运算符枚举的映射。

    namespace Op {
        // 二元运算符：覆盖 logic_or/logic_and/equality/comparison/term/factor 各层。
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

        // 一元（前缀）运算符：unary 产生式中的 - / ! / ++ / --。
        enum class Unary : u8 {
            Minus,  // -
            Not,    // !
            PreInc, // ++（前置）
            PreDec, // --（前置）
        };

        // 赋值运算符：= 与复合赋值（+= 等）。
        enum class Assignment : u8 {
            Assign,        // =
            PlusAssign,    // +=
            MinusAssign,   // -=
            StarAssign,    // *=
            SlashAssign,   // /=
            PercentAssign, // %=
        };

        // 运算符可读名（dump / 错误信息用）。重载区分 Binary / Unary / Assignment。
        [[nodiscard]]
        StringView to_string(Binary op) noexcept;

        [[nodiscard]]
        StringView to_string(Unary op) noexcept;

        [[nodiscard]]
        StringView to_string(Assignment op) noexcept;
    } // namespace Op

    // =========================================================================
    // 共享辅助类型（非 ASTNode：作为节点字段的值类型，持 UPtr 子节点）
    // =========================================================================
    //
    // 这些结构是 AST 节点字段的值类型（如 FunDeclNode 的参数列表、MapExprNode 的键值对），
    // 不是 ASTNode 派生（不参与多态，不加 Node 后缀），但同样提供 dump(usize) 以便父节点
    // 统一渲染。move-only（持 UPtr）。仅引用 StmtNode/ExprNode/PatternNode 基类（已完整）。

    // 函数参数：plainParams / defaultParam / varargs 统一为 Param。
    //   - default_value 有值 -> 默认参数（defaultParam）。
    //   - is_varargs=true -> varargs（"..." 前缀）。
    //   - 文法保证 is_varargs 与 default_value.has_value() 互斥（varargs 无默认值）。
    struct Param {
        String         name;
        UPtr<ExprNode> default_value = nullptr;
        bool           is_varargs    = false;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // matchPattern（match 分支模式）：expression 或 "_" 二选一。
    //   - value 为 nullptr -> "_" 通配兜底。
    //   - 否则 value 为任意 ExprNode（运行时求值后与 subject 比较相等，非绑定）。
    struct MatchPattern {
        UPtr<ExprNode> value; // nullptr -> "_" 通配

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // match 语句分支：matchPattern "=>" statement。
    struct MatchArm {
        MatchPattern   pattern;
        UPtr<StmtNode> body; // 单条语句（printStmt / exprStmt / block 等；多语句用 block）

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

    // 对应文法 varBinding 产生式：varTarget ("=" expression)?。
    //   - target 为 IdentifierPatternNode 或解构 pattern。
    //   - initializer 非空时表带初始化（nullptr 表无初始化）。
    struct VarBinding {
        UPtr<PatternNode> target;
        UPtr<ExprNode>    initializer;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // mapExpr 的键值对（键为任意 expression，运行时求值）。
    struct MapEntry {
        UPtr<ExprNode> key;
        UPtr<ExprNode> value;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // def 声明体的成员：静态变量 / 静态方法 / 实例方法（见 DefDeclNode）。
    //   - StaticVar：var 声明（varDecl），编译为类上的静态变量。
    //   - StaticMethod：fun 声明（funDecl，带 fun 关键字），无 this 绑定，靠裸名访问静态成员。
    //   - InstanceMethod：方法（identifier params block，无 fun 关键字），有 this 绑定。
    // node 统一为 StmtNode：StaticVar 持 VarDeclNode；StaticMethod/InstanceMethod 持 FunDeclNode。
    // 按出现顺序保留，以支撑静态变量初始化顺序（前一静态变量可被后续初始化器引用）。
    struct DefMember {
        enum class Kind : u8 { StaticVar, StaticMethod, InstanceMethod };

        Kind           kind;
        UPtr<StmtNode> node;

        [[nodiscard]]
        String dump(usize indent) const;
    };

    // =========================================================================
    // dump 基础设施
    // =========================================================================
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

    } // namespace detail::ast

    // =========================================================================
    // ProgramNode（AST 根）
    // =========================================================================

    // ProgramNode：program -> declaration*。整个编译单元的根，持顶层声明（StmtNode）列表。
    struct ProgramNode : ASTNode {
        ProgramNode(SourceLoc loc, List<UPtr<StmtNode>> declarations) :
            ASTNode{loc}, declarations{std::move(declarations)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<StmtNode>> declarations;
    };

    // =========================================================================
    // 语句节点（StmtNode）
    // =========================================================================

    // BlockNode：语句块（block -> "{" declaration* "}"），持 declaration 列表（统一为 StmtNode）。
    struct BlockNode : StmtNode {
        BlockNode(SourceLoc loc, List<UPtr<StmtNode>> statements) : StmtNode{loc}, statements{std::move(statements)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<StmtNode>> statements;
    };

    // 表达式语句：expression ";"。
    struct ExprStmtNode : StmtNode {
        ExprStmtNode(SourceLoc loc, UPtr<ExprNode> expr) : StmtNode{loc}, expr{std::move(expr)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> expr;
    };

    // print 语句：print expression ";"。
    struct PrintStmtNode : StmtNode {
        PrintStmtNode(SourceLoc loc, UPtr<ExprNode> expr) : StmtNode{loc}, expr{std::move(expr)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> expr;
    };

    // if 语句：if (cond) stmt (else stmt)?。else_branch 缺省表无 else。
    struct IfStmtNode : StmtNode {
        IfStmtNode(SourceLoc loc, UPtr<ExprNode> cond, UPtr<StmtNode> then_branch, UPtr<StmtNode> else_branch) :
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
        WhileStmtNode(SourceLoc loc, UPtr<ExprNode> cond, UPtr<StmtNode> body) :
            StmtNode{loc}, condition{std::move(cond)}, body{std::move(body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> condition;
        UPtr<StmtNode> body;
    };

    // for 语句（C 风格）：for (init; cond; incr) stmt。
    //   - init：varDecl / exprStmt / 空（";"）。统一为 StmtNode（nullptr 表空 init）。
    //   - condition / increment：nullptr 表省略。
    struct ForStmtNode : StmtNode {
        ForStmtNode(SourceLoc loc, UPtr<StmtNode> init, UPtr<ExprNode> condition, UPtr<ExprNode> increment,
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

    // for-in 语句：for (pattern in expr) stmt。pattern 为循环目标（identifier/"_"/listPattern），
    // 绑 next() 的值；listPattern 按位置解构（[k,v] 绑 next()[0]/[1]）。
    struct ForInStmtNode : StmtNode {
        ForInStmtNode(SourceLoc loc, UPtr<PatternNode> pattern, UPtr<ExprNode> iterable, UPtr<StmtNode> body) :
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
        ReturnStmtNode(SourceLoc loc, UPtr<ExprNode> value) : StmtNode{loc}, value{std::move(value)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> value;
    };

    // import 语句：import string as identifier ";"。
    //   - path：字符串字面量解析后的内容（模块路径）。
    //   - alias：绑定模块的本地名。
    struct ImportStmtNode : StmtNode {
        ImportStmtNode(SourceLoc loc, String path, String alias) :
            StmtNode{loc}, path{std::move(path)}, alias{std::move(alias)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String path;
        String alias;
    };

    // try 语句：try block (catch (id) block)?。
    //   - catch_param / catch_body 成对出现（parser 保证），均缺省表无 catch。
    //   - 语义阶段保证 catch 必有（TryWithoutHandler）。
    struct TryStmtNode : StmtNode {
        TryStmtNode(SourceLoc loc, UPtr<BlockNode> body, Opt<String> catch_param, UPtr<BlockNode> catch_body) :
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
        ThrowStmtNode(SourceLoc loc, UPtr<ExprNode> expr) : StmtNode{loc}, expr{std::move(expr)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> expr;
    };

    // match 语句：match (expr) { matchArm+ }。分支为 statement（MatchArm.body）。
    struct MatchStmtNode : StmtNode {
        MatchStmtNode(SourceLoc loc, UPtr<ExprNode> subject, List<MatchArm> arms) :
            StmtNode{loc}, subject{std::move(subject)}, arms{std::move(arms)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> subject;
        List<MatchArm> arms;
    };

    // --- 声明节点（StmtNode 派生：声明即「可出现在 program/block 顶层的语句」） ---

    // 函数声明：fun identifier params block。
    struct FunDeclNode : StmtNode {
        FunDeclNode(SourceLoc loc, String name, List<Param> params, UPtr<BlockNode> body) :
            StmtNode{loc}, name{std::move(name)}, params{std::move(params)}, body{std::move(body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String          name;
        List<Param>     params;
        UPtr<BlockNode> body;
    };

    // def 声明："def" identifier (":" identifier)? "{" (varDecl | funDecl | function)* "}"。
    //   - superclass：nullopt 表无父类（无 ":"）；实例方法经此继承，super 仍可用。
    //   - members：体内成员列表（DefMember，按出现顺序保留）。三种成员：
    //     StaticVar（var 声明）/ StaticMethod（fun 声明）/ InstanceMethod（裸 identifier 方法）。
    struct DefDeclNode : StmtNode {
        DefDeclNode(SourceLoc loc, String name, Opt<String> superclass, List<DefMember> members) :
            StmtNode{loc}, name{std::move(name)}, superclass{std::move(superclass)}, members{std::move(members)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String          name;
        Opt<String>     superclass;
        List<DefMember> members;
    };

    // var 声明：var varTarget ("=" expr)? ("," ...)* ";"。
    //   - bindings：每个 VarBinding 含一个 target（identifier 或 pattern）与可选初始化。
    struct VarDeclNode : StmtNode {
        VarDeclNode(SourceLoc loc, List<VarBinding> bindings) : StmtNode{loc}, bindings{std::move(bindings)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<VarBinding> bindings;
    };

    // =========================================================================
    // 表达式节点（ExprNode）
    // =========================================================================

    // ----- 字面量与基础表达式（primary） -----
    //
    // 注：parenExpr -> "(" expression ")" 不设独立节点--括号仅用于结合优先级，
    //     AST 直接保留内层表达式（语义无差，且 dump 不受影响）。

    // 整数字面量。文法 int 为 i48，此处用 i64 容纳，越界留语义阶段处理。
    struct IntegerLiteralNode : ExprNode {
        IntegerLiteralNode(SourceLoc loc, i64 value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        i64 value;
    };

    // 浮点字面量（f64）。
    struct FloatLiteralNode : ExprNode {
        FloatLiteralNode(SourceLoc loc, f64 value) noexcept : ExprNode{loc}, value{value} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        f64 value;
    };

    // 字符串字面量。value 为 lexer 解析转义后的内容（非源码原文）。
    struct StringLiteralNode : ExprNode {
        StringLiteralNode(SourceLoc loc, String value) : ExprNode{loc}, value{std::move(value)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        String value;
    };

    // 布尔字面量：true / false。
    struct BoolLiteralNode : ExprNode {
        BoolLiteralNode(SourceLoc loc, bool value) noexcept : ExprNode{loc}, value{value} {}

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
        IdentifierNode(SourceLoc loc, String name) : ExprNode{loc}, name{std::move(name)} {}

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

    // super 表达式。文法要求 super 须为 super.method(...) 形态（语义阶段校验），
    // AST 层仅作 callee 标记--作为 FieldAccessNode(SuperExprNode, method) 的 object 出现。
    struct SuperExprNode : ExprNode {
        using ExprNode::ExprNode;

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;
    };

    // ----- 运算符表达式 -----
    //
    // 文法层级见 grammar.txt：前置一元 + 后缀调用/取字段/取下标链；赋值（含复合与解构）
    // 右结合，作 expression 顶层。

    // 二元运算表达式：logic_or / logic_and / equality / comparison / term / factor
    // 各层统一为一个节点，运算种类由 op 区分（左结合，parser 已构建左倾树）。
    struct BinaryExprNode : ExprNode {
        BinaryExprNode(SourceLoc loc, Op::Binary op, UPtr<ExprNode> lhs, UPtr<ExprNode> rhs) :
            ExprNode{loc}, op{op}, lhs{std::move(lhs)}, rhs{std::move(rhs)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Binary     op;
        UPtr<ExprNode> lhs;
        UPtr<ExprNode> rhs;
    };

    // 一元（前缀）表达式：- / ! / ++ / -- 作用于后续 unary，左值合法性留语义阶段。
    struct UnaryExprNode : ExprNode {
        UnaryExprNode(SourceLoc loc, Op::Unary op, UPtr<ExprNode> operand) :
            ExprNode{loc}, op{op}, operand{std::move(operand)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        Op::Unary      op;
        UPtr<ExprNode> operand;
    };

    // 赋值（含复合赋值）：target op= value。
    // target 为左值表达式（identifier / obj.field / obj[index]），合法性留语义阶段。
    struct AssignmentNode : ExprNode {
        AssignmentNode(SourceLoc loc, Op::Assignment op, UPtr<ExprNode> target, UPtr<ExprNode> value) :
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
        DestructureAssignmentNode(SourceLoc loc, UPtr<PatternNode> target, UPtr<ExprNode> value) :
            ExprNode{loc}, target{std::move(target)}, value{std::move(value)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<PatternNode> target;
        UPtr<ExprNode>    value;
    };

    // 函数调用：callee(args)。args 为实参列表（空表表无参）。
    struct CallNode : ExprNode {
        CallNode(SourceLoc loc, UPtr<ExprNode> callee, List<UPtr<ExprNode>> args) :
            ExprNode{loc}, callee{std::move(callee)}, args{std::move(args)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode>       callee;
        List<UPtr<ExprNode>> args;
    };

    // 字段访问：object.name。
    struct FieldAccessNode : ExprNode {
        FieldAccessNode(SourceLoc loc, UPtr<ExprNode> object, String name) :
            ExprNode{loc}, object{std::move(object)}, name{std::move(name)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> object;
        String         name;
    };

    // 下标访问：object[index]。
    struct IndexAccessNode : ExprNode {
        IndexAccessNode(SourceLoc loc, UPtr<ExprNode> object, UPtr<ExprNode> index) :
            ExprNode{loc}, object{std::move(object)}, index{std::move(index)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode> object;
        UPtr<ExprNode> index;
    };

    // ----- 复合表达式（primary 的复合部分） -----

    // 列表字面量：[e, e, ...]。
    struct ListExprNode : ExprNode {
        ListExprNode(SourceLoc loc, List<UPtr<ExprNode>> elements) : ExprNode{loc}, elements{std::move(elements)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<ExprNode>> elements;
    };

    // map 字面量：{ k: v, ... }。键为任意 expression（运行时求值）。
    struct MapExprNode : ExprNode {
        MapExprNode(SourceLoc loc, List<MapEntry> entries) : ExprNode{loc}, entries{std::move(entries)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<MapEntry> entries;
    };

    // 区间表达式：a..b（含上界）/ a...b（不含上界）。产生 range 对象，VM 未实现。
    struct RangeExprNode : ExprNode {
        RangeExprNode(SourceLoc loc, bool is_exclusive, UPtr<ExprNode> lower, UPtr<ExprNode> upper) :
            ExprNode{loc}, is_exclusive{is_exclusive}, lower{std::move(lower)}, upper{std::move(upper)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        bool           is_exclusive; // true: a...b（不含上界）；false: a..b（含上界）
        UPtr<ExprNode> lower;
        UPtr<ExprNode> upper;
    };

    // if 表达式：if (cond) { then } else { else }。
    // 文法 ifExpr 分支为单表达式块（{ expression }），故 then/else 直接为 ExprNode。
    struct IfExprNode : ExprNode {
        IfExprNode(SourceLoc loc, UPtr<ExprNode> cond, UPtr<ExprNode> then_branch, UPtr<ExprNode> else_branch) :
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
        LambdaExprNode(SourceLoc loc, List<Param> params, UPtr<BlockNode> body) :
            ExprNode{loc}, params{std::move(params)}, body{std::move(body)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<Param>     params;
        UPtr<BlockNode> body;
    };

    // match 表达式：match (subject) { pat => expr ... }。
    struct MatchExprNode : ExprNode {
        MatchExprNode(SourceLoc loc, UPtr<ExprNode> subject, List<MatchExprArm> arms) :
            ExprNode{loc}, subject{std::move(subject)}, arms{std::move(arms)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        UPtr<ExprNode>     subject;
        List<MatchExprArm> arms;
    };

    // =========================================================================
    // 解构模式节点（PatternNode）
    // =========================================================================
    //
    // 对应文法 pattern 产生式。仅出现在 var 声明的 varTarget 与解构赋值（"=" 右侧候选）。
    //   - listPattern 映射为下标访问（位置 i 绑 list[i]），多余忽略、不足越界报错。
    //   - rest 仅 listPattern 支持（"..." 前缀，收集剩余为新 list）。

    // 标识符模式：绑定该名字。
    struct IdentifierPatternNode : PatternNode {
        IdentifierPatternNode(SourceLoc loc, String name) : PatternNode{loc}, name{std::move(name)} {}

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

    // 列表模式：[p, p, ..., ...rest?]。
    //   - elements：位置模式列表（可含 WildcardPatternNode 占位）。
    //   - rest：收集剩余 list[i..] 为新 list 的名字；nullopt 表无 rest（忽略剩余）。
    struct ListPatternNode : PatternNode {
        ListPatternNode(SourceLoc loc, List<UPtr<PatternNode>> elements, Opt<String> rest) :
            PatternNode{loc}, elements{std::move(elements)}, rest{std::move(rest)} {}

        [[nodiscard]]
        String dump(usize indent) const override;

        void accept(AstVisitor& visitor) override;

        List<UPtr<PatternNode>> elements;
        Opt<String>             rest;
    };

} // namespace aria

#endif // ARIA_AST_HPP
