#include "compile/CodeGen.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <ranges>
#include <tuple>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "memory/GC.hpp"
#include "memory/StringBuilder.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/value_register.hpp"
#include "util/str.hpp"
#include "util/util.hpp"

#include <format>

namespace aria {

    namespace {
        // 容量上限 = 操作数/索引位宽上限（越界统一以 > 判）；语义名供检查点与报错文案同源。
        constexpr u32 kMaxArity          = kU8OperandMax;
        constexpr u32 kMaxArguments      = kU8OperandMax;
        constexpr u32 kMaxConstants      = kU16OperandMax;
        constexpr u32 kMaxListElements   = kU16OperandMax;
        constexpr u32 kMaxMapEntries     = kU16OperandMax;
        constexpr u32 kMaxInterpSegments = kU8OperandMax;
        constexpr u32 kMaxLocals         = kU16OperandMax;

        // 二元 op -> 发射 OpCode；Or/And 走短路分支（JUMP_*_OR_POP）不经此。
        OpCode binary_opcode(const Op::Binary op) noexcept {
            switch (op) {
                case Op::Binary::EqualEqual:
                    return OpCode::EQUAL;
                case Op::Binary::EqualEqualEqual:
                    return OpCode::STRICT_EQUAL;
                case Op::Binary::BangEqual:
                    return OpCode::NOT_EQUAL;
                case Op::Binary::BangEqualEqual:
                    return OpCode::STRICT_NOT_EQUAL;
                case Op::Binary::Greater:
                    return OpCode::GREATER;
                case Op::Binary::GreaterEqual:
                    return OpCode::GREATER_EQUAL;
                case Op::Binary::Less:
                    return OpCode::LESS;
                case Op::Binary::LessEqual:
                    return OpCode::LESS_EQUAL;
                case Op::Binary::Plus:
                    return OpCode::ADD;
                case Op::Binary::Minus:
                    return OpCode::SUBTRACT;
                case Op::Binary::Star:
                    return OpCode::MULTIPLY;
                case Op::Binary::Slash:
                    return OpCode::DIVIDE;
                case Op::Binary::Percent:
                    return OpCode::MOD;
                default:
                    UNREACHABLE();
            }
        }

        // 复合赋值 op -> 对应二元 op；文法仅算术五种复合，= 不入表（直走 store）。
        Op::Binary compound_op(const Op::Assignment op) noexcept {
            switch (op) {
                case Op::Assignment::PlusAssign:
                    return Op::Binary::Plus;
                case Op::Assignment::MinusAssign:
                    return Op::Binary::Minus;
                case Op::Assignment::StarAssign:
                    return Op::Binary::Star;
                case Op::Assignment::SlashAssign:
                    return Op::Binary::Slash;
                case Op::Assignment::PercentAssign:
                    return Op::Binary::Percent;
                default:
                    UNREACHABLE();
            }
        }

        // 必传参数数：首个带默认值参数之前（文法定序 plain -> default -> varargs）；调用方须已过 validate_params。
        u8 min_arity(const List<Param>& params) noexcept {
            u8 count = 0;
            for (const auto& param: params) {
                if (param.default_value != nullptr || param.is_varargs) {
                    break;
                }
                ++count;
            }
            return count;
        }

        // `_` 位置不产生下标访问：该位置无值诉求，不冒访问失败面（如 map 缺键），由父层跳过取元素与递归。
        [[nodiscard]] bool is_wildcard_pattern(const PatternNode& pattern) noexcept {
            return dynamic_cast<const WildcardPatternNode*>(&pattern) != nullptr;
        }

        // 会发出下标访问的位置数，决定 Fill 绑定是否需隐藏局部复取源值。
        [[nodiscard]] usize pattern_access_count(const ListPatternNode& pattern) noexcept {
            usize count = 0;
            for (const auto& element: pattern.elements) {
                if (!is_wildcard_pattern(*element)) {
                    ++count;
                }
            }
            if (pattern.rest) {
                count += 1;
            }
            return count;
        }
    } // namespace

    // 静态服务入口：一次性对象上跑单次编译。
    Result<ObjFunction*, Error> CodeGen::compile(GC& gc, const ProgramNode& program, ObjModule* module,
                                                 const StringView entry_name) {
        CodeGen self{gc};
        return self.generate_bytecode(program, module, entry_name);
    }

    Result<ObjFunction*, Error> CodeGen::generate_bytecode(const ProgramNode& program, ObjModule* module,
                                                           const StringView entry_name) {
        // module 入临时根贯穿全程。
        const auto guard = gc_.make_guard(module);

        const auto entry = init_module(module, entry_name);

        try {
            // 遍历顶层声明（顶层 var/fun/import -> 模块全局；嵌套块内 var -> 局部）。
            for (const auto& decl: program.declarations) {
                emit_stmt(*decl);
            }
            emit_implicit_return(program.loc());
        } catch (AriaCompileException& e) {
            // 出错即 unwind 到此：~ModuleCtx 随一次性对象析构沿 enclosing_ 链释放入口 + 未还原的子上下文。
            return std::unexpected(e.error());
        }

#ifdef DEBUG_PRINT_COMPILED_CODE
        // 打印入口 CodeUnit 反汇编。
        io::println(stderr, "{}", cur_cu()->disassemble(entry_name));
#endif

        // mod_ctx_ 随析构释放，无需显式 reset。
        return entry;
    }

    ObjFunction* CodeGen::init_module(ObjModule* module, const StringView entry_name) {
        // 入口无参
        const auto entry = new_function(gc_, module, entry_name, 0, 0, false);
        module->set_entry(entry);
        mod_ctx_ = std::make_unique<ModuleCtx>(module);
        return entry;
    }

    FunctionCtx* CodeGen::cur_fn_ctx() const noexcept { return mod_ctx_->current_fn_ctx_; }

    CodeUnit* CodeGen::cur_cu() const noexcept { return &cur_fn_ctx()->fn_->unit(); }

