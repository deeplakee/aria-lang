#include "compile/CodeGen.hpp"

#include <memory>
#include <ranges>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

#include <format>

namespace aria {

    namespace {
        // 容量上限(值即操作数/索引位宽上限,事实源见 CodeUnit.hpp 的 kU8/kU16OperandMax;
        // 越界统一用 > 比较):kMaxArity 形参(u8)、kMaxArguments 实参(CALL 操作数 u8)、
        // kMaxConstants 常量池(u16 索引)、kMaxLocals 局部槽(u16,含 slot 0 哑元)。语义名
        // 集中定义使检查点与报错文案同源;kMaxUpvalues 同纪律,因登记侧共用而定义于
        // FunctionCtx.hpp。
        constexpr u32 kMaxArity     = kU8OperandMax;
        constexpr u32 kMaxArguments = kU8OperandMax;
        constexpr u32 kMaxConstants = kU16OperandMax;
        constexpr u32 kMaxLocals    = kU16OperandMax;

        // 整数字面量 i48 范围(Value::from_int 的 i48 尾部,与 NanBoxing.hpp 的 ASSERT 同源;
        // 超出 -> NumberOutOfRange)。
        constexpr i64 kIntMin = -(static_cast<i64>(1) << 47);
        constexpr i64 kIntMax = (static_cast<i64>(1) << 47) - 1;
    } // namespace

    // ============================================================
    // 入口
    // ============================================================

    Result<ObjFunction*, Error> CodeGen::compile(const ProgramNode& program, ObjModule* module,
                                                 const StringView entry_name) {
        // module 入临时根贯穿全程（根化链与各守卫窗口见类首「GC 安全」注）。
        const auto module_guard = gc_.make_guard(module);

        // 防御：lvalue_mode_ 复位为 Load（构造已置；此处防上一次 compile() throw 后残留跨复用）。
        lvalue_mode_ = LvalueMode::Load;

        const auto entry = init_module(module, entry_name);

        try {
            // 遍历顶层声明（顶层 var/fun/import -> 模块全局；嵌套块内 var -> 局部）。
            for (const auto& decl: program.declarations) {
                emit_stmt(*decl);
            }
            // 隐式 return nil（无显式 return 时的兜底；显式 return 后为死代码，无害）。
            const u32 line = program.loc_line();
            cur_cu()->emit_op(OpCode::LOAD_NIL, line);
            cur_cu()->emit_op(OpCode::RETURN, line);
        } catch (AriaCompileException& e) {
            // 出错即 unwind 到此：~ModuleCtx 沿 enclosing_ 链释放入口 + 出错未还原的子上下文。
            mod_ctx_.reset();
            return std::unexpected(e.error());
        }

#ifdef DEBUG_PRINT_COMPILED_CODE
        // 打印入口的 CodeUnit 反汇编（游标仍在入口，cur_cu() 即入口 unit）。
        io::println(stderr, "{}", cur_cu()->disassemble(entry_name));
#endif

        mod_ctx_.reset(); // 释放本模块上下文（含入口 fn 上下文）；游标随之失效但不再读
        return entry;
    }

    // 建模块入口函数 + set_entry + 构造 ModuleCtx（契约见 CodeGen.hpp init_module 注）。
    // 工厂不守入参，故入口名须显式 make_guard 跨 new_function 的 new_object。
    ObjFunction* CodeGen::init_module(ObjModule* module, const StringView entry_name) {
        const auto name  = new_string(gc_, entry_name);
        const auto guard = gc_.make_guard(name);
        const auto entry = new_function(gc_, module, name, 0);
        module->set_entry(entry);
        mod_ctx_ = std::make_unique<ModuleCtx>(module); // 创建入口 fn 上下文并就位游标
        return entry;
    }

    // 当前 CodeUnit = 当前函数 fn_->unit()，随游标派生（定义于此：需 ObjFunction 完整类型取 unit()）。
    FunctionCtx* CodeGen::cur_fn_ctx() const noexcept { return mod_ctx_->current_fn_ctx_; }

    CodeUnit* CodeGen::cur_cu() const noexcept { return &cur_fn_ctx()->fn_->unit(); }

    // ============================================================
    // 常量池辅助
    // ============================================================

