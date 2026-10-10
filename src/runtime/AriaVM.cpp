#include "runtime/AriaVM.hpp"

#include <filesystem>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "compile/Compiler.hpp"
#include "memory/GC.hpp"
#include "memory/StringBuilder.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjList.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "object/iterator/ObjRangeIterator.hpp"
#include "runtime/Eval.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "runtime/builtins/ExceptionClass.hpp"
#include "runtime/builtins/IteratorClass.hpp"
#include "runtime/builtins/ListClass.hpp"
#include "runtime/builtins/MapClass.hpp"
#include "runtime/builtins/ObjectClass.hpp"
#include "runtime/builtins/RangeClass.hpp"
#include "runtime/builtins/StringClass.hpp"
#include "runtime/opcode_profile.hpp"
#include "util/fs.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {
    namespace stdfs = std::filesystem;

    namespace {

        // 单例表收口:两张预置表长格的 VM 单例表(registers_ / string_constants_)填完后不许留空格。
        // 未填格(nullptr)装箱后是非法值、按名取到空指针,存活到消费点只崩不报错,故在填点炸出。
        template<typename T>
        void assert_slots_filled(const List<T*>& slots, const char* what) noexcept {
            for (const auto slot: slots) {
                ASSERT(slot != nullptr, what);
            }
        }

        // dispatch_loop 各异常站点(unwind 返 Error)统一收口 Result<Value, Error> 的 unexpected 态。
        Result<Value, Error> runtime_err(Error err) { return std::unexpected(std::move(err)); }

        // 元数规格措辞:0 -> "no arguments"、1 -> "1 argument"、N -> "N arguments"(单数只在恰好 1)。
        String arity_phrase(const usize count) {
            if (count == 0) {
                return "no arguments";
            }
            if (count == 1) {
                return "1 argument";
            }
            return std::format("{} arguments", count);
        }

    } // namespace

    ObjString* concat_values(GC& gc, const Span<const Value> values, const StringView sep) {
        // 一遍渲染扫描:串元素只量长(填充时直写 view 零拷贝),其余渲染一次收进 parts(填充时按序取走)。
        usize total = 0;
        auto  parts = List<String>{};
        for (const auto& element: values) {
            if (const auto str = try_as_obj<ObjString>(element)) {
                total += str->view().size();
            } else {
                auto rendered = stringify(element);
                total += rendered.size();
                parts.push_back(std::move(rendered));
            }
        }
        if (!values.empty()) {
            total += (values.size() - 1) * sep.size();
        }

        auto builder = StringBuilder{gc};
        builder.reserve(total);
        usize part_index = 0;
        for (usize index = 0; index < values.size(); ++index) {
            if (index > 0) {
                builder.append(sep);
            }
            if (const auto str = try_as_obj<ObjString>(values[index])) {
                builder.append(str->view());
            } else {
                builder.append(parts[part_index++]);
            }
        }
        return builder.take_string();
    }

    AriaVM::AriaVM() :
        gc_{}, current_{nullptr}, modules_{&gc_}, builtins_{&gc_}, source_roots_{}, registers_{kValueRegisterCount},
        string_constants_{str_table::kCount}, char_cache_{kCharCacheSlots} {
        current_ = new_movement(gc_); // 首笔分配:gc_ 尚无对象,顶部 maybe_collect 无可回收
        hook_vm_roots();
        init_source_roots();
        {
            // 构造临界区:GC 挂起,窗口内回收不可达,创建的白对象免逐个守卫;**解锁前须全部发布
            // 进 tracer 可达的家**(registers_ / string_constants_ / builtins_,主上下文经 current_)。
            const auto lock = gc_.make_lock();
            bootstrap_string_constants();
            bootstrap_char_cache();
            bootstrap_registers();
            Builtin::register_builtins(gc_, builtins_);
        }
    }

    void AriaVM::bootstrap_string_constants() {
        // 按下标(= 表序)逐格驻留填入;须在 ctor 构造临界区内调用(GC 挂起,创建免守卫),
        // 填入即经 string_constants_ 可达,解锁前发布完毕。
        for (usize index = 0; index < str_table::kCount; ++index) {
            string_constants_[index] = new_string(gc_, str_table::kConstants[index]);
        }
        assert_slots_filled(string_constants_, "string_constants_: unfilled slot after bootstrap");
    }

    void AriaVM::bootstrap_char_cache() {
        // 字节域全值域一次铸满(char 重载工厂内 &ch 同步用,无悬垂窗口);经驻留池铸造,与一切
        // 铸造路径恒同对象。须在 ctor 构造临界区内调用(GC 挂起,创建免守卫),填入即经
        // char_cache_ 可达,解锁前发布完毕;填完逐槽收口。
        for (usize byte = 0; byte < kCharCacheSlots; ++byte) {
            char_cache_[byte] = new_string(gc_, static_cast<char>(byte));
        }
        assert_slots_filled(char_cache_, "char_cache_: unfilled slot after bootstrap");
    }

    void AriaVM::bootstrap_registers() {
        // 编排顺序即依赖序:Object 根类先建(其余内建类以它作 super),算子实现格缓存待类表全填,
        // builtins_ 曝光最后。
        registers_[kObjectClassOffset]    = ObjectClass::make_class(gc_);
        registers_[kExceptionClassOffset] = ExceptionClass::make_class(gc_, object_class());
        registers_[kIteratorClassOffset]  = IteratorClass::make_class(gc_, object_class());
        registers_[kListClassOffset]      = ListClass::make_class(gc_, object_class());
        registers_[kMapClassOffset]       = MapClass::make_class(gc_, object_class());
        registers_[kStringClassOffset]    = StringClass::make_class(gc_, object_class());
        registers_[kRangeClassOffset]     = RangeClass::make_class(gc_, object_class());
        // 缺参印章:私有 ObjClass 身份令牌,用户不可达;equals 恒地址型,显式实参身份均异于印章。
        registers_[kDefaultMarkOffset] = new_class(gc_, "<default>", nullptr);
        // match 全臂未命中的共享兜底异常:字节码 LOAD_REG + THROW 抛出,同一对象身份恒一。
        registers_[kMatchNoArmOffset] = new_exception(gc_, Error::from_detail(ErrorCode::MatchNoArm, "no arm matched"));

        for (const auto& [name, offset]: ListClass::kOperatorFns) {
            const auto hit = list_class()->load_field(*this, new_string(gc_, name));
            ASSERT(hit.has_value(), "List class table is missing an operator hook (table and impl cells drifted)");
            registers_[offset] = hit->as_obj();
        }
        for (const auto& [name, offset]: StringClass::kOperatorFns) {
            const auto hit = string_class()->load_field(*this, new_string(gc_, name));
            ASSERT(hit.has_value(), "String class table is missing an operator hook (table and impl cells drifted)");
            registers_[offset] = hit->as_obj();
        }

        // 类裸名曝光:is_a 链判定与类值触达。
        builtins_.set(Value::from_obj(object_class()->name()), Value::from_obj(object_class()));
        builtins_.set(Value::from_obj(exception_class()->name()), Value::from_obj(exception_class()));
        builtins_.set(Value::from_obj(iterator_class()->name()), Value::from_obj(iterator_class()));
        builtins_.set(Value::from_obj(list_class()->name()), Value::from_obj(list_class()));
        builtins_.set(Value::from_obj(map_class()->name()), Value::from_obj(map_class()));
        builtins_.set(Value::from_obj(string_class()->name()), Value::from_obj(string_class()));
        builtins_.set(Value::from_obj(range_class()->name()), Value::from_obj(range_class()));
        assert_slots_filled(registers_, "registers_: unfilled slot after bootstrap");
    }

    void AriaVM::hook_vm_roots() {
        // 链根交接纪律由 run() 出口断言承担,此处不重复设防。
        gc_.set_vm_roots([this](GC& g) {
            modules_.trace(g);
            builtins_.trace(g);
            g.mark_object(current_);
            for (const auto reg: registers_) {
                g.mark_object(reg);
            }
            for (const auto string: string_constants_) {
                g.mark_object(string);
            }
            for (const auto entry: char_cache_) {
                g.mark_object(entry); // 单字符常量:恒驻留(tracer 标根,池表项同保不摘白)
            }
        });
    }

    ObjClass* AriaVM::object_class() const noexcept { return registers_[kObjectClassOffset]->as<ObjClass>(); }

    ObjClass* AriaVM::exception_class() const noexcept { return registers_[kExceptionClassOffset]->as<ObjClass>(); }

    ObjClass* AriaVM::iterator_class() const noexcept { return registers_[kIteratorClassOffset]->as<ObjClass>(); }

    ObjClass* AriaVM::list_class() const noexcept { return registers_[kListClassOffset]->as<ObjClass>(); }

    ObjClass* AriaVM::map_class() const noexcept { return registers_[kMapClassOffset]->as<ObjClass>(); }

    ObjClass* AriaVM::string_class() const noexcept { return registers_[kStringClassOffset]->as<ObjClass>(); }

    ObjClass* AriaVM::range_class() const noexcept { return registers_[kRangeClassOffset]->as<ObjClass>(); }

    void AriaVM::init_source_roots() {
        // 入口槽 [0] 占位 cwd;cwd 不可用时空串兜底(resolve_module 裸名分支跳过空根)。
        source_roots_.push_back(fs::current_dir().value_or(""));
        // 配置根 [1..] = 编译器相对 stdlib 源根,weakly_canonical 规范化、推导失败跳过;
        // 可经 set_source_roots 覆盖。
        if (const auto pd = fs::program_dir()) {
            const auto      stdlib = stdfs::path{*pd} / kStdlibRelPath;
            std::error_code ec;
            if (const auto real = stdfs::weakly_canonical(stdlib, ec); !ec && !real.empty()) {
                source_roots_.push_back(real.string());
            }
        }
    }

    void AriaVM::set_source_roots(List<String> roots) noexcept {
        source_roots_.resize(1);
        for (auto& root: roots) {
            source_roots_.push_back(std::move(root));
        }
    }

    Result<Value, Error> AriaVM::run(SourceFile& source, ObjModule* module) {
        auto compiled = Compiler::compile(gc_, source, module, kMainEntryName);
        if (!compiled) {
            return std::unexpected(std::move(compiled).error());
        }
        return run(*compiled);
    }

    InterpretResult AriaVM::interpret_run(SourceFile& source, ObjModule* module) {
        auto compiled = Compiler::compile(gc_, source, module, kMainEntryName);
        if (!compiled) {
            io::println(stderr, "{}", compiled.error().message());
            return InterpretResult::CompileError;
        }
        auto result = run(*compiled);
        if (!result) {
            io::println(stderr, "{}", result.error().message());
            return InterpretResult::RuntimeError;
        }
        return InterpretResult::Ok;
    }

    InterpretResult AriaVM::interpret_from_src(const StringView src) {
        // 合成入口模块 <script>(dir=cwd);guard 根化跨编译+执行。
        const auto module = new_module(gc_, kScriptModuleName);
        auto       guard  = gc_.make_guard(module);

        // 方法内局部,存活至返回,Error 渲染不悬垂。
        SourceFile source{String{kScriptModuleName}, String{kScriptModuleName}, String{src}};
        return interpret_run(source, module);
    }

    InterpretResult AriaVM::interpret_from_path(const StringView path) {
        // 读盘 + BOM/CRLF/UTF-8 处理;失败渲染路径返 LoadError(无 SourceLoc,故 Error 走无位置版)。
        auto loaded = SourceFile::from_path(path);
        if (!loaded) {
            const auto detail = std::format("cannot read source file '{}'", path);
            io::println(stderr, "{}", Error::from_detail(ErrorCode::FileReadFailed, detail).message());
            return InterpretResult::LoadError;
        }
        SourceFile source = std::move(*loaded);

        // 入口模块身份:name = basename 去 .aria、dir = dirname(absolute(path))(收口于
        // fs::module_name_and_dir);name 为空(目录/空/无文件名)与读盘失败同属加载失败。
        auto [name_s, dir_s] = fs::module_name_and_dir(path);
        if (name_s.empty()) {
            const auto detail = std::format("module path has no valid name: '{}'", path);
            io::println(stderr, "{}", Error::from_detail(ErrorCode::ModuleNotFound, detail).message());
            return InterpretResult::LoadError;
        }
        auto module = new_module(gc_, name_s, dir_s); // 3 参重载:显式 dir
        auto guard  = gc_.make_guard(module);

        return interpret_run(source, module);
    }

    Result<Value, Error> AriaVM::run(ObjFunction* fn) {
        // 入口锚 = 本轮入口上下文(即主上下文):dispatch_loop 出口(顶层 RETURN / 未捕获物化)均
        // 发生在链根,出口断言据此保证切换交接完整(漏交接当场炸,不等下一轮入口)。
        const auto entry_ctx = current_;
        // 入口槽 [0] 原地替换为入口模块 dir_(对齐 Python sys.path[0]);dir_ 可空,由 resolve_module 跳空根。
        source_roots_[0] = fn->module()->dir()->view();

        // 重复调用先清场:HALT 收场的上一轮不弹帧,不清场会把新帧叠在陈旧帧上。
        current_->reset();

        // 包空闭包:fn 跨 new_closure 顶 maybe_collect 须有根,make_guard 兜底。
        const auto guard   = gc_.make_guard(fn);
        const auto closure = new_closure(gc_, fn);

        // 首次进入(最外执行):出口边界恒零,弹空帧栈即出口。
        current_->push(Value::from_obj(closure)); // 入栈即根
        Eval::call_closure(*this, closure, 0);
        Result<Value, Error> result =
                Eval::dispatch_loop(*this) ? current_->pop() : runtime_err(rebake_uncaught_error());
        ASSERT(current_ == entry_ctx, "current_ is not the run entry context (unbalanced context switch)");

        // 结束再清场:防 run() 外的 GC 经 tracer 标到陈旧栈值;清场归本入口。
        current_->reset();
        return result;
    }

    Opt<Value> AriaVM::execute_closure(ObjClosure* closure, const Span<const Value> args) {
        return execute_call(Value::from_obj(closure), closure, args);
    }

    Opt<Value> AriaVM::execute_method(const Value receiver, ObjClosure* method, const Span<const Value> args) {
        return execute_call(receiver, method, args);
    }

    Opt<Value> AriaVM::execute_call(const Value slot0, ObjClosure* closure, const Span<const Value> args) {
        // 重入执行体:压 slot0 与 args 后同步驱动一次闭包调用至结算。成功 = 栈顶收取闭包返回值;
        // 失败 nullopt 且载荷留寄存器、残留帧已截净,调用方按 bool 契约 return false 交外层续搜。
        // args 契约见声明处(不指进值栈,或留足压档余量)。重入返回后写返回槽恒经 peek
        // 重寻址(重入期间值栈可能搬迁)。
        if (current_->recursion_depth() >= ObjMovement::kMaxRecursionDepth) {
            return fail(ErrorCode::StackOverflow, "nested execution too deep"); // 载荷入寄存器
        }
        if (args.size() > std::numeric_limits<u8>::max()) {
            return fail(ErrorCode::WrongArity, "too many arguments: {}", args.size()); // 载荷入寄存器
        }
        ASSERT(args.data() >= current_->stack_top() || args.data() + args.size() <= current_->stack_base() ||
                       current_->stack_size() + 1 + args.size() <= current_->stack_capacity(),
               "args alias the value stack without room to push");
        current_->push(slot0); // 入栈即根(槽 0 = 本调用返回槽)
        for (const auto& arg: args) {
            current_->push(arg);
        }
        const auto saved_ret = current_->enter_recursion(); // 换防本循环出口边界,取走外层值
        if (Eval::call_closure(*this, closure, args.size()) && Eval::dispatch_loop(*this)) {
            current_->leave_recursion(saved_ret);
            return current_->pop(); // 闭包返回值
        }
        current_->leave_recursion(saved_ret);
        return std::nullopt; // 错误值在寄存器
    }

    bool AriaVM::check_arity(const ObjFunction* fn, const u8 argc) {
        // 契约与文案见 AriaVM.hpp;检查纯读,无分配。
        const auto arity     = fn->arity();
        const auto min_arity = fn->min_arity();
        if (fn->is_varargs()) {
            if (argc < min_arity) {
                return arity_error_at_least(argc, min_arity);
            }
            return true;
        }
        if (argc < min_arity || argc > arity) {
            if (min_arity == arity) {
                return arity_error(argc, arity);
            }
            return arity_error_range(argc, min_arity, arity);
        }
        return true;
    }

    FailSignal AriaVM::arity_error(const usize argc, const usize expected) {
        return fail(ErrorCode::WrongArity, "function expects {}, got {}", arity_phrase(expected), argc);
    }

    FailSignal AriaVM::arity_error_range(const usize argc, const usize low, const usize high) {
        // 区间恒复数(n or m arguments):"0 or 1 argument" 这类读起来像单数,不成句。
        return fail(ErrorCode::WrongArity, "function expects {} or {} arguments, got {}", low, high, argc);
    }

    FailSignal AriaVM::arity_error_at_least(const usize argc, const usize low) {
        return fail(ErrorCode::WrongArity, "function expects at least {}, got {}", arity_phrase(low), argc);
    }

    u8 AriaVM::prepare_call_args(const ObjFunction* fn, const u8 argc) {
        // GC 走查:new_list 为唯一分配点 -- 额外实参 peek 在栈(栈即根),copy_from 走 trivial
        // 分配不触 GC,白色 list 随即 drop+push 入值栈根,窗口内无 GC 点;垫充压寄存器单例无分配。
        const auto arity = fn->arity();

        // ① 缺省垫充(仅当实参不足固定参数数;varargs 的 argc 可超 arity,差值不可作 u8 减)。
        const auto missing = argc < arity ? static_cast<u8>(arity - argc) : u8{0};
        for (u8 i = 0; i < missing; ++i) {
            current_->push(Value::from_obj(registers_[kDefaultMarkOffset]));
        }

        // ② varargs 打包:rest 槽深 = arity + 1;普通函数槽深 = arity。
        if (fn->is_varargs()) {
            const auto extras = argc > arity ? static_cast<u8>(argc - arity) : u8{0};
            const auto rest   = new_list(gc_);
            rest->elements().copy_from({current_->stack_top() - extras, extras});
            current_->drop(extras);
            current_->push(Value::from_obj(rest));
            return static_cast<u8>(arity + 1);
        }
        return arity;
    }

    void AriaVM::enter_coroutine(ObjMovement* coroutine) {
        // 顺序不变式:置链必须先于换指 -- 换指后恢复者只经 coroutine->previous_ 一条边可达,
        // 其调用区的实参/载荷以此跨切换后的 GC 点存活。
        coroutine->set_previous(current_);
        coroutine->set_state(ExecState::Running);
        current_->set_state(ExecState::Normal);
        current_ = coroutine;
    }

    void AriaVM::leave_coroutine(const ExecState departing_state) {
        const auto resumer = current_->take_previous();
        ASSERT(resumer != nullptr, "leaving context is not on the resume chain");
        resumer->set_state(ExecState::Running);
        current_->set_state(departing_state);
        current_ = resumer;
    }

    ObjModule* AriaVM::load_module(ObjString* canonical_path, const StringView import_specifier) {
        // 契约见 AriaVM.hpp;canonical_path 已由调用方根化。加载事实源 = modules_ 表成员资格
        //(编译成功才入表,失败不留表项)。

        // 1. 读盘(resolve_module 已 exists-check,读盘/编码仍可能失败:权限竞争/非法 UTF-8)。
        auto loaded_src = SourceFile::from_path(canonical_path->view());
        if (!loaded_src) {
            return fail(ErrorCode::ModuleNotFound, "failed to load module '{}': read/decode error", import_specifier);
        }
        SourceFile source = std::move(*loaded_src);

        // 2. 派生模块身份(name/dir):abs_path() 还原 canonical key。
        const auto [name_str, dir_str] = fs::module_name_and_dir(canonical_path->view());
        if (name_str.empty()) {
            return fail(ErrorCode::ModuleNotFound, "module path has no valid name: '{}'", import_specifier);
        }

        // 3. 建模块(工厂内部 intern name/dir)+ 自守跨编译/入表。
        auto module = new_module(gc_, name_str, dir_str);
        auto guard  = gc_.make_guard(module);

        // 4. 编译(入口名 kModuleEntryName)。编译期 Error 就地装箱透传;source 须存活到返回。
        if (auto compiled = Compiler::compile(gc_, source, module, kModuleEntryName); !compiled) {
            current_->raise(Value::from_obj(new_exception(gc_, compiled.error())));
            return nullptr;
        }

        // 5. 入表先于模块体 run-once(体由 IMPORT 分支调起);module 仍由 guard 根化,表 set 无 GC 点。
        modules_.set(Value::from_obj(canonical_path), Value::from_obj(module));
        return module;
    }


    Pair<ErrorCode, String> AriaVM::take_uncaught_error() const {
        const auto payload = *current_->take_error();
        if (const auto ex = try_as_obj<ObjException>(payload)) {
            return {ex->code(), String{ex->message()->view()}};
        }
        const auto msg = std::format("uncaught exception: {}", repr(payload));
        return {ErrorCode::UncaughtException, Error::make_message(ErrorCode::UncaughtException, msg)};
    }

    Error AriaVM::rebake_uncaught_error() const {
        // 自顶帧向下收集 (fn, mod, last_ip) 烘跟踪行(跨协程死帧已随 reset 退场)-> take 载荷 ->
        // reset 清场 -> 烘焙成自足 Error。物化路径仅 string 拼接无 GC 分配点。
        struct TraceEntry {
            ObjFunction* fn;     // 帧函数(名字渲染)
            ObjModule*   mod;    // 帧模块(位置串渲染,ObjModule::format_location)
            u32          ip_off; // 行号经 fn->unit().line_for_offset 查
        };
        List<TraceEntry> trace;

        auto& frames = current_->frames();
        for (usize i = frames.size(); i > 0; --i) {
            auto& [frame_closure, unit, module, ip, slots, last_ip] = frames[i - 1];
            trace.emplace_back(frame_closure->function(), module, static_cast<u32>(last_ip - unit->code.data()));
        }
        auto [code, msg] = take_uncaught_error();
        current_->reset();
        for (const auto& [fn, mod, ip_off]: trace) {
            const u32 line = fn->unit().line_for_offset(ip_off);
            msg += std::format("\n  at {} ({})", fn->name()->view(), mod->format_location(line));
        }
        return Error::from_baked(code, msg);
    }

} // namespace aria
