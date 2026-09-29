#include "compile/CodeGen.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <ranges>
#include <tuple>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/value_register.hpp"
#include "util/util.hpp"

#include <format>

namespace aria {

    namespace {
        // 容量上限(值即操作数/索引位宽上限,事实源见 CodeUnit.hpp 的 kU8/kU16OperandMax;越界统一
        // 用 > 比较):kMaxArity/kMaxArguments(u8)、kMaxConstants/kMaxListElements/kMaxMapEntries/
        // kMaxLocals(u16)。语义名集中定义使检查点与报错文案同源;kMaxUpvalues 因登记侧共用而定义于
        // FunctionCtx.hpp。
        constexpr u32 kMaxArity        = kU8OperandMax;
        constexpr u32 kMaxArguments    = kU8OperandMax;
        constexpr u32 kMaxConstants    = kU16OperandMax;
        constexpr u32 kMaxListElements = kU16OperandMax;
        constexpr u32 kMaxMapEntries   = kU16OperandMax;
        constexpr u32 kMaxLocals       = kU16OperandMax;

        // 整数字面量 i48 值域(事实源 aria.hpp Value::from_int);超出 -> NumberOutOfRange。

        // 二元 op -> 发射 OpCode（visitBinaryExprNode 与复合赋值共用单源）。Or/And 走短路分支
        // （JUMP_*_OR_POP）不经此，OpCode 亦无单条逻辑码 -> UNREACHABLE。
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

        // 复合赋值 op -> 对应二元 op（经 binary_opcode）;文法仅算术五种复合;= 不入表（直走 store）。
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

        // min_arity = 必传参数数 = 首个带默认值参数之前的个数(文法定序 plain -> default -> varargs,
        // 由 Parser 保证;rest 遇之即止 --varargs 永非必传)。纯读 params,调用方须已过 validate_params。
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

        // 位置形态 `_`（文法:pattern -> "_" 占位）。解构里 `_` 位置不产生下标访问(访问有失败面:
        // map 源缺键报 KeyError,而 `_` 不关心该位置);谓词供父层跳过该位置的取元素与递归。
        [[nodiscard]] bool is_wildcard_pattern(const PatternNode& pattern) noexcept {
            return dynamic_cast<const WildcardPatternNode*>(&pattern) != nullptr;
        }

        // 解构访问数 = 会发出下标访问的位置数(非 `_` 元素位 + rest 位),决定 Fill 绑定是否需隐藏
        // 局部复取源值(0 访问直弹、1 访问源值即消耗品、>=2 访问须复取)。
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
            emit_implicit_return(program.loc());
        } catch (AriaCompileException& e) {
            // 出错即 unwind 到此：~ModuleCtx 随一次性对象析构沿 enclosing_ 链释放入口 + 未还原的子上下文。
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
        // 入口无参,min_arity = arity、无 varargs
        const auto entry = new_function(gc_, module, entry_name, 0, 0, false);
        module->set_entry(entry);
        mod_ctx_ = std::make_unique<ModuleCtx>(module); // 创建入口 fn 上下文并就位游标
        return entry;
    }

    // 当前 CodeUnit = 当前函数 fn_->unit()，随游标派生（定义于此：需 ObjFunction 完整类型取 unit()）。
    FunctionCtx* CodeGen::cur_fn_ctx() const noexcept { return mod_ctx_->current_fn_ctx_; }

    CodeUnit* CodeGen::cur_cu() const noexcept { return &cur_fn_ctx()->fn_->unit(); }

    u16 CodeGen::add_constant_or_fail(const Value value, const SourceLoc loc) const {
        // 常量池溢出(>kMaxConstants) -> fail CodeUnitTooLarge（CodeUnit::add_constant 内部
        // ASSERT 兜底，本预检保证永不触达）。同值常量复用池内已有索引：去重收口在 FunctionCtx 的去重
        // 索引（编译期草稿），池本体只追加。
        if (cur_cu()->constants.size() > kMaxConstants) {
            fail(ErrorCode::CodeUnitTooLarge, loc, "too many constants (max {})", kMaxConstants);
        }
        return cur_fn_ctx()->add_constant(value);
    }

