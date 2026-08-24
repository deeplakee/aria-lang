#include "compile/CodeGen.hpp"

#include <memory>

#include "bytecode/CodeUnit.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"

#include <format>

namespace aria {

    // ============================================================
    // 入口
    // ============================================================

    Result<ObjFunction*, Error> CodeGen::compile(const ProgramNode& program, ObjModule& module) {
        // GC 已启用:module 入临时根贯穿全程。经 module.entry_ -> 常量池 -> 嵌套 fn 常量池 -> ...
        // 整链根化所有建设中 ObjFunction / 常量池 ObjString。每个子 fn 在 compile_function 起始即
        // add_constant 入父常量池(先于编译体),入池即经 module 根链可达;new_object -> add_constant
        // 间走 trivial 分配(constants.push/reallocate),按 GC 核心不变式不触发 GC,故该窗口无需守卫。
        auto module_guard = gc_.make_guard(&module);

        // 初始化模块编译上下文（建入口函数 + set_entry + 构造 ModuleCtx，含创建入口 fn 上下文与游标就位）。
        // 须在 module 已根化下调用(上方 module_guard)。
        const auto entry = init_module(module);

        try {
            // 遍历顶层声明（顶层 var/fun/import -> 模块全局；嵌套块内 var -> 局部）。
            for (auto& decl: program.declarations) {
                emit_stmt(decl.get());
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

        mod_ctx_.reset(); // 释放本模块上下文（含入口 fn 上下文）；游标随之失效但不再读
        return entry;
    }

    // 建模块入口函数（arity 0、匿名 <script>）+ set_entry + 构造 ModuleCtx（创建入口 fn 上下文、游标就位），
    // 返回入口函数。须在 module 已根化下调用（compile() 的 module_guard）；ModuleCtx 构造期 ASSERT entry 非空
    // （此处先 set_entry）。
    ObjFunction* CodeGen::init_module(ObjModule& module) {
        const auto entry = new_function(gc_, &module, nullptr, 0);
        module.set_entry(entry);
        mod_ctx_ = std::make_unique<ModuleCtx>(module); // 创建入口 fn 上下文并就位游标
        return entry;
    }

    // 当前 CodeUnit = 当前函数 fn_->unit()，随游标派生（定义于此：需 ObjFunction 完整类型取 unit()）。
    FunctionCtx* CodeGen::cur_fn_ctx() const noexcept { return mod_ctx_->current_fn_ctx_; }
    CodeUnit*    CodeGen::cur_cu() const noexcept { return &cur_fn_ctx()->fn_->unit(); }

    // ============================================================
    // 常量池辅助（emit 编码已下沉 CodeUnit，调用方经 cur_cu()->emit_* 直接发射）
    // 薄封装：只做操作 + 失败信号，不构造/抛错误、不持 loc。调用处检查返回值后用节点 loc 显式 fail。
    // ============================================================

    Opt<u16> CodeGen::add_constant(const Value v) const {
        if (cur_cu()->constants.size() >= 65536) {
            return std::nullopt;
        }
        return cur_cu()->add_constant(v);
    }

    Opt<u16> CodeGen::add_name(const StringView s) const {
        const auto str = new_string(gc_, s);
        return add_constant(Value::from_obj(str));
    }

    // ============================================================
    // 局部管理（登记经 FunctionCtx，发射经 cur_cu()）
    // ============================================================

    Result<u16, ErrorCode> CodeGen::declare_local(const StringView name) const {
        // 同作用域重名 -> RedefinedVariable（外层同名允许 shadow）；溢出 -> TooManyLocals。
        // 仅登记并标「定义但未初始化」（add_local 置 is_initialized=false），不发任何指令。
        // 调用方在初始化器求值 / 无初始化器发 LOAD_NIL 后 mark_initialized。loc/message 由调用处据返回码显式 fail。
        if (cur_fn_ctx()->is_defined_in_scope(name)) {
            return std::unexpected(ErrorCode::RedefinedVariable);
        }
        if (cur_fn_ctx()->locals_.size() >= 65536) {
            return std::unexpected(ErrorCode::TooManyLocals);
        }
        return cur_fn_ctx()->add_local(name); // 纯登记，is_initialized 默认 false
    }

    Opt<u16> CodeGen::resolve_local(const StringView name) const { return cur_fn_ctx()->find_local(name); }

    void CodeGen::begin_scope() const { cur_fn_ctx()->begin_scope(); }

    void CodeGen::end_scope(const u32 line) const { cur_cu()->emit_pop_n(cur_fn_ctx()->end_scope_pop_count(), line); }

    void CodeGen::pop_locals_to(const u32 target_depth, const u32 line) const {
        const u32 n = cur_fn_ctx()->pop_locals_deeper_than(target_depth);
        cur_cu()->emit_pop_n(n, line);
    }

    // ============================================================
    // 名字解析
    // ============================================================

    Result<CodeGen::ResolvedVar, ErrorCode> CodeGen::resolve_name(StringView name) {
        if (const auto local_idx = cur_fn_ctx()->find_local(name)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Local, .index = *local_idx};
        }
        // 外层函数局部 -> 需 upvalue 捕获（M4 未实现 -> not_impl）。沿 enclosing_ 链查；命中即 Upvalue，
        // 由调用方（visitIdentifierNode/compile_lvalue）走 not_impl 报编译期错--不静默落到全局，
        // 否则外层局部会与同名模块全局串台致闭包捕获错误变量（见 CLAUDE.md「作用域模型」）。
        for (auto e = cur_fn_ctx()->enclosing_; e != nullptr; e = e->enclosing_) {
            if (e->find_local(name)) {
                return ResolvedVar{.kind = ResolvedVar::Kind::Upvalue, .index = 0};
            }
        }
        // 否则视为模块全局（VM 运行期 LOAD_GLOBAL 查表，未定义报 UndefinedVariable）。
        // add_name 溢出 -> 透传 CodeUnitTooLarge，交调用处用节点 loc 显式 fail（本方法不持 loc）。
        if (const auto name_idx = add_name(name)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Global, .index = *name_idx};
        }
        return std::unexpected(ErrorCode::CodeUnitTooLarge);
    }

