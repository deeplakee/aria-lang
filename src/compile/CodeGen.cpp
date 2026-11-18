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
#include "object/ObjFnProto.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/value_register.hpp"
#include "util/str.hpp"
#include "util/util.hpp"

#include <format>

namespace aria {

    namespace {
        constexpr u32 kMaxArity          = kU8OperandMax;
        constexpr u32 kMaxArguments      = kU8OperandMax;
        constexpr u32 kMaxConstants      = kU16OperandMax;
        constexpr u32 kMaxListElements   = kU16OperandMax;
        constexpr u32 kMaxMapEntries     = kU16OperandMax;
        constexpr u32 kMaxInterpSegments = kU8OperandMax;
        constexpr u32 kMaxLocals         = kU16OperandMax;

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
                case Op::Binary::BitAnd:
                    return OpCode::BIT_AND;
                case Op::Binary::BitOr:
                    return OpCode::BIT_OR;
                case Op::Binary::BitXor:
                    return OpCode::BIT_XOR;
                case Op::Binary::Shl:
                    return OpCode::SHIFT_LEFT;
                case Op::Binary::Shr:
                    return OpCode::SHIFT_RIGHT;
                case Op::Binary::Ushr:
                    return OpCode::SHIFT_RIGHT_LOGICAL;
                default:
                    UNREACHABLE();
            }
        }

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
                case Op::Assignment::BitAndAssign:
                    return Op::Binary::BitAnd;
                case Op::Assignment::BitOrAssign:
                    return Op::Binary::BitOr;
                case Op::Assignment::BitXorAssign:
                    return Op::Binary::BitXor;
                case Op::Assignment::ShlAssign:
                    return Op::Binary::Shl;
                case Op::Assignment::ShrAssign:
                    return Op::Binary::Shr;
                case Op::Assignment::UshrAssign:
                    return Op::Binary::Ushr;
                default:
                    UNREACHABLE();
            }
        }

        u8 min_arity(const Span<Param> params) noexcept {
            u8 count = 0;
            for (const auto& param: params) {
                if (param.default_val != nullptr || param.is_varargs) {
                    break;
                }
                ++count;
            }
            return count;
        }

        // 通配符不访问源元素，避免无用读取引发越界或缺键错误。
        [[nodiscard]] bool is_wildcard_pattern(const PatternNode& pattern) noexcept {
            return dynamic_cast<const WildcardPatternNode*>(&pattern) != nullptr;
        }

        [[nodiscard]] usize pattern_access_count(const ListPatternNode& pattern) noexcept {
            usize count = 0;
            for (const auto& element: pattern.elems) {
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

    Result<ObjFnProto*, Error> CodeGen::compile(GC& gc, const ProgramNode& program, ObjModule* module,
                                                const StringView entry_name) {
        CodeGen self{gc};
        return self.generate_bytecode(program, module, entry_name);
    }

    Result<ObjFnProto*, Error> CodeGen::generate_bytecode(const ProgramNode& program, ObjModule* module,
                                                          const StringView entry_name) {
        const auto guard = gc_.make_guard(module);

        const auto entry = init_module(module, entry_name);

        try {
            for (const auto& decl: program.decls) {
                emit_stmt(*decl);
            }
            emit_implicit_return(program.loc());
        } catch (AriaCompileException& e) {
            // 出错时 ModuleCtx 沿上下文链释放尚未还原的子上下文。
            return std::unexpected(e.error());
        }

#ifdef DEBUG_PRINT_COMPILED_CODE
        io::println(stderr, "{}", cur_cu()->disassemble(entry_name));
#endif

        return entry;
    }

    ObjFnProto* CodeGen::init_module(ObjModule* module, const StringView entry_name) {
        const auto entry = new_fn_proto(gc_, module, entry_name, 0, 0, false);
        module->set_entry(entry);
        mod_ctx_ = std::make_unique<ModuleCtx>(module);
        return entry;
    }

    FunctionCtx* CodeGen::cur_fn_ctx() const noexcept { return mod_ctx_->current_fn_ctx_; }

    CodeUnit* CodeGen::cur_cu() const noexcept { return &cur_fn_ctx()->proto_->unit(); }

    u16 CodeGen::add_constant_or_fail(const Value value, const SourceLoc loc) const {
        if (cur_cu()->constants.size() > kMaxConstants) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "too many constants (max {})", kMaxConstants);
        }
        return cur_fn_ctx()->add_constant(value);
    }

    u16 CodeGen::add_name_or_fail(const StringView name, const SourceLoc loc) const {
        // 入池不触 GC，驻留串可直接发布到常量池。
        const auto str = new_string(gc_, name);
        return add_constant_or_fail(Value::from_obj(str), loc);
    }

    void CodeGen::begin_scope() const { cur_fn_ctx()->begin_scope(); }

    void CodeGen::end_scope(const u32 line) const {
        emit_pop_locals_to(cur_fn_ctx()->scope_depth_ - 1, line);
        cur_fn_ctx()->end_scope();
    }

    void CodeGen::emit_pop_locals_to(const u32 target_depth, const u32 line) const {
        // 关闭 upvalue 不弹栈，清理与 POP_N 配对。
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
        // nullopt 仅表示无捕获；容量越界必须报错，否则引用会误解析为全局。
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
        for (usize index = 0; index < node.elems.size(); ++index) {
            if (is_wildcard_pattern(*node.elems[index])) {
                continue; // `_` 不访问该位置
            }
            push_source();
            emit_int_literal(static_cast<i64>(index), line, node.loc());
            cur_cu()->emit_op(OpCode::LOAD_INDEX, line);
            node.elems[index]->accept(*this); // 递归绑定（模式不变）
        }
        if (node.rest) {
            push_source();
            emit_int_literal(static_cast<i64>(node.elems.size()), line, node.loc());
            cur_cu()->emit_op(OpCode::MAKE_RANGE, line);
            cur_cu()->emit_byte(kRangeFlagUnbounded, line); // [src, range]
            cur_cu()->emit_op(OpCode::LOAD_INDEX, line);    // [suffix]
            node.rest->accept(*this);
        }
    }

    void CodeGen::emit_expr(ExprNode& node) {
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

    void CodeGen::validate_params(const Span<Param> params, const SourceLoc loc) const {
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
                const auto member_op  = kind == FnKind::StaticMethod ? OpCode::MAKE_STATIC : OpCode::MAKE_METHOD;
                const auto member_idx = add_name_or_fail(name, loc);
                cur_cu()->emit_op(member_op, line);
                cur_cu()->emit_word(member_idx, line); // [class]
                break;
            }
            case FnKind::ModuleEntry:
                UNREACHABLE();
        }
    }

    void CodeGen::bind_class_value(const bool is_member, const StringView name, const SourceLoc loc) const {
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

    void CodeGen::compile_params(const Span<Param> params, const SourceLoc loc) {
        const u32 line = loc.line();
        for (usize i = 0; i < params.size(); ++i) {
            const auto& param = params[i];
            if (param.default_val != nullptr) {
                const u16 slot = i + 1; // 参数槽 1..n(槽 0 = this/哑元)
                cur_cu()->emit_load_local(slot, line);
                cur_cu()->emit_op(OpCode::LOAD_REG, line);
                cur_cu()->emit_byte(kDefaultMarkOffset, line);
                const u32 skip = cur_cu()->emit_jump(OpCode::JUMP_NE, line);
                emit_expr(*param.default_val);
                cur_cu()->emit_store_local(slot, line); // peek-store 换入参数槽
                cur_cu()->emit_op(OpCode::POP, line);   // STORE_LOCAL 不弹,弹掉求值副本恢复「栈高 == 已填槽数」
                patch_jump_or_fail(skip, loc);
            }
            cur_fn_ctx()->add_local(param.name);
        }
    }

    void CodeGen::emit_implicit_return(const SourceLoc loc) const {
        const u32 line = loc.line();
        switch (cur_fn_ctx()->kind_) {
            case FnKind::ModuleEntry: {
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

    void CodeGen::compile_function(const StringView name, const Span<Param> params, BlockNode& body,
                                   const SourceLoc decl_loc, const FnKind kind) {
        validate_params(params, decl_loc);

        const bool has_varargs = !params.empty() && params.back().is_varargs;
        const auto fixed_arity = static_cast<u8>(params.size() - (has_varargs ? 1 : 0));
        const auto proto = new_fn_proto(gc_, mod_ctx_->module_, name, fixed_arity, min_arity(params), has_varargs);
        const auto proto_index = add_constant_or_fail(Value::from_obj(proto), decl_loc);
        cur_cu()->emit_op(OpCode::CLOSURE, decl_loc.line());
        cur_cu()->emit_word(proto_index, decl_loc.line());

        bind_function_value(kind, name, decl_loc);

        const auto child          = new FunctionCtx{proto, cur_fn_ctx(), kind};
        mod_ctx_->current_fn_ctx_ = child;

        compile_params(params, decl_loc);

        emit_stmt(body);
        emit_implicit_return(body.loc());

        proto->upvalue_descs().copy_from(child->upvalues_);

#ifdef DEBUG_PRINT_COMPILED_CODE
        io::println(stderr, "{}", cur_cu()->disassemble(name));
#endif

        mod_ctx_->current_fn_ctx_ = child->enclosing_;
        delete child;
    }

    void CodeGen::visitProgramNode(ProgramNode& node) {
        for (const auto& decl: node.decls) {
            emit_stmt(*decl);
        }
    }

    void CodeGen::visitBlockNode(BlockNode& node) {
        const u32 line = node.line();
        begin_scope();
        for (const auto& stmt: node.stmts) {
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
        const u32 jf   = emit_cond_jump(*node.cond, line); // -> else / end
        emit_stmt(*node.then_br);
        if (node.else_br != nullptr) {
            const u32 jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
            patch_jump_or_fail(jf, node.loc());                       // -> else
            emit_stmt(*node.else_br);
            patch_jump_or_fail(jend, node.loc()); // -> end
        } else {
            patch_jump_or_fail(jf, node.loc()); // -> end
        }
    }

    void CodeGen::visitWhileStmtNode(WhileStmtNode& node) {
        const u32 line     = node.line();
        auto      loop_ctx = LoopCtx{.loop_scope_depth = cur_fn_ctx()->scope_depth_,
                                     .back_target      = cur_cu()->size()}; // 循环头 = 条件起点 = continue 后向目标
        const u32 patch    = emit_cond_jump(*node.cond, line);              // -> L_end 占位
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
        const bool has_cond = node.cond != nullptr;
        const bool has_incr = node.incr != nullptr;
        auto       loop_ctx = LoopCtx{.loop_scope_depth = loop_scope, .back_target = cur_cu()->size()};
        if (has_incr) {
            loop_ctx.continue_fwd_patches.emplace(); // 打开前向 continue 通道
        }
        if (has_cond) {
            const u32 patch = emit_cond_jump(*node.cond, line); // -> L_end 占位
            loop_ctx.exit_fwd_patches.push_back(patch);
        } // 无 cond: exit 列表空，收尾只有回边 + break 回填
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(*node.body);
        loop_ctx = util::pop_top(cur_fn_ctx()->loop_stack_);

        // continue 须在递增发射前回填，否则会跳到循环末尾。
        if (loop_ctx.continue_fwd_patches) {
            for (const u32 patch: *loop_ctx.continue_fwd_patches) {
                patch_jump_or_fail(patch, node.loc()); // -> L_incr
            }
        }
        if (has_incr) {
            emit_expr(*node.incr);
            cur_cu()->emit_op(OpCode::POP, line);
        }
        emit_loop_backedge_and_exits(loop_ctx, node.loc());
        end_scope(line);
    }

    void CodeGen::visitForInStmtNode(ForInStmtNode& node) {
        const u32 line = node.line();
        // 等价形式（lowering 蓝图）：
        //   {                                     // for-in scope（整循环存活）
        //     var <iter> = <iterable>.iter();       // <iter> 隐藏局部（"<iter>" 含 <> 不可作标识符，不撞用户名）
        //     while (<iter>.has_next()) {          // L_start = has_next 判断处
        //       {                                  // per-iteration scope（每轮 fresh）
        //         var <pattern> = <iter>.next();   // bind_pattern：declare + 值填槽（_ -> POP 丢弃）
        //         <body>
        //       }
        //     }
        //   }
        // continue 跳回 L_start（has_next），无 increment 步；下一轮值在每轮体首调 next() 取。
        begin_scope(); // for-in scope：仅 <iter>，循环全程存活
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;

        emit_expr(*node.iterable);
        emit_method_call0("iter", node.loc());                                // [iter_obj] 恰在 slot 位置
        const u16 iter_var_slot = define_local_or_fail("<iter>", node.loc()); // 值已在槽位，登记即初始化

        auto loop_ctx = LoopCtx{.loop_scope_depth = loop_scope, .back_target = cur_cu()->size()};
        cur_cu()->emit_load_local(iter_var_slot, line);
        emit_method_call0("has_next", node.loc());
        const u32 patch = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end 占位
        loop_ctx.exit_fwd_patches.push_back(patch);
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));

        begin_scope();
        cur_cu()->emit_load_local(iter_var_slot, line); // [iter]（receiver）
        emit_method_call0("next", node.loc());          // [value] 恰在 slot 位置
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
        if (cur_fn_ctx()->kind_ == FnKind::ModuleEntry) {
            if (node.value != nullptr) {
                fail(ErrorCode::ReturnValueAtTopLevel, node.loc(), "'return' at top level must not carry a value");
            }
            emit_implicit_return(node.loc());
            return;
        }
        const u32 line = node.line();
        emit_expr_or_nil(node.value, line);
        cur_cu()->emit_op(OpCode::RETURN, line);
    }

    void CodeGen::visitImportStmtNode(ImportStmtNode& node) {
        const u32  line     = node.line();
        const auto path_idx = add_string_literal_or_fail(*node.path);
        cur_cu()->emit_op(OpCode::IMPORT, line);
        cur_cu()->emit_word(path_idx, line);      // [module]
        bind_stack_value(node.alias, node.loc()); // [module] -> [] DEF_GLOBAL 弹值 / 值填槽
    }

    void CodeGen::visitTryStmtNode(TryStmtNode& node) {
        const u32 line = node.line();
        if (node.catch_body == nullptr) {
            fail(ErrorCode::TryWithoutHandler, node.loc(), "'try' requires a catch clause");
        }

        // 先插入外层 try 占位，再编译嵌套体，以保持记录按 begin 有序。
        // catch 参数由 unwind 直接填槽，两条路径须在块尾恢复相同栈深。
        const auto stack_depth = static_cast<u32>(cur_fn_ctx()->locals_.size()); // try 入口局部数(try scope 开前)
        const u32  begin       = cur_cu()->size();
        const auto rec_idx     = cur_cu()->try_records.size();
        cur_cu()->try_records.push(TryRecord{begin, 0, 0, 0});
        begin_scope();
        emit_stmt(*node.body); // 嵌套 try 在此预插占位，begin > 本层故整体升序
        end_scope(line);
        const u32 end    = cur_cu()->size();
        const u32 jskip  = cur_cu()->emit_jump(OpCode::JUMP, line); // 正常路径跳过 catch -> L_end
        const u32 handle = cur_cu()->size();
        begin_scope();                                              // catch 子句 scope 含 e + 体，两路径栈平衡
        std::ignore = define_local_or_fail(node.ename, node.loc()); // e 由 unwind 的 push 运行期填槽(== stack_depth)
        emit_stmt(*node.catch_body);
        end_scope(line);
        patch_jump_or_fail(jskip, node.loc()); // -> L_end
        cur_cu()->try_records[rec_idx].end         = end;
        cur_cu()->try_records[rec_idx].handle      = handle;
        cur_cu()->try_records[rec_idx].stack_depth = stack_depth;
    }

    void CodeGen::visitThrowStmtNode(ThrowStmtNode& node) {
        const u32 line = node.line();
        emit_expr(*node.expr);
        cur_cu()->emit_op(OpCode::THROW, line); // [v] -> [](派发 handler 时值落 catch 参数槽)
    }

    template<typename Arm>
    void CodeGen::validate_match_arms(const Span<Arm> arms) const {
        for (usize i = 1; i < arms.size(); ++i) {
            if (arms[i - 1].pattern.value == nullptr) {
                fail(ErrorCode::UnreachableArm, arms[i].body->loc(), "unreachable arm after '_'");
            }
        }
    }

    template<typename Node>
    void CodeGen::emit_match(Node& node) {
        validate_match_arms(node.arms);
        const u32 line = node.line();
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

        // 无父类时直接加载 Object 寄存器，避免被同名全局遮蔽。
        if (node.super != nullptr) {
            emit_expr(*node.super); // [super]
        } else {
            cur_cu()->emit_op(OpCode::LOAD_REG, line); // [Object]
            cur_cu()->emit_byte(kObjectClassOffset, line);
        }

        const auto name_idx = add_name_or_fail(node.name, node.loc());
        cur_cu()->emit_op(OpCode::MAKE_CLASS, line);
        cur_cu()->emit_word(name_idx, line); // [class]

        bind_class_value(node.is_member, node.name, node.loc());

        for (const auto& member: node.members) {
            member->accept(*this);
        }

        if (node.is_member || mod_ctx_->is_global_scope()) {
            cur_cu()->emit_op(OpCode::POP, line); // 弹驻留类值
        }
    }

    void CodeGen::visitVarDeclNode(VarDeclNode& node) {
        for (const auto& [target, initializer]: node.bindings) {
            // 初始化器先求值再登记名字，同名引用因此解析到外层。
            emit_expr_or_nil(initializer, target->line());
            bind_pattern(*target, PatternBindMode::Fill);
        }
    }

    void CodeGen::visitStaticVarMemberNode(StaticVarMemberNode& node) {
        const u32 line = node.line();
        emit_expr_or_nil(node.initializer, line); // [class, v]
        const auto member_idx = add_name_or_fail(node.name, node.loc());
        cur_cu()->emit_op(OpCode::MAKE_STATIC, line);
        cur_cu()->emit_word(member_idx, line); // [class]
    }

    void CodeGen::validate_int_literal(const i64 value, const SourceLoc loc) const {
        if (value < kIntMin || value > kIntMax) {
            fail(ErrorCode::NumberOutOfRange, loc, "integer literal {} out of range", value);
        }
    }

    bool CodeGen::try_emit_load_imm(const i64 value, const u32 line) const {
        if (value < std::numeric_limits<i8>::min() || value > std::numeric_limits<i8>::max()) {
            return false;
        }
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
        // 仅折叠直接数值字面量的取负，并按带符号的字面量检查值域。
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
            return add_name_or_fail(node.value, node.loc());
        }
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
        if (node.parts.size() > kMaxInterpSegments) {
            fail(ErrorCode::TooManyElements, node.loc(), "too many interpolation segments (max {})",
                 kMaxInterpSegments);
        }
        for (const auto& part: node.parts) {
            emit_expr(*part);
        }
        cur_cu()->emit_op(OpCode::BUILD_STRING, line);
        cur_cu()->emit_byte(node.parts.size(), line);
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
        // 名字定位信息为编译期常量，无接收者需预备或保存。
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
        const auto resolved = resolve_this_or_fail(node.loc());
        emit_load_var(resolved, node.line());
    }

    void CodeGen::visitSuperExprNode(SuperExprNode& node) {
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
                if (try_emit_negated_literal(operand)) {
                    return;
                }
                emit_expr(operand);
                cur_cu()->emit_op(OpCode::NEGATE, line);
                return;
            case Op::Unary::Not:
            case Op::Unary::BitNot:
                emit_expr(operand);
                cur_cu()->emit_op(node.op == Op::Unary::Not ? OpCode::NOT : OpCode::BIT_NOT, line);
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
        const auto member = dynamic_cast<FieldAccessNode*>(node.callee);
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
        cur_cu()->emit_op(OpCode::CALL_METHOD, line); // 纯调用
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
        if (dynamic_cast<ThisExprNode*>(node.object) == nullptr || !is_in_method()) {
            return false;
        }
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        switch (mode) {
            case LvalueMode::Prepare:
                return true; // 本分支零指令
            case LvalueMode::Load:
            case LvalueMode::Locate:
                cur_cu()->emit_op(OpCode::LOAD_THIS_FIELD, line);
                cur_cu()->emit_word(name_idx, line);
                return true;
            case LvalueMode::Store:
                cur_cu()->emit_op(OpCode::STORE_THIS_FIELD, line);
                cur_cu()->emit_word(name_idx, line);
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
                cur_cu()->emit_word(name_idx, line);
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
                cur_cu()->emit_op(OpCode::STORE_INDEX, line);
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
        if (node.elems.size() > kMaxListElements) {
            fail(ErrorCode::TooManyElements, node.loc(), "too many list elements (max {})", kMaxListElements);
        }
        for (const auto& element: node.elems) {
            emit_expr(*element);
        }
        cur_cu()->emit_op(OpCode::MAKE_LIST, line);
        cur_cu()->emit_word(node.elems.size(), line);
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
        cur_cu()->emit_word(node.entries.size(), line);
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
        emit_expr(*node.upper); // 端点左→右下栈,成 MAKE_RANGE 的 [from, to] 栈形
        cur_cu()->emit_op(OpCode::MAKE_RANGE, line);
        cur_cu()->emit_byte(node.is_exclusive ? kRangeFlagExclusive : kRangeFlagInclusive, line);
    }

    void CodeGen::visitIfExprNode(IfExprNode& node) {
        const u32 line = node.line();
        const u32 jf   = emit_cond_jump(*node.cond, line); // -> else
        emit_expr(*node.then_br);
        const u32 jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
        // -> else
        patch_jump_or_fail(jf, node.loc());
        emit_expr(*node.else_br);
        patch_jump_or_fail(jend, node.loc()); // -> end
    }

    void CodeGen::visitLambdaExprNode(LambdaExprNode& node) {
        // 名字恒为 kAnonymousName（"<>" 不可作标识符）。
        compile_function(kAnonymousName, node.params, *node.body, node.loc(), FnKind::Lambda);
    }

    void CodeGen::visitMatchExprNode(MatchExprNode& node) { emit_match(node); }

    void CodeGen::visitSequenceExprNode(SequenceExprNode& node) {
        // 非末位求值后 POP 弃，末位留栈即序列值；单元素已由 parser 透明化，size >= 2 是入参不变式。
        ASSERT(node.exprs.size() >= 2, "sequence node requires two or more expressions");
        for (usize index = 0; index + 1 < node.exprs.size(); ++index) {
            emit_expr(*node.exprs[index]);
            cur_cu()->emit_op(OpCode::POP, node.exprs[index]->line());
        }
        emit_expr(*node.exprs.back());
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
                // 隐藏局部名自增序号，同 scope 只增必不同、天然唯一；宿主在 hidden_names_，视图
                // 存活覆盖编译期。
                const u16  source_slot = define_local_or_fail(hidden_names_.make_next(), node.loc());
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