    u16 CodeGen::add_name_or_fail(const StringView name, const SourceLoc loc) const {
        // intern name 入常量池返回索引；new_string 结果立即入池（trivial 分配不触发 GC，见类首 GC
        // 安全注），无需守卫--已 intern 的串与池内同值项同指针，去重命中时池里那份本就是它。
        // 溢出由 add_constant_or_fail fail。
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
        // 同作用域重名 -> RedefinedVariable（外层同名允许 shadow）；溢出 -> TooManyLocals。
        // 仅登记不发指令（值填槽：调用方保证值已压栈、登记槽位即值位置，登记即初始化）。
        if (cur_fn_ctx()->is_defined_in_scope(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "redefined local variable '{}'", name);
        }
        if (cur_fn_ctx()->locals_.size() > kMaxLocals) {
            fail(ErrorCode::TooManyLocals, loc, "too many locals (max {})", kMaxLocals);
        }
        return cur_fn_ctx()->add_local(name); // 纯登记
    }

    void CodeGen::define_global_or_fail(const StringView name, const SourceLoc loc) const {
        // 同名全局已登记 -> RedefinedVariable（局部侧对应物 define_local_or_fail）。
        if (!mod_ctx_->declare_global(name)) {
            fail(ErrorCode::RedefinedVariable, loc, "redefined global variable '{}'", name);
        }
        const u32  line     = loc.line();
        const auto name_idx = add_name_or_fail(name, loc);
        cur_cu()->emit_op(OpCode::DEF_GLOBAL, line);
        cur_cu()->emit_word(name_idx, line); // 弹值定义全局
    }

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
        // 契约见 CodeGen.hpp bind_stack_value 注。
        if (mod_ctx_->is_global_scope()) {
            define_global_or_fail(name, loc); // 弹值定义全局
        } else {
            std::ignore = define_local_or_fail(name, loc); // 值填槽：值恰在 locals_.size() 槽位，登记即初始化
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
        // 验证左值种类后设置模式并分派；清空职责在 take_lvalue_mode（漏 take 由 emit_expr ASSERT 捕获）。
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
        // 迭代协议三站点（iter/has_next/next）零实参方法调用：两段式发 PREPARE_METHOD + CALL_METHOD 0
        // （迭代器在调用区槽 0）。行号从 loc 现场求值（同源不双传）。
        const u32  line     = loc.line();
        const auto name_idx = add_name_or_fail(name, loc);
        emit_prepare_method(name_idx, line);
        cur_cu()->emit_op(OpCode::CALL_METHOD, line);
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
                cur_cu()->emit_byte(slot, line); // 索引域由 add_upvalue 容量检查保证 <= u8
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
                cur_cu()->emit_byte(slot, line); // 索引域由 add_upvalue 容量检查保证 <= u8
                return;
        }
        UNREACHABLE();
    }

    void CodeGen::bind_pattern(PatternNode& node, const PatternBindMode mode) {
        // 契约见 CodeGen.hpp bind_pattern 注：置模式后交节点自身 visit 发射。模式对整个子树恒定
        // （递归经 accept 双分派），故不 take、不还原--模式的设置点只有本函数。
        pattern_mode_ = mode;
        node.accept(*this);
    }

    template<typename PushSource>
    void CodeGen::emit_list_pattern_accesses(ListPatternNode& node, const u32 line, PushSource&& push_source) {
        // 契约见 CodeGen.hpp 注：循环体只写一次，调用点只给「本次访问的源值怎么来」。
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
            // rest 位 = 后缀 [元素数..]：无上界 range 作下标键（走切片，空尾得空 list，见
            // ObjList::slice）；绑名经 rest 节点自身 visit（与位置位同一路径）。
            push_source();
            emit_int_literal(static_cast<i64>(node.elements.size()), line, node.loc());
            cur_cu()->emit_op(OpCode::MAKE_RANGE, line);
            cur_cu()->emit_byte(kRangeFlagUnbounded, line); // [src, range]
            cur_cu()->emit_op(OpCode::LOAD_INDEX, line);    // [suffix]
            node.rest->accept(*this);
        }
    }

    void CodeGen::emit_expr(ExprNode& node) {
        // rvalue 上下文恒 Load：emit_lvalue 分派后必恢复为 Load。断言（非预防性赋值）以在开发期捕获漏恢复。
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
        // 契约见 CodeGen.hpp validate_params 注;此处只读 params,不触碰编译器状态。
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
        // 具名 fun（Function）绑定到全局（顶层）或局部（嵌套,值填槽）;Lambda 留栈作表达式值不绑定;
        // 方法三态留栈不绑定、就地注册--fun 静态 MAKE_STATIC 不戳 defining class（静态槽读恒原值）,
        // 实例方法族 MAKE_METHOD 戳（VM 侧方法性标记 + super 来源）。
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
        // 类名绑定先于体编译（三腿分派，对齐 bind_function_value 函数先例；clox classDeclaration
        // 的 OP_CLASS+OP_DEFINE_GLOBAL 先于类体同形）。
        //   - 成员腿（嵌套类）：仿全局腿早绑，DUP2 对复制挂表--栈 [enclosing, class] 复制栈顶对
        //     -> [e, c, e, c]，MAKE_STATIC 以 peek(1)=enclosing 为接收类挂本类、drop 值副本 ->
        //     [e, c, e]，POP 弃 klass 副本恢复 [e, c]（DUP2 对复制惯用法同 IndexAccess 定位腿）；
        //     体内自引用经全路径 A.B，裸名不解析（文法：无 enclosing 链、无裸名）。
        //   - 全局腿：DUP 后 DEF_GLOBAL 提前入全局，原类值驻栈贯穿类体供 MAKE_STATIC/MAKE_METHOD
        //     peek、尾部 POP 归位（visitDefDeclNode ⑤）；构建窗口内裸名经全局解析到构建中类对象
        //     （MAKE_CLASS 即完整类对象，成员挂载持续进行），初始化器 throw 后类名保持绑定（残留
        //     语义，对齐 Ruby；重定义仍由编译期注册表兜底）。
        //   - 局部腿（块内/函数内）：值填槽预登记，类值恰在 locals_.size() 槽位、登记即初始化，
        //     异常路径槽与半成品随截栈同弃。
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
        // 参数登记与缺省序言单循环交错、按声明序：先编缺省表达式、后登记本参数名 -- 轮到本槽时前序
        // 参数必已登记可引用，自身/后序参数名对解析结构性不可见、按常规链落外层/全局（同 Python/C++
        // 语义）。
        // 缺省序言（印章方案）：call_closure 已把未传槽 [argc+1..n] 垫充缺省印章（寄存器 DefaultMark），
        // 逐缺省槽 LOAD_LOCAL 与印章 EQUAL 判等，命中（未传）才求值默认值 STORE_LOCAL 换入；序言后栈空。
        const u32 line = loc.line();
        for (usize i = 0; i < params.size(); ++i) {
            const auto& param = params[i];
            if (param.default_value != nullptr) {
                const u16 slot = i + 1; // 参数槽 1..n(槽 0 = this/哑元)
                cur_cu()->emit_load_local(slot, line);
                cur_cu()->emit_op(OpCode::LOAD_REG, line);
                cur_cu()->emit_byte(kDefaultMarkOffset, line);
                cur_cu()->emit_op(OpCode::EQUAL, line);
                const u32 skip = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line);
                emit_expr(*param.default_value);
                cur_cu()->emit_store_local(slot, line); // peek-store 换入参数槽
                cur_cu()->emit_op(OpCode::POP, line);   // STORE_LOCAL 不弹,弹掉求值副本恢复「栈高 == 已填槽数」
                patch_jump_or_fail(skip, loc);
            }
            // 形参即函数前 n 个局部变量(slot 1..n,this 后);重名已在 validate_params 检查,直接登记。
            cur_fn_ctx()->add_local(param.name);
        }
    }

    void CodeGen::emit_implicit_return(const SourceLoc loc) const {
        const u32 line = loc.line();
        switch (cur_fn_ctx()->kind_) {
            case FnKind::ModuleEntry: {
                // 入口(主脚本与导入模块同规)返回值恒为模块对象:IMPORT 命中/未命中两路栈效应的
                // 统一靠它兑现--体跑完 RETURN 通用写回 callee 槽(即 IMPORT 预留结果槽)。
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
        // 参数合法性检查先于 new_function 等分配：失败即抛 AriaCompileException，跳过下方所有发射与分配。
        validate_params(params, decl_loc);

        // varargs 恒末位(Parser 结构性保证):arity 为固定参数数(rest 槽的值由 call_closure 打包
        // 多余实参为 list 就位,帧参数槽深 = arity + is_varargs);call_closure 据此只保下界。
        const bool has_varargs = !params.empty() && params.back().is_varargs;
        const auto fixed_arity = static_cast<u8>(params.size() - (has_varargs ? 1 : 0));
        const auto fn = new_function(gc_, mod_ctx_->module_, name, fixed_arity, min_arity(params), has_varargs);
        // 入池后即经 module 根链可达（trivial 窗口见类首 GC 安全注）。
        const auto fn_idx = add_constant_or_fail(Value::from_obj(fn), decl_loc);
        // CLOSURE fn_idx:VM 执行时现场包 ObjClosure,按捕获描述表(下方 flush 进
        // fn->upvalue_descs_)逐个建/复用 upvalue(表在元数据不进字节码流,CLOSURE 定长 3B)。
        cur_cu()->emit_op(OpCode::CLOSURE, decl_loc.line());
        cur_cu()->emit_word(fn_idx, decl_loc.line());

        // 分派须在 CLOSURE 之后、子上下文建立之前：局部绑定先于体编译，体内自引用此名时
        // 父帧局部须已登记且已初始化（嵌套具名函数递归自捕获）。
        bind_function_value(kind, name, decl_loc);

        // 切到子函数上下文并摆动游标:cu 由游标派生随之自动切到子 unit,无需 save/restore。new 分配
        // (非 UPtr),enclosing_ 回父(父编译期长于子,裸指针稳定);实例方法族槽 0 = 具名局部 this。
        const auto child          = new FunctionCtx{fn, cur_fn_ctx(), kind};
        mod_ctx_->current_fn_ctx_ = child;

        // 形参登记与缺省序言收口 compile_params（单循环交错,语义见其注）。
        compile_params(params, decl_loc);

        // 编译体（BlockNode 自带 scope）。
        // emit_stmt 抛异常时 unwind 跳过下方还原,子留在 enclosing_ 链上交 ~ModuleCtx 沿链释放。
        emit_stmt(body);
        emit_implicit_return(body.loc());

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

    void CodeGen::visitProgramNode(ProgramNode& node) {
        // 仅编排顶层声明，本节点不直接发射（行号由各子节点自持）。
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
        emit_expr(*node.condition);
        const u32 jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> else / end
        emit_stmt(*node.then_branch);
        if (node.else_branch != nullptr) {
            const u32 jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
            // -> else
            patch_jump_or_fail(jf, node.loc());
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
        emit_expr(*node.condition);
        const u32 patch = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end 占位
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
        // continue: 有 incr -> 前向跳 L_incr（回填）；无 incr -> 后向跳循环头（back_target）。
        // 循环头 = 条件起点 = L_cond
        auto loop_ctx = LoopCtx{.loop_scope_depth = loop_scope, .back_target = cur_cu()->size()};
        if (has_incr) {
            loop_ctx.continue_fwd_patches.emplace(); // 打开前向 continue 通道
        }
        if (has_cond) {
            emit_expr(*node.condition);
            const u32 patch = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end 占位
            loop_ctx.exit_fwd_patches.push_back(patch);
        } // 无 cond: exit 列表空，收尾只有回边 + break 回填
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
        emit_stmt(*node.body);
        loop_ctx = util::pop_top(cur_fn_ctx()->loop_stack_);

        // continue（前向）须回填到 L_incr：此刻 cur_cu()->size() 即递增区起点，且须先于递增发射--
        // 若等递增与 JUMP_BACK 发完再回填，cur_cu()->size() 已是 L_end，continue 会错跳到 L_end 提前出循环。
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
        // lowering 等价形式（无预占/peek-store，值填槽--下个局部 slot 即当前栈高，见指令集 §4.3）：
        // 外层 for-in scope 挂隐藏局部 <iter> = iterable.iter()（"<iter>" 含 <> 不可作标识符，不撞用户名）；
        // 循环头 = has_next() 判断处（continue 跳此，无 increment 步）；每轮 per-iteration scope 里
        // <pattern> = iter.next()（bind_pattern Fill）后跑体、end_scope 收口（每轮 fresh 绑定）。
        begin_scope(); // for-in scope（D）：仅 <iter>，循环全程存活
        const u32 loop_scope = cur_fn_ctx()->scope_depth_;

        // 隐藏局部 <iter>，值填槽：iterable.iter() 出值后 declare，值即 <iter>（无 LOAD_NIL 预占、无
        // STORE_LOCAL/POP）。
        // [iterable]（receiver）
        emit_expr(*node.iterable);
        emit_method_call0("iter", node.loc());                                // [iter_obj] 恰在 slot 位置
        const u16 iter_var_slot = define_local_or_fail("<iter>", node.loc()); // 值已在槽位，登记即初始化

        // 循环头 = has_next 判断处
        auto loop_ctx = LoopCtx{.loop_scope_depth = loop_scope, .back_target = cur_cu()->size()};
        // [iter]（receiver）
        cur_cu()->emit_load_local(iter_var_slot, line);
        // [bool]
        emit_method_call0("has_next", node.loc());
        const u32 patch = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end 占位
        loop_ctx.exit_fwd_patches.push_back(patch);
        cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));

        // per-iteration scope：pattern + 体每轮 fresh（值填槽）。体经 emit_stmt 作为不透明子节点，
        // 若为 block 则自带更深层 scope；break/continue 跳出时由 emit_pop_locals_to(loop_scope) 代弹。
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
        // 入口上下文(主脚本与导入模块同规,见 ModuleCtx 构造):返回值恒为模块对象--带值 return
        // 编译期拒绝,裸 return 即模块体提前退出(压模块对象常量后 RETURN,与隐式收尾同序)。
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
        // IMPORT path:u16 压模块值于栈顶；绑定与 var/fun 同形：顶层 -> DEF_GLOBAL alias，嵌套 -> 值填槽。
        // path/alias 经 add_name_or_fail（无需守卫，见类首 GC 安全注）。
        const auto path_idx = add_name_or_fail(node.path, node.loc());
        cur_cu()->emit_op(OpCode::IMPORT, line);
        cur_cu()->emit_word(path_idx, line); // [module]
        // path 已先入池经 module 根链可达，故 alias 的 new_string 不会回收已入池的 path（类首 GC 安全注）。
        bind_stack_value(node.alias, node.loc()); // [module] -> [] DEF_GLOBAL 弹值 / 值填槽
    }

    void CodeGen::visitTryStmtNode(TryStmtNode& node) {
        const u32 line = node.line();
        // try 须有 catch（parse 层允许无 catch，语义收口在此）。
        if (node.catch_body == nullptr) {
            fail(ErrorCode::TryWithoutHandler, node.loc(), "'try' requires a catch clause");
        }

        // lowering：入口预插 try_records 占位（begin 已定、余待结尾回填）-> 编译 try 体（嵌套 try 在此
        // 各自预插，记录按 begin 升序）-> JUMP 跳过 catch -> L_catch 起 begin_scope + 登记 catch 参数
        // （e 由 unwind 截栈到 slots+stack_depth 后 push 填槽，恰落该槽，无 STORE_LOCAL）-> 编译 catch
        // 体 -> 末尾回填 end/handle/stack_depth。栈平衡：两路径 end_scope 都回到 stack_depth，在 L_end
        // 齐平（走查见 exception-implementation-pitfalls.md 坑 #4/#10）。
        const auto stack_depth = static_cast<u32>(cur_fn_ctx()->locals_.size()); // try 入口局部数(try scope 开前)
        const u32  begin       = cur_cu()->size();
        const auto rec_idx     = cur_cu()->try_records.size();
        // 预插占位(begin 已定,余待回填)
        cur_cu()->try_records.push(TryRecord{begin, 0, 0, 0});
        begin_scope();         // try 体 scope
        emit_stmt(*node.body); // 嵌套 try 在此编译,各自入口预插占位(begin > 本层)-> 整体升序(坑 #4)
        end_scope(line);
        const u32 end   = cur_cu()->size();
        const u32 jskip = cur_cu()->emit_jump(OpCode::JUMP, line); // 正常路径跳过 catch -> L_end
        // L_catch
        const u32 handle = cur_cu()->size();
        begin_scope(); // catch 子句 scope(包 e + catch 体 -- e 须入 scope,两路径栈平衡,坑 #10)
        std::ignore =
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
        const u32 line = node.line();
        // 求值抛出表达式后 THROW 弹值入寄存器,运行期由 unwind 查异常记录表派发(语义见
        // AriaVM dispatch_loop 的 THROW case):原值不包 ObjException,catch 绑原值保类型(坑 #7)。
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

    // match 降糖总口(模板定义,MatchStmtNode/MatchExprNode 两实例化点即下方两 visit):纯降糖零新
    // 指令,subject 求值一次驻留栈上跨臂复用(命中臂入口 POP 消费,全臂未命中由 THROW 的 unwind
    // 清栈),逐臂展开「DUP + 模式 + EQUAL + 未命中跳下臂」链;语句臂净零值、表达式臂每臂恰一值。
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
            if (pattern.value != nullptr) {             // "_" 通配:不比较直入
                cur_cu()->emit_op(OpCode::DUP, line);   // [s, s] 副本供比较,subject 本尊保留
                emit_expr(*pattern.value);              // [s, s, p]
                cur_cu()->emit_op(OpCode::EQUAL, line); // [s, bool]
                // 未命中 -> 下臂
                miss_jump = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line);
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
        // name 由 compile_function 内部 intern + make_guard，此处只传 StringView；node.loc() 作 decl_loc。
        // 五种 kind 的绑定/注册分派收口在 compile_function（穷尽 switch，见其注）。
        compile_function(node.name, node.params, *node.body, node.loc(), node.kind);
    }

    void CodeGen::visitDefDeclNode(DefDeclNode& node) {
        const u32 line = node.line();

        // ① superclass:有 -> 裸名解析 + 读取(运行期解析 superclass 值,跨模块导入类可用;编译期不查
        //    全局,未命中沿用运行期 UndefinedVariable);无 -> LOAD_REG ObjectClass(def Foo 等价
        //    def Foo : Object;根类在值寄存器组,按索引加载不经名字查,用户 shadow 免疫)。
        if (node.superclass) {
            const auto resolved = resolve_name_or_fail(*node.superclass, node.loc());
            emit_load_var(resolved, line); // [super]
        } else {
            cur_cu()->emit_op(OpCode::LOAD_REG, line); // [Object]
            cur_cu()->emit_byte(kObjectClassOffset, line);
        }

        // ② MAKE_CLASS name:peek superclass 建类写回原槽,class 值留栈跨整个类体。
        const auto name_idx = add_name_or_fail(node.name, node.loc());
        cur_cu()->emit_op(OpCode::MAKE_CLASS, line);
        cur_cu()->emit_word(name_idx, line); // [class]

        // ③ 类名绑定先于体编译:体内自引用(方法体/静态初始化器)要求绑定先于体,三腿收口
        //    bind_class_value(成员位 DUP2 对复制挂表 + 全局腿 DUP 早绑 + 局部腿值填槽,见其注)。
        bind_class_value(node.is_member, node.name, node.loc());

        // ④ 成员按源序发射(静态变量初始化顺序即此序,前一静态可被后续初始化器引用);成员各自经
        //    accept 分派。成员重名不查重:成员即表写入(与体外 Foo.x = v 同形态),后写遮蔽。
        for (const auto& member: node.members) {
            member->accept(*this);
        }

        // ⑤ 尾部弹栈:头部绑定留有驻留类值的腿在此归位([class] -> [])--全局腿 DEF_GLOBAL 弹的
        //    是 DUP 副本,成员腿 MAKE_STATIC 弹的是值副本,驻留原值都在尾部弹;局部腿无尾,类值即
        //    局部、随作用域收尾弹出。
        if (node.is_member || mod_ctx_->is_global_scope()) {
            cur_cu()->emit_op(OpCode::POP, line); // [class] -> [] 弹驻留类值
        }
    }

    void CodeGen::visitVarDeclNode(VarDeclNode& node) {
        for (const auto& [target, initializer]: node.bindings) {
            // 初始化器先于声明名求值：求值后栈高 == locals_.size()，值恰在待声明槽位（值填槽）；init
            // 里的同名引用沿 resolve 链落外层（遮蔽场合捕获外层、落全局则运行期 UndefinedVariable）。
            // target 为 identifier 或解构 pattern，两形态同经 bind_pattern（Fill）。
            emit_expr_or_nil(initializer.get(), target->line());
            bind_pattern(*target, PatternBindMode::Fill);
        }
    }

    void CodeGen::visitStaticVarMemberNode(StaticVarMemberNode& node) {
        // 静态变量成员（def 体 var）：求值初始化器(无则 nil)+ MAKE_STATIC。初始化器在类定义点、
        // enclosing 作用域求值(eager);类名自引用两腿均可用(类名绑定先于体:全局腿经头部
        // DEF_GLOBAL 解析到构建中类对象,局部腿经预登记局部槽)。
        const u32 line = node.line();
        emit_expr_or_nil(node.initializer.get(), line); // [class, v]
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
        // LOAD_IMM 的 u8 操作数在 VM 侧按 i8 位型重解释做符号扩展（bit_cast<i8>）；此处先经 i8
        // 保证符号语义、再转 u8 写字节（免窄化告警）。
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
        // -<数值字面量>：负常量本身就是一条加载指令（LOAD_IMM 按 i8 有符号解释，池内常量亦能取负），
        // 取负并进常量即可，无须另发运行期 NEGATE；只认直接操作数这一层（-(-5) 外层是 UnaryExpr）。
        // 值域按字面量自身值判（emit_int_literal 那一道闸），越界报文带源码写出的符号。
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

    void CodeGen::visitStringLiteralNode(StringLiteralNode& node) {
        const u32  line = node.line();
        const auto idx  = add_name_or_fail(node.value, node.loc());
        cur_cu()->emit_op(OpCode::LOAD_CONST, line);
        cur_cu()->emit_word(idx, line);
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
        // 入口 take：Prepare = no-op（locator 是槽位/捕获索引/全局名，编译期常量，无接收者可备）；
        // Locate（复合赋值/前置自增定位腿）与 Load 同形--重解析免费，无副本可留。
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
        // this 是关键字非标识符:专用解析(resolve_this_or_fail,沿 ctx 链找最近实例方法、嵌套
        // 经 upvalue 捕获、永不落全局),发射同普通局部读取(Local 恒槽 0 / Upvalue 捕获索引)。
        const auto resolved = resolve_this_or_fail(node.loc());
        emit_load_var(resolved, node.line());
    }

    void CodeGen::visitSuperExprNode(SuperExprNode& node) {
        // super.成员（文法单形，Load rvalue 读）：语境检查（仅直接方法帧可承载）后发 LOAD_SUPER_FIELD，
        // 方法闭包由 VM 绑 this 成 bound method、静态槽原值直读。super.m(args) 经 visitCallNode 通用
        // 路径复用本 visit，无特判分支；写形态非左值（validate_lvalue_target 拒绝）。
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
                // 负字面量形态（详见 try_emit_negated_literal）：命中即一条负常量加载，否则一般路径。
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
                // E += 1 / E -= 1，复合赋值同族（定位腿走 Locate）。统一压 +1、由 ADD/SUBTRACT 决定方向
                // --若 PreDec 压 -1 再 SUBTRACT 会算成 E - (-1) = E + 1，方向反。
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

    void CodeGen::visitDestructureAssignmentNode(DestructureAssignmentNode& node) {
        // 目标恒为 listPattern（Parser 只在此形态建本节点）。右值求值一次后 DUP 一份留作本表达式
        // 的值；Store 模式的 bind_pattern 净消耗栈顶一值，消费的是副本。语句位由 exprStmt 收尾弹值。
        const u32 line = node.line();
        emit_expr(*node.value);
        cur_cu()->emit_op(OpCode::DUP, line);
        bind_pattern(*node.target, PatternBindMode::Store);
    }

    bool CodeGen::try_emit_method_call(const CallNode& node) {
        // 契约见 CodeGen.hpp；未命中（callee 非成员访问）不发射任何字节。
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
        // 实参上限 kMaxArguments（CALL 操作数 u8）：先检后发，避免 emit 完数百个实参表达式才报错。
        if (node.args.size() > kMaxArguments) {
            fail(ErrorCode::TooManyArguments, node.loc(), "too many arguments (max {})", kMaxArguments);
        }
        // recv.name(args) 两段式发射（见 try_emit_method_call）；未命中交下方一般路径。
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
        // this.x 且 this 为当前帧局部（直接实例方法帧）-> THIS_FIELD 系指令，this 取帧槽 0 不经栈；
        // 未命中返回 false 交调用方走经栈路径。Prepare = no-op（无接收者可备）；Locate 与 Load 同形
        // --写腿不经栈取 this（槽 0 编译期常量），定位腿发 DUP 副本反而滞留（无人消费）。
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
        // 四模式（take 入口取）：Load = 读；Prepare = 只发接收者（普通 = 首腿）；Store = 只发 store
        // 指令（值由调用方压在栈顶）；Locate = 复合赋值/前置自增定位腿。super.成员 不经此。
        const auto mode = take_lvalue_mode();
        const u32  line = node.line();
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

    void CodeGen::visitIndexAccessNode(IndexAccessNode& node) {
        // 四模式(take 入口取,visitFieldAccessNode 同款):Load = rvalue 读;Prepare = 普通 = 首腿(只发
        // obj + idx 备对);Store = 只发 STORE_INDEX(obj/idx 由 Prepare 腿备好);Locate = 定位腿,DUP2
        // 复制 (obj, idx) 对跨越 load 供 Store 腿复用,locator 单次求值(compound-assignment-lowering.md §4.3)。
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
                // obj/idx 已由 Prepare 腿压在值下,只发 store 指令
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
        // 元素数上限 kMaxListElements(MAKE_LIST 操作数 u16):先检后发(visitCallNode 同款)。
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
        // 条目数上限 kMaxMapEntries(MAKE_MAP 操作数 u16,条目对数):先检后发(visitListExprNode 同款)。
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
            // 无上界开区间:压 [from] 单值,编 unbounded 位(含否上界无意义,不编 exclusive 位)。
            // [from] -> [range]
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
        emit_expr(*node.condition);
        const u32 jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> else
        emit_expr(*node.then_branch);
        const u32 jend = cur_cu()->emit_jump(OpCode::JUMP, line); // -> end
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

    void CodeGen::visitMatchExprNode(MatchExprNode& node) { emit_match(node); }

    void CodeGen::visitSequenceExprNode(SequenceExprNode& node) {
        // 逐元素求值，非末位求值后 POP 弃（同 ExprStmt 惯用法），末位留栈即序列值。零新指令。
        // 单元素已由 parser 透明化，size >= 2 是本 visit 的入参不变式。
        ASSERT(node.expressions.size() >= 2, "sequence node requires two or more expressions");
        for (usize index = 0; index + 1 < node.expressions.size(); ++index) {
            emit_expr(*node.expressions[index]);
            cur_cu()->emit_op(OpCode::POP, node.expressions[index]->line());
        }
        emit_expr(*node.expressions.back());
    }

    void CodeGen::visitIdentifierPatternNode(IdentifierPatternNode& node) {
        // 栈顶值即待绑值；identifier 位置与 listPattern 的 rest 位（同为 IdentifierPatternNode）都经此。
        // Fill：按名绑为当前作用域的新变量（收 bind_stack_value）。Store：写既有名（resolve + STORE_*），
        // STORE_* 是 peek-store（值留栈），故补 POP 使本模式净消耗栈顶一值。
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
        // `_` 忽略该值：弹掉栈顶（Fill 的到达形态只有根位--for-in 目标 `var`/`for (_ in xs)`；
        // 列表位置位由父层按 is_wildcard_pattern 跳过，不取元素故不达此处）。
        cur_cu()->emit_op(OpCode::POP, node.line());
    }

    void CodeGen::visitListPatternNode(ListPatternNode& node) {
        // 位置绑定 = 逐位置下标访问（文法：listPattern 映射为下标访问，位置 i 取 [i]，多余忽略、
        // 不足由下标越界报错；rest 位取后缀 [元素数..]）。
        const u32 line = node.line();

        switch (pattern_mode_) {
            case PatternBindMode::Fill: {
                // 新名值填槽，三者都不偏离「栈高 == 局部数」不变式（见 pattern_access_count 注）：
                //   0 次访问：源值即废，弹出（初始化器/next() 的副作用照跑，不取值）。
                //   1 次访问：源值本身即消耗品，无须隐藏局部--取出的元素恰落在源值那个槽位。
                //   >=2 次：源值先填成隐藏局部（值填槽），逐位置经它复取。
                const usize access_count = pattern_access_count(node);
                if (access_count == 0) {
                    cur_cu()->emit_op(OpCode::POP, line);
                    return;
                }
                if (access_count == 1) {
                    emit_list_pattern_accesses(node, line, [] {});
                    return;
                }
                // 隐藏局部名带槽号：同作用域不许重名（define_local_or_fail 查 is_defined_in_scope），
                // 而同一作用域里两条解构语句必占不同槽（作用域内局部只增不减），故带槽号天然唯一。
                const auto source_name = std::format("<destructure_{}>", cur_fn_ctx()->locals_.size());
                const u16  source_slot = define_local_or_fail(source_name, node.loc());
                const auto push_source = [this, source_slot, line] { cur_cu()->emit_load_local(source_slot, line); };
                emit_list_pattern_accesses(node, line, push_source);
                return;
            }
            case PatternBindMode::Store: {
                // 既有名按名写，源值恒驻栈顶：每次访问前 DUP 供本次取元素（LOAD_INDEX 吃掉源与下标两
                // 个），取出值交子节点写目标（identifier 子节点自行 POP）；故本层只需收尾弹掉自己的源值
                // （净消耗栈顶一值；右值的副本由调用点持有）。
                emit_list_pattern_accesses(node, line, [this, line] { cur_cu()->emit_op(OpCode::DUP, line); });
                cur_cu()->emit_op(OpCode::POP, line); // 弹本层源值
                return;
            }
        }
        UNREACHABLE();
    }

} // namespace aria