    // ============================================================
    // lvalue / 局部槽 load-store
    // ============================================================

    void CodeGen::check_local_initialized(const u16 slot, const SourceLoc& loc) {
        // 读点 init 检查：使用定义但未初始化的局部 -> UninitializedVariable（Python 风格 definite-assignment）。
        if (!cur_fn_ctx()->is_initialized(slot)) {
            fail(ErrorCode::UninitializedVariable, loc, "使用未初始化的变量: {}", cur_fn_ctx()->locals_[slot].name);
        }
    }

    void CodeGen::emit_load(const Lvalue& lv, const u32 line, const SourceLoc& loc) {
        switch (const auto& [kind, index] = lv; kind) {
            case Lvalue::Kind::Local:
                check_local_initialized(index, loc); // 读点 init 检查
                cur_cu()->emit_load_local(index, line);
                return;
            case Lvalue::Kind::Global:
                cur_cu()->emit_op(OpCode::LOAD_GLOBAL, line);
                cur_cu()->emit_word(index, line);
                return;
            // Upvalue/Field/Index 由 compile_lvalue 的 not_impl 挡住，到不了这里。
            default:
                UNREACHABLE();
        }
    }

    void CodeGen::emit_store(const Lvalue& lv, const u32 line) const {
        switch (const auto& [kind, index] = lv; kind) {
            case Lvalue::Kind::Local:
                cur_cu()->emit_store_local(index, line);
                cur_fn_ctx()->mark_initialized(index); // 赋值即初始化
                return;
            case Lvalue::Kind::Global:
                cur_cu()->emit_op(OpCode::STORE_GLOBAL, line);
                cur_cu()->emit_word(index, line);
                return;
            // Upvalue/Field/Index 由 compile_lvalue 的 not_impl 挡住，到不了这里。
            default:
                UNREACHABLE();
        }
    }

    CodeGen::Lvalue CodeGen::compile_lvalue(ExprNode* target) {
        if (const auto id = dynamic_cast<IdentifierNode*>(target)) {
            const auto r = resolve_name(id->name);
            if (!r) {
                fail(ErrorCode::CodeUnitTooLarge, id->loc(), "常量池溢出(>65535)");
            }
            // ResolvedVar 与 Lvalue 的 identifier 三种 kind 同形（Local/Upvalue/Global），index 语义一致，直传。
            switch (const auto& [kind, index] = *r; kind) {
                case ResolvedVar::Kind::Local:
                    return Lvalue{.kind = Lvalue::Kind::Local, .index = index};
                case ResolvedVar::Kind::Global:
                    return Lvalue{.kind = Lvalue::Kind::Global, .index = index};
                case ResolvedVar::Kind::Upvalue:
                    not_impl(id, "闭包/upvalue 捕获");
            }
            UNREACHABLE();
        }
        if (dynamic_cast<FieldAccessNode*>(target)) {
            not_impl(target, "字段赋值");
        }
        if (dynamic_cast<IndexAccessNode*>(target)) {
            not_impl(target, "下标赋值");
        }
        fail(ErrorCode::InvalidAssignmentTarget, target->loc(), "非法赋值左值");
    }

    // ============================================================
    // 模式绑定
    // ============================================================