    u16 CodeGen::add_constant_or_fail(const Value value, const SourceLoc loc) const {
        // 溢出预检 fail CodeUnitTooLarge，使 CodeUnit::add_constant 内部 ASSERT 永不触达。
        if (cur_cu()->constants.size() > kMaxConstants) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "too many constants (max {})", kMaxConstants);
        }
        return cur_fn_ctx()->add_constant(value);
    }

    u16 CodeGen::add_name_or_fail(const StringView name, const SourceLoc loc) const {
        // new_string 立即入池（trivial 分配不触 GC），无需守卫：已 intern 的串与池内同值项同指针。
        const auto str = new_string(gc_, name);
        return add_constant_or_fail(Value::from_obj(str), loc);
    }

    void CodeGen::begin_scope() const { cur_fn_ctx()->begin_scope(); }

    void CodeGen::end_scope(const u32 line) const {
        emit_pop_locals_to(cur_fn_ctx()->scope_depth_ - 1, line);
        cur_fn_ctx()->end_scope();
    }

    void CodeGen::emit_pop_locals_to(const u32 target_depth, const u32 line) const {
        // CLOSE_UPVALUE 只关不弹（弹栈全由 POP_N 承担），两指令均无分配无安全点。
        u32  count        = 0; // 弹区局部总数（含被捕获者，POP_N 计数）
        bool has_captured = false;
        for (const auto& local: std::views::reverse(cur_fn_ctx()->locals_)) {
            if (local.depth <= target_depth) {
                break; // 弹区到此为止(活局部按 depth 非递减序排列;slot 0 哑元 depth=0 恒在界外)
            }
            ++count;
            has_captured = has_captured || local.is_captured;
        }
        cur_cu()->emit_pop_n(count, line); // count==0 无指令
        if (has_captured) {
            cur_cu()->emit_op(OpCode::CLOSE_UPVALUE, line); // 批量关 top 及以上的开 upvalue
        }
    }

    u16 CodeGen::define_local_or_fail(const StringView name, const SourceLoc loc) const {
        if (cur_fn_ctx()->is_defined_in_scope(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "redefined local variable '{}'", name);
        }
        if (cur_fn_ctx()->locals_.size() > kMaxLocals) {
            fail(ErrorCode::TooManyLocals, loc, "too many locals (max {})", kMaxLocals);
        }
        return cur_fn_ctx()->add_local(name); // 纯登记
    }

    void CodeGen::define_global_or_fail(const StringView name, const SourceLoc loc) const {
        if (!mod_ctx_->declare_global(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "redefined global variable '{}'", name);
        }
        const u32  line     = loc.line();
        const auto name_idx = add_name_or_fail(name, loc);
        cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
        cur_cu()->emit_word(name_idx, line); // 弹值定义全局
    }

    CodeGen::ResolvedVar CodeGen::resolve_name_or_fail(const StringView name, const SourceLoc loc) {
        if (const auto local_idx = cur_fn_ctx()->find_local(name)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Local, .index = *local_idx};
        }
        if (const auto upvalue_idx = resolve_upvalue(cur_fn_ctx(), name, loc)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Upvalue, .index = *upvalue_idx};
        }
        const auto name_idx = add_name_or_fail(name, loc);
        return ResolvedVar{.kind = ResolvedVar::Kind::Global, .index = name_idx};
    }

    Opt<u8> CodeGen::resolve_upvalue(FunctionCtx* ctx, const StringView name, const SourceLoc loc) {
        // nullopt 只表「无外层可捕获 -> 落全局」（容量越界已翻译为 fail，不外泄越界信号，
        // 否则捕获引用静默串台全局）。
        if (ctx == nullptr || ctx->enclosing_ == nullptr) {
            return std::nullopt;
        }
        if (const auto slot = ctx->enclosing_->find_local(name)) {
            ctx->enclosing_->locals_[*slot].is_captured = true;
            return add_upvalue_or_fail(ctx, UpvalueDesc{.is_local = true, .index = *slot}, loc);
        }
        if (const auto upvalue = resolve_upvalue(ctx->enclosing_, name, loc)) {
            return add_upvalue_or_fail(ctx, UpvalueDesc{.is_local = false, .index = *upvalue}, loc);
        }
        return std::nullopt;
    }

    CodeGen::ResolvedVar CodeGen::resolve_this_or_fail(const SourceLoc loc) {
        if (const auto slot = cur_fn_ctx()->find_local(kThisName)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Local, .index = *slot};
        }
        if (const auto upvalue_idx = resolve_upvalue(cur_fn_ctx(), kThisName, loc)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Upvalue, .index = *upvalue_idx};
        }
        fail(ErrorCode::ThisOutsideClass, loc, "'this' outside method");
    }

    u8 CodeGen::add_upvalue_or_fail(FunctionCtx* ctx, const UpvalueDesc desc, const SourceLoc loc) const {
        if (const auto idx = ctx->add_upvalue(desc)) {
            return *idx;
        }
        fail(ErrorCode::TooManyUpvalues, loc, "too many upvalues (max {})", kMaxUpvalues);
    }

    void CodeGen::patch_jump_or_fail(const u32 src_off, const SourceLoc loc) const {
        if (!cur_cu()->patch_jump(src_off)) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "function too large: jump offset exceeds {}", kU16OperandMax);
        }
    }

    u32 CodeGen::emit_cond_jump(ExprNode& cond, const u32 line) {
        // 条件跳转发射收口:条件恰为 `==` 二元时直发融合指令 JUMP_NE(与
        // [cond][JUMP_FALSE] 逐语义等价,省一次派发往返);其余形态原样 [cond][JUMP_FALSE]。
        // 融合判定必须在 AST 形状上做 -- 发射期「尾字节恰为 EQUAL」不等于「该 EQUAL 的值即
        // 本次条件」(|| 链 rhs 的 EQUAL 与外层跳转之间隔着值消费关系,按尾字节误融合会双弹
        // 操作数、还覆盖 || 已回填的链尾补丁地址)。
        if (const auto* eq = dynamic_cast<const BinaryExprNode*>(&cond);
            eq != nullptr && eq->op == Op::Binary::EqualEqual) {
            emit_expr(*eq->lhs);
            emit_expr(*eq->rhs);
            return cur_cu()->emit_jump(OpCode::JUMP_NE, line);
        }
        emit_expr(cond);
        return cur_cu()->emit_jump(OpCode::JUMP_FALSE, line);
    }

    void CodeGen::emit_jump_back_or_fail(const u32 target_off, const SourceLoc loc) const {
        if (!cur_cu()->emit_jump_back(target_off, loc.line())) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "function too large: backward jump offset exceeds {}",
                 kU16OperandMax);
        }
    }

    void CodeGen::emit_loop_backedge_and_exits(const LoopCtx& loop_ctx, const SourceLoc loc) const {
        emit_jump_back_or_fail(loop_ctx.back_target, loc);

        for (const u32 fp: loop_ctx.exit_fwd_patches) {
            patch_jump_or_fail(fp, loc); // -> L_end
        }
    }

    void CodeGen::bind_stack_value(const StringView name, const SourceLoc loc) const {
        if (mod_ctx_->is_global_scope()) {
            define_global_or_fail(name, loc); // 弹值定义全局
        } else {
            std::ignore = define_local_or_fail(name, loc); // 值填槽：值恰在槽位，登记即初始化
        }
    }

    void CodeGen::validate_lvalue_target(ExprNode& target) const {
        if (dynamic_cast<IdentifierNode*>(&target) != nullptr) {
            return;
        }
        if (dynamic_cast<FieldAccessNode*>(&target) != nullptr) {
            return;
        }
        if (dynamic_cast<IndexAccessNode*>(&target) != nullptr) {
            return;
        }
        fail(ErrorCode::InvalidAssignmentTarget, target.loc(), "invalid assignment target");
    }

    void CodeGen::emit_lvalue(ExprNode& node, const LvalueMode mode) {
        validate_lvalue_target(node);
        lvalue_mode_ = mode;
        node.accept(*this);
    }

    CodeGen::LvalueMode CodeGen::take_lvalue_mode() { return std::exchange(lvalue_mode_, LvalueMode::Load); }

    void CodeGen::emit_prepare_method(const u16 name_idx, const u32 line) const {
        cur_cu()->emit_op(OpCode::PREPARE_METHOD, line);
        cur_cu()->emit_word(name_idx, line);
    }

    void CodeGen::emit_method_call0(const StringView name, const SourceLoc loc) const {
        // 迭代协议三方（iter/has_next/next）的 0 参方法调用；迭代器在调用区槽 0。
        const u32  line     = loc.line();
        const auto name_idx = add_name_or_fail(name, loc);
        emit_prepare_method(name_idx, line);
        cur_cu()->emit_op(OpCode::CALL_METHOD, line);
        cur_cu()->emit_byte(0, line);
    }

    bool CodeGen::is_in_method() const { return is_method(cur_fn_ctx()->kind_); }

    void CodeGen::emit_load_var(const ResolvedVar& var, const u32 line) const {
        switch (const auto [kind, slot] = var; kind) {
            case ResolvedVar::Kind::Local:
                cur_cu()->emit_load_local(slot, line);
                return;
            case ResolvedVar::Kind::Global:
                cur_cu()->emit_op(OpCode::LOAD_GLOBAL, line);
                cur_cu()->emit_word(slot, line);
                return;
            case ResolvedVar::Kind::Upvalue:
                cur_cu()->emit_op(OpCode::LOAD_UPVALUE, line);
                cur_cu()->emit_byte(slot, line); // 索引域由 add_upvalue 容量检查保证 <= u8
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::emit_store_var(const ResolvedVar& var, const u32 line) const {
        switch (const auto [kind, slot] = var; kind) {
            case ResolvedVar::Kind::Local:
                cur_cu()->emit_store_local(slot, line);
                return;
            case ResolvedVar::Kind::Global:
                cur_cu()->emit_op(OpCode::STORE_GLOBAL, line);
                cur_cu()->emit_word(slot, line);
                return;
            case ResolvedVar::Kind::Upvalue:
                cur_cu()->emit_op(OpCode::STORE_UPVALUE, line);
                cur_cu()->emit_byte(slot, line); // 索引域由 add_upvalue 容量检查保证 <= u8
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::bind_pattern(PatternNode& node, const PatternBindMode mode) {
        pattern_mode_ = mode;
        node.accept(*this);
    }

    template<typename PushSource>
    void CodeGen::emit_list_pattern_accesses(ListPatternNode& node, const u32 line, PushSource&& push_source) {
        for (usize index = 0; index < node.elements.size(); ++index) {
            if (is_wildcard_pattern(*node.elements[index])) {
                continue; // `_` 不访问该位置
            }
            push_source();
            emit_int_literal(static_cast<i64>(index), line, node.loc());
            cur_cu()->emit_op(OpCode::LOAD_INDEX, line); // [src, idx] -> [element]
            node.elements[index]->accept(*this);         // 递归绑定（模式不变）
        }
        if (node.rest) {
            // rest 位 = 无上界 range 作下标键（切片，空尾得空 list）；绑名经 rest 节点自身 visit。
            push_source();
            emit_int_literal(static_cast<i64>(node.elements.size()), line, node.loc());
            cur_cu()->emit_op(OpCode::MAKE_RANGE, line);
            cur_cu()->emit_byte(kRangeFlagUnbounded, line); // [src, range]
            cur_cu()->emit_op(OpCode::LOAD_INDEX, line);    // [suffix]
            node.rest->accept(*this);
        }
    }

    void CodeGen::emit_expr(ExprNode& node) {
        // emit_lvalue 分派后必恢复 Load；ASSERT 捕获漏恢复（非预防性赋值）。
        ASSERT(lvalue_mode_ == LvalueMode::Load,
               "lvalue_mode_ must be Load (emit_lvalue did not restore it after dispatch)");
        node.accept(*this);
    }

    void CodeGen::emit_stmt(StmtNode& node) { node.accept(*this); }

    void CodeGen::emit_expr_or_nil(ExprNode* expr, const u32 line) {
        if (expr != nullptr) {
            emit_expr(*expr);
        } else {
            cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        }
    }

    void CodeGen::validate_params(const List<Param>& params, const SourceLoc loc) const {
        if (params.size() > kMaxArity) {
            fail(ErrorCode::TooManyParameters, loc, "too many parameters (max {})", kMaxArity);
        }

        for (usize i = 0; i < params.size(); ++i) {
            for (usize j = i + 1; j < params.size(); ++j) {
                if (params[i].name == params[j].name) {
                    fail(ErrorCode::DuplicateParam, loc, "duplicate parameter '{}'", params[i].name);
                }
            }
        }
    }

    void CodeGen::bind_function_value(const FnKind kind, const StringView name, const SourceLoc loc) const {
        // MAKE_STATIC 不戳 defining class（静态槽读恒原值），MAKE_METHOD 戳（方法性标记 + super 来源）。
        const u32 line = loc.line();
        switch (kind) {
            case FnKind::Function:
                bind_stack_value(name, loc); // [closure] -> [] 全局 DEF_GLOBAL / 局部值填槽
                break;
            case FnKind::Lambda:
                break; // 留栈作 lambda 表达式值,不绑定
            case FnKind::StaticMethod:
            case FnKind::Method:
            case FnKind::InitMethod: {
                // 类成员注册:[class, closure] -> [class],class 值留栈跨整个类体。
                const auto member_op  = kind == FnKind::StaticMethod ? OpCode::MAKE_STATIC : OpCode::MAKE_METHOD;
                const auto member_idx = add_name_or_fail(name, loc);
                cur_cu()->emit_op(member_op, line);
                cur_cu()->emit_word(member_idx, line); // [class]
                break;
            }
            case FnKind::ModuleEntry:
                // 入口 ctx 由 ModuleCtx 构造直出(不经 compile_function),永不到此。
                UNREACHABLE();
        }
    }

    void CodeGen::bind_class_value(const bool is_member, const StringView name, const SourceLoc loc) const {
        // 类名绑定先于类体编译（体内自引用要求名字先存在）；顶层与类体两种绑定须与 visitDefDeclNode 尾部的 POP 配对。
        if (is_member) {
            const auto member_idx = add_name_or_fail(name, loc); // 名字经 MAKE_CLASS 已入池,复用同池项
            cur_cu()->emit_op(OpCode::DUP2, loc.line());         // [e, c, e, c]
            cur_cu()->emit_op(OpCode::MAKE_STATIC, loc.line());  // [e, c, e] peek(1)=enclosing 挂表
            cur_cu()->emit_word(member_idx, loc.line());
            cur_cu()->emit_op(OpCode::POP, loc.line()); // [e, c] 弃 klass 副本
        } else if (mod_ctx_->is_global_scope()) {
            cur_cu()->emit_op(OpCode::DUP, loc.line()); // [class, class]
            define_global_or_fail(name, loc);           // [class] 弹走 DUP 副本绑定全局
        } else {
            std::ignore = define_local_or_fail(name, loc); // 值填槽，纯登记
        }
    }

    void CodeGen::compile_params(const List<Param>& params, const SourceLoc loc) {
        // 未传槽由 VM 预垫缺省印章（寄存器 DefaultMark）：逐缺省槽与印章判等，命中才求值默认值换入；
        // 缺省表达式解析不到的名字按常规链落外层/全局；序言后栈空。
        const u32 line = loc.line();
        for (usize i = 0; i < params.size(); ++i) {
            const auto& param = params[i];
            if (param.default_value != nullptr) {
                const u16 slot = i + 1; // 参数槽 1..n(槽 0 = this/哑元)
                cur_cu()->emit_load_local(slot, line);
                cur_cu()->emit_op(OpCode::LOAD_REG, line);
                cur_cu()->emit_byte(kDefaultMarkOffset, line);
                const u32 skip = cur_cu()->emit_jump(OpCode::JUMP_NE, line);
                emit_expr(*param.default_value);
                cur_cu()->emit_store_local(slot, line); // peek-store 换入参数槽
                cur_cu()->emit_op(OpCode::POP, line);   // STORE_LOCAL 不弹,弹掉求值副本恢复「栈高 == 已填槽数」
                patch_jump_or_fail(skip, loc);
            }
            // 重名已由 validate_params 查，直接登记。
            cur_fn_ctx()->add_local(param.name);
        }
    }

    void CodeGen::emit_implicit_return(const SourceLoc loc) const {
        const u32 line = loc.line();
        switch (cur_fn_ctx()->kind_) {
            case FnKind::ModuleEntry: {
                // RETURN 通用写回 callee 槽，即 IMPORT 预留的结果槽。
                const auto module_idx = add_constant_or_fail(Value::from_obj(mod_ctx_->module_), loc);
                cur_cu()->emit_op(OpCode::LOAD_CONST, line);
                cur_cu()->emit_word(module_idx, line);
                break;
            }
            case FnKind::InitMethod:
                cur_cu()->emit_load_local(0, line);
                break;
            case FnKind::Function:
            case FnKind::Lambda:
            case FnKind::StaticMethod:
            case FnKind::Method:
                cur_cu()->emit_op(OpCode::LOAD_NIL, line);
                break;
        }
        cur_cu()->emit_op(OpCode::RETURN, line);
    }

    void CodeGen::compile_function(const StringView name, const List<Param>& params, BlockNode& body,
                                   const SourceLoc decl_loc, const FnKind kind) {
        // 失败即抛 AriaCompileException，先于所有分配与发射。
        validate_params(params, decl_loc);

        // varargs 恒末位（Parser 保证）：rest 槽由 call_closure 打包多余实参为 list，帧参数槽深 = arity + is_varargs。
        const bool has_varargs = !params.empty() && params.back().is_varargs;
        const auto fixed_arity = static_cast<u8>(params.size() - (has_varargs ? 1 : 0));
        const auto fn = new_function(gc_, mod_ctx_->module_, name, fixed_arity, min_arity(params), has_varargs);
        // 入池后即经 module 根链可达。
        const auto fn_idx = add_constant_or_fail(Value::from_obj(fn), decl_loc);
        // CLOSURE fn_idx：VM 现场按捕获描述表（下方 flush 进 fn 元数据）包 ObjClosure 并建/复用 upvalue。
        cur_cu()->emit_op(OpCode::CLOSURE, decl_loc.line());
        cur_cu()->emit_word(fn_idx, decl_loc.line());

        bind_function_value(kind, name, decl_loc);

        // 切到子上下文：cu 随游标自动切到子 unit，无需 save/restore；enclosing_ 回父。
        const auto child          = new FunctionCtx{fn, cur_fn_ctx(), kind};
        mod_ctx_->current_fn_ctx_ = child;

        compile_params(params, decl_loc);

        // BlockNode 自带 scope；emit_stmt 抛异常时子留下交 ~ModuleCtx 沿链释放。
        emit_stmt(body);
        emit_implicit_return(body.loc());

        // 把子上下文的捕获描述 flush 进 fn 元数据（CLOSURE 先发射不碍事：表在 ObjFunction 上，
        // VM 执行 CLOSURE 时才读）。
        fn->upvalue_descs().copy_from(child->upvalues_);

#ifdef DEBUG_PRINT_COMPILED_CODE
        // 打印刚编译完成函数的反汇编。
        io::println(stderr, "{}", cur_cu()->disassemble(name));
#endif

        // 成功：还原父游标并 delete 子上下文。
        mod_ctx_->current_fn_ctx_ = child->enclosing_;
        delete child;
    }

    void CodeGen::visitProgramNode(ProgramNode& node) {
        // 仅编排顶层声明，不直接发射。
        for (const auto& decl: node.declarations) {
            emit_stmt(*decl);
        }
    }

    void CodeGen::visitBlockNode(BlockNode& node) {
        const u32 line = node.line();
        begin_scope();
        for (const auto& stmt: node.statements) {
            emit_stmt(*stmt);
        }
        end_scope(line);
    }

    void CodeGen::visitExprStmtNode(ExprStmtNode& node) {
        const u32 line = node.line();
        emit_expr(*node.expr);
        cur_cu()->emit_op(OpCode::POP, line);
    }

    void CodeGen::visitIfStmtNode(IfStmtNode& node) {
        const u32 line = node.line();
        const u32 jf   = emit_cond_jump(*node.condition, line); // -> else / end
        emit_stmt(*node.then_branch);
        if (node.else_branch != nullptr) {
            const u32 jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
            patch_jump_or_fail(jf, node.loc());                       // -> else
            emit_stmt(*node.else_branch);
            patch_jump_or_fail(jend, node.loc()); // -> end
        } else {
            patch_jump_or_fail(jf, node.loc()); // -> end
        }
    }

    void CodeGen::visitWhileStmtNode(WhileStmtNode& node) {
        const u32 line     = node.line();
        auto      loop_ctx = LoopCtx{.loop_scope_depth = cur_fn_ctx()->scope_depth_,
                                     .back_target      = cur_cu()->size()}; // 循环头 = 条件起点 = continue 后向目标
        const u32 patch    = emit_cond_jump(*node.condition, line);         // -> L_end 占位
        loop_ctx.exit_fwd_patches.push_back(patch);
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(*node.body);
        loop_ctx = util::pop_top(cur_fn_ctx()->loop_stack_);

        emit_loop_backedge_and_exits(loop_ctx, node.loc());
    }

    void CodeGen::visitForStmtNode(ForStmtNode& node) {
        const u32 line = node.line();
        begin_scope();
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;
        if (node.init != nullptr) {
            emit_stmt(*node.init);
        }
        const bool has_cond = node.condition != nullptr;
        const bool has_incr = node.increment != nullptr;
        // continue 通道随 has_incr 打开：有 incr 前向跳 L_incr（回填），无 incr 后向跳循环头。
        auto loop_ctx = LoopCtx{.loop_scope_depth = loop_scope, .back_target = cur_cu()->size()};
        if (has_incr) {
            loop_ctx.continue_fwd_patches.emplace(); // 打开前向 continue 通道
        }
        if (has_cond) {
            const u32 patch = emit_cond_jump(*node.condition, line); // -> L_end 占位
            loop_ctx.exit_fwd_patches.push_back(patch);
        } // 无 cond: exit 列表空，收尾只有回边 + break 回填
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(*node.body);
        loop_ctx = util::pop_top(cur_fn_ctx()->loop_stack_);

        // continue 回填须先于递增发射：此刻 cur_cu()->size() 即 L_incr；若递增与 JUMP_BACK 发完再回填，
        // size() 已是 L_end，continue 会错跳提前出循环。
        if (loop_ctx.continue_fwd_patches) {
            for (const u32 patch: *loop_ctx.continue_fwd_patches) {
                patch_jump_or_fail(patch, node.loc()); // -> L_incr
            }
        }
        if (has_incr) {
            emit_expr(*node.increment);
            cur_cu()->emit_op(OpCode::POP, line);
        }
        emit_loop_backedge_and_exits(loop_ctx, node.loc());
        end_scope(line);
    }

    void CodeGen::visitForInStmtNode(ForInStmtNode& node) {
        const u32 line = node.line();
        // lowering：外层 for-in scope 挂隐藏局部 <iter> = iterable.iter()（"<>" 不可作标识符，不撞名）；
        // 循环头 = has_next() 判断处（continue 跳此，无增量步）；每轮 per-iteration scope 内
        // <pattern> = iter.next()（Fill）后跑体、end_scope 收口，每轮 fresh 绑定。
        begin_scope(); // for-in scope：仅 <iter>，循环全程存活
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;

        // 隐藏局部 <iter> 值填槽：iter() 出值即登记，无预占槽。
        // [iterable]（receiver）

        emit_expr(*node.iterable);
        emit_method_call0("iter", node.loc());                                // [iter_obj] 恰在 slot 位置
        const u16 iter_var_slot = define_local_or_fail("<iter>", node.loc()); // 值已在槽位，登记即初始化

        auto loop_ctx = LoopCtx{.loop_scope_depth = loop_scope, .back_target = cur_cu()->size()};
        // [iter]（receiver）
        cur_cu()->emit_load_local(iter_var_slot, line);
        // [bool]
        emit_method_call0("has_next", node.loc());
        const u32 patch = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end 占位
        loop_ctx.exit_fwd_patches.push_back(patch);
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));

        // per-iteration scope：pattern + 体每轮 fresh；体为 block 自带更深层 scope，break/continue
        // 跳出由 emit_pop_locals_to(loop_scope) 代弹。
        begin_scope();
        cur_cu()->emit_load_local(iter_var_slot, line); // [iter]（receiver）
        emit_method_call0("next", node.loc());          // [value] 恰在 slot 位置
        // Fill 绑定：identifier 值填槽（不发指令）/ 解构逐位置填槽 / _ 弹掉该值
        bind_pattern(*node.pattern, PatternBindMode::Fill);
        emit_stmt(*node.body);
        end_scope(line); // per-iter：POP_N 弹 pattern（id）；_ 无局部 -> emit_pop_n(0) 无指令

        loop_ctx = util::pop_top(cur_fn_ctx()->loop_stack_);

        emit_loop_backedge_and_exits(loop_ctx, node.loc());
        end_scope(line); // for-in：POP_N 弹 <iter>
    }

    void CodeGen::visitBreakStmtNode(BreakStmtNode& node) {
        const u32 line = node.line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::BreakOutsideLoop, node.loc(), "'break' outside loop");
        }
        auto& loop_ctx = cur_fn_ctx()->loop_stack_.top();
        emit_pop_locals_to(loop_ctx.loop_scope_depth, line);
        const u32 patch = cur_cu()->emit_jump(OpCode::JUMP, line); // -> L_end（回填）
        loop_ctx.exit_fwd_patches.push_back(patch);
    }

    void CodeGen::visitContinueStmtNode(ContinueStmtNode& node) {
        const u32 line = node.line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::ContinueOutsideLoop, node.loc(), "'continue' outside loop");
        }
        auto& loop_ctx = cur_fn_ctx()->loop_stack_.top();
        emit_pop_locals_to(loop_ctx.loop_scope_depth, line);
        if (loop_ctx.continue_fwd_patches) {
            const u32 patch = cur_cu()->emit_jump(OpCode::JUMP, line); // -> L_incr（回填）
            loop_ctx.continue_fwd_patches->push_back(patch);
        } else {
            emit_jump_back_or_fail(loop_ctx.back_target, node.loc()); // 后向跳循环头（条件判断处）
        }
    }

    void CodeGen::visitReturnStmtNode(ReturnStmtNode& node) {
        // 带值 return 编译期拒绝；裸 return 即压模块对象后 RETURN 提前退出，与隐式收尾同序。
        if (cur_fn_ctx()->kind_ == FnKind::ModuleEntry) {
            if (node.value != nullptr) {
                fail(ErrorCode::ReturnValueAtTopLevel, node.loc(), "'return' at top level must not carry a value");
            }
            emit_implicit_return(node.loc());
            return;
        }
        const u32 line = node.line();
        emit_expr_or_nil(node.value.get(), line);
        cur_cu()->emit_op(OpCode::RETURN, line);
    }

    void CodeGen::visitImportStmtNode(ImportStmtNode& node) {
        const u32 line = node.line();
        // IMPORT 压模块值于栈顶；绑定与 var/fun 同形（顶层 DEF_GLOBAL，嵌套值填槽）。
        // path 文法钉死字符串字面量（Parser 保证指向 StringLiteralNode），与字面量表达式共用驻留口。
        const auto path_idx = add_string_literal_or_fail(dynamic_cast<StringLiteralNode&>(*node.path));
        cur_cu()->emit_op(OpCode::IMPORT, line);
        cur_cu()->emit_word(path_idx, line); // [module]
        // path 已入池经 module 根链可达，alias 的 new_string 不会回收 path。
        bind_stack_value(node.alias, node.loc()); // [module] -> [] DEF_GLOBAL 弹值 / 值填槽
    }

    void CodeGen::visitTryStmtNode(TryStmtNode& node) {
        const u32 line = node.line();
        // try 须有 catch（parse 层允许无 catch，语义收口在此）。
        if (node.catch_body == nullptr) {
            fail(ErrorCode::TryWithoutHandler, node.loc(), "'try' requires a catch clause");
        }

        // lowering：入口预插 try_records 占位（begin 已定，余末尾回填）-> 编译体 -> JUMP 跳过 catch；
        // catch 参数 e 由 unwind 截栈到 slots + stack_depth 后 push 恰落槽（无 STORE_LOCAL）；
        // 两路径 end_scope 均回到 stack_depth，栈在 L_end 齐平。
        const auto stack_depth = static_cast<u32>(cur_fn_ctx()->locals_.size()); // try 入口局部数(try scope 开前)
        const u32  begin       = cur_cu()->size();
        const auto rec_idx     = cur_cu()->try_records.size();
        cur_cu()->try_records.push(TryRecord{begin, 0, 0, 0});
        begin_scope();
        emit_stmt(*node.body); // 嵌套 try 在此预插占位，begin > 本层故整体升序
        end_scope(line);
        const u32 end   = cur_cu()->size();
        const u32 jskip = cur_cu()->emit_jump(OpCode::JUMP, line); // 正常路径跳过 catch -> L_end
        // L_catch
        const u32 handle = cur_cu()->size();
        begin_scope();                                              // catch 子句 scope 含 e + 体，两路径栈平衡
        std::ignore = define_local_or_fail(node.ename, node.loc()); // e 由 unwind 的 push 运行期填槽(== stack_depth)
        emit_stmt(*node.catch_body);
        end_scope(line);
        patch_jump_or_fail(jskip, node.loc()); // -> L_end
        // 回填占位项；内层记录已在体编译期间插于本项之后，构造即升序、不排序。
        cur_cu()->try_records[rec_idx].end         = end;
        cur_cu()->try_records[rec_idx].handle      = handle;
        cur_cu()->try_records[rec_idx].stack_depth = stack_depth;
    }

    void CodeGen::visitThrowStmtNode(ThrowStmtNode& node) {
        const u32 line = node.line();
        // THROW 弹值入挂起寄存器，由 unwind 查异常记录表派发；原值不包 ObjException，catch 绑原值保类型。
        // [v]
        emit_expr(*node.expr);
        cur_cu()->emit_op(OpCode::THROW, line); // [v] -> [](派发 handler 时值落 catch 参数槽)
    }

    template<typename Arm>
    void CodeGen::validate_match_arms(const List<Arm>& arms) const {
        for (usize i = 1; i < arms.size(); ++i) {
            if (arms[i - 1].pattern.value == nullptr) {
                fail(ErrorCode::UnreachableArm, arms[i].body->loc(), "unreachable arm after '_'");
            }
        }
    }

    // match 降糖总口（模板定义，两 visit 为实例化点）；subject 驻栈跨臂复用，全臂未命中由 THROW
    // 的 unwind 清栈。
    template<typename Node>
    void CodeGen::emit_match(Node& node) {
        validate_match_arms(node.arms);
        const u32 line = node.line();
        // [s]
        emit_expr(*node.subject);
        List<u32> end_jumps; // 各臂命中路径 -> L_end
        Opt<u32>  miss_jump; // 上臂 JUMP_FALSE 占位(通配臂无),回填到下臂起点
        for (const auto& [pattern, body]: node.arms) {
            if (const auto taken = util::take(miss_jump)) {
                patch_jump_or_fail(*taken, node.loc()); // 上臂未命中 -> 本臂
            }
            if (pattern.value != nullptr) {           // "_" 通配:不比较直入
                cur_cu()->emit_op(OpCode::DUP, line); // [s, s] 副本供比较,subject 本尊保留
                emit_expr(*pattern.value);            // [s, s, p]
                miss_jump = cur_cu()->emit_jump(OpCode::JUMP_NE, line);
            }
            cur_cu()->emit_op(OpCode::POP, line); // 命中:[s] -> [] 丢弃 subject 进臂体
            emit_arm_body(*body);
            const u32 end_jump = cur_cu()->emit_jump(OpCode::JUMP, line);
            end_jumps.push_back(end_jump); // -> L_end(越兜底)
        }
        // 全臂未命中:LOAD_REG 抛共享 MatchNoArm 异常,THROW 后 unwind 接管不落 L_end。
        if (miss_jump) {
            patch_jump_or_fail(*miss_jump, node.loc()); // 末臂未命中 -> 兜底
        }
        cur_cu()->emit_op(OpCode::LOAD_REG, line); // [e] 共享异常单例
        cur_cu()->emit_byte(kMatchNoArmOffset, line);
        cur_cu()->emit_op(OpCode::THROW, line); // [] 弹 e 入挂起寄存器,unwind
        for (const u32 jump: end_jumps) {
            patch_jump_or_fail(jump, node.loc()); // -> L_end
        }
    }

    void CodeGen::emit_arm_body(StmtNode& body) { emit_stmt(body); }

    void CodeGen::emit_arm_body(ExprNode& body) { emit_expr(body); }

    void CodeGen::visitMatchStmtNode(MatchStmtNode& node) { emit_match(node); }

    void CodeGen::visitFunDeclNode(FunDeclNode& node) {
        compile_function(node.name, node.params, *node.body, node.loc(), node.kind);
    }

    void CodeGen::visitDefDeclNode(DefDeclNode& node) {
        const u32 line = node.line();

        // ① superclass：运行期解析读取（编译期不查全局，未命中沿用运行期 UndefinedVariable）；
        //    无父类则 LOAD_REG ObjectClass（def Foo 等价 def Foo : Object，不经名字查、shadow 免疫）。
        if (node.super != nullptr) {
            emit_expr(*node.super); // [super]
        } else {
            cur_cu()->emit_op(OpCode::LOAD_REG, line); // [Object]
            cur_cu()->emit_byte(kObjectClassOffset, line);
        }

        // ② MAKE_CLASS name:peek superclass 建类写回原槽,class 值留栈跨整个类体。
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        cur_cu()->emit_op(OpCode::MAKE_CLASS, line);
        cur_cu()->emit_word(name_idx, line); // [class]

        // ③ 类名绑定先于体编译（体内自引用要求先在），收口在 bind_class_value。
        bind_class_value(node.is_member, node.name, node.loc());

        // ④ 成员按源序发射（静态变量初始化即此序）；重名不查重，成员即表写入、后写遮蔽。
        for (const auto& member: node.members) {
            member->accept(*this);
        }

        // ⑤ 尾部弹栈：顶层与类体绑定驻留的类值在此归位（与 bind_class_value 配对）；局部绑定无此尾。
        if (node.is_member || mod_ctx_->is_global_scope()) {
            cur_cu()->emit_op(OpCode::POP, line); // [class] -> [] 弹驻留类值
        }
    }

    void CodeGen::visitVarDeclNode(VarDeclNode& node) {
        for (const auto& [target, initializer]: node.bindings) {
            // 初始化器先于声明名求值：求值后栈高 == locals_.size()，值恰在待声明槽位；init 同名引用
            // 沿 resolve 链落外层（落全局则运行期 UndefinedVariable）。
            emit_expr_or_nil(initializer.get(), target->line());
            bind_pattern(*target, PatternBindMode::Fill);
        }
    }

    void CodeGen::visitStaticVarMemberNode(StaticVarMemberNode& node) {
        // 初始化器在类定义点、enclosing 作用域求值（eager）；绑定先于体编译，所以类名自引用在顶层与类体内都可用。
        const u32 line = node.line();
        emit_expr_or_nil(node.initializer.get(), line); // [class, v]
        const auto member_idx = add_name_or_fail(node.name, node.loc());
        cur_cu()->emit_op(OpCode::MAKE_STATIC, line);
        cur_cu()->emit_word(member_idx, line); // [class]
    }

    void CodeGen::validate_int_literal(const i64 value, const SourceLoc loc) const {
        // 整数字面量 i48 值域唯一闸门；越界 NumberOutOfRange。
        if (value < kIntMin || value > kIntMax) {
            fail(ErrorCode::NumberOutOfRange, loc, "integer literal {} out of range", value);
        }
    }

    bool CodeGen::try_emit_load_imm(const i64 value, const u32 line) const {
        if (value < std::numeric_limits<i8>::min() || value > std::numeric_limits<i8>::max()) {
            return false;
        }
        // LOAD_IMM 的 u8 操作数 VM 按 i8 位型重解释，故先经 i8 保符号再转 u8（免窄化告警）。
        cur_cu()->emit_op(OpCode::LOAD_IMM, line);
        cur_cu()->emit_byte(static_cast<u8>(static_cast<i8>(value)), line);
        return true;
    }

    void CodeGen::emit_int_literal(const i64 value, const u32 line, const SourceLoc loc) const {
        if (try_emit_load_imm(value, line)) {
            return;
        }
        validate_int_literal(value, loc);
        const auto idx = add_constant_or_fail(Value::from_int(value), loc);
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    void CodeGen::emit_float_literal(const f64 value, const u32 line, const SourceLoc loc) const {
        const auto idx = add_constant_or_fail(Value::from_f64(value), loc);
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    bool CodeGen::try_emit_negated_literal(ExprNode& operand) const {
        // 负常量本身即一条加载指令，取负并进常量即可，无须运行期 NEGATE；只认直接操作数层
        // （-(-5) 外层仍是 UnaryExpr）。值域按字面量自身值判，越界报文带源符号。
        if (const auto i_node = dynamic_cast<IntegerLiteralNode*>(&operand)) {
            emit_int_literal(-i_node->value, i_node->line(), i_node->loc());
            return true;
        }
        if (const auto f_node = dynamic_cast<FloatLiteralNode*>(&operand)) {
            emit_float_literal(-f_node->value, f_node->line(), f_node->loc());
            return true;
        }
        return false;
    }

    void CodeGen::visitIntegerLiteralNode(IntegerLiteralNode& node) {
        emit_int_literal(node.value, node.line(), node.loc());
    }

    void CodeGen::visitFloatLiteralNode(FloatLiteralNode& node) {
        emit_float_literal(node.value, node.line(), node.loc());
    }

    u16 CodeGen::add_string_literal_or_fail(const StringLiteralNode& node) const {
        if (!node.shape.is_escape) {
            // 无转义:内层原文视图直驻留,零拷贝。
            return add_name_or_fail(node.value, node.loc());
        }
        // 转义串:定长(词法期记账)一次定容,解码直写后接管铸串(驻留判定在 take_string 内)。
        StringBuilder builder{gc_};
        builder.reserve(node.shape.decoded_len);
        const auto written = str::decode_string_content(node.value, builder.begin());
        builder.resize(written);
        return add_constant_or_fail(Value::from_obj(builder.take_string()), node.loc());
    }

    void CodeGen::visitStringLiteralNode(StringLiteralNode& node) {
        const u32  line = node.line();
        const auto idx  = add_string_literal_or_fail(node);
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    void CodeGen::visitInterpolatedStringNode(InterpolatedStringNode& node) {
        const u32 line = node.line();
        // 段数上限（BUILD_STRING 操作数 u8），先检后发；字面段 = StringLiteralNode 走驻留常量。
        if (node.parts.size() > kMaxInterpSegments) {
            fail(ErrorCode::TooManyElements, node.loc(), "too many interpolation segments (max {})",
                 kMaxInterpSegments);
        }
        for (const auto& part: node.parts) {
            emit_expr(*part);
        }
        cur_cu()->emit_op(OpCode::BUILD_STRING, line);
        cur_cu()->emit_byte(node.parts.size(), line); // [v1..vn] -> [str]
    }

    void CodeGen::visitBoolLiteralNode(BoolLiteralNode& node) {
        const u32 line = node.line();
        cur_cu()->emit_op(node.value ? OpCode::LOAD_TRUE : OpCode::LOAD_FALSE, line);
    }

    void CodeGen::visitNilLiteralNode(NilLiteralNode& node) {
        const u32 line = node.line();
        cur_cu()->emit_op(OpCode::LOAD_NIL, line);
    }

    void CodeGen::visitIdentifierNode(IdentifierNode& node) {
        // Prepare no-op：identifier locator 是编译期常量（槽位/捕获索引/名字），无接收者可备；
        // Locate 与 Load 同形，重解析免费。
        const auto mode     = take_lvalue_mode();
        const u32  line     = node.line();
        const auto resolved = resolve_name_or_fail(node.name, node.loc());
        switch (mode) {
            case LvalueMode::Prepare:
                return;
            case LvalueMode::Load:
            case LvalueMode::Locate:
                emit_load_var(resolved, line);
                return;
            case LvalueMode::Store:
                emit_store_var(resolved, line);
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::visitThisExprNode(ThisExprNode& node) {
        // this 是关键字非标识符，走专用解析；发射同普通局部读取。
        const auto resolved = resolve_this_or_fail(node.loc());
        emit_load_var(resolved, node.line());
    }

    void CodeGen::visitSuperExprNode(SuperExprNode& node) {
        // super.成员（单形，rvalue 读）：语境检查后 LOAD_SUPER_FIELD；方法闭包由 VM 绑 this，静态槽原值直读。
        if (!is_in_method()) {
            fail(ErrorCode::SuperOutsideMethod, node.loc(), "'super' outside method");
        }
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        const u32  line     = node.line();
        cur_cu()->emit_op(OpCode::LOAD_SUPER_FIELD, line);
        cur_cu()->emit_word(name_idx, line); // [bound]
    }

    void CodeGen::visitBinaryExprNode(BinaryExprNode& node) {
        const u32 line = node.line();
        emit_expr(*node.lhs);
        // 短路逻辑运算：lhs 真假跳留值、跳过 rhs；否则弹 lhs 求 rhs。跳转回填到 rhs 之后（L_end）。
        if (node.op == Op::Binary::Or) {
            const u32 j = cur_cu()->emit_jump(OpCode::JUMP_TRUE_OR_POP, line);
            emit_expr(*node.rhs);
            patch_jump_or_fail(j, node.loc()); // -> L_end（rhs 之后）
            return;
        }
        if (node.op == Op::Binary::And) {
            const u32 j = cur_cu()->emit_jump(OpCode::JUMP_FALSE_OR_POP, line);
            emit_expr(*node.rhs);
            patch_jump_or_fail(j, node.loc()); // -> L_end（rhs 之后）
            return;
        }
        emit_expr(*node.rhs);
        cur_cu()->emit_op(binary_opcode(node.op), line);
    }

    void CodeGen::visitUnaryExprNode(UnaryExprNode& node) {
        const u32 line = node.line();
        switch (auto& operand = *node.operand; node.op) {
            case Op::Unary::Minus:
                // 负字面量形态：命中即一条负常量加载，否则一般路径。
                if (try_emit_negated_literal(operand)) {
                    return;
                }
                emit_expr(operand);
                cur_cu()->emit_op(OpCode::NEGATE, line);
                return;
            case Op::Unary::Not:
                emit_expr(operand);
                cur_cu()->emit_op(OpCode::NOT, line);
                return;
            case Op::Unary::PreInc:
            case Op::Unary::PreDec: {
                // 与复合赋值同族（先 Locate 定位）；统一压 +1 由 ADD/SUBTRACT 定方向（PreDec 压 -1 会算反）。
                emit_lvalue(operand, LvalueMode::Locate);
                cur_cu()->emit_op(OpCode::LOAD_IMM, line);
                cur_cu()->emit_byte(1, line);
                cur_cu()->emit_op(node.op == Op::Unary::PreInc ? OpCode::ADD : OpCode::SUBTRACT, line);
                emit_lvalue(operand, LvalueMode::Store); // peek-store 留新值
                return;
            }
            default:
                UNREACHABLE();
        }
    }

    void CodeGen::visitAssignmentNode(AssignmentNode& node) {
        const u32 line = node.line();
        if (node.op == Op::Assignment::Assign) {
            // 普通 =：Prepare -> <e> -> Store。STORE_FIELD 栈形 [obj, v] 要求接收者先于值入栈，
            // 发射权在第一步的目标节点；与复合赋值只差中间步骤（直接压值 vs 先 op）。
            emit_lvalue(*node.target, LvalueMode::Prepare);
            emit_expr(*node.value);
            emit_lvalue(*node.target, LvalueMode::Store);
            return;
        }
        // 复合赋值：Locate -> <e> -> op -> Store；运行时 locator 的副本供写入步复用，定位只求值一次。
        emit_lvalue(*node.target, LvalueMode::Locate);
        emit_expr(*node.value);
        cur_cu()->emit_op(binary_opcode(compound_op(node.op)), line);
        emit_lvalue(*node.target, LvalueMode::Store);
    }

    void CodeGen::visitDestructureAssignmentNode(DestructureAssignmentNode& node) {
        // 右值求值一次后 DUP 留作表达式值；Store 的 bind_pattern 净耗一值，消费的是副本。
        const u32 line = node.line();
        emit_expr(*node.value);
        cur_cu()->emit_op(OpCode::DUP, line);
        bind_pattern(*node.target, PatternBindMode::Store);
    }

    bool CodeGen::try_emit_method_call(const CallNode& node) {
        // 未命中（callee 非成员访问）不发射任何字节。
        const auto member = dynamic_cast<FieldAccessNode*>(node.callee.get());
        if (member == nullptr) {
            return false;
        }
        const u32  line     = node.line();
        const auto name_idx = add_name_or_fail(member->name, member->loc());
        emit_expr(*member->object);          // [recv]
        emit_prepare_method(name_idx, line); // 解析此刻完成（实参求值之前）
        for (const auto& arg: node.args) {
            emit_expr(*arg);
        }
        cur_cu()->emit_op(OpCode::CALL_METHOD, line); // 纯调用：[recv, target, a1..aN] -> [r]
        cur_cu()->emit_byte(node.args.size(), line);
        return true;
    }

    void CodeGen::visitCallNode(CallNode& node) {
        const u32 line = node.line();
        // 实参上限（CALL 操作数 u8），先检后发。
        if (node.args.size() > kMaxArguments) {
            fail(ErrorCode::TooManyArguments, node.loc(), "too many arguments (max {})", kMaxArguments);
        }
        if (try_emit_method_call(node)) {
            return;
        }
        emit_expr(*node.callee);
        for (const auto& arg: node.args) {
            emit_expr(*arg);
        }
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(node.args.size(), line);
    }

    bool CodeGen::try_emit_this_field(const FieldAccessNode& node, const LvalueMode mode, const u32 line) const {
        // 模式差异：Prepare 不发指令；Locate 与 Load 同形（this 不经值栈，DUP 副本无人消费只会滞留）。
        if (dynamic_cast<ThisExprNode*>(node.object.get()) == nullptr || !is_in_method()) {
            return false;
        }
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        switch (mode) {
            case LvalueMode::Prepare:
                return true; // 本分支零指令
            case LvalueMode::Load:
            case LvalueMode::Locate:
                cur_cu()->emit_op(OpCode::LOAD_THIS_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [..] -> [.., v]
                return true;
            case LvalueMode::Store:
                cur_cu()->emit_op(OpCode::STORE_THIS_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [v] -> [v]，peek-store 留值
                return true;
        }
        UNREACHABLE();
    }

    void CodeGen::visitFieldAccessNode(FieldAccessNode& node) {
        // 差异点：Store 的值由调用方压在栈顶；super.成员 不经此。
        const auto mode = take_lvalue_mode();
        const u32  line = node.line();
        if (try_emit_this_field(node, mode, line)) {
            return;
        }

        // 一般对象/嵌套捕获 this 走经栈路径。
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        switch (mode) {
            case LvalueMode::Prepare:
                // Prepare：只发接收者（垫在值下方，供 Store 使用），不读值
                emit_expr(*node.object); // [obj]
                return;
            case LvalueMode::Load:
                emit_expr(*node.object); // [obj]
                cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [obj.x]
                return;
            case LvalueMode::Store:
                // 接收者已由 Prepare 压在值下方，只发 store 指令
                cur_cu()->emit_op(OpCode::STORE_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [obj, v] -> [v]
                return;
            case LvalueMode::Locate:
                // DUP 副本供写入步复用，定位只求值一次。
                emit_expr(*node.object);              // [obj]
                cur_cu()->emit_op(OpCode::DUP, line); // [obj, obj]
                cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [obj, obj.x]
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::visitIndexAccessNode(IndexAccessNode& node) {
        // Locate 用 DUP2 复制 (obj, idx) 对交写入步复用，定位只求值一次。
        const auto mode = take_lvalue_mode();
        const u32  line = node.line();
        switch (mode) {
            case LvalueMode::Prepare:
                emit_expr(*node.object); // [obj]
                emit_expr(*node.index);  // [obj, idx]
                return;
            case LvalueMode::Load:
                emit_expr(*node.object);
                emit_expr(*node.index);                      // [obj, idx]
                cur_cu()->emit_op(OpCode::LOAD_INDEX, line); // [obj[idx]]
                return;
            case LvalueMode::Store:
                // obj/idx 已由 Prepare 压在值下方,只发 store 指令
                cur_cu()->emit_op(OpCode::STORE_INDEX, line); // [obj, idx, v] -> [v]
                return;
            case LvalueMode::Locate:
                emit_expr(*node.object);
                emit_expr(*node.index);                      // [obj, idx]
                cur_cu()->emit_op(OpCode::DUP2, line);       // [obj, idx, obj, idx]
                cur_cu()->emit_op(OpCode::LOAD_INDEX, line); // [obj, idx, obj[idx]]
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::visitListExprNode(ListExprNode& node) {
        const u32 line = node.line();
        // 元素数上限（MAKE_LIST 操作数 u16），先检后发。
        if (node.elements.size() > kMaxListElements) {
            fail(ErrorCode::TooManyElements, node.loc(), "too many list elements (max {})", kMaxListElements);
        }
        for (const auto& element: node.elements) {
            emit_expr(*element);
        }
        cur_cu()->emit_op(OpCode::MAKE_LIST, line);
        cur_cu()->emit_word(node.elements.size(), line); // [v1..vn] -> [list]
    }

    void CodeGen::visitMapExprNode(MapExprNode& node) {
        const u32 line = node.line();
        // 条目对数上限（MAKE_MAP 操作数 u16），先检后发。
        if (node.entries.size() > kMaxMapEntries) {
            fail(ErrorCode::TooManyElements, node.loc(), "too many map entries (max {})", kMaxMapEntries);
        }
        for (const auto& [key, value]: node.entries) {
            emit_expr(*key);
            emit_expr(*value); // 键值交替下栈,成 MAKE_MAP 的 [k1,v1..kn,vn] 栈形
        }
        cur_cu()->emit_op(OpCode::MAKE_MAP, line);
        cur_cu()->emit_word(node.entries.size(), line); // [k1,v1..kn,vn] -> [map]
    }

    void CodeGen::visitRangeExprNode(RangeExprNode& node) {
        const u32 line = node.line();
        emit_expr(*node.lower);
        if (node.upper == nullptr) {
            // 无上界：仅编 unbounded 位（含否对无上界无意义）。[from] -> [range]
            cur_cu()->emit_op(OpCode::MAKE_RANGE, line);
            cur_cu()->emit_byte(kRangeFlagUnbounded, line);
            return;
        }
        // [from, to] -> [range]
        emit_expr(*node.upper); // 端点左→右下栈,成 MAKE_RANGE 的 [from, to] 栈形
        cur_cu()->emit_op(OpCode::MAKE_RANGE, line);
        cur_cu()->emit_byte(node.is_exclusive ? kRangeFlagExclusive : kRangeFlagInclusive, line);
    }

    void CodeGen::visitIfExprNode(IfExprNode& node) {
        const u32 line = node.line();
        const u32 jf   = emit_cond_jump(*node.condition, line); // -> else
        emit_expr(*node.then_branch);
        const u32 jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
        // -> else
        patch_jump_or_fail(jf, node.loc());
        emit_expr(*node.else_branch);
        patch_jump_or_fail(jend, node.loc()); // -> end
    }

    void CodeGen::visitLambdaExprNode(LambdaExprNode& node) {
        // 名字恒为 kAnonymousName（"<>" 不可作标识符）。
        compile_function(kAnonymousName, node.params, *node.body, node.loc(), FnKind::Lambda);
    }

    void CodeGen::visitMatchExprNode(MatchExprNode& node) { emit_match(node); }

    void CodeGen::visitSequenceExprNode(SequenceExprNode& node) {
        // 非末位求值后 POP 弃，末位留栈即序列值；单元素已由 parser 透明化，size >= 2 是入参不变式。
        ASSERT(node.expressions.size() >= 2, "sequence node requires two or more expressions");
        for (usize index = 0; index + 1 < node.expressions.size(); ++index) {
            emit_expr(*node.expressions[index]);
            cur_cu()->emit_op(OpCode::POP, node.expressions[index]->line());
        }
        emit_expr(*node.expressions.back());
    }

    void CodeGen::visitIdentifierPatternNode(IdentifierPatternNode& node) {
        // 栈顶值即待绑值（identifier 位与 listPattern rest 位都经此）。Fill 绑新名；Store 写既有名，
        // STORE_* 是 peek-store 故补 POP 净耗栈顶一值。
        switch (pattern_mode_) {
            case PatternBindMode::Fill:
                bind_stack_value(node.name, node.loc());
                return;
            case PatternBindMode::Store: {
                const u32  line     = node.line();
                const auto resolved = resolve_name_or_fail(node.name, node.loc());
                emit_store_var(resolved, line);
                cur_cu()->emit_op(OpCode::POP, line);
                return;
            }
        }
        UNREACHABLE();
    }

    void CodeGen::visitWildcardPatternNode(WildcardPatternNode& node) {
        // `_` 忽略该值即弹栈；列表位置位由父层按 is_wildcard_pattern 跳过不达此处，Fill 只在根位到达。
        cur_cu()->emit_op(OpCode::POP, node.line());
    }

    void CodeGen::visitListPatternNode(ListPatternNode& node) {
        // 逐位置下标访问（多余忽略、不足越界报错），rest 取后缀。
        const u32 line = node.line();

        switch (pattern_mode_) {
            case PatternBindMode::Fill: {
                // 都不偏离「栈高 == 局部数」不变式：0 访问源值即废弹出（副作用照跑），1 访问源值即
                // 消耗品（取出的元素恰落源槽位），>=2 源值填隐藏局部逐位置复取。
                const usize access_count = pattern_access_count(node);
                if (access_count == 0) {
                    cur_cu()->emit_op(OpCode::POP, line);
                    return;
                }
                if (access_count == 1) {
                    emit_list_pattern_accesses(node, line, [] {});
                    return;
                }
                // 隐藏局部名带槽号：同 scope 局部只增必不同槽，天然唯一。
                const auto source_name = std::format("<destructure_{}>", cur_fn_ctx()->locals_.size());
                const u16  source_slot = define_local_or_fail(source_name, node.loc());
                const auto push_source = [this, source_slot, line] { cur_cu()->emit_load_local(source_slot, line); };
                emit_list_pattern_accesses(node, line, push_source);
                return;
            }
            case PatternBindMode::Store: {
                // 既有名按名写，源值恒驻栈顶：每次访问前 DUP 供取元素，取出值交子节点写目标；
                // 本层收尾 POP 弹自己的源值即净耗一值（右值副本由调用点持有）。
                emit_list_pattern_accesses(node, line, [this, line] { cur_cu()->emit_op(OpCode::DUP, line); });
                cur_cu()->emit_op(OpCode::POP, line); // 弹本层源值
                return;
            }
        }
        UNREACHABLE();
    }

} // namespace aria
