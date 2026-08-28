#include "compile/CodeGen.hpp"

#include <memory>

#include "bytecode/CodeUnit.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

#include <format>

namespace aria {

    namespace {
        // 合成函数名(`<>` 是标识符中不可用的符号,故不可能与用户具名 fun 冲突):
        //   kMainName      -- 主入口模块的入口函数名(`<main>`,arity 0 模块体包装;VM 直接 run 它,
        //                     非用户可调用 fun,故用尖括号合成名避免与用户标识符碰撞);
        //   kModuleName    -- 运行期导入模块的入口函数名(`<module>`,与 CPython 模块体 code object 同名)--
        //                     当前由 init_module 统一产 `<main>`;导入磁盘加载链路落地后,导入编译路径
        //                     产 `<module>`(届时为另一已知「这是导入」的编译入口,无需参数机制)。
        //   kAnonymousName -- lambda 函数名(`<anonymous>`),compile_function 据此判定「lambda -> 留栈不绑定」。
        // 集中定义,使 visitLambdaExprNode 的创建点与 compile_function 的判定点不致漂移。
        constexpr StringView                  kMainName      = "<main>";
        [[maybe_unused]] constexpr StringView kModuleName    = "<module>";
        constexpr StringView                  kAnonymousName = "<anonymous>";

        // 容量上限(均由操作数/索引位宽决定;值为该位宽最大值,越界判定统一用 > 比较):
        //   kMaxArity     -- 函数形参上限 255(ObjFunction arity 为 u8);
        //   kMaxArguments -- 单次调用实参上限 255(CALL 操作数 u8);
        //   kMaxConstants -- 常量池最大索引 65535(u16 索引,即最多 65536 项);
        //   kMaxLocals    -- 单函数局部最大槽号 65535(u16 槽,含 slot 0 哑元,故用户局部最多 65535).
        // 集中定义,使各检查点与报错文案共享同一来源,无散落魔数。
        constexpr u32 kMaxArity     = 255;
        constexpr u32 kMaxArguments = 255;
        constexpr u32 kMaxConstants = 65535;
        constexpr u32 kMaxLocals    = 65535;

        // 整数字面量 i48 范围(Value::from_int 的 i48 尾部,与 NanBoxing.hpp 的 ASSERT 同源;
        // 超出 -> NumberOutOfRange):
        //   kIntMin -- -(2^47);
        //   kIntMax -- 2^47 - 1.
        // 集中定义,使 visitIntegerLiteralNode 的范围检查与报错文案共享同一来源,无散落魔数。
        constexpr i64 kIntMin = -(static_cast<i64>(1) << 47);
        constexpr i64 kIntMax = (static_cast<i64>(1) << 47) - 1;
    } // namespace

    // ============================================================
    // 入口
    // ============================================================

