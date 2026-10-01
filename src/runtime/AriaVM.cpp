#include "runtime/AriaVM.hpp"

#include <cmath>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "bytecode/Disassembler.hpp"
#include "bytecode/code.hpp"
#include "compile/Compiler.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjRange.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "object/iterator/ObjRangeIterator.hpp"
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

        // 读 1 字节操作数(假定字节码良构),推进 ip。
        u8 read_u8(CallFrame* frame) noexcept { return *frame->ip++; }

        // 读 2 字节小端 u16 操作数(假定字节码良构),推进 ip。拼装走 util::make_u16。
        u16 read_u16(CallFrame* frame) noexcept {
            const u16 value = util::make_u16(frame->ip[0], frame->ip[1]);
            frame->ip += 2;
            return value;
        }

        // 取常量池 u16 索引处的 ObjString*(全局名/字段名/模块路径与别名等),推进 ip。
        // 良构前提:该常量必为经 intern 驻留的 ObjString*(编译期保证)。故此处不二次校验,
        // 直接 as_obj 取 ObjString*(若编译期出错,后续按 intern 同指针查表会查不到,
        // 属编译器 bug 而非运行期可恢复错)。供 DEF/LOAD/STORE_GLOBAL 与 IMPORT 复用。
        ObjString* read_name(CallFrame* frame) noexcept {
            const auto idx = read_u16(frame);
            return Object::as<ObjString>(frame->unit->constants[idx].as_obj());
        }

        // 把 import 串(specifier)解析为命中文件的绝对规范路径(模块表键 = weakly_canonical
        // 候选,符号链接经它规避双加载)。spec 末尾 ".aria" 可选(剥后缀查找再统一补回,使
        // lib/math ≡ lib/math.aria);相对 spec(./ ../ . ..)以当前模块目录为单基、caller-local,
        // **永不逃逸到别的源根**;裸名沿 source_roots 逐个 exists-check(对齐 Python sys.path)。
        // 当前模块路径为空(合成模块)时相对解析直接返 nullopt。设计全文见
        // .claude/reference/runtime/import-path-resolution.md「加载层设计基线」。
        Opt<String> resolve_module(const StringView spec, const StringView current_module_path,
                                   const List<String>& source_roots) {
            // 1. 剥末段 ".aria" 后缀(段长 > 扩展名长且以 ".aria" 结尾)。
            String spec_str{spec};
            {
                const auto last_slash = spec_str.find_last_of('/');
                const auto last_seg   = (last_slash == String::npos) ? StringView{spec_str}
                                                                     : StringView{spec_str}.substr(last_slash + 1);
                if (last_seg.size() > kAriaExtension.size() && last_seg.ends_with(kAriaExtension)) {
                    spec_str.erase(spec_str.size() - kAriaExtension.size());
                }
            }

            // 2. 选基:相对 -> 当前模块目录(单基);裸名 -> source_roots(逐个试)。
            const bool is_relative = spec.starts_with("./") || spec.starts_with("../") || spec == "." || spec == "..";
            List<stdfs::path> bases;
            if (is_relative) {
                if (current_module_path.empty()) {
                    return std::nullopt; // 合成模块无绝对路径,无法相对解析
                }
                bases.push_back(stdfs::path{String{current_module_path}}.parent_path());
            } else {
                for (const auto& root: source_roots) {
                    if (!root.empty()) {
                        bases.emplace_back(root);
                    }
                }
            }

            // 3. 逐基:<base>/<spec>.aria -> weakly_canonical -> exists 则为键。
            auto file_rel = spec_str;
            file_rel.append(kAriaExtension);
            std::error_code ec;
            for (const auto& base: bases) {
                auto canon = stdfs::weakly_canonical(base / file_rel, ec);
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (stdfs::exists(canon, ec) && !ec) {
                    return canon.string();
                }
                ec.clear();
            }
            return std::nullopt;
        }

        // 构造运行时错误结果(Result<Value, Error> 的 unexpected 态):dispatch_loop 各异常站点
        // (unwind 返 somed Error)的统一收口。
        Result<Value, Error> runtime_err(Error err) { return std::unexpected(std::move(err)); }

        // 执行跟踪:每条指令执行**前**打印字节码/栈/帧信息(stderr,调试用;与 println 的 stdout
        // 分流)。常态编译,宏只守 dispatch_loop 内调用点,关闭时零开销;主循环顶在取 opcode 前
        // 调用 -- 此时 frame.ip 指向待执行指令,据此解码(仅读不推进 VM 的 ip)。栈渲染经
        // format_value_debug 不用 format_value:后者 Obj 走可重载虚 to_string,在 dispatch_loop
        // 内会重入 VM 致无限递归;debug_repr 纯 C++,绝不触用户重载。输出形制见 runtime.md。
        [[maybe_unused]] void trace_execution(ObjMovement* ctx) {
            auto&       frames = ctx->frames();
            const auto& frame  = frames.top();
            const auto  ip_off = static_cast<u32>(frame.ip - frame.unit->code.data());
            const auto  instr  = Disassembler::disassembleInstruction(frame.unit, ip_off);

            io::print(stderr, "[trace] ctx {:p}  {}  {} @{:04X}  {}\n", static_cast<const void*>(ctx),
                      frame.module->to_string(), frame.closure->function()->to_string(), ip_off, instr);

            // 栈行与 ^ 列号一趟同步算:^ 对齐到当前帧栈底(slots 所指槽)的 [ 下方;slots 越过
            // 栈顶时(异常态)所有槽位都满足 p < slots,累加自然停在全部段之和,无需分支。
            const String prefix = std::format("        stack[{}]: ", ctx->stack_size());
            usize        col    = prefix.size();
            String       stack_str;
            for (Value* p = ctx->stack_base(); p < ctx->stack_top(); ++p) {
                const auto s = std::format("[ {} ]", format_value_debug(*p));
                if (p < frame.slots) {
                    col += s.size();
                }
                stack_str += s;
            }
            if (stack_str.empty()) {
                stack_str = "(empty)";
            }

            io::print(stderr, "{}{}\n", prefix, stack_str);

            // 帧栈底标记行:行首纯空格对齐,只留 ^ 与 frame 索引(fn/ip 已在字节码行,不重复)。
            io::print(stderr, "{}^ frame[{}]\n", String(col, ' '), static_cast<u32>(frames.size() - 1));
        }

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

    // 构造:分配执行上下文(GC 对象) -> 注册 VM 根 tracer -> bootstrap 常量串表 + 寄存器组 ->
    // 注册 builtins(两张单例表 registers_ / string_constants_ 各按注册表长度预置格位)。gc_ 值
    // 成员居声明首,逆序析构下 tracer 与成员同生共死;主上下文随 ~GC 的 free_all_ 释放。
    AriaVM::AriaVM() :
        gc_{}, current_{nullptr}, modules_{&gc_}, builtins_{&gc_}, source_roots_{}, registers_{kValueRegisterCount},
        string_constants_{str_table::kCount} {
        current_ = new_movement(gc_); // 首笔分配:gc_ 尚无对象,顶部 maybe_collect 无可回收
        hook_vm_roots();
        init_source_roots();
        {
            // 构造临界区:GC 挂起,窗口内回收不可达,创建的白对象免逐个守卫;**解锁前须全部发布
            // 进 tracer 可达的家**(registers_ / string_constants_ / builtins_,tracer 已挂接;
            // 主上下文经 current_ 可达)。
            const auto lock = gc_.make_lock();
            bootstrap_string_constants();
            bootstrap_registers();
            Builtin::register_builtins(gc_, builtins_);
        }
    }

    void AriaVM::bootstrap_string_constants() {
        // 按下标(即表序)逐格驻留填入:str 字面量形态编译期经 index_of 折算同一下标,故下标即格位,
        // 与拼写表同源同序。须在 ctor 构造临界区内调用(GC 挂起,创建免守卫);填入即经 string_constants_
        // 可达,故解锁前发布完毕。须先于 bootstrap_registers:String 类 bootstrap 的钩子缓存按名取串,读的就是本表。
        for (usize index = 0; index < str_table::kCount; ++index) {
            string_constants_[index] = new_string(gc_, str_table::kConstants[index]);
        }
        assert_slots_filled(string_constants_, "string_constants_: unfilled slot after bootstrap");
    }

    void AriaVM::bootstrap_registers() {
        // 编排顺序即依赖序:Object 根类先建(其余内建类以它作 super);算子实现格缓存须待类表全填;
        // builtins_ 曝光最后。新增单例两处收口:注册表(runtime/value_register.hpp)加一行,本函数
        // 加一行编排。
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
        // VM 根 tracer:collect 时标五类根 -- 四类表(modules_ / builtins_ / registers_ /
        // string_constants_)+ current_ 一点(各上下文内部与 previous_ resume 链经
        // ObjMovement::trace / 对象图级联;清单见 runtime.md「共享状态」)。链根交接纪律由
        // run() 出口断言承担,此处不重复设防。
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
        });
    }

    ObjClass* AriaVM::object_class() const noexcept { return Object::as<ObjClass>(registers_[kObjectClassOffset]); }

    ObjClass* AriaVM::exception_class() const noexcept {
        return Object::as<ObjClass>(registers_[kExceptionClassOffset]);
    }

    ObjClass* AriaVM::iterator_class() const noexcept { return Object::as<ObjClass>(registers_[kIteratorClassOffset]); }

    ObjClass* AriaVM::list_class() const noexcept { return Object::as<ObjClass>(registers_[kListClassOffset]); }

    ObjClass* AriaVM::map_class() const noexcept { return Object::as<ObjClass>(registers_[kMapClassOffset]); }

    ObjClass* AriaVM::string_class() const noexcept { return Object::as<ObjClass>(registers_[kStringClassOffset]); }

    ObjClass* AriaVM::range_class() const noexcept { return Object::as<ObjClass>(registers_[kRangeClassOffset]); }

    void AriaVM::init_source_roots() {
        // 入口槽 [0] 占位 cwd(REPL / 未显式设 dir_ 时裸名搜 cwd);cwd 不可用时空串兜底
        //   (resolve_module 裸名分支跳过空根)。
        source_roots_.push_back(fs::current_dir().value_or(""));
        // 配置根 [1..] = 编译器相对 stdlib 源根(kStdlibRelPath),weakly_canonical 规范化,
        //   推导失败跳过;可经 set_source_roots 覆盖。
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
        // 入口锚:本轮入口上下文(即主上下文)。dispatch_loop 的出口(顶层 RETURN / 未捕获物化)
        // 都发生在 resume 链链根 = 入口上下文,出口断言据此钉住切换交接的完整 -- 漏交接当场炸,
        // 不等下一轮入口。
        const auto entry_ctx = current_;
        // 入口槽 [0] 原地替换为入口模块 dir_(对齐 Python sys.path[0],配置根 [1..] 不动);
        // dir_ 可空(cwd 不可用的 <script>),由 resolve_module 跳空根处理,此处不判空。
        source_roots_[0] = fn->module()->dir()->view();

        // 重复调用先清场:HALT 收场的上一轮不弹帧,不清场会把新帧叠在陈旧帧上。
        current_->reset();

        // 包空闭包:fn 跨 new_closure 顶 maybe_collect 须有根,make_guard 兜底;建成传入
        // run_closure 即压栈(入栈即根化)。
        auto       guard   = gc_.make_guard(fn);
        const auto closure = new_closure(gc_, fn);
        auto       result  = run_closure(closure); // 值拷贝,下方清场不影响返回值;持对象由调用方根化
        ASSERT(current_ == entry_ctx, "current_ is not the run entry context (unbalanced context switch)");
        // 结束再清场:防 run() 外的 GC 经 tracer 标到陈旧栈值(清场归本入口,run_closure 为
        // 重入接缝不自清)。
        current_->reset();
        return result;
    }

    Result<Value, Error> AriaVM::run_closure(ObjClosure* closure) {
        current_->push(Value::from_obj(closure)); // 入栈即根
        if (!call_closure(closure, 0)) {
            // 进帧失败(重入路径帧满为真实分支):载荷直转 Result -- 不经 unwind(帧栈叠着
            // 调用者的帧,弹不得),一帧未进亦无跟踪可烘。
            const auto [code, msg] = take_uncaught_error();
            return runtime_err(Error::from_baked(code, msg));
        }
        return dispatch_loop();
    }

    bool AriaVM::call_value(const Value callee, const u8 argc) {
        if (!callee.is_obj()) {
            return fail(ErrorCode::CallNonCallable, "type {} does not support '__call__'", type_name(callee));
        }

        switch (Object* obj = callee.as_obj(); obj->type()) {
            case ObjType::CLOSURE:
                return call_closure(Object::as<ObjClosure>(obj), argc);
            case ObjType::NATIVE_FN:
                return call_native(Object::as<ObjNativeFn>(obj), argc);
            case ObjType::CLASS:
                return call_class(Object::as<ObjClass>(obj), argc);
            case ObjType::BOUND_METHOD:
                return call_bound_method(Object::as<ObjBoundMethod>(obj), argc);
            default: {
                // 其余对象类型:按调用钩子 `__call__` 取实现后调。调用区就地复用 [callee, a1..aN]
                // 恰是 [this, args],槽 0 兼返回槽,故直接交 call_value 递归分发;取不到即报错
                //(措辞随宿主)。
                const auto target = obj->op_call_impl(*this);
                if (!target) {
                    return false; // 载荷已在挂起错误寄存器
                }
                return call_value(*target, argc);
            }
        }
    }

    bool AriaVM::call_class(ObjClass* obj, const u8 argc) {
        // 槽 0 原位换实例(即新帧 this),GC 点仅 new_instance,建成即写槽。init 恒有值(非可
        // 调用值由 call_value 报 CallNonCallable)。
        const auto instance  = new_instance(gc_, obj);
        current_->peek(argc) = Value::from_obj(instance); // 建成即写槽:instance 经值栈根化(即新帧 this)
        return call_value(obj->init(), argc);
    }

    bool AriaVM::call_bound_method(const ObjBoundMethod* obj, const u8 argc) {
        // 槽 0 原位覆写为 receiver(this 替代 callee,实参槽位不动)。方法值无需守卫:覆写后
        // 经类表槽/缓存可达。
        current_->peek(argc) = obj->receiver(); // 槽 0:bound -> this(实参槽位不动)
        return call_value(obj->method(), argc);
    }

    bool AriaVM::check_arity(const ObjFunction* fn, const u8 argc) {
        // 契约与文案见 AriaVM.hpp check_arity 注。检查纯读,无分配。
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
        // 契约见 AriaVM.hpp prepare_call_args 注。GC 走查:new_list 是唯一分配点 --额外
        // 实参 peek 在栈(「栈即根」),copy_from 走 trivial 分配不触 GC,白色 list 随即
        // drop+push 入值栈根,窗口内无 GC 点(MAKE_LIST case 同构);垫充压寄存器单例无分配。
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

    bool AriaVM::call_closure(ObjClosure* obj, const u8 argc) {
        // 编排:元数检查 -> 帧余量检查 -> 实参整形 -> 进帧;bool 契约同 call_* 族
        //(false ⟺ 载荷已 raise)。
        const auto fn = obj->function();
        if (!check_arity(fn, argc)) {
            return false;
        }
        if (current_->frames_full()) {
            return fail(ErrorCode::StackOverflow, "call frame stack overflow");
        }
        current_->enter_frame(obj, prepare_call_args(fn, argc));
        return true;
    }

    bool AriaVM::call_native(const ObjNativeFn* obj, const u8 argc) {
        // 原生函数同步调用,不进帧(契约见 ObjNativeFn.hpp)。entered_ctx 是全部事后簿记
        // (drop/寄存器断言)的锚点:成功路径 current_ 可能已被切换型原生(coroutine.resume/
        // yield)换至对侧,drop 恒落在 entered_ctx 上、使栈顶停在该调用的槽 0(切换模型下即
        // 「预留结果槽」,由对侧原语写,见 vm-design.md §4.9);false 路径「禁止 false + 切换」
        // 为永久契约(失败载荷在调用方上下文,切走了就没人消费)。实参留栈到 drop 亦是 GC 红利:
        // 切换型原生执行全程实参皆调用者栈根。
        const auto entered_ctx = current_;
        const auto slots       = Span<Value>{&current_->peek(argc), static_cast<usize>(argc + 1)};
        // 进场前寄存器应空(上次错误已被 take_error 取走 / reset 清空)。
        ASSERT(!current_->has_error(), "pending error not cleared before native call");
        if (obj->fn()(*this, slots)) {
            ASSERT(!entered_ctx->has_error(), "native fn returned true but raised error");
            entered_ctx->drop(argc);
            return true;
        }
        ASSERT(current_ == entered_ctx, "native fn returned false after switching current_");
        // 失败:载荷留寄存器交调用方 take_error(bool 契约)。
        ASSERT(entered_ctx->has_error(), "native fn returned false but raised no error");
        return false;
    }

    void AriaVM::enter_coroutine(ObjMovement* coroutine) {
        // 顺序不变式:置链必须先于换指 -- 换指后恢复者只经 coroutine->previous_ 这一条边可达
        //(GC 根只标 current_),其调用区的实参/载荷以此跨切换后的 GC 点存活(「实参留栈到
        // drop」是 call_native 的既定红利)。
        coroutine->set_previous(current_);
        coroutine->set_state(ExecState::Running);
        current_->set_state(ExecState::Normal);
        current_ = coroutine;
    }

    void AriaVM::leave_coroutine(const ExecState departing_state) {
        const auto resumer = current_->previous();
        ASSERT(resumer != nullptr, "leaving context is not on the resume chain");
        resumer->set_state(ExecState::Running);
        current_->set_state(departing_state);
        current_->set_previous(nullptr);
        current_ = resumer;
    }

    ObjModule* AriaVM::load_module(ObjString* canonical_path, const StringView import_specifier) {
        // 契约总览见 AriaVM.hpp;canonical_path 已由调用方根化。加载事实源 = modules_ 表成员资格:
        // 编译成功才入表,失败不留表项。

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

        // 4. 编译(入口名 kModuleEntryName)。编译期 Error 就地装箱透传;module 尚未入表仅由 guard
        //    根化,source 须存活到 compile() 返回。
        if (auto compiled = Compiler::compile(gc_, source, module, kModuleEntryName); !compiled) {
            current_->raise(Value::from_obj(new_exception(gc_, compiled.error())));
            return nullptr;
        }

        // 5. 入表先于模块体 run-once(体由 IMPORT 分支调起);canonical_path/module 均已根化
        //    (表 set 无 GC 点,根化跨的是上方编译期分配)。
        modules_.set(Value::from_obj(canonical_path), Value::from_obj(module));
        return module;
    }

    bool AriaVM::run_import(const ObjString* path) {
        // 契约见 AriaVM.hpp;三类失败点(解析/加载/进帧)一律 return false(载荷已 raise),
        // unwind 留 dispatch_loop 调用点。
        const auto& frame = current_->frames().top(); // 导入方帧:abs_path 作相对解析基;进帧后不再引用
        // 相对分支取当前模块 dirname 作基;dir_ 可空时 abs_path 返空串,相对分支拒绝。
        const auto canonical_path_str = resolve_module(path->view(), frame.module->abs_path(), source_roots_);
        if (!canonical_path_str) {
            return fail(ErrorCode::ModuleNotFound, "module not found: '{}'", path->view());
        }
        const auto canonical_path = new_string(gc_, *canonical_path_str);
        // guard 跨 load_module 的编译期分配(intern weak root 不保命;表 set 无 GC 点)。
        auto guard = gc_.make_guard(canonical_path);
        if (const auto module_entry = modules_.find(Value::from_obj(canonical_path)); module_entry != nullptr) {
            current_->push(module_entry->value); // 命中:体执行中即循环导入,复用半初始化对象
            return true;
        }
        const auto module = load_module(canonical_path, path->view());
        if (module == nullptr) {
            return false; // 载荷已在寄存器
        }
        // entry 经 module->entry_ 根可达;闭包建成即压栈根化。
        const auto entry = module->entry();
        ASSERT(entry != nullptr, "load_module returned a module whose entry is not set");
        const auto closure = new_closure(gc_, entry);
        current_->push(Value::from_obj(closure));
        // 进帧失败(栈溢出等):帧未进,callee 仍在栈顶(unwind 截栈时一并丢弃)。
        return call_closure(closure, 0);
    }

    template<OpCode Op>
    bool AriaVM::run_binary_operator() {
        // 对象左值取本对象算子实现后调;调用区 [lhs, rhs] 即 [this, arg1](槽 0 保持 receiver);
        // 非对象左值落 run_binary_numeric。
        // GC 走查:receiver 占调用区槽 0(栈即根);取到的实现必可达(类表值经类 -> 寄存器组 /
        // 实例字段值经槽 0 的实例 / 内建实现格经寄存器组),故无白色在途窗口、不挂守卫。
        if (const auto lhs = current_->peek(1); lhs.is_obj()) {
            if (const auto target = get_obj_binary_op_impl<Op>(*lhs.as_obj())) {
                return call_value(*target, 1);
            }
            return false;
        }
        return run_binary_numeric<Op>();
    }

    bool AriaVM::run_negate() {
        // [v] -> [r]:整数/浮点就地取负;对象左值取 __neg__ 实现后调用(一元恒零实参,调用区
        // [v] 即 [this]);其余类型报 InvalidOperand。GC 走查同 run_binary_operator。
        const Value operand = current_->peek(0);
        if (operand.is_int()) {
            current_->peek(0) = Value::from_int(-operand.as_int());
            return true;
        }
        if (operand.is_f64()) {
            current_->peek(0) = Value::from_f64(-operand.as_f64());
            return true;
        }
        if (operand.is_obj()) {
            if (const auto target = operand.as_obj()->op_negate_impl(*this)) {
                return call_value(*target, 0);
            }
            return false;
        }
        return fail(ErrorCode::InvalidOperand, "negate requires a number, got {}", type_name(operand));
    }

    template<OpCode Op>
    Opt<Value> AriaVM::get_obj_binary_op_impl(Object& obj) {
        // 指令 -> 算子实现槽的编译期映射(Op 由调用点穷举);槽语义见 Object.hpp 的算子协议
        // (返回「该算子的实现」)。
        if constexpr (Op == OpCode::ADD) {
            return obj.op_add_impl(*this);
        } else if constexpr (Op == OpCode::SUBTRACT) {
            return obj.op_sub_impl(*this);
        } else if constexpr (Op == OpCode::MULTIPLY) {
            return obj.op_mul_impl(*this);
        } else if constexpr (Op == OpCode::DIVIDE) {
            return obj.op_div_impl(*this);
        } else if constexpr (Op == OpCode::MOD) {
            return obj.op_mod_impl(*this);
        } else if constexpr (Op == OpCode::GREATER) {
            return obj.op_greater_impl(*this);
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            return obj.op_greater_equal_impl(*this);
        } else if constexpr (Op == OpCode::LESS) {
            return obj.op_less_impl(*this);
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            return obj.op_less_equal_impl(*this);
        } else {
            UNREACHABLE(); // Op 恒为上列 9 个二元指令之一(调用点穷举)
        }
    }

    template<OpCode Op>
    bool AriaVM::run_binary_numeric() {
        // 数值二元的入口:弹 2、类型守卫、按域分流(两个域语义不同,实现各住自己的辅助方法)。
        const Value b = current_->pop();
        const Value a = current_->pop();
        if (!(is_num(a) && is_num(b))) {
            return fail(ErrorCode::TypeMismatch, "operator '{}' requires numbers, got {} and {}", op_symbol(Op),
                        type_name(a), type_name(b));
        }
        if (a.is_int() && b.is_int()) {
            return run_binary_int<Op>(a.as_int(), b.as_int());
        }
        // 任一 F64 升浮点:另一侧按字面值升。
        return run_binary_f64<Op>(a.is_f64() ? a.as_f64() : static_cast<f64>(a.as_int()),
                                  b.is_f64() ? b.as_f64() : static_cast<f64>(b.as_int()));
    }

    template<OpCode Op>
    bool AriaVM::run_binary_int(const i64 lhs, const i64 rhs) {
        // 整数域九算子:结果压栈。除/模零是域特有失败(整数无 inf/nan),就地 fail;% 为 C++ 语义。
        if constexpr (Op == OpCode::ADD) {
            current_->push(Value::from_int(lhs + rhs));
        } else if constexpr (Op == OpCode::SUBTRACT) {
            current_->push(Value::from_int(lhs - rhs));
        } else if constexpr (Op == OpCode::MULTIPLY) {
            current_->push(Value::from_int(lhs * rhs));
        } else if constexpr (Op == OpCode::DIVIDE) {
            if (rhs == 0) {
                return fail(ErrorCode::DivisionByZero, "integer division by zero");
            }
            current_->push(Value::from_int(lhs / rhs));
        } else if constexpr (Op == OpCode::MOD) {
            if (rhs == 0) {
                return fail(ErrorCode::ModuloByZero, "integer modulo by zero");
            }
            current_->push(Value::from_int(lhs % rhs));
        } else if constexpr (Op == OpCode::GREATER) {
            current_->push(Value::from_bool(lhs > rhs));
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            current_->push(Value::from_bool(lhs >= rhs));
        } else if constexpr (Op == OpCode::LESS) {
            current_->push(Value::from_bool(lhs < rhs));
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            current_->push(Value::from_bool(lhs <= rhs));
        } else {
            UNREACHABLE(); // Op 恒为上列 9 个二元指令之一(调用点穷举)
        }
        return true;
    }

    template<OpCode Op>
    bool AriaVM::run_binary_f64(const f64 lhs, const f64 rhs) const {
        // 浮点域九算子:结果压栈。按 IEEE(除零得 inf/nan、% 走 fmod,无失败路径),故无 fail 分支。
        if constexpr (Op == OpCode::ADD) {
            current_->push(Value::from_f64(lhs + rhs));
        } else if constexpr (Op == OpCode::SUBTRACT) {
            current_->push(Value::from_f64(lhs - rhs));
        } else if constexpr (Op == OpCode::MULTIPLY) {
            current_->push(Value::from_f64(lhs * rhs));
        } else if constexpr (Op == OpCode::DIVIDE) {
            current_->push(Value::from_f64(lhs / rhs));
        } else if constexpr (Op == OpCode::MOD) {
            current_->push(Value::from_f64(std::fmod(lhs, rhs)));
        } else if constexpr (Op == OpCode::GREATER) {
            current_->push(Value::from_bool(lhs > rhs));
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            current_->push(Value::from_bool(lhs >= rhs));
        } else if constexpr (Op == OpCode::LESS) {
            current_->push(Value::from_bool(lhs < rhs));
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            current_->push(Value::from_bool(lhs <= rhs));
        } else {
            UNREACHABLE(); // Op 恒为上列 9 个二元指令之一(调用点穷举)
        }
        return true;
    }

    bool AriaVM::run_load_field(ObjString* name) {
        // 非对象(含 nil)统一「does not support field access」文案(协议分派需 Object*);
        // 对象 miss 的文案由协议 override 烘焙。
        const Value obj = current_->peek(0);
        if (!obj.is_obj()) {
            return fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (const auto result = obj.as_obj()->load_field(*this, name)) {
            current_->peek(0) = *result; // 写回原槽:[obj] -> [v]
            return true;
        }
        return false; // 载荷已在寄存器
    }

    bool AriaVM::run_store_field(ObjString* name) {
        // 契约见 AriaVM.hpp;非对象守卫同 run_load_field。
        const Value obj = current_->peek(1);
        if (!obj.is_obj()) {
            return fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (!obj.as_obj()->store_field(*this, name, current_->peek(0))) {
            return false; // 载荷已在寄存器
        }
        current_->peek(1) = current_->peek(0); // 单槽下移:弹 obj 留 v,[obj, v] -> [v]
        current_->drop(1);
        return true;
    }

    bool AriaVM::run_load_this_field(ObjString* name) {
        // 契约见 AriaVM.hpp;this 取顶帧槽 0(帧槽「栈即根」同 run_load_super_field),经
        // load_field 协议出值压栈([] -> [v]),miss 文案由协议 override 就地烘焙。
        const Value this_value = current_->frames().top().slots[0];
        if (const auto inst = try_obj<ObjInstance>(this_value)) {
            if (const auto result = inst->load_field(*this, name)) {
                current_->push(*result); // [] -> [v]
                return true;
            }
            return false;
        }
        return fail(ErrorCode::TypeMismatch, "field access requires an instance, got {}", type_name(this_value));
    }

    bool AriaVM::run_store_this_field(ObjString* name) {
        // 契约见 AriaVM.hpp;写腿:[v] -> [v](peek-store,值留栈,this 不经值栈)。store_field
        // 恒成功(实例字段动态),false 分支为契约透传防御形态。非实例兜底同 LOAD 腿。
        const Value this_value = current_->frames().top().slots[0];
        if (const auto inst = try_obj<ObjInstance>(this_value)) {
            if (inst->store_field(*this, name, current_->peek(0))) { // false ⟺ 已 fail(契约)
                return true;                                         // 值留栈(peek-store)
            }
            return false;
        }
        return fail(ErrorCode::TypeMismatch, "field assignment requires an instance, got {}", type_name(this_value));
    }

    bool AriaVM::run_prepare_method(ObjString* name) {
        // 契约见 AriaVM.hpp。两段式第一段:接收者在栈顶(实参尚未求值)。协议解析期间它须在栈
        // (「栈即根」)-- 基类默认的 load_field 会铸 bound、内置 override 的 miss 会装箱,两者皆是
        // 分配点。解析先于实参求值。
        const Value recv = current_->peek(0);
        if (!recv.is_obj()) {
            return fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(recv));
        }
        if (const auto target = recv.as_obj()->load_field_unbound(*this, name)) {
            current_->push(*target); // 待调值压栈:跨指令存活,GC 根由值栈承担(「栈即根」)
            return true;
        }
        return false;
    }

    bool AriaVM::run_call_method(const u8 argc) {
        // 契约见 AriaVM.hpp。纯调用,不再解析:[recv, target, a1..aN] 里实参整体下移一格补掉待调值
        // 占的那格 -> [recv, a1..aN](槽 0 = receiver = this);待调值交 call_value 统一分发
        // (argc == 0 时下移为空转)。
        const Value target = current_->peek(argc);
        for (u8 i = argc; i >= 1; --i) {
            current_->peek(i) = current_->peek(i - 1);
        }
        current_->drop(1);
        return call_value(target, argc);
    }

    bool AriaVM::run_load_index() {
        // 契约见 AriaVM.hpp;非对象守卫同 run_load_field(文案与协议基类默认同串)。
        const Value idx = current_->peek(0);
        const Value obj = current_->peek(1);
        if (!obj.is_obj()) {
            return fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        if (const auto result = obj.as_obj()->load_index(*this, idx)) {
            current_->peek(1) = *result; // 写回 obj 槽再弹 idx:[obj, idx] -> [v]
            current_->drop(1);
            return true;
        }
        return false; // 载荷已在寄存器
    }

    bool AriaVM::run_store_index() {
        // 契约见 AriaVM.hpp;非对象守卫同 run_load_index。
        const Value value = current_->peek(0);
        const Value idx   = current_->peek(1);
        const Value obj   = current_->peek(2);
        if (!obj.is_obj()) {
            return fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        if (!obj.as_obj()->store_index(*this, idx, value)) {
            return false; // 载荷已在寄存器
        }
        current_->peek(2) = value; // 值下移两格:弹 obj、idx 留 v,[obj, idx, v] -> [v]
        current_->drop(2);
        return true;
    }

    bool AriaVM::run_make_range(const u8 flags) {
        // 契约见 AriaVM.hpp。端点 peek 在栈跨 new_range 顶部 maybe_collect(「栈即根」,端点为标量
        // 整数非对象);铸完 drop 再 push(窗口内无 GC 点)。非整数端点 TypeMismatch,文案报端点类型。
        if ((flags & kRangeFlagUnbounded) != 0) {
            const Value from = current_->peek(0);
            if (!from.is_int()) {
                return fail(ErrorCode::TypeMismatch, "range bound must be an integer, got {}", type_name(from));
            }
            const auto range = new_range(gc_, from.as_int());
            current_->drop(1);
            current_->push(Value::from_obj(range));
            return true;
        }
        const bool  exclusive = (flags & kRangeFlagExclusive) != 0;
        const Value to        = current_->peek(0);
        const Value from      = current_->peek(1);
        if (!from.is_int() || !to.is_int()) {
            return fail(ErrorCode::TypeMismatch, "range bounds must be integers, got {} and {}", type_name(from),
                        type_name(to));
        }
        const auto range = new_range(gc_, from.as_int(), to.as_int(), exclusive);
        current_->drop(2);
        current_->push(Value::from_obj(range));
        return true;
    }

    bool AriaVM::run_load_super_field(ObjString* name) {
        // 契约见 AriaVM.hpp;miss 时类措辞 fail 已入寄存器,本函数只透传信号。
        const auto& frame    = current_->frames().top();
        const auto  defining = frame.closure->defining_class();
        ASSERT(defining != nullptr, "closure has no defining class (compiler invariant)");
        const auto super = defining->superclass();
        ASSERT(super != nullptr, "method class has no superclass (compiler invariant)");
        const auto hit = super->load_field(*this, name); // 从父类起读穿透(类协议)
        if (!hit) {
            return false;
        }
        if (const auto member = *hit; is_method(member)) {
            // 方法命中:建成立即压栈,this 经帧槽根化;方法对象经 super 链根可达
            current_->push(Value::from_obj(new_bound_method(gc_, member, frame.slots[0])));
        } else {
            current_->push(member); // 静态槽原值直读,不绑定不缓存
        }
        return true;
    }

    Pair<ErrorCode, String> AriaVM::take_uncaught_error() const {
        const auto payload = *current_->take_error();
        if (const auto ex = try_obj<ObjException>(payload)) {
            return {ex->code(), String{ex->message()->view()}};
        }
        const auto msg = std::format("uncaught exception: {}", format_value(payload));
        return {ErrorCode::UncaughtException, Error::make_message(ErrorCode::UncaughtException, msg)};
    }

    Opt<Error> AriaVM::unwind() {
        // 前提:寄存器已有载荷(入口断言把关)。搜索阶段不动帧栈/值栈,命中就地回退派发,全未命中
        // 交 reset 清场。链式多跳:本上下文全帧未命中且在 resume 链上 -> 协程置 Failed 死去、载荷
        // 转投 caller 继续搜,逐跳向链根推进(中间层无 handler 即连死,其 Running 瞬态被 Failed
        // 覆盖合法);链终止 = 主上下文,未捕获物化。
        struct TraceEntry {
            ObjFunction* fn;     // 帧函数(名字渲染)
            ObjModule*   mod;    // 帧模块(位置串渲染,ObjModule::format_location)
            u32          ip_off; // 行号经 fn->unit().line_for_offset 查
        };

        ASSERT(current_->has_error(), "no pending payload");

        while (true) {
            List<TraceEntry> trace; // 收集序即物化序:内 -> 外,最内帧紧贴错误消息行(主流 traceback
                                    // 惯例)。每跳重新收集:死在边界的协程帧不并入物化侧的跟踪
            // 自最内(栈顶)向外搜索 try 记录;命中帧保留 -- handler 偏移与栈基址都属于它。
            auto& frames = current_->frames();
            for (usize i = frames.size() - 1; i < frames.size(); --i) { // 无符号反向:下溢即终止
                auto& [closure, unit, module, ip, slots, last_ip] = frames[i];
                const u32 ip_off                                  = static_cast<u32>(last_ip - unit->code.data());
                if (const auto rec = unit->find_try_handler(ip_off)) {
                    // 命中:回退到命中帧并转入 catch handler(统一在 ObjMovement::unwind_to_handler);
                    // 调用方 break 回循环顶重取帧(坑 #11)。命中可能在多跳之后 -- current_ 已非进入
                    // unwind 时的上下文,调用方一律经循环顶自 current_ 重取帧,无需特判。
                    current_->unwind_to_handler(i, **rec);
                    return std::nullopt;
                }
                trace.emplace_back(closure->function(), module, ip_off);
            }

            // 本上下文全帧未命中:在链上转投一跳,链终止(主上下文)才物化。
            if (const auto caller = current_->previous()) {
                // 转投一跳,四步定序承重:take 先于 reset(寄存器在 reset 内一并清空);reset 先关开指
                // 再清场(死协程不留栈,向外泄漏的捕获闭包取值安全);raise 收尾 -- caller 寄存器此刻
                // 必空(call_native 进场断言锁「进场前寄存器空」,切换型原生返 true 不写载荷,挂起期间
                // 亦无人可写非执行上下文的寄存器)。take 到 raise 之间无 GC 点,载荷局部持有不丢根。
                const auto payload = current_->take_error();
                current_->reset();
                leave_coroutine(ExecState::Failed);
                caller->raise(*payload);
            } else {
                // 未捕获出口:reset 一次清场,拆 (码, 消息) 逐帧烘焙跟踪行物化。trace 恒非空(调用点
                // 帧栈非空不变式,每跳皆然 -- 挂起于切换点的上下文帧栈必非空);物化路径仅 std::string
                // 拼接,无 GC 分配点,fn/mod 裸指针不悬垂。
                auto [code, msg] = take_uncaught_error();
                current_->reset();
                for (const auto& [fn, mod, ip_off]: trace) {
                    const u32 line = fn->unit().line_for_offset(ip_off);
                    msg += std::format("\n  at {} ({})", fn->name()->view(), mod->format_location(line));
                }
                return Error::from_baked(code, msg);
            }
        }
    }

    Result<Value, Error> AriaVM::dispatch_loop() {
        // 栈/帧/寄存器一律经 current_ 访问;M6 切换模型下「正在执行的字节码所在上下文恒等于
        // current_」,切换点唯一且显式,本循环永不重入(§4.9)。

        while (true) {
            // 不变式:此处帧栈恒非空(唯一弹空帧的顶层 RETURN 立即 return;CALL/IMPORT 切帧后
            // break 回循环顶重取)。取指前记本帧指令起始指针(last_ip):报错行号锚点,unwind
            // 查表同用此字段(顶帧 = 故障指令,外层帧 = CALL 站点)。
            auto frame     = &current_->frames().top();
            frame->last_ip = frame->ip;
#ifdef DEBUG_TRACE_EXECUTION
            // 取 opcode 前打印执行状态(见 trace_execution)。
            trace_execution(current_);
#endif
            // 各 case 按 bytecode/code.hpp 枚举序排列。退出约定:成功路径一律 break 回循环顶;
            // 错误站点(raise / run_* 返 false)一律 goto unwind_check -- 收口处 unwind 返 Error
            // 即未捕获(return 终止循环),返 nullopt 即已派发 handler、帧引用已废,落回循环顶
            // 重取。**switch 之后不得新增引用 frame 的代码**(坑 #11 的防御前提,unwind_check
            // 标签体同守)。取指经 ARIA_FETCH_OPCODE 宏:常态构建即原表达式,探针构建在此单点
            // 记账(见 runtime/opcode_profile.hpp)。
            switch (auto op = ARIA_FETCH_OPCODE(frame)) {
                case OpCode::HALT:
                    // 真实程序不产 HALT(仅手写字节码用);运行态空 previous 即主上下文(切换点
                    // 先置链、挂起即解链),协程内落 HALT 会打破 run() 出口断言,提前在此钉住。
                    ASSERT(current_->previous() == nullptr, "HALT reached inside a coroutine");
                    return Value::nil_val();

                // 数据加载与存储
                case OpCode::LOAD_CONST: {
                    const auto idx = read_u16(frame);
                    current_->push(frame->unit->constants[idx]);
                    break;
                }
                case OpCode::LOAD_NIL:
                    current_->push(Value::nil_val());
                    break;
                case OpCode::LOAD_TRUE:
                    current_->push(Value::true_val());
                    break;
                case OpCode::LOAD_FALSE:
                    current_->push(Value::false_val());
                    break;
                case OpCode::LOAD_IMM: {
                    const u8 raw = read_u8(frame);
                    // u8 操作数按 i8 位型重解释做符号扩展(发射侧先经 i8 再转 u8,见 CodeGen)。
                    current_->push(Value::from_i32(std::bit_cast<i8>(raw)));
                    break;
                }
                case OpCode::LOAD_REG: {
                    // [] -> [regs[n]]:压 VM 值寄存器(单例对象,bootstrap 填充;索引即注册表枚举值,
                    // 编译器只发合法下标,同 LOAD_LOCAL 槽访问不设防)。
                    current_->push(Value::from_obj(registers_[read_u8(frame)]));
                    break;
                }
                case OpCode::LOAD_LOCAL: {
                    const auto slot = read_u16(frame);
                    current_->push(frame->slots[slot]);
                    break;
                }
                // N 短变体:槽号即枚举名尾号,零操作数;逐条独立 case 写死槽号常量,免运行期换算。
                case OpCode::LOAD_LOCAL_1:
                    current_->push(frame->slots[1]);
                    break;
                case OpCode::LOAD_LOCAL_2:
                    current_->push(frame->slots[2]);
                    break;
                case OpCode::LOAD_LOCAL_3:
                    current_->push(frame->slots[3]);
                    break;
                case OpCode::LOAD_LOCAL_4:
                    current_->push(frame->slots[4]);
                    break;
                case OpCode::LOAD_LOCAL_5:
                    current_->push(frame->slots[5]);
                    break;
                case OpCode::LOAD_LOCAL_6:
                    current_->push(frame->slots[6]);
                    break;
                case OpCode::LOAD_LOCAL_7:
                    current_->push(frame->slots[7]);
                    break;
                case OpCode::LOAD_LOCAL_8:
                    current_->push(frame->slots[8]);
                    break;
                case OpCode::STORE_LOCAL: {
                    const auto slot    = read_u16(frame);
                    frame->slots[slot] = current_->peek(0);
                    break;
                }
                case OpCode::STORE_LOCAL_1:
                    frame->slots[1] = current_->peek(0);
                    break;
                case OpCode::STORE_LOCAL_2:
                    frame->slots[2] = current_->peek(0);
                    break;
                case OpCode::STORE_LOCAL_3:
                    frame->slots[3] = current_->peek(0);
                    break;
                case OpCode::STORE_LOCAL_4:
                    frame->slots[4] = current_->peek(0);
                    break;
                case OpCode::STORE_LOCAL_5:
                    frame->slots[5] = current_->peek(0);
                    break;
                case OpCode::STORE_LOCAL_6:
                    frame->slots[6] = current_->peek(0);
                    break;
                case OpCode::STORE_LOCAL_7:
                    frame->slots[7] = current_->peek(0);
                    break;
                case OpCode::STORE_LOCAL_8:
                    frame->slots[8] = current_->peek(0);
                    break;
                case OpCode::LOAD_UPVALUE: {
                    // 压本闭包第 idx 个 upvalue 的当前值(开/闭两态统一经 value_slot() 取址)。
                    const u8 idx = read_u8(frame);
                    current_->push(*frame->closure->upvalues()[idx]->value_slot());
                    break;
                }
                case OpCode::STORE_UPVALUE: {
                    // peek-store 到该 upvalue:open 态写穿到栈槽,closed 态写自持。
                    const u8 idx                                   = read_u8(frame);
                    *frame->closure->upvalues()[idx]->value_slot() = current_->peek(0);
                    break;
                }
                case OpCode::CLOSE_UPVALUE: {
                    // 关闭所有槽址 >= 当前栈顶的开 upvalue,无弹栈 -- 弹栈由前置 POP_N 承担(编译器
                    // 在弹区 POP_N 之后发射,弹区槽已位于 top 之上,不 push 不覆写即安全)。
                    current_->close_upvalues(current_->stack_top());
                    break;
                }
                case OpCode::DEF_GLOBAL: {
                    // [v] -> []:以常量池 name 为键在当前模块 globals 首次定义(顶层 var -- 唯一
                    // 创建全局的入口)。栈即根:v 用 peek 不弹 -- 留 v 在值栈跨 set(保守惯例),
                    // set 返回后才 drop。
                    ObjString* name = read_name(frame);
                    frame->module->globals().set(Value::from_obj(name), current_->peek(0));
                    current_->drop(1); // 写完才弹,栈效应仍为 [v] -> []
                    break;
                }
                case OpCode::LOAD_GLOBAL: {
                    // [] -> [v]:按名查当前模块 globals,miss 回退 VM 级 builtins_(Python 式查找链);
                    // 皆未命中 -> UndefinedVariable。push 先写栈再 grow,载荷已入栈后方可能 collect。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    auto        entry = frame->module->globals().find(key);
                    if (entry == nullptr) {
                        entry = builtins_.find(key); // 回退 builtins_
                        if (entry == nullptr) {
                            raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                            goto unwind_check;
                        }
                    }
                    current_->push(entry->value);
                    break;
                }
                case OpCode::STORE_GLOBAL: {
                    // [v] -> [v]:peek-store 到模块 globals;未定义 -> UndefinedVariable(赋值不
                    // 隐式创建)。无分配。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame->module->globals().find(key);
                    if (entry == nullptr) {
                        raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                        goto unwind_check;
                    }
                    entry->value = current_->peek(0);
                    break;
                }
                case OpCode::LOAD_FIELD:
                    // name:u16;[obj] -> [v]
                    if (!run_load_field(read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_FIELD:
                    // name:u16;[obj, v] -> [v](单槽下移留 v)
                    if (!run_store_field(read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LOAD_INDEX:
                    // [obj, idx] -> [v]
                    if (!run_load_index()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_INDEX:
                    // [obj, idx, v] -> [v](peek-store,值下移两格)
                    if (!run_store_index()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LOAD_THIS_FIELD:
                    // name:u16;[] -> [v]
                    if (!run_load_this_field(read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_THIS_FIELD:
                    // name:u16;[v] -> [v](peek-store)
                    if (!run_store_this_field(read_name(frame))) {
                        goto unwind_check;
                    }
                    break;

                // 算术与逻辑
                // 相等性(EQUAL 走 == 内容相等;STRICT 走 === 严格相等)
                case OpCode::EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(value_equal(a, b)));
                    break;
                }
                case OpCode::NOT_EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(!value_equal(a, b)));
                    break;
                }
                case OpCode::STRICT_EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(value_identical(a, b)));
                    break;
                }
                case OpCode::STRICT_NOT_EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(!value_identical(a, b)));
                    break;
                }
                // 比较
                case OpCode::GREATER:
                    if (!run_binary_operator<OpCode::GREATER>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::GREATER_EQUAL:
                    if (!run_binary_operator<OpCode::GREATER_EQUAL>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LESS:
                    if (!run_binary_operator<OpCode::LESS>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LESS_EQUAL:
                    if (!run_binary_operator<OpCode::LESS_EQUAL>()) {
                        goto unwind_check;
                    }
                    break;
                // 算术(五算子共用执行体 run_binary_operator)
                case OpCode::ADD:
                    if (!run_binary_operator<OpCode::ADD>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::SUBTRACT:
                    if (!run_binary_operator<OpCode::SUBTRACT>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MULTIPLY:
                    if (!run_binary_operator<OpCode::MULTIPLY>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::DIVIDE:
                    if (!run_binary_operator<OpCode::DIVIDE>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MOD:
                    if (!run_binary_operator<OpCode::MOD>()) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::NOT:
                    current_->push(Value::from_bool(!is_truthy(current_->pop())));
                    break;
                case OpCode::NEGATE:
                    if (!run_negate()) {
                        goto unwind_check;
                    }
                    break;

                // 栈操作
                case OpCode::POP:
                    current_->drop(1);
                    break;
                case OpCode::POP_N: {
                    const u8 n = read_u8(frame);
                    current_->drop(n);
                    break;
                }
                case OpCode::DUP:
                    current_->push(current_->peek(0));
                    break;
                case OpCode::DUP2: {
                    const Value b = current_->peek(0);
                    const Value a = current_->peek(1);
                    current_->push(a);
                    current_->push(b);
                    break;
                }

                // 调试
                case OpCode::NOP:
                    break;

                // 控制流(u16 无符号;前向 JUMP* ip+=off,后向 JUMP_BACK ip-=off;
                // 偏移以读完操作数后的 ip 为基准,同 Disassembler 解码约定)
                case OpCode::JUMP: {
                    const u16 off = read_u16(frame);
                    frame->ip += off;
                    break;
                }
                case OpCode::JUMP_TRUE: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(current_->pop())) {
                        frame->ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_TRUE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(current_->peek(0))) {
                        frame->ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current_->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_FALSE: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(current_->pop())) {
                        frame->ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_FALSE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(current_->peek(0))) {
                        frame->ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current_->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_BACK: {
                    const u16 off = read_u16(frame);
                    frame->ip -= off;
                    // safe point:循环回边触发回收;maybe_collect 不移动值栈/帧,frame 指针跨调用有效。
                    gc_.maybe_collect();
                    break;
                }

                // 函数与闭包
                case OpCode::CALL: {
                    const u8 argc = read_u8(frame);
                    // 良构不变式:栈上必有 callee + argc 个实参。
                    ASSERT(current_->stack_size() >= static_cast<usize>(argc) + 1, "malformed stack");
                    if (const Value callee = current_->peek(argc); !call_value(callee, argc)) {
                        goto unwind_check;
                    }
                    break;
                }
                case OpCode::CLOSURE: {
                    // fn:u16;[] -> [closure]:取常量池 ObjFunction 现场包 ObjClosure,按捕获描述表
                    // (upvalue_descs_,存 fn 元数据不进字节码流,指令集 §4.13)逐个填:is_local 捕直接
                    // 外围帧局部槽(经 capture_upvalue 单点收口「同一局部一份引用」),否则复制外围
                    // 闭包的第 index 个 upvalue(共享同一份引用)。
                    // 根安全(「栈即根」):闭包建成立即压栈,desc 循环内 new_upvalue 顶 maybe_collect
                    // 不再威胁闭包,免守卫。
                    const auto idx     = read_u16(frame);
                    const auto fn      = Object::as<ObjFunction>(frame->unit->constants[idx].as_obj());
                    auto       closure = new_closure(gc_, fn);
                    current_->push(Value::from_obj(closure)); // 立即入栈:值栈即根,跨 desc 循环免守卫
                    for (const auto& [is_local, index]: fn->upvalue_descs()) {
                        if (is_local) {
                            closure->add_upvalue(current_->capture_upvalue(gc_, frame->slots + index));
                        } else {
                            closure->add_upvalue(frame->closure->upvalues()[index]);
                        }
                    }
                    break;
                }

                // 类与对象
                case OpCode::MAKE_CLASS: {
                    // name:u16;[super] -> [class]:peek super 不先弹 -- new_class 顶 maybe_collect
                    // 须 super 在栈(「栈即根」);非类值是**语言可达**错误(superclass 运行期才知
                    // 值类型),故 raise 而非 ASSERT。建成写回原槽;init 继承收进对象构造。
                    if (const auto super = try_obj<ObjClass>(current_->peek(0))) {
                        const auto klass  = new_class(gc_, read_name(frame), super);
                        current_->peek(0) = Value::from_obj(klass);
                        break;
                    }

                    raise(ErrorCode::TypeMismatch, "superclass must be a class, got {}", type_name(current_->peek(0)));
                    goto unwind_check;
                }
                case OpCode::MAKE_METHOD: {
                    // name:u16;[class, closure] -> [class]:实例方法注册(静态经 MAKE_STATIC;仅收闭包
                    // -- 方法性 = defining class 戳)。栈形经 ASSERT 钉(值恒来自上一条 CLOSURE,语言
                    // 写不出违例)。副作用:set_field 命中 "init" 同步 init_ + 闭包戳 defining class
                    // (一职双任:super 来源 + 方法性标记,读路径据非空判绑)。
                    const auto klass  = try_obj<ObjClass>(current_->peek(1));
                    const auto method = current_->peek(0);
                    ASSERT(klass != nullptr, "slot-1 is not a class (malformed stack)");
                    klass->set_field(read_name(frame), method);

                    const auto closure = try_obj<ObjClosure>(method);
                    ASSERT(closure != nullptr, "slot-0 is not a closure (method registration is closure-only)");
                    closure->set_defining_class(klass);
                    current_->drop(1);
                    break;
                }
                case OpCode::MAKE_STATIC: {
                    // name:u16;[class, value] -> [class]:静态成员注册(var 声明与 fun 静态方法同经此;
                    // 不戳 defining class ⟹ 读恒原值)。与 MAKE_METHOD 同形,栈形 ASSERT 钉。
                    const auto klass = try_obj<ObjClass>(current_->peek(1));
                    ASSERT(klass != nullptr, "slot-1 is not a class (malformed stack)");
                    klass->set_field(read_name(frame), current_->peek(0));
                    current_->drop(1); // 弹 value 留 class:[class, value] -> [class]
                    break;
                }
                case OpCode::LOAD_SUPER_FIELD:
                    // name:u16;[] -> [v]
                    if (!run_load_super_field(read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::PREPARE_METHOD:
                    // name:u16;[recv] -> [recv, target]
                    if (!run_prepare_method(read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::CALL_METHOD:
                    // argc:u8;[recv, target, a1..aN] -> [r]
                    if (!run_call_method(read_u8(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MAKE_LIST: {
                    // n:u16;[v1..vn] -> [list]:元素 peek 在栈跨 new_list 顶部 maybe_collect(「栈即
                    // 根」),整段拷入走 trivial 分配不触 GC,拷完 drop n 再 push(窗口内无 GC 点)。
                    const u16  count = read_u16(frame);
                    const auto list  = new_list(gc_);
                    list->elements().copy_from({current_->stack_top() - count, count});
                    current_->drop(count);
                    current_->push(Value::from_obj(list));
                    break;
                }
                case OpCode::MAKE_MAP: {
                    // n:u16;[k1,v1..kn,vn] -> [map]:键值 peek 在栈跨 new_map 顶部 maybe_collect
                    // (「栈即根」);逐对 set 与 rehash 走 GC 分配器不触 GC(HashTable 注释),拷完
                    // drop 2n 再 push(窗口内无 GC 点)。重复键后键胜(set 命中原槽覆写)。
                    const u16  count = read_u16(frame);
                    const auto map   = new_map(gc_);
                    const auto base  = current_->stack_top() - count * 2;
                    for (usize i = 0; i < count; ++i) {
                        map->table().set(base[i * 2], base[i * 2 + 1]);
                    }
                    current_->drop(count * 2);
                    current_->push(Value::from_obj(map));
                    break;
                }
                case OpCode::MAKE_RANGE:
                    // flags:u8;[from, to] -> [range](无上界 [from] -> [range])
                    if (!run_make_range(read_u8(frame))) {
                        goto unwind_check;
                    }
                    break;

                // 模块导入
                case OpCode::IMPORT:
                    // path:u16;[..., module]
                    if (!run_import(read_name(frame))) {
                        goto unwind_check;
                    }
                    break;

                // 异常
                case OpCode::THROW: {
                    // 用户 throw:弹抛出值,原值入寄存器(不包 ObjException -- catch 绑原值保类型)后
                    // unwind。
                    current_->raise(current_->pop());
                    goto unwind_check;
                }

                // 返回(exit_frame 后 frame 失效,故先取返回值)
                case OpCode::RETURN: {
                    const Value ret = current_->pop(); // 取返回值(exit_frame 将丢弃其下方栈区)
                    current_->exit_frame(); // 弹帧 + 关本帧区间开指(值迁入各自 upvalue 自持)+ 值栈顶复位,一体
                    if (current_->frames().empty()) {
                        const auto resumer = current_->previous();
                        if (resumer == nullptr) {
                            return ret; // 顶层(主入口 <main>)返回:写回值即主模块对象(入口收尾所压)
                        }
                        // 协程最外帧(闭包体)返回即完成:值写 resumer 预留槽 -> reset 清场
                        //(死协程不留栈,[closure] 槽一并清;ret 已弹入局部,reset
                        // 不殃及;reset 先关开指,泄漏闭包取值安全)-> leave_coroutine 置 Done、解链、resumer
                        // 置 Running 并换指。不往协程自己栈上 push 返回值。
                        resumer->peek(0) = ret;
                        current_->reset();
                        leave_coroutine(ExecState::Done);
                        break;
                    }
                    current_->push(ret); // 通用写回 callee 槽:IMPORT 的模块值 = 模块体自己的返回值
                    break;
                }

                default:
                    UNREACHABLE();
            }
            continue; // 正常路径:break 出 switch 后回循环顶,不落 unwind_check
        // unwind 收口:错误站点(raise / run_* 返 false)统一跳此。未捕获 -> 物化 Error 终止
        // 循环;已派发 handler -> 落回循环尾、回循环顶重取帧。标签体不引用 frame(坑 #11)。
        unwind_check:
            if (auto u = unwind()) {
                return runtime_err(std::move(*u));
            }
        }
    }

} // namespace aria