    void CodeGen::bind_pattern(PatternNode* pat) {
        // 栈顶已有一值（for-in 的 next() 产物），按模式绑定为 per-iteration 局部。值填槽模型：
        // 声明发生在值已在栈顶之时，slot = 当前栈高 = 值所在位置，值即该局部（无 STORE_LOCAL/POP）。
        if (const auto id = dynamic_cast<IdentifierPatternNode*>(pat)) {
            const auto slot = declare_local(id->name); // 纯登记，slot = 值位置；值填槽不发指令
            if (!slot) {
                switch (slot.error()) {
                    case ErrorCode::RedefinedVariable:
                        fail(ErrorCode::RedefinedVariable, pat->loc(), "重复定义局部变量: {}", id->name);
                    case ErrorCode::TooManyLocals:
                        fail(ErrorCode::TooManyLocals, pat->loc(), "局部变量过多(>65535)");
                    default:
                        UNREACHABLE();
                }
            }
            cur_fn_ctx()->mark_initialized(*slot); // 值已在槽
            return;
        }
        const u32 line = pat->loc_line();
        if (dynamic_cast<WildcardPatternNode*>(pat) != nullptr) {
            cur_cu()->emit_op(OpCode::POP, line); // 丢弃值
            return;
        }
        if (dynamic_cast<ListPatternNode*>(pat) != nullptr) {
            not_impl(pat, "列表模式解构");
            return;
        }
        not_impl(pat, "未知模式");
    }

    // ============================================================
    // 遍历入口
    // ============================================================

    // 遍历入口：accept 双分派；出错时 visit 内 fail() 抛 AriaCompileException 自动 unwind，无需 ok() 短路。
    void CodeGen::emit_expr(ExprNode* n) { n->accept(*this); }

    void CodeGen::emit_stmt(StmtNode* n) { n->accept(*this); }

    // ============================================================
    // 函数编译（FunDecl / Lambda 共用）
    // ============================================================

