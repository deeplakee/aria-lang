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

        // 二元 op -> 发射 OpCode（visitBinaryExprNode 与复合赋值共用单源）。域为 13 个值产
        // op；Or/And 走短路分支（emit_jump + JUMP_*_OR_POP 留值跳转）不经此，OpCode 亦无
        // 单条逻辑码 -> UNREACHABLE。
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

        // 复合赋值 op -> 对应二元 op（经 binary_opcode 与二元表达式共用发射）。文法仅算术
        // 五种复合（Parser::assignment_op 为咽喉点）；= 不入表（visitAssignmentNode 直走 store）。
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
    } // namespace

    // ============================================================
    // 入口
    // ============================================================

    // 静态服务入口：一次性对象上跑单次编译（契约见 CodeGen.hpp compile 注）。
    Result<ObjFunction*, Error> CodeGen::compile(GC& gc, const ProgramNode& program, ObjModule* module,
                                                 const StringView entry_name) {
        CodeGen self{gc};
        return self.generate_bytecode(program, module, entry_name);
    }

    Result<ObjFunction*, Error> CodeGen::generate_bytecode(const ProgramNode& program, ObjModule* module,
                                                           const StringView entry_name) {
        // module 入临时根贯穿全程（根化链与各守卫窗口见类首「GC 安全」注）。
        const auto guard = gc_.make_guard(module);

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
            // 出错即 unwind 到此：随一次性对象析构，~ModuleCtx 沿 enclosing_ 链释放
            // 入口 + 出错未还原的子上下文。
            return std::unexpected(e.error());
        }

#ifdef DEBUG_PRINT_COMPILED_CODE
        // 打印入口的 CodeUnit 反汇编（游标仍在入口，cur_cu() 即入口 unit）。
        io::println(stderr, "{}", cur_cu()->disassemble(entry_name));