    u16 CodeGen::add_constant_or_fail(const Value value, const SourceLoc loc) const {
        // 常量池溢出(>kMaxConstants) -> fail CodeUnitTooLarge（CodeUnit::add_constant 内部
        // ASSERT 兜底，本预检保证永不触达）。
        if (cur_cu()->constants.size() > kMaxConstants) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "常量池溢出(>{})", kMaxConstants);
        }
        return cur_cu()->add_constant(value);
    }

    u16 CodeGen::add_name_or_fail(const StringView name, const SourceLoc loc) const {
        // intern name 入常量池返回索引；new_string 结果立即 add_constant（trivial push 不触发 GC，
        // 见类首 GC 安全注），无需守卫。溢出由 add_constant_or_fail fail。
        const auto str = new_string(gc_, name);
        return add_constant_or_fail(Value::from_obj(str), loc);
    }

    // ============================================================
    // 局部管理（登记经 FunctionCtx，发射经 cur_cu()）
    // ============================================================

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

    u16 CodeGen::declare_local_or_fail(const StringView name, const SourceLoc loc) const {
        // 同作用域重名 -> RedefinedVariable（外层同名允许 shadow）；溢出 -> TooManyLocals。
        // 仅登记不发指令；mark_initialized 时机见 CodeGen.hpp declare_local_or_fail 注。
        if (cur_fn_ctx()->is_defined_in_scope(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "重复定义局部变量: {}", name);
        }
        if (cur_fn_ctx()->locals_.size() > kMaxLocals) {
            fail(ErrorCode::TooManyLocals, loc, "局部变量过多(>{})", kMaxLocals);
        }
        return cur_fn_ctx()->add_local(name); // 纯登记，is_initialized 默认 false
    }

    // ============================================================
    // 名字解析
    // ============================================================

    CodeGen::ResolvedVar CodeGen::resolve_name_or_fail(const StringView name, const SourceLoc loc) {
        // 解析序「局部 -> upvalue -> 全局」，契约见 CodeGen.hpp resolve_name_or_fail 注。
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
        // 算法契约见 CodeGen.hpp。add_upvalue_or_fail 把容量越界翻译为 fail -- nullopt 只表
        // 「无外层可捕获 -> 落全局」,不外泄越界信号(否则捕获引用静默串台全局)。
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

    u8 CodeGen::add_upvalue_or_fail(FunctionCtx* ctx, const UpvalueDesc desc, const SourceLoc loc) const {
        if (const auto idx = ctx->add_upvalue(desc)) {
            return *idx;
        }
        fail(ErrorCode::TooManyUpvalues, loc, "闭包捕获变量过多(>{})", kMaxUpvalues);
    }

    // ============================================================
    // 跳转回填 / 全局登记失败翻译
    // ============================================================

    void CodeGen::patch_jump_or_fail(const usize src_off, const SourceLoc loc) const {
        if (!cur_cu()->patch_jump(src_off)) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "跳转偏移超过 64KB");
        }
    }

    void CodeGen::emit_jump_back_or_fail(const u32 target_off, const u32 line, const SourceLoc loc) const {
        if (!cur_cu()->emit_jump_back(target_off, line)) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "回边偏移超过 64KB");
        }
    }

    void CodeGen::declare_global_or_fail(const StringView name, const SourceLoc loc) const {
        if (!mod_ctx_->declare_global(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "重复定义全局变量: {}", name);
        }
    }

    // ============================================================
    // lvalue / 局部槽 load-store
    // ============================================================

    void CodeGen::check_local_initialized(const u16 slot, const SourceLoc loc) const {
        // 读点 init 检查：使用定义但未初始化的局部 -> UninitializedVariable（Python 风格 definite-assignment）。
        if (!cur_fn_ctx()->is_initialized(slot)) {
            fail(ErrorCode::UninitializedVariable, loc, "使用未初始化的变量: {}", cur_fn_ctx()->locals_[slot].name);
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
        fail(ErrorCode::InvalidAssignmentTarget, target.loc(), "非法赋值左值");
    }

    void CodeGen::emit_lvalue(ExprNode& node, const LvalueMode mode) {
        // 验证左值种类后设置模式并分派；清空职责在 take_lvalue_mode（漏 take 由 emit_expr ASSERT 捕获）。
        validate_lvalue_target(node);
        lvalue_mode_ = mode;
        node.accept(*this);
    }

    CodeGen::LvalueMode CodeGen::take_lvalue_mode() { return std::exchange(lvalue_mode_, LvalueMode::Load); }

    void CodeGen::emit_method_call0(const StringView name, const u32 line, const SourceLoc loc) const {
        cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
        const auto name_idx = add_name_or_fail(name, loc);
        cur_cu()->emit_word(name_idx, line);
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(0, line);
    }

    void CodeGen::emit_load_var(const ResolvedVar& var, const u32 line, const SourceLoc loc) const {
        // 契约见 CodeGen.hpp emit_load_var 注。
        switch (const auto [kind, slot] = var; kind) {
            case ResolvedVar::Kind::Local:
                check_local_initialized(slot, loc);
                cur_cu()->emit_load_local(slot, line);
                return;
            case ResolvedVar::Kind::Global:
                cur_cu()->emit_op(OpCode::LOAD_GLOBAL, line);
                cur_cu()->emit_word(slot, line);
                return;
            case ResolvedVar::Kind::Upvalue:
                cur_cu()->emit_op(OpCode::LOAD_UPVALUE, line);
                cur_cu()->emit_byte(static_cast<u8>(slot), line); // 索引域由 add_upvalue 容量检查保证 <= u8
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::emit_store_var(const ResolvedVar& var, const u32 line, const SourceLoc loc) const {
        // 契约见 CodeGen.hpp emit_store_var 注。
        switch (const auto [kind, slot] = var; kind) {
            case ResolvedVar::Kind::Local:
                cur_cu()->emit_store_local(slot, line);
                cur_fn_ctx()->mark_initialized(slot);
                return;
            case ResolvedVar::Kind::Global:
                cur_cu()->emit_op(OpCode::STORE_GLOBAL, line);
                cur_cu()->emit_word(slot, line);
                return;
            case ResolvedVar::Kind::Upvalue:
                cur_cu()->emit_op(OpCode::STORE_UPVALUE, line);
                cur_cu()->emit_byte(static_cast<u8>(slot), line); // 索引域由 add_upvalue 容量检查保证 <= u8
                return;
        }
        UNREACHABLE();
    }

    // ============================================================
    // 模式绑定
    // ============================================================

    void CodeGen::bind_pattern(PatternNode& node) {
        // 契约与值填槽模型见 CodeGen.hpp bind_pattern 注。
        if (const auto id = dynamic_cast<IdentifierPatternNode*>(&node)) {
            // 纯登记，slot = 值位置；值填槽不发指令
            const auto slot = declare_local_or_fail(id->name, node.loc());
            cur_fn_ctx()->mark_initialized(slot); // 值已在槽
            return;
        }
        const u32 line = node.loc_line();
        if (dynamic_cast<WildcardPatternNode*>(&node) != nullptr) {
            cur_cu()->emit_op(OpCode::POP, line);
            return;
        }
        if (dynamic_cast<ListPatternNode*>(&node) != nullptr) {
            not_impl(node, "列表模式解构");
        }
        not_impl(node, "未知模式");
    }

    // ============================================================
    // 遍历入口
    // ============================================================

    void CodeGen::emit_expr(ExprNode& node) {
        // rvalue 上下文恒 Load：emit_lvalue 分派后必恢复为 Load。断言（非预防性赋值）以在开发期捕获漏恢复。
        ASSERT(lvalue_mode_ == LvalueMode::Load,
               "lvalue_mode_ 应为 Load（rvalue 上下文）；非 Load 表明 emit_lvalue 分派后未恢复");
        node.accept(*this);
    }

    void CodeGen::emit_stmt(StmtNode& node) { node.accept(*this); }

    // ============================================================
    // 函数编译（FunDecl / Lambda 共用）
    // ============================================================

    void CodeGen::validate_params(const List<Param>& params, const SourceLoc loc) const {
        // 契约见 CodeGen.hpp validate_params 注；此处只读 params，不触碰编译器状态。
        if (params.size() > kMaxArity) {
            fail(ErrorCode::TooManyParameters, loc, "形参过多(>{})", kMaxArity);
        }

        for (const auto& param: params) {
            if (param.is_varargs || param.default_value != nullptr) {
                not_impl(loc, "默认参数 / varargs");
            }
        }

        for (usize i = 0; i < params.size(); ++i) {
            for (usize j = i + 1; j < params.size(); ++j) {
                if (params[i].name == params[j].name) {
                    fail(ErrorCode::DuplicateParam, loc, "形参重名: {}", params[i].name);
                }
            }
        }
    }

    void CodeGen::compile_function(const StringView name, const List<Param>& params, BlockNode& body,
                                   SourceLoc decl_loc) {
        // 参数合法性检查先于 new_function 等分配：失败即抛 AriaCompileException，跳过下方所有发射与分配。
        validate_params(params, decl_loc);

        const auto loc  = body.loc();
        const u32  line = body.loc_line();

        // name_str 在下方 new_function 调用中可能被回收,故 make_guard 保护
        const auto name_str   = new_string(gc_, name);
        auto       name_guard = gc_.make_guard(name_str);
        const auto fn         = new_function(gc_, mod_ctx_->module_, name_str, static_cast<u8>(params.size()));
        // 入池后即经 module 根链可达（trivial 窗口见类首 GC 安全注）。
        const auto fn_idx = add_constant_or_fail(Value::from_obj(fn), loc);
        // CLOSURE fn_idx:VM 执行时现场包 ObjClosure,按捕获描述表(下方 flush 进
        // fn->upvalue_descs_)逐个建/复用 upvalue(表在元数据不进字节码流,CLOSURE 定长 3B)。
        cur_cu()->emit_op(OpCode::CLOSURE, line);
        cur_cu()->emit_word(fn_idx, line);

        // lambda(name == `<anonymous>`)留栈作表达式值不绑定，故可以跳过;具名 fun 绑定全局/局部。
        if (name != kAnonymousName) {
            if (mod_ctx_->is_global_scope()) {
                declare_global_or_fail(name, loc);
                const auto name_idx = add_name_or_fail(name, loc);
                cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
                cur_cu()->emit_word(name_idx, line);
            } else {
                // 嵌套 fun -> 局部(值填槽:fn 已压在 slot 位置,declare 登记该 slot 即该局部,无 store/pop)
                const auto slot = declare_local_or_fail(name, loc);
                cur_fn_ctx()->mark_initialized(slot);
            }
        }

        // 切到子函数上下文并摆动游标:cu 由游标派生,随游标自动切到子 unit,无需 save/restore。
        // new 分配(非 UPtr),enclosing_ 回父(父编译期长于子,裸指针稳定)。
        const auto child          = new FunctionCtx{*cur_fn_ctx(), fn};
        mod_ctx_->current_fn_ctx_ = child;
        for (const auto& param: params) {
            // 形参即函数前 n 个局部变量(slot 1..n);重名已在上方检查,故直接 add_local 无需再查。
            const auto slot = cur_fn_ctx()->add_local(param.name);
            cur_fn_ctx()->mark_initialized(slot);
        }

        // 编译体（BlockNode 自带 scope）。
        emit_stmt(body);

        // emit_stmt 抛异常时 unwind 跳过下方还原,子留在 enclosing_ 链上交 ~ModuleCtx 沿链释放。

        // 隐式 return nil(兜底;显式 return 后为死代码,无害)。
        cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        cur_cu()->emit_op(OpCode::RETURN, line);

        // 体编译完成,把子上下文登记的捕获描述 flush 进 fn 元数据(发射 CLOSURE 先于 flush 不碍事:
        // 描述表在 ObjFunction 上、不在字节码流,VM 执行 CLOSURE 时才读)。
        fn->upvalue_descs().copy_from(child->upvalues_);

#ifdef DEBUG_PRINT_COMPILED_CODE
        // 打印刚编译完成函数的 CodeUnit 反汇编（游标仍在子，cur_cu() 即子 unit；name 为本函数名）。
        io::println(stderr, "{}", cur_cu()->disassemble(name));
#endif

        // 成功:还原父游标(cu 自动回父 unit)并 delete 子上下文。
        mod_ctx_->current_fn_ctx_ = child->enclosing_;
        delete child;
    }

    // ============================================================
    // not_impl
    // ============================================================

    void CodeGen::not_impl(const ASTNode& node, StringView feature) const {
        fail(ErrorCode::NotImplemented, node.loc(), "{} 尚未支持", feature);
    }

    void CodeGen::not_impl(SourceLoc loc, StringView feature) const {
        // loc 直接传入（调用方仅有 SourceLoc 而无节点时用，如 validate_params）。与 ASTNode& 重载同一消息格式。
        fail(ErrorCode::NotImplemented, loc, "{} 尚未支持", feature);
    }

    // ============================================================
    // 根节点
    // ============================================================

    void CodeGen::visitProgramNode(ProgramNode& node) {
        // 仅编排顶层声明，本节点不直接发射（行号由各子节点自持）。
        for (const auto& decl: node.declarations) {
            emit_stmt(*decl);
        }
    }

    // ============================================================
    // 语句节点
    // ============================================================

    void CodeGen::visitBlockNode(BlockNode& node) {
        const u32 line = node.loc_line();
        begin_scope();
        for (const auto& stmt: node.statements) {
            emit_stmt(*stmt);
        }
        end_scope(line);
    }

    void CodeGen::visitExprStmtNode(ExprStmtNode& node) {
        const u32 line = node.loc_line();
        emit_expr(*node.expr);
        cur_cu()->emit_op(OpCode::POP, line);
    }

    void CodeGen::visitPrintStmtNode(PrintStmtNode& node) {
        const u32 line = node.loc_line();
        emit_expr(*node.expr);
        cur_cu()->emit_op(OpCode::PRINT, line);
    }

    void CodeGen::visitIfStmtNode(IfStmtNode& node) {
        const u32 line = node.loc_line();
        emit_expr(*node.condition);
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> else / end
        emit_stmt(*node.then_branch);
        if (node.else_branch != nullptr) {
            const auto jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
            // -> else
            patch_jump_or_fail(jf, node.loc());
            emit_stmt(*node.else_branch);
            patch_jump_or_fail(jend, node.loc()); // -> end
        } else {
            patch_jump_or_fail(jf, node.loc()); // -> end
        }
    }

    void CodeGen::visitWhileStmtNode(WhileStmtNode& node) {
        const u32 line    = node.loc_line();
        const u32 l_start = cur_cu()->size();
        emit_expr(*node.condition);
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

        auto loop_ctx                 = make_loop_ctx(cur_fn_ctx()->scope_depth_);
        loop_ctx.continue_back_target = l_start; // continue 后向跳 L_start
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(*node.body);
        const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

        emit_jump_back_or_fail(l_start, line, node.loc());
        patch_jump_or_fail(jf, node.loc()); // -> L_end
        for (const auto bp: loop.break_fwd_patches) {
            patch_jump_or_fail(bp, node.loc());
        }
    }

    void CodeGen::visitForStmtNode(ForStmtNode& node) {
        const u32 line = node.loc_line();
        begin_scope();
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;
        if (node.init != nullptr) {
            emit_stmt(*node.init);
        }
        const u32  l_cond   = cur_cu()->size();
        const bool has_cond = node.condition != nullptr;
        const bool has_incr = node.increment != nullptr;
        Opt<usize> jf; // 条件假跳 L_end 占位；无 cond 时留空（永不回填）
        if (has_cond) {
            emit_expr(*node.condition);
            jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end
        }
        // continue: 有 incr -> 前向跳 L_incr（回填）；无 incr -> 后向跳 L_cond。
        auto loop_ctx = make_loop_ctx(loop_scope);
        if (!has_incr) {
            loop_ctx.continue_back_target = l_cond; // 无 incr: continue 后向跳 L_cond
        } // 有 incr: 留空，走前向 continue_fwd_patches -> L_incr
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(*node.body);
        const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

        // continue（前向）须回填到 L_incr：此刻 cur_cu()->size() 即递增区起点，且须先于递增发射--
        // 若等递增与 JUMP_BACK 发完再回填，cur_cu()->size() 已是 L_end，continue 会错跳到 L_end 提前出循环。
        for (const auto patch: loop.continue_fwd_patches) {
            patch_jump_or_fail(patch, node.loc()); // -> L_incr
        }
        if (has_incr) {
            emit_expr(*node.increment);
            cur_cu()->emit_op(OpCode::POP, line);
        }
        emit_jump_back_or_fail(l_cond, line, node.loc());

        if (jf) {
            patch_jump_or_fail(*jf, node.loc()); // -> L_end
        }
        for (const auto bp: loop.break_fwd_patches) {
            patch_jump_or_fail(bp, node.loc()); // -> L_end
        }
        end_scope(line);
    }

    void CodeGen::visitForInStmtNode(ForInStmtNode& node) {
        const u32 line = node.loc_line();
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
        begin_scope(); // for-in scope（D）：仅 <iter>，循环全程存活
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;

        // 隐藏局部 <iter>，值填槽：iterable.iter() 出值后 declare，值即 <iter>（无 LOAD_NIL 预占、无
        // STORE_LOCAL/POP）。
        // [iterable]（receiver）
        emit_expr(*node.iterable);
        emit_method_call0("iter", line, node.loc()); // [iter_obj] 恰在 slot 位置
        const u16 iter_var_slot = declare_local_or_fail("<iter>", node.loc());
        cur_fn_ctx()->mark_initialized(iter_var_slot); // 值已在槽

        const u32 l_start = cur_cu()->size();
        // [iter]（receiver）
        cur_cu()->emit_load_local(iter_var_slot, line);
        // [bool]
        emit_method_call0("has_next", line, node.loc());
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

        auto loop_ctx                 = make_loop_ctx(loop_scope);
        loop_ctx.continue_back_target = l_start; // continue 后向跳 L_start（has_next 判断处）
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));

        // per-iteration scope：pattern + 体每轮 fresh（值填槽）。体经 emit_stmt 作为不透明子节点，
        // 若为 block 则自带更深层 scope；break/continue 跳出时由 emit_pop_locals_to(loop_scope) 代弹。
        begin_scope();
        cur_cu()->emit_load_local(iter_var_slot, line); // [iter]（receiver）
        emit_method_call0("next", line, node.loc());    // [value] 恰在 slot 位置
        // id: declare 值填槽（不发指令）/ _: POP 丢弃
        bind_pattern(*node.pattern);
        emit_stmt(*node.body);
        end_scope(line); // per-iter：POP_N 弹 pattern（id）；_ 无局部 -> emit_pop_n(0) 无指令

        const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

        emit_jump_back_or_fail(l_start, line, node.loc());
        patch_jump_or_fail(jf, node.loc()); // -> L_end
        for (const auto bp: loop.break_fwd_patches) {
            patch_jump_or_fail(bp, node.loc());
        }
        end_scope(line); // for-in：POP_N 弹 <iter>
    }

    void CodeGen::visitBreakStmtNode(BreakStmtNode& node) {
        const u32 line = node.loc_line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::BreakOutsideLoop, node.loc(), "break 不在循环内");
        }
        auto& loop = cur_fn_ctx()->loop_stack_.top();
        emit_pop_locals_to(loop.loop_scope_depth, line);
        loop.break_fwd_patches.push_back(cur_cu()->emit_jump(OpCode::JUMP, line)); // -> L_end（回填）
    }

    void CodeGen::visitContinueStmtNode(ContinueStmtNode& node) {
        const u32 line = node.loc_line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::ContinueOutsideLoop, node.loc(), "continue 不在循环内");
        }
        auto& loop = cur_fn_ctx()->loop_stack_.top();
        emit_pop_locals_to(loop.loop_scope_depth, line);
        if (loop.continue_back_target) {
            emit_jump_back_or_fail(*loop.continue_back_target, line, node.loc());
        } else {
            loop.continue_fwd_patches.push_back(cur_cu()->emit_jump(OpCode::JUMP, line)); // -> L_incr（回填）
        }
    }

    void CodeGen::visitReturnStmtNode(ReturnStmtNode& node) {
        const u32 line = node.loc_line();
        // 入口 <main> 亦为函数，故顶层 return 合法（cur_fn_ctx()->fn_ 恒非空）。
        if (node.value != nullptr) {
            emit_expr(*node.value);
        } else {
            cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        }
        cur_cu()->emit_op(OpCode::RETURN, line);
    }

    void CodeGen::visitImportStmtNode(ImportStmtNode& node) {
        const u32 line = node.loc_line();
        // IMPORT path:u16 压模块值于栈顶；绑定与 var/fun 同形：顶层 -> DEF_GLOBAL alias，嵌套 -> 值填槽。
        // path/alias 经 add_name_or_fail（无需守卫，见类首 GC 安全注）。
        const auto path_idx = add_name_or_fail(node.path, node.loc());
        cur_cu()->emit_op(OpCode::IMPORT, line);
        cur_cu()->emit_word(path_idx, line); // [module]
        if (mod_ctx_->is_global_scope()) {
            // 顶层 import -> 模块全局。path 已先入池经 module 根链可达，故 alias 的 new_string
            // 不会回收已入池的 path。
            declare_global_or_fail(node.alias, node.loc());
            const auto alias_idx = add_name_or_fail(node.alias, node.loc());
            cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
            cur_cu()->emit_word(alias_idx, line); // []  弹值定义全局
        } else {
            // 嵌套 import -> 局部（值填槽，模型见 CodeGen.hpp bind_pattern 注）。
            const auto slot = declare_local_or_fail(node.alias, node.loc());
            cur_fn_ctx()->mark_initialized(slot); // 值已在槽
        }
    }

    void CodeGen::visitTryStmtNode(TryStmtNode& node) {
        const u32 line = node.loc_line();
        // try 须有 catch（parse 层允许无 catch，语义收口在此）。
        if (node.catch_body == nullptr) {
            fail(ErrorCode::TryWithoutHandler, node.loc(), "try 须有 catch");
        }

        // lowering(入口预插占位 + 结尾回填,pitfalls 坑 #4;catch 参数走值填槽,坑 #10):
        //   L_try:  try 体(受保护区间 [begin, end),编译期入 cur_cu()->try_records)
        //   end:    JUMP L_end           ; 正常路径跳过 catch
        //   L_catch:                     ; unwind 截栈到 slots+stack_depth 后 push 异常值,
        //                                 ; 恰落 catch 参数槽(stack_depth) -- 无 STORE_LOCAL
        //   L_end:
        //
        // 栈平衡:try 体 end_scope 与 catch 子句 end_scope(弹 e + catch 体局部)都回到
        // stack_depth,两路径在 L_end 齐平(坑 #10 校验)。
        const auto stack_depth = static_cast<u32>(cur_fn_ctx()->locals_.size()); // try 入口局部数(try scope 开前)
        const auto begin       = cur_cu()->size();
        const auto rec_idx     = cur_cu()->try_records.size();
        // 预插占位(begin 已定,余待回填)
        cur_cu()->try_records.push(TryRecord{begin, 0, 0, 0});
        begin_scope();         // try 体 scope
        emit_stmt(*node.body); // 嵌套 try 在此编译,各自入口预插占位(begin > 本层)-> 整体升序(坑 #4)
        end_scope(line);
        const auto end   = cur_cu()->size();
        const auto jskip = cur_cu()->emit_jump(OpCode::JUMP, line); // 正常路径跳过 catch -> L_end
        // L_catch
        const auto handle = cur_cu()->size();
        begin_scope(); // catch 子句 scope(包 e + catch 体 -- e 须入 scope,两路径栈平衡,坑 #10)
        const auto catch_slot = declare_local_or_fail(*node.catch_param, node.loc());
        cur_fn_ctx()->mark_initialized(catch_slot); // e 由 unwind 的 push 在运行期填槽(== stack_depth),
        emit_stmt(*node.catch_body);
        end_scope(line);
        patch_jump_or_fail(jskip, node.loc()); // -> L_end
        // 回填占位项(嵌套 try 的内层记录已在体编译期间插于本项之后,构造即升序,不排序,坑 #4)。
        cur_cu()->try_records[rec_idx].end         = end;
        cur_cu()->try_records[rec_idx].handle      = handle;
        cur_cu()->try_records[rec_idx].stack_depth = stack_depth;
    }

    void CodeGen::visitThrowStmtNode(ThrowStmtNode& node) {
        const u32 line = node.loc_line();
        // 求值抛出表达式后 THROW 弹值入寄存器,运行期由 unwind 查异常记录表派发(语义见
        // AriaVM dispatch_loop 的 THROW case):原值不包 ObjException,catch 绑原值保类型(坑 #7)。
        // [v]
        emit_expr(*node.expr);
        cur_cu()->emit_op(OpCode::THROW, line); // [v] -> [](派发 handler 时值落 catch 参数槽)
    }

    void CodeGen::visitMatchStmtNode(MatchStmtNode& node) { not_impl(node, "match 语句"); }

    void CodeGen::visitFunDeclNode(FunDeclNode& node) {
        // name 由 compile_function 内部 intern + make_guard，此处只传 StringView；node.loc() 作 decl_loc。
        compile_function(node.name, node.params, *node.body, node.loc());
    }

    void CodeGen::visitDefDeclNode(DefDeclNode& node) { not_impl(node, "def 类与对象"); }

    void CodeGen::visitVarDeclNode(VarDeclNode& node) {
        const u32 line = node.loc_line();
        for (const auto& [target, initializer]: node.bindings) {
            // 仅 IdentifierPattern 可跑；ListPattern -> not_impl。
            const auto id = dynamic_cast<IdentifierPatternNode*>(target.get());
            if (id == nullptr) {
                not_impl(*target, "列表模式解构 var 声明");
            }
            if (mod_ctx_->is_global_scope()) {
                // 顶层 var -> 模块全局（DEF_GLOBAL 弹值定义）。name 在 emit_expr 之后入池
                // （无需守卫，见类首 GC 安全注），与 visitImportStmtNode 顶层分支同形。
                declare_global_or_fail(id->name, id->loc());
                if (initializer != nullptr) {
                    emit_expr(*initializer);
                } else {
                    cur_cu()->emit_op(OpCode::LOAD_NIL, line);
                }
                const auto name_idx = add_name_or_fail(id->name, id->loc());
                cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
                cur_cu()->emit_word(name_idx, line);
            } else {
                // 嵌套 var -> 局部（值填槽，模型见 CodeGen.hpp bind_pattern 注；无初始化器 LOAD_NIL 填槽）。
                const auto slot = declare_local_or_fail(id->name, id->loc());
                if (initializer != nullptr) {
                    emit_expr(*initializer); // 值恰好压在 slot（不变式：declare 与 init 相邻）
                } else {
                    cur_cu()->emit_op(OpCode::LOAD_NIL, id->loc_line()); // 无初始化器：nil 填槽
                }
                cur_fn_ctx()->mark_initialized(slot);
            }
        }
    }

    // ============================================================
    // 表达式节点
    // ============================================================

    void CodeGen::visitIntegerLiteralNode(IntegerLiteralNode& node) {
        const u32 line  = node.loc_line();
        const i64 value = node.value;
        if (value >= -128 && value <= 127) {
            // LOAD_IMM 的 u8 操作数在 VM 侧按 i8 位型重解释做符号扩展（bit_cast<i8>）；此处先经
            // i8 保证符号语义、再转 u8 写字节（免窄化告警）。范围外的整数走常量池 LOAD_CONST。
            cur_cu()->emit_op(OpCode::LOAD_IMM, line);
            cur_cu()->emit_byte(static_cast<u8>(static_cast<i8>(value)), line);
            return;
        }
        if (value < kIntMin || value > kIntMax) {
            fail(ErrorCode::NumberOutOfRange, node.loc(), "整数字面量超出 i48 范围: {}", value);
            return;
        }
        const auto idx = add_constant_or_fail(Value::from_int(value), node.loc());
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    void CodeGen::visitFloatLiteralNode(FloatLiteralNode& node) {
        const u32  line = node.loc_line();
        const auto idx  = add_constant_or_fail(Value::from_f64(node.value), node.loc());
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    void CodeGen::visitStringLiteralNode(StringLiteralNode& node) {
        const u32  line = node.loc_line();
        const auto idx  = add_name_or_fail(node.value, node.loc());
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    void CodeGen::visitBoolLiteralNode(BoolLiteralNode& node) {
        const u32 line = node.loc_line();
        cur_cu()->emit_op(node.value ? OpCode::LOAD_TRUE : OpCode::LOAD_FALSE, line);
    }

    void CodeGen::visitNilLiteralNode(NilLiteralNode& node) {
        const u32 line = node.loc_line();
        cur_cu()->emit_op(OpCode::LOAD_NIL, line);
    }

    void CodeGen::visitIdentifierNode(IdentifierNode& node) {
        // 入口 take：按 mode 分派 Load/Store（Locate 预留位，同 Load）。
        const auto mode     = take_lvalue_mode();
        const u32  line     = node.loc_line();
        const auto resolved = resolve_name_or_fail(node.name, node.loc());
        switch (mode) {
            case LvalueMode::Load:
            case LvalueMode::Locate:
                emit_load_var(resolved, line, node.loc());
                return;
            case LvalueMode::Store:
                emit_store_var(resolved, line, node.loc());
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::visitThisExprNode(ThisExprNode& node) { not_impl(node, "this（类与对象）"); }

    void CodeGen::visitSuperExprNode(SuperExprNode& node) { not_impl(node, "super（类与对象）"); }

    void CodeGen::visitBinaryExprNode(BinaryExprNode& node) {
        const u32 line = node.loc_line();
        emit_expr(*node.lhs);
        // 短路逻辑运算：lhs 真假跳留值、跳过 rhs；否则弹 lhs 求 rhs。跳转回填到 rhs 之后（L_end）。
        if (node.op == Op::Binary::Or) {
            const auto j = cur_cu()->emit_jump(OpCode::JUMP_TRUE_OR_POP, line);
            emit_expr(*node.rhs);
            patch_jump_or_fail(j, node.loc()); // -> L_end（rhs 之后）
            return;
        }
        if (node.op == Op::Binary::And) {
            const auto j = cur_cu()->emit_jump(OpCode::JUMP_FALSE_OR_POP, line);
            emit_expr(*node.rhs);
            patch_jump_or_fail(j, node.loc()); // -> L_end（rhs 之后）
            return;
        }
        emit_expr(*node.rhs);
        switch (node.op) {
            case Op::Binary::EqualEqual:
                cur_cu()->emit_op(OpCode::EQUAL, line);
                return;
            case Op::Binary::EqualEqualEqual:
                cur_cu()->emit_op(OpCode::STRICT_EQUAL, line);
                return;
            case Op::Binary::BangEqual:
                cur_cu()->emit_op(OpCode::NOT_EQUAL, line);
                return;
            case Op::Binary::BangEqualEqual:
                cur_cu()->emit_op(OpCode::STRICT_NOT_EQUAL, line);
                return;
            case Op::Binary::Greater:
                cur_cu()->emit_op(OpCode::GREATER, line);
                return;
            case Op::Binary::GreaterEqual:
                cur_cu()->emit_op(OpCode::GREATER_EQUAL, line);
                return;
            case Op::Binary::Less:
                cur_cu()->emit_op(OpCode::LESS, line);
                return;
            case Op::Binary::LessEqual:
                cur_cu()->emit_op(OpCode::LESS_EQUAL, line);
                return;
            case Op::Binary::Plus:
                cur_cu()->emit_op(OpCode::ADD, line);
                return;
            case Op::Binary::Minus:
                cur_cu()->emit_op(OpCode::SUBTRACT, line);
                return;
            case Op::Binary::Star:
                cur_cu()->emit_op(OpCode::MULTIPLY, line);
                return;
            case Op::Binary::Slash:
                cur_cu()->emit_op(OpCode::DIVIDE, line);
                return;
            case Op::Binary::Percent:
                cur_cu()->emit_op(OpCode::MOD, line);
                return;
            default:
                UNREACHABLE();
        }
    }

    void CodeGen::visitUnaryExprNode(UnaryExprNode& node) {
        const u32 line = node.loc_line();
        switch (node.op) {
            case Op::Unary::Minus:
                emit_expr(*node.operand);
                cur_cu()->emit_op(OpCode::NEGATE, line);
                return;
            case Op::Unary::Not:
                emit_expr(*node.operand);
                cur_cu()->emit_op(OpCode::NOT, line);
                return;
            case Op::Unary::PreInc:
            case Op::Unary::PreDec: {
                // E += 1 / E -= 1。统一压 +1，由 ADD/SUBTRACT 决定方向--
                // 若 PreDec 压 -1 再 SUBTRACT 会算成 E - (-1) = E + 1，方向反。
                emit_lvalue(*node.operand, LvalueMode::Load);
                cur_cu()->emit_op(OpCode::LOAD_IMM, line);
                cur_cu()->emit_byte(1, line);
                cur_cu()->emit_op(node.op == Op::Unary::PreInc ? OpCode::ADD : OpCode::SUBTRACT, line);
                emit_lvalue(*node.operand, LvalueMode::Store); // peek-store 留新值
                return;
            }
            default:
                UNREACHABLE();
        }
    }

    void CodeGen::visitAssignmentNode(AssignmentNode& node) {
        const u32 line = node.loc_line();
        if (node.op == Op::Assignment::Assign) {
            // 普通 =：value -> store（peek-store 留值）
            emit_expr(*node.value);
            emit_lvalue(*node.target, LvalueMode::Store);
            return;
        }
        // 复合赋值：load target -> value -> op -> store target（Identifier 重 resolve 廉价，locator-once 自然成立）
        emit_lvalue(*node.target, LvalueMode::Load);
        emit_expr(*node.value);
        switch (node.op) {
            case Op::Assignment::PlusAssign:
                cur_cu()->emit_op(OpCode::ADD, line);
                break;
            case Op::Assignment::MinusAssign:
                cur_cu()->emit_op(OpCode::SUBTRACT, line);
                break;
            case Op::Assignment::StarAssign:
                cur_cu()->emit_op(OpCode::MULTIPLY, line);
                break;
            case Op::Assignment::SlashAssign:
                cur_cu()->emit_op(OpCode::DIVIDE, line);
                break;
            case Op::Assignment::PercentAssign:
                cur_cu()->emit_op(OpCode::MOD, line);
                break;
            default:
                UNREACHABLE();
        }
        emit_lvalue(*node.target, LvalueMode::Store);
    }

    void CodeGen::visitDestructureAssignmentNode(DestructureAssignmentNode& node) { not_impl(node, "解构赋值"); }

    void CodeGen::visitCallNode(CallNode& node) {
        const u32 line = node.loc_line();
        // 实参上限 kMaxArguments（CALL 操作数 u8）：先检后发，避免 emit 完数百个实参表达式才报错。
        if (node.args.size() > kMaxArguments) {
            fail(ErrorCode::TooManyArguments, node.loc(), "实参数超过 {}", kMaxArguments);
        }
        emit_expr(*node.callee);
        for (const auto& arg: node.args) {
            emit_expr(*arg);
        }
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(static_cast<u8>(node.args.size()), line);
    }

    void CodeGen::visitFieldAccessNode(FieldAccessNode& node) { not_impl(node, "字段访问（LOAD_FIELD 未由 VM 实现）"); }

    void CodeGen::visitIndexAccessNode(IndexAccessNode& node) { not_impl(node, "下标访问（LOAD_INDEX 未由 VM 实现）"); }

    void CodeGen::visitListExprNode(ListExprNode& node) { not_impl(node, "列表字面量"); }

    void CodeGen::visitMapExprNode(MapExprNode& node) { not_impl(node, "map 字面量"); }

    void CodeGen::visitRangeExprNode(RangeExprNode& node) { not_impl(node, "区间表达式"); }

    void CodeGen::visitIfExprNode(IfExprNode& node) {
        const u32 line = node.loc_line();
        emit_expr(*node.condition);
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> else
        emit_expr(*node.then_branch);
        const auto jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
        // -> else
        patch_jump_or_fail(jf, node.loc());
        emit_expr(*node.else_branch);
        patch_jump_or_fail(jend, node.loc()); // -> end
    }

    void CodeGen::visitLambdaExprNode(LambdaExprNode& node) {
        // lambda 判据即 name == `<anonymous>`（机制见 compile_function 注）；decl_loc 同 visitFunDeclNode。
        compile_function(kAnonymousName, node.params, *node.body, node.loc());
    }

    void CodeGen::visitMatchExprNode(MatchExprNode& node) { not_impl(node, "match 表达式"); }

    // ============================================================
    // 解构模式节点
    // ============================================================

    void CodeGen::visitIdentifierPatternNode(IdentifierPatternNode& node) {
        // 独立出现（非经 bind_pattern 调用）：无独立语义，不发射。
        (void) node;
    }

    void CodeGen::visitWildcardPatternNode(WildcardPatternNode& node) { (void) node; }

    void CodeGen::visitListPatternNode(ListPatternNode& node) { not_impl(node, "列表模式解构"); }

} // namespace aria