    void CodeGen::compile_function(ObjString* name, List<Param>& params, BlockNode* body) {
        const SourceLoc loc  = body->loc();
        const u32       line = body->loc_line(); // 父序列压函数值 / 绑定 / 隐式 return 均用此行

        // 默认参数 / varargs -> not_impl（VM CALL 精确 arity，无默认/varargs 支持）。
        for (const auto& p: params) {
            if (p.is_varargs || p.default_value != nullptr) {
                not_impl(body, "默认参数 / varargs");
            }
        }
        // 形参重名 -> DuplicateParam。
        for (usize i = 0; i < params.size(); ++i) {
            for (usize j = i + 1; j < params.size(); ++j) {
                if (params[i].name == params[j].name) {
                    fail(ErrorCode::DuplicateParam, loc, "形参重名: {}", params[i].name);
                }
            }
        }
        if (params.size() > 255) {
            fail(ErrorCode::TooManyLocals, loc, "形参过多(>255)");
        }

        const auto fn = new_function(gc_, mod_ctx_->module_, name, static_cast<u8>(params.size()));
        // fn 此刻白色无根,但 add_constant -> constants.push -> reallocate<T> 走 trivial 分配
        // (不触发 GC,见 GC.hpp 核心不变式),故 fn 跨 add_constant 不会被回收,无需守卫。
        // 入父常量池后即经 module 根链可达。
        const auto fn_idx = add_constant(Value::from_obj(fn)); // 入父（当前）序列常量池
        if (!fn_idx) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "常量池溢出(>65535)");
        }

        // 父序列：压函数值 + 绑定名字（仍发射入父 unit = 当前 cur_cu()）。
        if (name != nullptr) {
            if (mod_ctx_->is_global_scope()) {
                // 顶层 fun -> 模块全局（name 已是 intern ObjString*，declare_global 按内容判重）
                if (!mod_ctx_->declare_global(name->view())) {
                    fail(ErrorCode::RedefinedVariable, loc, "重复定义全局: {}", name->view());
                }
                cur_cu()->emit_op(OpCode::LOAD_CONST, line);
                cur_cu()->emit_word(*fn_idx, line);
                const auto name_idx = add_constant(Value::from_obj(name)); // 复用 intern 串，免 add_name 二次 intern
                if (!name_idx) {
                    fail(ErrorCode::CodeUnitTooLarge, loc, "常量池溢出(>65535)");
                }
                cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
                cur_cu()->emit_word(*name_idx, line);
            } else {
                // 嵌套 fun -> 局部（值填槽：declare 仅登记标未初始化，LOAD_CONST 把 fn 压在 slot 即该局部，无
                // store/pop）
                const auto slot = declare_local(name->view());
                if (!slot) {
                    switch (slot.error()) {
                        case ErrorCode::RedefinedVariable:
                            fail(ErrorCode::RedefinedVariable, loc, "重复定义局部变量: {}", name->view());
                        case ErrorCode::TooManyLocals:
                            fail(ErrorCode::TooManyLocals, loc, "局部变量过多(>65535)");
                        default:
                            UNREACHABLE();
                    }
                }
                cur_cu()->emit_op(OpCode::LOAD_CONST, line);
                cur_cu()->emit_word(*fn_idx, line); // fn 恰好压在 slot（不变式：declare 与 init 相邻）
                cur_fn_ctx()->mark_initialized(*slot);
            }
        } else {
            // lambda：函数值留栈作表达式值
            cur_cu()->emit_op(OpCode::LOAD_CONST, line);
            cur_cu()->emit_word(*fn_idx, line);
        }

        // 切到子函数上下文：new 分配（非 UPtr），enclosing_ 回父（父函数编译期长于子，裸指针稳定）。
        // 摆动 ModuleCtx 游标即可--cu 由游标派生，随游标自动切到子 unit，无需 save/restore。
        auto child                = new FunctionCtx{*cur_fn_ctx(), *fn}; // child->enclosing_ = 当前游标
        mod_ctx_->current_fn_ctx_ = child;
        for (const auto& p: params) {
            cur_fn_ctx()->add_local(p.name); // caller 压栈，登记但不预留（不 emit LOAD_NIL）
            cur_fn_ctx()->mark_initialized(static_cast<u16>(cur_fn_ctx()->locals_.size() - 1)); // 形参已初始化
        }

        // 编译体（BlockNode 自带 scope）。
        emit_stmt(body);

        // 若 emit_stmt(body) 抛 AriaCompileException：unwind 跳过下方 delete/还原，子留在 enclosing_ 链上，
        // 交 ~ModuleCtx 沿链释放（compile() 顶层 catch 后 mod_ctx_.reset()）。

        // 隐式 return nil（成功路径）。
        cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        cur_cu()->emit_op(OpCode::RETURN, line);

        // 成功：还原父游标（cu 随之自动回父 unit）并手动 delete 子上下文。
        mod_ctx_->current_fn_ctx_ = child->enclosing_; // = 父
        delete child;
    }

    // ============================================================
    // not_impl
    // ============================================================

    void CodeGen::not_impl(ASTNode* node, StringView feature) {
        fail(ErrorCode::NotImplemented, node->loc(), "{} 尚未支持", feature);
    }

    // ============================================================
    // 根节点
    // ============================================================

    void CodeGen::visitProgramNode(ProgramNode* node) {
        // 仅编排顶层声明，本节点不直接发射（行号由各子节点自持）。
        for (auto& decl: node->declarations) {
            emit_stmt(decl.get());
        }
    }

    // ============================================================
    // 语句节点
    // ============================================================

    void CodeGen::visitBlockNode(BlockNode* node) {
        const u32 line = node->loc_line();
        begin_scope();
        for (auto& s: node->statements) {
            emit_stmt(s.get());
        }
        end_scope(line);
    }

    void CodeGen::visitExprStmtNode(ExprStmtNode* node) {
        const u32 line = node->loc_line();
        emit_expr(node->expr.get());
        cur_cu()->emit_op(OpCode::POP, line);
    }

    void CodeGen::visitPrintStmtNode(PrintStmtNode* node) {
        const u32 line = node->loc_line();
        emit_expr(node->expr.get());
        cur_cu()->emit_op(OpCode::PRINT, line);
    }

    void CodeGen::visitIfStmtNode(IfStmtNode* node) {
        const u32 line = node->loc_line();
        emit_expr(node->condition.get());
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> else / end
        emit_stmt(node->then_branch.get());
        if (node->else_branch != nullptr) {
            const auto jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
            if (!cur_cu()->patch_jump(jf))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> else
            emit_stmt(node->else_branch.get());
            if (!cur_cu()->patch_jump(jend))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> end
        } else {
            if (!cur_cu()->patch_jump(jf))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> end
        }
    }

    void CodeGen::visitWhileStmtNode(WhileStmtNode* node) {
        const u32 line       = node->loc_line();
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;
        const u32 l_start    = cur_cu()->size();
        emit_expr(node->condition.get());
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

        auto loop_ctx = LoopCtx{.loop_scope_depth     = loop_scope,
                                .continue_back_target = {l_start},
                                .continue_fwd_patches = {},
                                .break_fwd_patches    = {}};
        cur_fn_ctx()->loop_stack_.push_back(std::move(loop_ctx));
        emit_stmt(node->body.get());
        auto loop = std::move(cur_fn_ctx()->loop_stack_.back());
        cur_fn_ctx()->loop_stack_.pop_back();

        if (!cur_cu()->emit_jump_back(l_start, line))
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "回边偏移超过 64KB");
        if (!cur_cu()->patch_jump(jf))
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> L_end
        for (const auto bp: loop.break_fwd_patches) {
            if (!cur_cu()->patch_jump(bp))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB");
        }
    }

    void CodeGen::visitForStmtNode(ForStmtNode* node) {
        const u32 line = node->loc_line();
        begin_scope();
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;
        if (node->init != nullptr) {
            emit_stmt(node->init.get());
        }
        const u32  l_cond   = cur_cu()->size();
        const bool has_cond = node->condition != nullptr;
        usize      jf       = 0;
        if (has_cond) {
            emit_expr(node->condition.get());
            jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end
        }
        // continue: 有 incr -> 前向跳 L_incr（回填）；无 incr -> 后向跳 L_cond。
        auto loop_ctx = LoopCtx{.loop_scope_depth     = loop_scope,
                                .continue_back_target = node->increment != nullptr ? std::nullopt : Opt{l_cond},
                                .continue_fwd_patches = {},
                                .break_fwd_patches    = {}};
        cur_fn_ctx()->loop_stack_.push_back(std::move(loop_ctx));
        emit_stmt(node->body.get());
        auto loop = std::move(cur_fn_ctx()->loop_stack_.back());
        cur_fn_ctx()->loop_stack_.pop_back();

        const u32 l_incr = cur_cu()->size();
        // continue（前向）须回填到 L_incr：此处 cur_cu()->size()==L_incr（递增区起点），先于递增发射。
        // 若等递增与 JUMP_BACK 发完再回填，cur_cu()->size() 已是 L_end，continue 会错跳到 L_end 提前出循环。
        for (const auto cp: loop.continue_fwd_patches) {
            if (!cur_cu()->patch_jump(cp))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> L_incr
        }
        if (node->increment != nullptr) {
            emit_expr(node->increment.get());
            cur_cu()->emit_op(OpCode::POP, line);
        }
        if (!cur_cu()->emit_jump_back(l_cond, line))
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "回边偏移超过 64KB");

        const u32 l_end = cur_cu()->size();
        if (has_cond) {
            if (!cur_cu()->patch_jump(jf))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> L_end
        }
        for (const auto bp: loop.break_fwd_patches) {
            if (!cur_cu()->patch_jump(bp))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> L_end
        }
        (void) l_incr;
        end_scope(line);
    }

    void CodeGen::visitForInStmtNode(ForInStmtNode* node) {
        const u32 line = node->loc_line();
        begin_scope();                          // for-in scope（D）：仅 <iter>，循环全程存活
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;

        // 隐藏局部 <iter>，值填槽：iterable.iter() 出值后 declare，值即 <iter>（无 LOAD_NIL 预占、无 STORE_LOCAL/POP）。
        emit_expr(node->iterable.get());        // [iterable]
        cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
        const auto iter_name = add_name("iter");
        if (!iter_name)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        cur_cu()->emit_word(*iter_name, line); // [iter_fn]
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(0, line); // [iter_obj] 恰在 slot 位置
        const auto iter_slot_opt = declare_local("<iter>");
        if (!iter_slot_opt) {
            switch (iter_slot_opt.error()) {
                case ErrorCode::RedefinedVariable:
                    fail(ErrorCode::RedefinedVariable, node->loc(), "重复定义局部: <iter>");
                case ErrorCode::TooManyLocals:
                    fail(ErrorCode::TooManyLocals, node->loc(), "局部变量过多(>65535)");
                default:
                    UNREACHABLE();
            }
        }
        const u16 iter_slot = *iter_slot_opt;
        cur_fn_ctx()->mark_initialized(iter_slot); // 值已在槽

        const u32 l_start = cur_cu()->size();
        cur_cu()->emit_load_local(iter_slot, line); // [iter]
        cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
        const auto has_next_name = add_name("has_next");
        if (!has_next_name)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        cur_cu()->emit_word(*has_next_name, line); // [has_next_fn]
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(0, line);                                  // [bool]
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

        auto loop_ctx = LoopCtx{.loop_scope_depth     = loop_scope,
                                .continue_back_target = Opt{l_start},
                                .continue_fwd_patches = {},
                                .break_fwd_patches    = {}};
        cur_fn_ctx()->loop_stack_.push_back(std::move(loop_ctx));

        // per-iteration scope：pattern + 体每轮 fresh（值填槽）。体经 emit_stmt 作为不透明子节点，
        // 若为 block 则自带更深层 scope；break/continue 跳出时由 pop_locals_to(loop_scope) 代弹。
        begin_scope();
        cur_cu()->emit_load_local(iter_slot, line); // [iter]
        cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
        const auto next_name = add_name("next");
        if (!next_name)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        cur_cu()->emit_word(*next_name, line); // [next_fn]
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(0, line);      // [value] 恰在 slot 位置
        bind_pattern(node->pattern.get()); // id: declare 值填槽（不发指令）/ _: POP 丢弃
        emit_stmt(node->body.get());
        end_scope(line); // per-iter：POP_N 弹 pattern（id）；_ 无局部 -> emit_pop_n(0) 无指令

        auto loop = std::move(cur_fn_ctx()->loop_stack_.back());
        cur_fn_ctx()->loop_stack_.pop_back();

        if (!cur_cu()->emit_jump_back(l_start, line))
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "回边偏移超过 64KB");
        if (!cur_cu()->patch_jump(jf))
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> L_end
        for (const auto bp: loop.break_fwd_patches) {
            if (!cur_cu()->patch_jump(bp))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB");
        }
        end_scope(line); // for-in：POP_N 弹 <iter>
    }

    void CodeGen::visitBreakStmtNode(BreakStmtNode* node) {
        const u32 line = node->loc_line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::BreakOutsideLoop, node->loc(), "break 不在循环内");
        }
        auto& loop = cur_fn_ctx()->loop_stack_.back();
        pop_locals_to(loop.loop_scope_depth, line);
        loop.break_fwd_patches.push_back(cur_cu()->emit_jump(OpCode::JUMP, line)); // -> L_end（回填）
    }

    void CodeGen::visitContinueStmtNode(ContinueStmtNode* node) {
        const u32 line = node->loc_line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::ContinueOutsideLoop, node->loc(), "continue 不在循环内");
        }
        auto& loop = cur_fn_ctx()->loop_stack_.back();
        pop_locals_to(loop.loop_scope_depth, line);
        if (loop.continue_back_target) {
            if (!cur_cu()->emit_jump_back(*loop.continue_back_target, line))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "回边偏移超过 64KB");
        } else {
            loop.continue_fwd_patches.push_back(cur_cu()->emit_jump(OpCode::JUMP, line)); // -> L_incr（回填）
        }
    }

    void CodeGen::visitReturnStmtNode(ReturnStmtNode* node) {
        const u32 line = node->loc_line();
        // 入口 <script> 亦为函数，故顶层 return 合法（cur_fn_ctx()->fn_ 恒非空）。
        if (node->value != nullptr) {
            emit_expr(node->value.get());
        } else {
            cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        }
        cur_cu()->emit_op(OpCode::RETURN, line);
    }

    void CodeGen::visitImportStmtNode(ImportStmtNode* node) {
        const u32 line = node->loc_line();
        // IMPORT path:u16 alias:u16（VM 绑定为模块全局；栈中性）。
        // import 别名入表：补漏检 `import "x" as U; var U = 1;`（现报 RedefinedVariable）。
        auto alias_str = new_string(gc_, node->alias); // intern（常量池复用）
        // alias_str 裸持跨下方 new_string(path)：后者 maybe_collect 可能回收未根持有的 alias_str，故先入临时根。
        auto alias_guard = gc_.make_guard(alias_str);
        if (!mod_ctx_->declare_global(node->alias)) {
            fail(ErrorCode::RedefinedVariable, node->loc(), "重复定义全局: {}", node->alias);
        }
        const auto path_idx = add_constant(Value::from_obj(new_string(gc_, node->path)));
        if (!path_idx)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        const auto alias_idx = add_constant(Value::from_obj(alias_str)); // 复用 intern 串
        if (!alias_idx)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        cur_cu()->emit_op(OpCode::IMPORT, line);
        cur_cu()->emit_word(*path_idx, line);
        cur_cu()->emit_word(*alias_idx, line);
    }

    void CodeGen::visitTryStmtNode(TryStmtNode* node) {
        if (node->catch_body == nullptr && node->finally_body == nullptr) {
            fail(ErrorCode::TryWithoutHandler, node->loc(), "try 须有 catch 或 finally");
        }
        not_impl(node, "try/catch/finally 异常处理");
    }

    void CodeGen::visitThrowStmtNode(ThrowStmtNode* node) { not_impl(node, "throw 异常抛出"); }

    void CodeGen::visitMatchStmtNode(MatchStmtNode* node) { not_impl(node, "match 语句"); }

    void CodeGen::visitFunDeclNode(FunDeclNode* node) {
        auto name_str = new_string(gc_, node->name); // intern
        // name_str 裸持跨 compile_function（其内 new_function 与函数体编译均可能 new_string ->
        // maybe_collect 回收未根持有的 name_str），故先入临时根。
        auto name_guard = gc_.make_guard(name_str);
        compile_function(name_str, node->params, node->body.get());
    }

    void CodeGen::visitDefDeclNode(DefDeclNode* node) { not_impl(node, "def 类与对象"); }

    void CodeGen::visitVarDeclNode(VarDeclNode* node) {
        const u32 line = node->loc_line();
        for (auto& b: node->bindings) {
            // 仅 IdentifierPattern 可跑；ListPattern -> not_impl。
            auto id = dynamic_cast<IdentifierPatternNode*>(b.target.get());
            if (id == nullptr) {
                not_impl(b.target.get(), "列表模式解构 var 声明");
            }
            if (mod_ctx_->is_global_scope()) {
                // 顶层 var -> 模块全局（intern 一次供常量池复用；declare_global 按内容判重）
                auto name_str = new_string(gc_, id->name); // intern（常量池复用）
                // name_str 裸持跨 emit_expr(initializer)：初始化器可能分配（lambda -> new_function、
                // 字符串字面量 -> new_string）触发 maybe_collect 回收未根持有的 name_str，故先入临时根。
                auto name_guard = gc_.make_guard(name_str);
                if (!mod_ctx_->declare_global(id->name)) {
                    fail(ErrorCode::RedefinedVariable, id->loc(), "重复定义全局: {}", id->name);
                }
                if (b.initializer != nullptr) {
                    emit_expr(b.initializer.get());
                } else {
                    cur_cu()->emit_op(OpCode::LOAD_NIL, line);
                }
                const auto name_idx = add_constant(Value::from_obj(name_str)); // 复用，免 add_name 二次 intern
                if (!name_idx)
                    fail(ErrorCode::CodeUnitTooLarge, id->loc(), "常量池溢出(>65535)");
                cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
                cur_cu()->emit_word(*name_idx, line);
            } else {
                // 嵌套 var -> 局部（值填槽：declare 仅登记标未初始化；初始化器值恰好压在 slot 即该局部，
                // 无 store/pop；无初始化器则 LOAD_NIL 填槽。最后 mark_initialized）
                const auto slot = declare_local(id->name);
                if (!slot) {
                    switch (slot.error()) {
                        case ErrorCode::RedefinedVariable:
                            fail(ErrorCode::RedefinedVariable, id->loc(), "重复定义局部变量: {}", id->name);
                        case ErrorCode::TooManyLocals:
                            fail(ErrorCode::TooManyLocals, id->loc(), "局部变量过多(>65535)");
                        default:
                            UNREACHABLE();
                    }
                }
                if (b.initializer != nullptr) {
                    emit_expr(b.initializer.get()); // 值恰好压在 slot（不变式：declare 与 init 相邻）
                } else {
                    cur_cu()->emit_op(OpCode::LOAD_NIL, id->loc_line()); // 无初始化器：nil 填槽
                }
                cur_fn_ctx()->mark_initialized(*slot);
            }
        }
    }

    // ============================================================
    // 表达式节点
    // ============================================================

    void CodeGen::visitIntegerLiteralNode(IntegerLiteralNode* node) {
        const u32 line = node->loc_line();
        const i64 v    = node->value;
        if (v >= -128 && v <= 127) {
            cur_cu()->emit_op(OpCode::LOAD_IMM, line);
            cur_cu()->emit_byte(static_cast<u8>(static_cast<i8>(v)), line);
            return;
        }
        constexpr i64 kIntMin = -(static_cast<i64>(1) << 47);
        constexpr i64 kIntMax = (static_cast<i64>(1) << 47) - 1;
        if (v < kIntMin || v > kIntMax) {
            fail(ErrorCode::NumberOutOfRange, node->loc(), "整数字面量超出 i48 范围: {}", v);
            return;
        }
        const auto idx = add_constant(Value::from_int(v));
        if (!idx)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(*idx, line);
    }

    void CodeGen::visitFloatLiteralNode(FloatLiteralNode* node) {
        const u32  line = node->loc_line();
        const auto idx  = add_constant(Value::from_f64(node->value));
        if (!idx)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(*idx, line);
    }

    void CodeGen::visitStringLiteralNode(StringLiteralNode* node) {
        const u32  line = node->loc_line();
        const auto idx  = add_name(node->value);
        if (!idx)
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)");
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(*idx, line);
    }

    void CodeGen::visitBoolLiteralNode(BoolLiteralNode* node) {
        const u32 line = node->loc_line();
        cur_cu()->emit_op(node->value ? OpCode::LOAD_TRUE : OpCode::LOAD_FALSE, line);
    }

    void CodeGen::visitNilLiteralNode(NilLiteralNode* node) {
        const u32 line = node->loc_line();
        cur_cu()->emit_op(OpCode::LOAD_NIL, line);
    }

    void CodeGen::visitIdentifierNode(IdentifierNode* node) {
        const u32  line = node->loc_line();
        const auto r    = resolve_name(node->name);
        if (!r) {
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "常量池溢出(>65535)"); // add_name 溢出透传
        }
        auto [var_type, slot] = *r;
        if (var_type == ResolvedVar::Kind::Local) {
            check_local_initialized(slot, node->loc()); // 读点 init 检查（未初始化 -> UninitializedVariable）
            cur_cu()->emit_load_local(slot, line);
        } else if (var_type == ResolvedVar::Kind::Global) {
            cur_cu()->emit_op(OpCode::LOAD_GLOBAL, line);
            cur_cu()->emit_word(slot, line);
        } else {
            // Upvalue
            not_impl(node, "闭包/upvalue 捕获");
        }
    }

    void CodeGen::visitThisExprNode(ThisExprNode* node) { not_impl(node, "this（类与对象）"); }

    void CodeGen::visitSuperExprNode(SuperExprNode* node) { not_impl(node, "super（类与对象）"); }

    void CodeGen::visitBinaryExprNode(BinaryExprNode* node) {
        const u32 line = node->loc_line();
        // 短路逻辑运算：lhs 真假跳留值、跳过 rhs；否则弹 lhs 求 rhs。跳转回填到 rhs 之后（L_end）。
        if (node->op == Op::Binary::Or || node->op == Op::Binary::And) {
            emit_expr(node->lhs.get());
            const auto j = cur_cu()->emit_jump(
                    node->op == Op::Binary::Or ? OpCode::JUMP_TRUE_OR_POP : OpCode::JUMP_FALSE_OR_POP, line);
            emit_expr(node->rhs.get());
            if (!cur_cu()->patch_jump(j))
                fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> L_end（rhs 之后）
            return;
        }
        // 算术 / 比较：lhs、rhs 各求值一次，再发射 op。
        emit_expr(node->lhs.get());
        emit_expr(node->rhs.get());
        switch (node->op) {
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
            case Op::Binary::Or:
            case Op::Binary::And:
                break; // 已在上方短路分支处理（不可达）
        }
        not_impl(node, "未知二元运算符");
    }

    void CodeGen::visitUnaryExprNode(UnaryExprNode* node) {
        const u32 line = node->loc_line();
        switch (node->op) {
            case Op::Unary::Minus:
                emit_expr(node->operand.get());
                cur_cu()->emit_op(OpCode::NEGATE, line);
                return;
            case Op::Unary::Not:
                emit_expr(node->operand.get());
                cur_cu()->emit_op(OpCode::NOT, line);
                return;
            case Op::Unary::PreInc:
            case Op::Unary::PreDec: {
                // E += 1 / E -= 1（locator 只求值一次）。统一压 +1，由 ADD/SUBTRACT 决定方向--
                // 若 PreDec 压 -1 再 SUBTRACT 会算成 E - (-1) = E + 1，方向反。
                const auto lv = compile_lvalue(node->operand.get());
                emit_load(lv, line, node->loc());
                cur_cu()->emit_op(OpCode::LOAD_IMM, line);
                cur_cu()->emit_byte(1, line);
                cur_cu()->emit_op(node->op == Op::Unary::PreInc ? OpCode::ADD : OpCode::SUBTRACT, line);
                emit_store(lv, line); // peek-store 留新值
                return;
            }
        }
        not_impl(node, "未知一元运算符");
    }

    void CodeGen::visitAssignmentNode(AssignmentNode* node) {
        const u32  line = node->loc_line();
        const auto lv   = compile_lvalue(node->target.get());
        if (node->op == Op::Assignment::Assign) {
            emit_expr(node->value.get());
            emit_store(lv, line); // peek-store 留值
            return;
        }
        // 复合赋值：load -> value -> op -> store（locator 单次求值）
        emit_load(lv, line, node->loc());
        emit_expr(node->value.get());
        switch (node->op) {
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
            case Op::Assignment::Assign:
                break; // 已处理
        }
        emit_store(lv, line);
    }

    void CodeGen::visitDestructureAssignmentNode(DestructureAssignmentNode* node) { not_impl(node, "解构赋值"); }

    void CodeGen::visitCallNode(CallNode* node) {
        const u32 line = node->loc_line();
        // 实参上限 255（CALL 操作数 u8）：先检后发，避免 emit 完数百个实参表达式才报错。
        if (node->args.size() > 255) {
            fail(ErrorCode::TooManyArguments, node->loc(), "实参数超过 255");
        }
        emit_expr(node->callee.get());
        for (auto& arg: node->args) {
            emit_expr(arg.get());
        }
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(static_cast<u8>(node->args.size()), line);
    }

    void CodeGen::visitFieldAccessNode(FieldAccessNode* node) { not_impl(node, "字段访问（LOAD_FIELD 未由 VM 实现）"); }

    void CodeGen::visitIndexAccessNode(IndexAccessNode* node) { not_impl(node, "下标访问（LOAD_INDEX 未由 VM 实现）"); }

    void CodeGen::visitListExprNode(ListExprNode* node) { not_impl(node, "列表字面量"); }

    void CodeGen::visitMapExprNode(MapExprNode* node) { not_impl(node, "map 字面量"); }

    void CodeGen::visitRangeExprNode(RangeExprNode* node) { not_impl(node, "区间表达式"); }

    void CodeGen::visitIfExprNode(IfExprNode* node) {
        const u32 line = node->loc_line();
        emit_expr(node->condition.get());
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> else
        emit_expr(node->then_branch.get());
        const auto jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
        if (!cur_cu()->patch_jump(jf))
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> else
        emit_expr(node->else_branch.get());
        if (!cur_cu()->patch_jump(jend))
            fail(ErrorCode::CodeUnitTooLarge, node->loc(), "跳转偏移超过 64KB"); // -> end
    }

    void CodeGen::visitLambdaExprNode(LambdaExprNode* node) {
        compile_function(nullptr, node->params, node->body.get());
    }

    void CodeGen::visitMatchExprNode(MatchExprNode* node) { not_impl(node, "match 表达式"); }

    // ============================================================
    // 解构模式节点
    // ============================================================

    void CodeGen::visitIdentifierPatternNode(IdentifierPatternNode* node) {
        // 独立出现（非经 bind_pattern 调用）：无独立语义，不发射。
        (void) node;
    }

    void CodeGen::visitWildcardPatternNode(WildcardPatternNode* node) { (void) node; }

    void CodeGen::visitListPatternNode(ListPatternNode* node) { not_impl(node, "列表模式解构"); }

} // namespace aria