    Result<ObjFunction*, Error> CodeGen::compile(const ProgramNode& program, ObjModule& module) {
        // GC 已启用:module 入临时根贯穿全程。经 module.entry_ -> 常量池 -> 嵌套 fn 常量池 -> ...
        // 整链根化所有建设中 ObjFunction / 常量池 ObjString。每个子 fn 在 compile_function 起始即
        // add_constant 入父常量池(先于编译体),入池即经 module 根链可达;new_object -> add_constant
        // 间走 trivial 分配(constants.push/reallocate),按 GC 核心不变式不触发 GC,故该窗口无需守卫。
        const auto module_guard = gc_.make_guard(&module);

        // 防御：lvalue_mode_ 复位为 Load（构造已置；此处防上一次 compile() throw 后残留跨复用）。
        lvalue_mode_ = LvalueMode::Load;

        // 初始化模块编译上下文（建入口函数 + set_entry + 构造 ModuleCtx，含创建入口 fn 上下文与游标就位）。
        // 须在 module 已根化下调用(上方 module_guard)。
        const auto entry = init_module(module);

        try {
            // 遍历顶层声明（顶层 var/fun/import -> 模块全局；嵌套块内 var -> 局部）。
            for (const auto& decl: program.declarations) {
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

    // 建模块入口函数（arity 0、名 `<main>`，主入口模块体包装）+ set_entry + 构造 ModuleCtx（创建入口 fn 上下文、
    // 游标就位），返回入口函数。须在 module 已根化下调用（compile() 的 module_guard）；ModuleCtx 构造期 ASSERT
    // entry 非空（此处先 set_entry）。工厂不再替调用方守卫入参,故 `<main>` 名须显式 make_guard 跨 new_function 的
    // new_object。 （运行期导入模块的入口函数名 `<module>` 见上 kModuleName,由未来导入编译路径产出,当前路径统一产
    // `<main>`。）
    ObjFunction* CodeGen::init_module(ObjModule& module) {
        const auto name  = new_string(gc_, kMainName);
        const auto guard = gc_.make_guard(name);
        const auto entry = new_function(gc_, &module, name, 0);
        module.set_entry(entry);
        mod_ctx_ = std::make_unique<ModuleCtx>(module); // 创建入口 fn 上下文并就位游标
        return entry;
    }

    // 当前 CodeUnit = 当前函数 fn_->unit()，随游标派生（定义于此：需 ObjFunction 完整类型取 unit()）。
    FunctionCtx* CodeGen::cur_fn_ctx() const noexcept { return mod_ctx_->current_fn_ctx_; }

    CodeUnit* CodeGen::cur_cu() const noexcept { return &cur_fn_ctx()->fn_->unit(); }

    // ============================================================
    // 常量池辅助（emit 编码已下沉 CodeUnit，调用方经 cur_cu()->emit_* 直接发射）
    // 单层 _or_fail：操作 + 失败即 fail（持 loc，[[noreturn]]）并返回解包值。原薄封装透传层（add_constant/
    // add_name 仅作操作 + 失败信号、不持 loc）唯一消费者即对应 _or_fail，透传空转，故内联至此。
    // add_name_or_fail 经 add_constant_or_fail 复用溢出检查，免拷「溢出检查 + add_constant」逻辑。
    // ============================================================

    u16 CodeGen::add_constant_or_fail(const Value value, const SourceLoc& loc) const {
        // 常量池溢出(>kMaxConstants) -> fail CodeUnitTooLarge。CodeUnit::add_constant 内部亦有
        // ASSERT(size < 65536) 兜底，本预检保证永不触达。失败即 fail（[[noreturn]]），之后 add_constant 恒成功。
        if (cur_cu()->constants.size() > kMaxConstants) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "常量池溢出(>{})", kMaxConstants);
        }
        return cur_cu()->add_constant(value);
    }

    u16 CodeGen::add_name_or_fail(const StringView name, const SourceLoc& loc) const {
        // intern name 成 ObjString 并入常量池，返回索引。new_string 结果立即 add_constant_or_fail
        // （trivial push 不触发 GC，见 GC.hpp 核心不变式），无需守卫。溢出由 add_constant_or_fail fail。
        const auto str = new_string(gc_, name);
        return add_constant_or_fail(Value::from_obj(str), loc);
    }

    // ============================================================
    // 局部管理（登记经 FunctionCtx，发射经 cur_cu()）
    // ============================================================

    void CodeGen::begin_scope() const { cur_fn_ctx()->begin_scope(); }

    void CodeGen::end_scope(const u32 line) const {
        const u32 n = cur_fn_ctx()->end_scope_pop_count();
        cur_cu()->emit_pop_n(n, line);
    }

    void CodeGen::pop_locals_to(const u32 target_depth, const u32 line) const {
        // 仅计数并 emit POP_N(运行期弹栈),不破坏编译期 locals_ 登记:break/continue 后的语句仍在作用域内,
        // 可引用这些局部;只有 end_scope 才真正 pop_locals_deeper_than 移除。
        const u32 n = cur_fn_ctx()->count_locals_deeper_than(target_depth);
        cur_cu()->emit_pop_n(n, line);
    }

    u16 CodeGen::declare_local_or_fail(const StringView name, const SourceLoc& loc) const {
        // 同作用域重名 -> RedefinedVariable（外层同名允许 shadow）；溢出 -> TooManyLocals。
        // 仅登记并标「定义但未初始化」（add_local 置 is_initialized=false），不发任何指令。
        // 调用方在初始化器求值 / 无初始化器发 LOAD_NIL 后 mark_initialized。失败即 fail（[[noreturn]]）。
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

    CodeGen::ResolvedVar CodeGen::resolve_name_or_fail(const StringView name, const SourceLoc& loc) {
        // 裸名解析：当前函数局部命中 -> Local（index=局部槽）；外层函数局部 -> Upvalue（M4 未实现，调用方
        // emit_load_var/emit_store_var 走 not_impl 报编译期错--不静默落到全局，否则外层局部与同名模块全局
        // 串台致闭包捕获错误变量，见 CLAUDE.md「作用域模型」）；否则视为模块全局（VM 运行期 LOAD_GLOBAL
        // 查表，未定义报 UndefinedVariable）。Global 分支经 add_name_or_fail 入池，溢出即 fail（持 loc）。
        if (const auto local_idx = cur_fn_ctx()->find_local(name)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Local, .index = *local_idx};
        }
        for (auto ctx = cur_fn_ctx()->enclosing_; ctx != nullptr; ctx = ctx->enclosing_) {
            if (ctx->find_local(name)) {
                return ResolvedVar{.kind = ResolvedVar::Kind::Upvalue, .index = 0};
            }
        }
        const auto name_idx = add_name_or_fail(name, loc);
        return ResolvedVar{.kind = ResolvedVar::Kind::Global, .index = name_idx};
    }

    // ============================================================
    // 跳转回填 / 全局登记失败翻译（void：仅翻译失败，无解包）
    // 与上面 _or_fail 同一职责约定（操作 + 失败即 fail），但底层方法返 bool（patch_jump/emit_jump_back/
    // declare_global），无解包值，故为 void 封装。文案收口于此。
    // ============================================================

    void CodeGen::patch_jump_or_fail(const usize src_off, const SourceLoc& loc) const {
        // patch_jump 越界(跳转偏移超 u16 上限) -> fail CodeUnitTooLarge「跳转偏移超过 64KB」。
        // 与上面 _or_fail 同一职责约定;patch_jump 无返回值,故本封装 void(仅翻译失败,无解包)。
        if (!cur_cu()->patch_jump(src_off)) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "跳转偏移超过 64KB");
        }
    }

    void CodeGen::emit_jump_back_or_fail(const u32 target_off, const u32 line, const SourceLoc& loc) const {
        // emit_jump_back 越界(回边偏移超 u16 上限/反向) -> fail CodeUnitTooLarge「回边偏移超过 64KB」。
        // 同 patch_jump_or_fail:void 封装,仅翻译失败。line 供 JUMP_BACK 发射行号(patch_jump 不发射故无)。
        if (!cur_cu()->emit_jump_back(target_off, line)) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "回边偏移超过 64KB");
        }
    }

    void CodeGen::declare_global_or_fail(const StringView name, const SourceLoc& loc) const {
        // declare_global 重定义 -> fail RedefinedVariable。void 封装(declare_global 返 bool,无解包),
        // 同 patch_jump_or_fail/emit_jump_back_or_fail;与 declare_local_or_fail 对称,文案收口于此。
        if (!mod_ctx_->declare_global(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "重复定义全局变量: {}", name);
        }
    }

    // ============================================================
    // lvalue / 局部槽 load-store
    // ============================================================

    void CodeGen::check_local_initialized(const u16 slot, const SourceLoc& loc) const {
        // 读点 init 检查：使用定义但未初始化的局部 -> UninitializedVariable（Python 风格 definite-assignment）。
        if (!cur_fn_ctx()->is_initialized(slot)) {
            fail(ErrorCode::UninitializedVariable, loc, "使用未初始化的变量: {}", cur_fn_ctx()->locals_[slot].name);
        }
    }

    void CodeGen::validate_lvalue_target(ExprNode* target) const {
        // 赋值左值种类合法性：Identifier/FieldAccess/IndexAccess 是合法左值种类，放行（未实现的由各自
        // visit 节点在被 emit_lvalue 分派时 not_impl）；其余节点种类 -> InvalidAssignmentTarget。
        // 由 emit_lvalue 在分派前调用：复合/前置自增自减的首次 emit_lvalue(Load) 先于 rhs，故彼等先于 rhs
        // 报；普通 = 的 emit_lvalue(Store) 后于 rhs，非法左值在 rhs 编译后才抛（字节码随 throw 丢弃）。
        if (dynamic_cast<IdentifierNode*>(target) != nullptr) {
            return;
        }
        if (dynamic_cast<FieldAccessNode*>(target) != nullptr) {
            return;
        }
        if (dynamic_cast<IndexAccessNode*>(target) != nullptr) {
            return;
        }
        fail(ErrorCode::InvalidAssignmentTarget, target->loc(), "非法赋值左值");
    }

    void CodeGen::emit_lvalue(ExprNode* n, const LvalueMode m) {
        // 验证左值种类后设置模式并分派。目标节点入口经 take_lvalue_mode() 取值并清空为 Load，故子节点经
        // emit_expr 时 flag 已清空（emit_expr 入口 ASSERT 之为 Load）。不在分派后恢复--清空职责在
        // take_lvalue_mode，漏 take 会被 emit_expr 的 ASSERT 在开发期捕获。
        validate_lvalue_target(n);
        lvalue_mode_ = m;
        n->accept(*this);
    }

    CodeGen::LvalueMode CodeGen::take_lvalue_mode() {
        // 取当前 lvalue_mode_ 并清空为 Load（一次性 take）。访问节点据此返回值分支 Load/Store，不直接读写
        // lvalue_mode_；清空确保子节点经 emit_expr 时 flag 已为 Load。
        return std::exchange(lvalue_mode_, LvalueMode::Load);
    }

    // 在栈顶 receiver 上调用 0 参方法 name：LOAD_FIELD name; CALL 0。receiver 由调用方在调用前压栈
    // （emit_expr / emit_load_local 等），调用后栈顶即方法返回值（[receiver] -> [retval]）。封装 for-in
    // 的 iter()/has_next()/next() 三处同型 LOAD_FIELD+CALL 0 模式；name 入常量池经 add_name_or_fail。
    void CodeGen::emit_method_call0(const StringView name, const u32 line, const SourceLoc& loc) const {
        cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
        const auto name_idx = add_name_or_fail(name, loc);
        cur_cu()->emit_word(name_idx, line);
        cur_cu()->emit_op(OpCode::CALL, line);
        cur_cu()->emit_byte(0, line);
    }

    // 按已解析变量发射读取（Load / Locate）：Local 先读点 init 检查再 emit_load_local（未初始化 ->
    // UninitializedVariable）；Global LOAD_GLOBAL（VM 运行期查表）；Upvalue -> not_impl（M4 闭包未实现）。
    // visitIdentifierNode 经 switch(mode) 分派至此。var.index 为局部槽或全局名字常量池索引；loc 供
    // check_local_initialized / not_impl（走其 SourceLoc 重载）复用。
    void CodeGen::emit_load_var(const ResolvedVar& var, const u32 line, const SourceLoc& loc) const {
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
                not_impl(loc, "闭包/upvalue 捕获");
        }
        UNREACHABLE();
    }

    // 按已解析变量发射写入（Store，peek-store 留栈顶值）：Local emit_store_local + mark_initialized（赋值即
    // 初始化，不做 init 检查）；Global STORE_GLOBAL（VM 运行期查表）；Upvalue -> not_impl（M4 闭包未实现）。
    void CodeGen::emit_store_var(const ResolvedVar& var, const u32 line, const SourceLoc& loc) const {
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
                not_impl(loc, "闭包/upvalue 捕获");
        }
        UNREACHABLE();
    }

    // ============================================================
    // 模式绑定
    // ============================================================

    void CodeGen::bind_pattern(PatternNode* pat) {
        // 栈顶已有一值（for-in 的 next() 产物），按模式绑定为 per-iteration 局部。值填槽模型：
        // 声明发生在值已在栈顶之时，slot = 当前栈高 = 值所在位置，值即该局部（无 STORE_LOCAL/POP）。
        if (const auto id = dynamic_cast<IdentifierPatternNode*>(pat)) {
            const auto slot = declare_local_or_fail(id->name, pat->loc()); // 纯登记，slot = 值位置；值填槽不发指令
            cur_fn_ctx()->mark_initialized(slot);                          // 值已在槽
            return;
        }
        const u32 line = pat->loc_line();
        if (dynamic_cast<WildcardPatternNode*>(pat) != nullptr) {
            cur_cu()->emit_op(OpCode::POP, line);
            return;
        }
        if (dynamic_cast<ListPatternNode*>(pat) != nullptr) {
            not_impl(pat, "列表模式解构");
        }
        not_impl(pat, "未知模式");
    }

    // ============================================================
    // 遍历入口
    // ============================================================

    // 遍历入口：accept 双分派；出错时 visit 内 fail() 抛 AriaCompileException 自动 unwind，无需 ok() 短路。
    void CodeGen::emit_expr(ExprNode* n) {
        // rvalue 上下文恒 Load：emit_lvalue 分派后必恢复为 Load。断言（非预防性赋值）以在开发期捕获漏恢复。
        ASSERT(lvalue_mode_ == LvalueMode::Load,
               "lvalue_mode_ 应为 Load（rvalue 上下文）；非 Load 表明 emit_lvalue 分派后未恢复");
        n->accept(*this);
    }

    void CodeGen::emit_stmt(StmtNode* n) { n->accept(*this); }

    // ============================================================
    // 函数编译（FunDecl / Lambda 共用）
    // ============================================================

    void CodeGen::validate_params(const List<Param>& params, const SourceLoc& loc) const {
        // 形参合法性检查（FunDecl / Lambda 共用，compile_function 编译体前调用）：
        //   >kMaxArity -> TooManyParameters；默认参数 / varargs -> not_impl；形参重名 -> DuplicateParam。
        // 只读 params、不触碰编译器状态（无 cur_cu / cur_fn_ctx / GC 依赖），首错即 fail / not_impl 抛出，
        // 与原内联检查同一职责与顺序，首错即止行为不变。loc 为声明节点位置（fun 关键字，compile_function
        // 经 decl_loc 传入）而非 body->loc()（body 的 '{'），更贴近参数列表所在；只需位置无需整节点，故入参
        // 为 const SourceLoc& 而非 ASTNode*（not_impl 走其 SourceLoc 重载）。
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

    void CodeGen::compile_function(const StringView name, const List<Param>& params, BlockNode* body,
                                   const SourceLoc& decl_loc) {
        // 参数合法性检查：decl_loc 为声明节点位置（fun 关键字，visit 层经 node->loc() 传入），非 body 的 '{'，
        // 供 validate_params 报参数错；先于 new_function 等分配，失败即抛 AriaCompileException 跳过下方所有
        // 发射与分配。首错即止行为与检查顺序与原内联实现一致。下方体发射行号仍取 body->loc_line()。
        validate_params(params, decl_loc);

        const auto loc  = body->loc();
        const u32  line = body->loc_line();

        // name_str 在下方 new_function 调用中可能被回收,故 make_guard 保护
        const auto name_str   = new_string(gc_, name);
        auto       name_guard = gc_.make_guard(name_str);
        const auto fn         = new_function(gc_, mod_ctx_->module_, name_str, static_cast<u8>(params.size()));
        // fn 创建后跨 add_constant 无需守卫:constants.push -> reallocate 走 trivial 分配不触发 GC(见 GC.hpp
        // 核心不变式);入池后即经 module 根链可达。
        const auto fn_idx = add_constant_or_fail(Value::from_obj(fn), loc);
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(fn_idx, line);

        // lambda(name == `<anonymous>`)留栈作表达式值不绑定，故可以跳过;具名 fun 绑定全局/局部。
        if (name != kAnonymousName) {
            if (mod_ctx_->is_global_scope()) {
                // 顶层 fun -> 模块全局
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
        const auto child          = new FunctionCtx{*cur_fn_ctx(), *fn};
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

        // 成功:还原父游标(cu 自动回父 unit)并 delete 子上下文。
        mod_ctx_->current_fn_ctx_ = child->enclosing_;
        delete child;
    }

    // ============================================================
    // not_impl
    // ============================================================

    void CodeGen::not_impl(ASTNode* node, StringView feature) const {
        fail(ErrorCode::NotImplemented, node->loc(), "{} 尚未支持", feature);
    }

    void CodeGen::not_impl(const SourceLoc& loc, StringView feature) const {
        // loc 直接传入（调用方仅有 SourceLoc 而无节点时用，如 validate_params）。与 ASTNode* 重载同一消息格式。
        fail(ErrorCode::NotImplemented, loc, "{} 尚未支持", feature);
    }

    // ============================================================
    // 根节点
    // ============================================================

    void CodeGen::visitProgramNode(ProgramNode* node) {
        // 仅编排顶层声明，本节点不直接发射（行号由各子节点自持）。
        for (const auto& decl: node->declarations) {
            emit_stmt(decl.get());
        }
    }

    // ============================================================
    // 语句节点
    // ============================================================

    void CodeGen::visitBlockNode(BlockNode* node) {
        const u32 line = node->loc_line();
        begin_scope();
        for (const auto& stmt: node->statements) {
            emit_stmt(stmt.get());
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
            patch_jump_or_fail(jf, node->loc());                       // -> else
            emit_stmt(node->else_branch.get());
            patch_jump_or_fail(jend, node->loc()); // -> end
        } else {
            patch_jump_or_fail(jf, node->loc()); // -> end
        }
    }

    void CodeGen::visitWhileStmtNode(WhileStmtNode* node) {
        const u32 line    = node->loc_line();
        const u32 l_start = cur_cu()->size();
        emit_expr(node->condition.get());
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

        auto loop_ctx                 = make_loop_ctx(cur_fn_ctx()->scope_depth_);
        loop_ctx.continue_back_target = l_start; // continue 后向跳 L_start
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(node->body.get());
        const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

        emit_jump_back_or_fail(l_start, line, node->loc());
        patch_jump_or_fail(jf, node->loc()); // -> L_end
        for (const auto bp: loop.break_fwd_patches) {
            patch_jump_or_fail(bp, node->loc());
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
        const bool has_incr = node->increment != nullptr;
        Opt<usize> jf; // 条件假跳 L_end 占位；无 cond 时留空（永不回填）
        if (has_cond) {
            emit_expr(node->condition.get());
            jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end
        }
        // continue: 有 incr -> 前向跳 L_incr（回填）；无 incr -> 后向跳 L_cond。
        auto loop_ctx = make_loop_ctx(loop_scope);
        if (!has_incr) {
            loop_ctx.continue_back_target = l_cond; // 无 incr: continue 后向跳 L_cond
        } // 有 incr: 留空，走前向 continue_fwd_patches -> L_incr
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(node->body.get());
        const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

        // continue（前向）须回填到 L_incr：此刻 cur_cu()->size() 即递增区起点，且须先于递增发射--
        // 若等递增与 JUMP_BACK 发完再回填，cur_cu()->size() 已是 L_end，continue 会错跳到 L_end 提前出循环。
        for (const auto cp: loop.continue_fwd_patches) {
            patch_jump_or_fail(cp, node->loc()); // -> L_incr
        }
        if (has_incr) {
            emit_expr(node->increment.get());
            cur_cu()->emit_op(OpCode::POP, line);
        }
        emit_jump_back_or_fail(l_cond, line, node->loc());

        if (jf) {
            patch_jump_or_fail(*jf, node->loc()); // -> L_end
        }
        for (const auto bp: loop.break_fwd_patches) {
            patch_jump_or_fail(bp, node->loc()); // -> L_end
        }
        end_scope(line);
    }

    void CodeGen::visitForInStmtNode(ForInStmtNode* node) {
        const u32 line = node->loc_line();
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
        emit_expr(node->iterable.get());              // [iterable]（receiver）
        emit_method_call0("iter", line, node->loc()); // [iter_obj] 恰在 slot 位置
        const u16 iter_var_slot = declare_local_or_fail("<iter>", node->loc());
        cur_fn_ctx()->mark_initialized(iter_var_slot); // 值已在槽

        const u32 l_start = cur_cu()->size();
        cur_cu()->emit_load_local(iter_var_slot, line);                // [iter]（receiver）
        emit_method_call0("has_next", line, node->loc());              // [bool]
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

        auto loop_ctx                 = make_loop_ctx(loop_scope);
        loop_ctx.continue_back_target = l_start; // continue 后向跳 L_start（has_next 判断处）
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));

        // per-iteration scope：pattern + 体每轮 fresh（值填槽）。体经 emit_stmt 作为不透明子节点，
        // 若为 block 则自带更深层 scope；break/continue 跳出时由 pop_locals_to(loop_scope) 代弹。
        begin_scope();
        cur_cu()->emit_load_local(iter_var_slot, line); // [iter]（receiver）
        emit_method_call0("next", line, node->loc());   // [value] 恰在 slot 位置
        bind_pattern(node->pattern.get());              // id: declare 值填槽（不发指令）/ _: POP 丢弃
        emit_stmt(node->body.get());
        end_scope(line); // per-iter：POP_N 弹 pattern（id）；_ 无局部 -> emit_pop_n(0) 无指令

        const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

        emit_jump_back_or_fail(l_start, line, node->loc());
        patch_jump_or_fail(jf, node->loc()); // -> L_end
        for (const auto bp: loop.break_fwd_patches) {
            patch_jump_or_fail(bp, node->loc());
        }
        end_scope(line); // for-in：POP_N 弹 <iter>
    }

    void CodeGen::visitBreakStmtNode(BreakStmtNode* node) {
        const u32 line = node->loc_line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::BreakOutsideLoop, node->loc(), "break 不在循环内");
        }
        auto& loop = cur_fn_ctx()->loop_stack_.top();
        pop_locals_to(loop.loop_scope_depth, line);
        loop.break_fwd_patches.push_back(cur_cu()->emit_jump(OpCode::JUMP, line)); // -> L_end（回填）
    }

    void CodeGen::visitContinueStmtNode(ContinueStmtNode* node) {
        const u32 line = node->loc_line();
        if (cur_fn_ctx()->loop_stack_.empty()) {
            fail(ErrorCode::ContinueOutsideLoop, node->loc(), "continue 不在循环内");
        }
        auto& loop = cur_fn_ctx()->loop_stack_.top();
        pop_locals_to(loop.loop_scope_depth, line);
        if (loop.continue_back_target) {
            emit_jump_back_or_fail(*loop.continue_back_target, line, node->loc());
        } else {
            loop.continue_fwd_patches.push_back(cur_cu()->emit_jump(OpCode::JUMP, line)); // -> L_incr（回填）
        }
    }

    void CodeGen::visitReturnStmtNode(ReturnStmtNode* node) {
        const u32 line = node->loc_line();
        // 入口 <main> 亦为函数，故顶层 return 合法（cur_fn_ctx()->fn_ 恒非空）。
        if (node->value != nullptr) {
            emit_expr(node->value.get());
        } else {
            cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        }
        cur_cu()->emit_op(OpCode::RETURN, line);
    }

    void CodeGen::visitImportStmtNode(ImportStmtNode* node) {
        const u32 line = node->loc_line();
        // IMPORT path:u16 压模块值于栈顶；绑定按作用域走（与 var/fun 同形 lowering）：
        //   顶层 -> DEF_GLOBAL alias（弹值定义全局）；嵌套 -> 值填槽（IMPORT 压 [module] 在下个 slot
        //   位置，declare 登记该 slot + mark_initialized），无 STORE_LOCAL。对齐文法「绑模块到当前作用域
        //   （函数体=局部）」。
        // path 入池 + IMPORT 压模块值于栈顶（两分支共用）。path/alias 经 add_name_or_fail：new_string(intern)
        // 结果立即 add_constant 入池（trivial push 不触发 GC，见 GC.hpp 核心不变式），无需守卫。
        const auto path_idx = add_name_or_fail(node->path, node->loc());
        cur_cu()->emit_op(OpCode::IMPORT, line);
        cur_cu()->emit_word(path_idx, line); // [module]
        if (mod_ctx_->is_global_scope()) {
            // 顶层 import -> 模块全局（declare_global 内容判重；DEF_GLOBAL 弹值定义）。path 已先入池
            // 经 module 根链可达，故 alias 的 new_string 不会回收已入池的 path。
            declare_global_or_fail(node->alias, node->loc());
            const auto alias_idx = add_name_or_fail(node->alias, node->loc());
            cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
            cur_cu()->emit_word(alias_idx, line); // []  弹值定义全局
        } else {
            // 嵌套 import -> 局部（值填槽：IMPORT 已压 [module] 在栈顶 = 下个 slot 位置，declare 登记该
            // slot 并 mark_initialized；无 STORE_LOCAL/POP。与 for-in <iter> 值填槽同形）。
            const auto slot = declare_local_or_fail(node->alias, node->loc());
            cur_fn_ctx()->mark_initialized(slot); // 值已在槽
        }
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
        // 顶层 fun：name 经 compile_function 内部 intern + make_guard（每方只守自己创建的），故此处只传 StringView。
        // node->loc() 传作 decl_loc，供 compile_function -> validate_params 取声明节点 loc（fun 关键字）报参数错。
        compile_function(node->name, node->params, node->body.get(), node->loc());
    }

    void CodeGen::visitDefDeclNode(DefDeclNode* node) { not_impl(node, "def 类与对象"); }

    void CodeGen::visitVarDeclNode(VarDeclNode* node) {
        const u32 line = node->loc_line();
        for (const auto& [target, initializer]: node->bindings) {
            // 仅 IdentifierPattern 可跑；ListPattern -> not_impl。
            const auto id = dynamic_cast<IdentifierPatternNode*>(target.get());
            if (id == nullptr) {
                not_impl(target.get(), "列表模式解构 var 声明");
            }
            if (mod_ctx_->is_global_scope()) {
                // 顶层 var -> 模块全局（declare_global 内容判重；DEF_GLOBAL 弹值定义）。name 经
                // add_name_or_fail 在 emit_expr 之后入池：new_string(intern) 结果立即 add_constant
                // （trivial push 不触发 GC，见 GC.hpp 核心不变式），无需守卫；与 visitImportStmtNode
                // 顶层分支同形。
                declare_global_or_fail(id->name, id->loc());
                if (initializer != nullptr) {
                    emit_expr(initializer.get());
                } else {
                    cur_cu()->emit_op(OpCode::LOAD_NIL, line);
                }
                const auto name_idx = add_name_or_fail(id->name, id->loc());
                cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
                cur_cu()->emit_word(name_idx, line);
            } else {
                // 嵌套 var -> 局部（值填槽：declare 仅登记标未初始化；初始化器值恰好压在 slot 即该局部，
                // 无 store/pop；无初始化器则 LOAD_NIL 填槽。最后 mark_initialized）
                const auto slot = declare_local_or_fail(id->name, id->loc());
                if (initializer != nullptr) {
                    emit_expr(initializer.get()); // 值恰好压在 slot（不变式：declare 与 init 相邻）
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

    void CodeGen::visitIntegerLiteralNode(IntegerLiteralNode* node) {
        const u32 line = node->loc_line();
        const i64 v    = node->value;
        if (v >= -128 && v <= 127) {
            cur_cu()->emit_op(OpCode::LOAD_IMM, line);
            cur_cu()->emit_byte(static_cast<u8>(static_cast<i8>(v)), line);
            return;
        }
        if (v < kIntMin || v > kIntMax) {
            fail(ErrorCode::NumberOutOfRange, node->loc(), "整数字面量超出 i48 范围: {}", v);
            return;
        }
        const auto idx = add_constant_or_fail(Value::from_int(v), node->loc());
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    void CodeGen::visitFloatLiteralNode(FloatLiteralNode* node) {
        const u32  line = node->loc_line();
        const auto idx  = add_constant_or_fail(Value::from_f64(node->value), node->loc());
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
    }

    void CodeGen::visitStringLiteralNode(StringLiteralNode* node) {
        const u32  line = node->loc_line();
        const auto idx  = add_name_or_fail(node->value, node->loc());
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
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
        // 入口 take：取模式并清空为 Load（子节点经 emit_expr 时已为 Load）。按 mode 分派到 emit_load_var /
        // emit_store_var，两者各自按 var.kind 发射 Local/Global/Upvalue。Locate 预留（Identifier 无 receiver，
        // 同 Load）；Upvalue 走 not_impl（M4 闭包）。
        const auto mode     = take_lvalue_mode();
        const u32  line     = node->loc_line();
        const auto resolved = resolve_name_or_fail(node->name, node->loc());
        switch (mode) {
            case LvalueMode::Load:
            case LvalueMode::Locate:
                emit_load_var(resolved, line, node->loc());
                return;
            case LvalueMode::Store:
                emit_store_var(resolved, line, node->loc());
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::visitThisExprNode(ThisExprNode* node) { not_impl(node, "this（类与对象）"); }

    void CodeGen::visitSuperExprNode(SuperExprNode* node) { not_impl(node, "super（类与对象）"); }

    void CodeGen::visitBinaryExprNode(BinaryExprNode* node) {
        const u32 line = node->loc_line();
        emit_expr(node->lhs.get());
        // 短路逻辑运算：lhs 真假跳留值、跳过 rhs；否则弹 lhs 求 rhs。跳转回填到 rhs 之后（L_end）。
        if (node->op == Op::Binary::Or) {
            const auto j = cur_cu()->emit_jump(OpCode::JUMP_TRUE_OR_POP, line);
            emit_expr(node->rhs.get());
            patch_jump_or_fail(j, node->loc()); // -> L_end（rhs 之后）
            return;
        }
        if (node->op == Op::Binary::And) {
            const auto j = cur_cu()->emit_jump(OpCode::JUMP_FALSE_OR_POP, line);
            emit_expr(node->rhs.get());
            patch_jump_or_fail(j, node->loc()); // -> L_end（rhs 之后）
            return;
        }
        // 算术 / 比较：lhs、rhs 各求值一次，再发射 op。
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
            default:
                UNREACHABLE();
        }
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
                // E += 1 / E -= 1。统一压 +1，由 ADD/SUBTRACT 决定方向--
                // 若 PreDec 压 -1 再 SUBTRACT 会算成 E - (-1) = E + 1，方向反。
                emit_lvalue(node->operand.get(), LvalueMode::Load);
                cur_cu()->emit_op(OpCode::LOAD_IMM, line);
                cur_cu()->emit_byte(1, line);
                cur_cu()->emit_op(node->op == Op::Unary::PreInc ? OpCode::ADD : OpCode::SUBTRACT, line);
                emit_lvalue(node->operand.get(), LvalueMode::Store); // peek-store 留新值
                return;
            }
            default:
                UNREACHABLE();
        }
    }

    void CodeGen::visitAssignmentNode(AssignmentNode* node) {
        const u32 line = node->loc_line();
        // 左值种类验证（InvalidAssignmentTarget / Field/Index not_impl）由 emit_lvalue 在分派前完成。
        if (node->op == Op::Assignment::Assign) {
            // 普通 =：value -> store（peek-store 留值）
            emit_expr(node->value.get());
            emit_lvalue(node->target.get(), LvalueMode::Store);
            return;
        }
        // 复合赋值：load target -> value -> op -> store target（Identifier 重 resolve 廉价，locator-once 自然成立）
        emit_lvalue(node->target.get(), LvalueMode::Load);
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
            default:
                UNREACHABLE();
        }
        emit_lvalue(node->target.get(), LvalueMode::Store);
    }

    void CodeGen::visitDestructureAssignmentNode(DestructureAssignmentNode* node) { not_impl(node, "解构赋值"); }

    void CodeGen::visitCallNode(CallNode* node) {
        const u32 line = node->loc_line();
        // 实参上限 kMaxArguments（CALL 操作数 u8）：先检后发，避免 emit 完数百个实参表达式才报错。
        if (node->args.size() > kMaxArguments) {
            fail(ErrorCode::TooManyArguments, node->loc(), "实参数超过 {}", kMaxArguments);
        }
        emit_expr(node->callee.get());
        for (const auto& arg: node->args) {
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
        patch_jump_or_fail(jf, node->loc());                       // -> else
        emit_expr(node->else_branch.get());
        patch_jump_or_fail(jend, node->loc()); // -> end
    }

    void CodeGen::visitLambdaExprNode(LambdaExprNode* node) {
        // lambda 名 `<anonymous>`（`<>` 标识符不可用，具独特辨识度）；compile_function 据名 == `<anonymous>`
        // 判定 lambda -> 函数值留栈不绑定名字。name 经 compile_function 内部 intern + make_guard，故此处只传
        // StringView。node->loc() 传作 decl_loc，供 compile_function -> validate_params 取声明节点 loc 报参数错。
        compile_function(kAnonymousName, node->params, node->body.get(), node->loc());
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