#endif

        // mod_ctx_ 随一次性对象析构释放（~ModuleCtx 沿 enclosing_ 链），无需显式 reset。
        return entry;
    }

    // 建模块入口函数 + set_entry + 构造 ModuleCtx（契约见 CodeGen.hpp init_module 注）。
    ObjFunction* CodeGen::init_module(ObjModule* module, const StringView entry_name) {
        const auto entry = new_function(gc_, module, entry_name, 0);
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

    u16 CodeGen::define_local_or_fail(const StringView name, const SourceLoc loc) const {
        // 同作用域重名 -> RedefinedVariable（外层同名允许 shadow）；溢出 -> TooManyLocals。
        // 仅登记不发指令（值填槽：调用方保证值已压栈、登记槽位即值位置，登记即初始化）。
        if (cur_fn_ctx()->is_defined_in_scope(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "重复定义局部变量: {}", name);
        }
        if (cur_fn_ctx()->locals_.size() > kMaxLocals) {
            fail(ErrorCode::TooManyLocals, loc, "局部变量过多(>{})", kMaxLocals);
        }
        return cur_fn_ctx()->add_local(name); // 纯登记
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

    CodeGen::ResolvedVar CodeGen::resolve_this_or_fail(const SourceLoc loc) {
        // this 专用解析（契约见 CodeGen.hpp 注）：链上找不到实例方法即 fail，永不落全局。
        if (const auto slot = cur_fn_ctx()->find_local(kThisName)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Local, .index = *slot};
        }
        if (const auto upvalue_idx = resolve_upvalue(cur_fn_ctx(), kThisName, loc)) {
            return ResolvedVar{.kind = ResolvedVar::Kind::Upvalue, .index = *upvalue_idx};
        }
        fail(ErrorCode::ThisOutsideClass, loc, "this 不在实例方法内");
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

    void CodeGen::bind_stack_value(const StringView name, const SourceLoc loc) const {
        // 契约见 CodeGen.hpp bind_stack_value 注；行号就地取 loc（声明行）。
        const u32 line = static_cast<u32>(loc.line());
        if (mod_ctx_->is_global_scope()) {
            declare_global_or_fail(name, loc);
            const auto name_idx = add_name_or_fail(name, loc);
            cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
            cur_cu()->emit_word(name_idx, line); // 弹值定义全局
        } else {
            define_local_or_fail(name, loc); // 值填槽：值恰在 locals_.size() 槽位，登记即初始化
        }
    }

    // ============================================================
    // lvalue / 局部槽 load-store
    // ============================================================

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

    bool CodeGen::is_in_method() const { return is_method(cur_fn_ctx()->kind_); }

    void CodeGen::emit_load_var(const ResolvedVar& var, const u32 line) const {
        // 契约见 CodeGen.hpp emit_load_var 注。
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
                cur_cu()->emit_byte(static_cast<u8>(slot), line); // 索引域由 add_upvalue 容量检查保证 <= u8
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::emit_store_var(const ResolvedVar& var, const u32 line) const {
        // 契约见 CodeGen.hpp emit_store_var 注。
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
            // 值填槽：declare 登记的 slot 即值位置，不发指令
            define_local_or_fail(id->name, node.loc());
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

    void CodeGen::emit_expr_or_nil(ExprNode* expr, const u32 line) {
        if (expr != nullptr) {
            emit_expr(*expr);
        } else {
            cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        }
    }

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
                                   const SourceLoc decl_loc, const FnKind kind) {
        // 参数合法性检查先于 new_function 等分配：失败即抛 AriaCompileException，跳过下方所有发射与分配。
        validate_params(params, decl_loc);

        // 声明区发射（CLOSURE/绑定/注册）行号与报错位置统一取声明处 decl_loc--声明语句的执行点在
        // 声明首 token（fun 关键字/成员名,与 var/def/import 绑定取语句行的惯例一致）;体尾隐式返回
        // 属体区,行号取 body。
        const u32 decl_line = static_cast<u32>(decl_loc.line());
        const u32 line      = body.loc_line();

        const auto fn = new_function(gc_, mod_ctx_->module_, name, static_cast<u8>(params.size()));
        // 入池后即经 module 根链可达（trivial 窗口见类首 GC 安全注）。
        const auto fn_idx = add_constant_or_fail(Value::from_obj(fn), decl_loc);
        // CLOSURE fn_idx:VM 执行时现场包 ObjClosure,按捕获描述表(下方 flush 进
        // fn->upvalue_descs_)逐个建/复用 upvalue(表在元数据不进字节码流,CLOSURE 定长 3B)。
        cur_cu()->emit_op(OpCode::CLOSURE, decl_line);
        cur_cu()->emit_word(fn_idx, decl_line);

        // kind 全分派（穷尽 switch,-Wswitch 提示漏项）：具名 fun（Function）绑定到全局（顶层）或
        // 局部（嵌套,值填槽）;Lambda 留栈作表达式值不绑定;方法三态留栈不绑定,就地注册——fun 静态
        // MAKE_STATIC 不戳 defining class（静态槽读恒原值）,实例方法族 MAKE_METHOD 戳（VM 侧方法性
        // 标记 + super 来源）。分派都在 CLOSURE 之后、子上下文建立之前:局部绑定须先于体编译,体内
        // 自引用此名时父帧局部须已登记且已初始化（嵌套具名函数递归自捕获）。
        switch (kind) {
            case FnKind::Function:
                bind_stack_value(name, decl_loc); // [closure] -> [] 全局 DEF_GLOBAL / 局部值填槽
                break;
            case FnKind::Lambda:
                break; // 留栈作 lambda 表达式值,不绑定
            case FnKind::StaticMethod:
            case FnKind::Method:
            case FnKind::InitMethod: {
                // 类成员注册:[class, closure] -> [class],class 值留栈跨整个类体。
                const auto member_op  = kind == FnKind::StaticMethod ? OpCode::MAKE_STATIC : OpCode::MAKE_METHOD;
                const auto member_idx = add_name_or_fail(name, decl_loc);
                cur_cu()->emit_op(member_op, decl_line);
                cur_cu()->emit_word(member_idx, decl_line); // [class]
                break;
            }
        }

        // 切到子函数上下文并摆动游标:cu 由游标派生,随游标自动切到子 unit,无需 save/restore。
        // new 分配(非 UPtr),enclosing_ 回父(父编译期长于子,裸指针稳定)。kind 随上下文:
        // 实例方法族槽 0 = 具名局部 this(帧 [this, a1..aN],arity 不含 this)。
        const auto child          = new FunctionCtx{fn, cur_fn_ctx(), kind};
        mod_ctx_->current_fn_ctx_ = child;
        for (const auto& param: params) {
            // 形参即函数前 n 个局部变量(slot 1..n,this 后);重名已在上方检查,故直接 add_local 无需再查。
            cur_fn_ctx()->add_local(param.name);
        }

        // 编译体（BlockNode 自带 scope）。
        emit_stmt(body);

        // emit_stmt 抛异常时 unwind 跳过下方还原,子留在 enclosing_ 链上交 ~ModuleCtx 沿链释放。

        // 隐式 return 尾:init 方法返回 this(实例化不变式 Foo() 得实例——call_class 槽 0 原位换
        // 实例后以返回值为实例化结果,LOAD_LOCAL 0 即 this);其余返回 nil(兜底;显式 return 后为
        // 死代码,无害)。
        if (kind == FnKind::InitMethod) {
            cur_cu()->emit_load_local(0, line);
        } else {
            cur_cu()->emit_op(OpCode::LOAD_NIL, line);
        }
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

    void CodeGen::not_impl(const ASTNode& node, const StringView feature) const {
        fail(ErrorCode::NotImplemented, node.loc(), "{} 尚未支持", feature);
    }

    void CodeGen::not_impl(const SourceLoc loc, const StringView feature) const {
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

        auto loop_ctx                 = LoopCtx{.loop_scope_depth = cur_fn_ctx()->scope_depth_};
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
        auto loop_ctx = LoopCtx{.loop_scope_depth = loop_scope};
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
        emit_method_call0("iter", line, node.loc());                          // [iter_obj] 恰在 slot 位置
        const u16 iter_var_slot = define_local_or_fail("<iter>", node.loc()); // 值已在槽位，登记即初始化

        const u32 l_start = cur_cu()->size();
        // [iter]（receiver）
        cur_cu()->emit_load_local(iter_var_slot, line);
        // [bool]
        emit_method_call0("has_next", line, node.loc());
        const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

        auto loop_ctx                 = LoopCtx{.loop_scope_depth = loop_scope};
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
        emit_expr_or_nil(node.value.get(), line);
        cur_cu()->emit_op(OpCode::RETURN, line);
    }

    void CodeGen::visitImportStmtNode(ImportStmtNode& node) {
        const u32 line = node.loc_line();
        // IMPORT path:u16 压模块值于栈顶；绑定与 var/fun 同形：顶层 -> DEF_GLOBAL alias，嵌套 -> 值填槽。
        // path/alias 经 add_name_or_fail（无需守卫，见类首 GC 安全注）。
        const auto path_idx = add_name_or_fail(node.path, node.loc());
        cur_cu()->emit_op(OpCode::IMPORT, line);
        cur_cu()->emit_word(path_idx, line); // [module]
        // path 已先入池经 module 根链可达，故 alias 的 new_string 不会回收已入池的 path（类首 GC 安全注）。
        bind_stack_value(node.alias, node.loc()); // [module] -> [] DEF_GLOBAL 弹值 / 值填槽
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
        define_local_or_fail(*node.catch_param, node.loc()); // e 由 unwind 的 push 运行期填槽(== stack_depth)
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
        // 五种 kind 的绑定/注册分派收口在 compile_function（穷尽 switch，见其注）。
        compile_function(node.name, node.params, *node.body, node.loc(), node.kind);
    }

    void CodeGen::visitDefDeclNode(DefDeclNode& node) {
        const u32 line = node.loc_line();

        // ① superclass:有 -> 裸名解析 + 读取(运行期解析 superclass 值,跨模块导入类可用;非类值
        //    由运行期 MAKE_CLASS 报 TypeMismatch;编译期不查全局,未命中沿用运行期 UndefinedVariable);
        //    无 -> LOAD_OBJECT(def Foo 等价 def Foo : Object)。
        if (node.superclass) {
            const auto resolved = resolve_name_or_fail(*node.superclass, node.loc());
            emit_load_var(resolved, line); // [super]
        } else {
            cur_cu()->emit_op(OpCode::LOAD_OBJECT, line); // [Object]
        }

        // ② MAKE_CLASS name:peek superclass 建类写回原槽,class 值留栈跨整个类体。
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        cur_cu()->emit_op(OpCode::MAKE_CLASS, line);
        cur_cu()->emit_word(name_idx, line); // [class]

        // ③ 成员按源序发射(静态变量初始化顺序即此序,前一静态可被后续初始化器引用);成员各自经
        //    accept 分派(静态变量 -> visitStaticVarMemberNode,fun/方法 -> visitFunDeclNode 按节点
        //    kind)。成员重名不查重:成员即表写入(与体外 Foo.x = v 同形态),后写遮蔽,见 grammar。
        for (const auto& member: node.members) {
            member->accept(*this);
        }

        // ④ 尾绑定:类体全部建成,异常路径半成品类随 unwind 截栈丢弃后类名从未绑定。类名经
        //    MAKE_CLASS 已入池,全局腿 bind_stack_value 再入池一次（常量池不去重,同全局引用常态）。
        bind_stack_value(node.name, node.loc()); // [class] -> [] DEF_GLOBAL 弹值 / 值填槽
    }

    void CodeGen::visitVarDeclNode(VarDeclNode& node) {
        for (const auto& [target, initializer]: node.bindings) {
            // 仅 IdentifierPattern 可跑；ListPattern -> not_impl。
            const auto id = dynamic_cast<IdentifierPatternNode*>(target.get());
            if (id == nullptr) {
                not_impl(*target, "列表模式解构 var 声明");
            }
            // 初始化器先于声明名求值，绑定与 fun/def/import 同收 bind_stack_value：init 不登记
            // 当前帧局部，求值后栈高 == locals_.size()，值恰在 declare 槽位（值填槽）；init 里的
            // 同名引用沿 resolve 链落外层（遮蔽场合捕获外层、落全局则运行期 UndefinedVariable）。
            emit_expr_or_nil(initializer.get(), id->loc_line());
            bind_stack_value(id->name, id->loc());
        }
    }

    void CodeGen::visitStaticVarMemberNode(StaticVarMemberNode& node) {
        // 静态变量成员（def 体 var）：求值初始化器(无则 nil)+ MAKE_STATIC。eager 语义随 lowering
        // 自然成立(初始化器在类定义点求值);初始化器在 enclosing 作用域解析名字(类名尚未绑定,
        // 自引用 -> 运行期 UndefinedVariable)。
        const u32 line = node.loc_line();
        emit_expr_or_nil(node.initializer.get(), line); // [class, v]
        const auto member_idx = add_name_or_fail(node.name, node.loc());
        cur_cu()->emit_op(OpCode::MAKE_STATIC, line);
        cur_cu()->emit_word(member_idx, line); // [class]
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
        // 入口 take：Prepare = no-op（locator 是槽位/捕获索引/全局名，编译期常量，无接收者可备）；
        // Locate（复合赋值/前置自增定位腿）与 Load 同形——重解析免费，无副本可留。
        const auto mode     = take_lvalue_mode();
        const u32  line     = node.loc_line();
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
        // this 是关键字非标识符:专用解析(resolve_this_or_fail,沿 ctx 链找最近实例方法、嵌套
        // 经 upvalue 捕获、永不落全局),发射同普通局部读取(Local 恒槽 0 / Upvalue 捕获索引)。
        const auto resolved = resolve_this_or_fail(node.loc());
        emit_load_var(resolved, node.loc_line());
    }

    void CodeGen::visitSuperExprNode(SuperExprNode& node) {
        // super.成员（文法单形，Load rvalue 读）：语境检查（仅直接方法帧可承载——嵌套函数/静态
        // 方法/顶层一律禁）后发 LOAD_SUPER_FIELD，方法闭包由 VM 绑 this 成 bound method、静态槽
        // 原值直读。super.m(args) 经 visitCallNode 通用路径复用本 visit（emit_expr(callee) 出
        // [bound] 后 args + CALL），无特判分支；写形态非左值（validate_lvalue_target 拒绝）。
        if (!is_in_method()) {
            fail(ErrorCode::SuperOutsideMethod, node.loc(), "super 不在实例方法内");
        }
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        const u32  line     = node.loc_line();
        cur_cu()->emit_op(OpCode::LOAD_SUPER_FIELD, line);
        cur_cu()->emit_word(name_idx, line); // [bound]
    }

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
        cur_cu()->emit_op(binary_opcode(node.op), line);
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
                // E += 1 / E -= 1，复合赋值同族：定位腿走 Locate（运行时 locator 得 <obj> DUP
                // LOAD_FIELD 副本；编译期常量 locator 在目标节点内折叠为 Load 同形）。统一压 +1，
                // 由 ADD/SUBTRACT 决定方向--若 PreDec 压 -1 再 SUBTRACT 会算成 E - (-1) = E + 1，方向反。
                emit_lvalue(*node.operand, LvalueMode::Locate);
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
            // 普通 =：Prepare（接收者准备，编译期常量 locator 目标恒 no-op）-> <e> -> Store。
            // STORE_FIELD 栈形 [obj, v] -> [v] 要求接收者先于值入栈，发射权在首腿的目标节点；
            // 与复合赋值只差中间腿（值 vs op）。
            emit_lvalue(*node.target, LvalueMode::Prepare);
            emit_expr(*node.value);
            emit_lvalue(*node.target, LvalueMode::Store);
            return;
        }
        // 复合赋值：Locate -> <e> -> op -> Store。定位腿 emit_lvalue(Locate)：运行时 locator
        // （FieldAccess）得 <obj> DUP LOAD_FIELD 副本跨腿复用；编译期常量 locator（Identifier/
        // 帧内 this.x）在目标节点内折叠为 Load 同形，重解析免费。
        emit_lvalue(*node.target, LvalueMode::Locate);
        emit_expr(*node.value);
        cur_cu()->emit_op(binary_opcode(compound_op(node.op)), line);
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

    bool CodeGen::try_emit_this_field(const FieldAccessNode& node, const LvalueMode mode, const u32 line) const {
        // this.x 且 this 为当前帧局部（直接实例方法帧）-> THIS_FIELD 系指令，this 取帧槽 0 不经
        // 栈。返回是否命中本形态（未命中交调用方走一般经栈路径）。Prepare = no-op（无接收者可
        // 备，名字进池留给读/写腿，池内去重）；Locate 与 Load 同形——写腿不经栈取 this（槽 0
        // 编译期常量），定位腿发 DUP 副本反而滞留（无人消费）。
        if (dynamic_cast<ThisExprNode*>(node.object.get()) == nullptr || !is_in_method()) {
            return false;
        }
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        switch (mode) {
            case LvalueMode::Prepare:
                return true; // 本腿零指令
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
        // 四模式（take 入口取）：Load = rvalue 读；Prepare = 定位准备（只发接收者，普通 = 首腿）；
        // Store = 赋值目标（只发 store 指令，值由调用方压在栈顶）；Locate = 复合赋值/前置自增
        // 定位腿。super.成员 不经此（独立 SuperExprNode）。
        const auto mode = take_lvalue_mode();
        const u32  line = node.loc_line();
        if (try_emit_this_field(node, mode, line)) {
            return;
        }

        // 一般对象/嵌套捕获 this，接收者经栈（捕获 this 走 LOAD_UPVALUE，一般对象各自发射）；
        // 各臂自带完整发射序列。
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        switch (mode) {
            case LvalueMode::Prepare:
                // 普通 = 首腿：只发接收者（为 Store 腿垫栈），不读值
                emit_expr(*node.object); // [obj]
                return;
            case LvalueMode::Load:
                emit_expr(*node.object); // [obj]
                cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [obj.x]
                return;
            case LvalueMode::Store:
                // 接收者已由 Prepare 腿压在值下，只发 store 指令
                cur_cu()->emit_op(OpCode::STORE_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [obj, v] -> [v]
                return;
            case LvalueMode::Locate:
                // DUP 副本供本节点 Store 腿复用，locator 单次求值（compound-assignment-lowering.md §4.2）
                emit_expr(*node.object);              // [obj]
                cur_cu()->emit_op(OpCode::DUP, line); // [obj, obj]
                cur_cu()->emit_op(OpCode::LOAD_FIELD, line);
                cur_cu()->emit_word(name_idx, line); // [obj, obj.x]
                return;
        }
        UNREACHABLE();
    }

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
        // 名字恒为 kAnonymousName（`<>` 非法标识符，供 <fn ...> 渲染与堆栈跟踪）；留栈不绑定由
        // Lambda 态表达。decl_loc 同 visitFunDeclNode。
        compile_function(kAnonymousName, node.params, *node.body, node.loc(), FnKind::Lambda);
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
