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
#include "object/ObjException.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjList.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "object/iterator/ObjRangeIterator.hpp"
#include "runtime/Eval.hpp"
#include "runtime/builtins/BoundMethodClass.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "runtime/builtins/ClassClass.hpp"
#include "runtime/builtins/CoroutineClass.hpp"
#include "runtime/builtins/ExceptionClass.hpp"
#include "runtime/builtins/FunctionClass.hpp"
#include "runtime/builtins/IteratorClass.hpp"
#include "runtime/builtins/ListClass.hpp"
#include "runtime/builtins/MapClass.hpp"
#include "runtime/builtins/ModuleClass.hpp"
#include "runtime/builtins/ObjectClass.hpp"
#include "runtime/builtins/RangeClass.hpp"
#include "runtime/builtins/StringClass.hpp"
#include "runtime/opcode_profile.hpp"
#include "runtime/stdlib/MathModule.hpp"
#include "util/fs.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {
    namespace {

        template<typename T>
        void assert_slots_filled(const List<T*>& slots, const char* what) noexcept {
            for (const auto slot: slots) {
                ASSERT(slot != nullptr, what);
            }
        }

        Result<Value, Error> runtime_err(Error err) { return std::unexpected(std::move(err)); }

        [[nodiscard]]
        StringView stem_of(const StringView spec) {
            const auto last_slash = spec.find_last_of('/');
            const auto last_seg   = (last_slash == StringView::npos) ? spec : spec.substr(last_slash + 1);
            if (last_seg.size() > kAriaExtension.size() && last_seg.ends_with(kAriaExtension)) {
                return spec.substr(0, spec.size() - kAriaExtension.size());
            }
            return spec;
        }

        [[nodiscard]]
        Opt<String> probe_bases(const List<stdfs::path>& bases, const StringView file_name) {
            std::error_code ec;
            for (const auto& base: bases) {
                const auto canon = stdfs::weakly_canonical(base / file_name, ec);
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

    } // namespace

    AriaVM::AriaVM() :
        gc_{}, current_{nullptr}, modules_{&gc_}, builtins_{&gc_}, source_roots_{}, registers_{kValueRegisterCount},
        str_table_{str_table::kCount}, char_cache_{kCharCacheSlots} {
        current_ = new_movement(gc_, *this); // 首笔分配:gc_ 尚无对象,顶部 maybe_collect 无可回收
        hook_vm_roots();
        init_source_roots();
        {
            // 启动时暂禁 GC，解锁前须将全部对象发布到 tracer 可达处。
            const auto lock = gc_.make_lock();
            bootstrap_str_table();
            bootstrap_char_cache();
            bootstrap_registers();
            Builtin::register_builtins(gc_, builtins_);
        }
    }

    void AriaVM::bootstrap_str_table() {
        for (usize index = 0; index < str_table::kCount; ++index) {
            str_table_[index] = new_string(gc_, str_table::kConstants[index]);
        }
        assert_slots_filled(str_table_, "str_table_: unfilled slot after bootstrap");
    }

    void AriaVM::bootstrap_char_cache() {
        for (usize byte = 0; byte < kCharCacheSlots; ++byte) {
            char_cache_[byte] = new_string(gc_, static_cast<char>(byte));
        }
        assert_slots_filled(char_cache_, "char_cache_: unfilled slot after bootstrap");
    }

    void AriaVM::bootstrap_registers() {
        registers_[kObjectClassOffset]      = ObjectClass::make_class(gc_);
        registers_[kExceptionClassOffset]   = ExceptionClass::make_class(gc_, object_class());
        registers_[kIteratorClassOffset]    = IteratorClass::make_class(gc_, object_class());
        registers_[kListClassOffset]        = ListClass::make_class(gc_, object_class());
        registers_[kMapClassOffset]         = MapClass::make_class(gc_, object_class());
        registers_[kStringClassOffset]      = StringClass::make_class(gc_, object_class());
        registers_[kRangeClassOffset]       = RangeClass::make_class(gc_, object_class());
        registers_[kFunctionClassOffset]    = FunctionClass::make_class(gc_, object_class());
        registers_[kBoundMethodClassOffset] = BoundMethodClass::make_class(gc_, object_class());
        registers_[kModuleClassOffset]      = ModuleClass::make_class(gc_, object_class());
        registers_[kCoroutineClassOffset]   = CoroutineClass::make_class(gc_, object_class());
        registers_[kClassClassOffset]       = ClassClass::make_class(gc_, object_class());
        // 缺参印章仅靠私有对象身份判等，用户无法传入。
        registers_[kDefaultMarkOffset] = new_class(gc_, "<default>", nullptr);
        registers_[kMatchNoArmOffset] = new_exception(gc_, Error::from_detail(ErrorCode::MatchNoArm, "no arm matched"));

        builtins_.set(Value::from_obj(object_class()->name()), Value::from_obj(object_class()));
        builtins_.set(Value::from_obj(exception_class()->name()), Value::from_obj(exception_class()));
        builtins_.set(Value::from_obj(iterator_class()->name()), Value::from_obj(iterator_class()));
        builtins_.set(Value::from_obj(list_class()->name()), Value::from_obj(list_class()));
        builtins_.set(Value::from_obj(map_class()->name()), Value::from_obj(map_class()));
        builtins_.set(Value::from_obj(string_class()->name()), Value::from_obj(string_class()));
        builtins_.set(Value::from_obj(range_class()->name()), Value::from_obj(range_class()));
        builtins_.set(Value::from_obj(function_class()->name()), Value::from_obj(function_class()));
        builtins_.set(Value::from_obj(bound_method_class()->name()), Value::from_obj(bound_method_class()));
        builtins_.set(Value::from_obj(module_class()->name()), Value::from_obj(module_class()));
        builtins_.set(Value::from_obj(coroutine_class()->name()), Value::from_obj(coroutine_class()));
        builtins_.set(Value::from_obj(class_class()->name()), Value::from_obj(class_class()));
        assert_slots_filled(registers_, "registers_: unfilled slot after bootstrap");
    }

    void AriaVM::hook_vm_roots() {
        gc_.set_vm_roots([this](GC& g) {
            modules_.trace(g);
            builtins_.trace(g);
            g.mark_object(current_);
            for (const auto reg: registers_) {
                g.mark_object(reg);
            }
            for (const auto string: str_table_) {
                g.mark_object(string);
            }
            for (const auto ch: char_cache_) {
                g.mark_object(ch); // 单字符常量:恒驻留(tracer 标根,池表项同保不摘白)
            }
        });
    }

#define ARIA_VALUE_REGISTER_ACCESSOR_DEF_CLASS(name, accessor, type) \
    type* AriaVM::accessor() const noexcept { return registers_[k##name##Offset]->as<type>(); }
    ARIA_VALUE_REGISTER_CLASS_LIST(ARIA_VALUE_REGISTER_ACCESSOR_DEF_CLASS)
#undef ARIA_VALUE_REGISTER_ACCESSOR_DEF_CLASS

#define ARIA_VALUE_REGISTER_ACCESSOR_DEF_SINGLETON(name, accessor, type) \
    type* AriaVM::accessor() const noexcept { return registers_[k##name##Offset]; }
    ARIA_VALUE_REGISTER_SINGLETON_LIST(ARIA_VALUE_REGISTER_ACCESSOR_DEF_SINGLETON)
#undef ARIA_VALUE_REGISTER_ACCESSOR_DEF_SINGLETON

    void AriaVM::init_source_roots() {
        source_roots_.push_back(fs::current_dir().value_or(""));
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
        const auto module = new_module(gc_, kScriptModuleName);
        auto       guard  = gc_.make_guard(module);

        SourceFile source{String{kScriptModuleName}, String{kScriptModuleName}, String{src}};
        return interpret_run(source, module);
    }

    InterpretResult AriaVM::interpret_from_path(const StringView path) {
        auto loaded = SourceFile::from_path(path);
        if (!loaded) {
            const auto detail = std::format("cannot read source file '{}'", path);
            io::println(stderr, "{}", Error::from_detail(ErrorCode::FileReadFailed, detail).message());
            return InterpretResult::LoadError;
        }
        SourceFile source = std::move(*loaded);

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

    Result<Value, Error> AriaVM::run(ObjFnProto* proto) {
        // 执行须最终回到入口上下文，出口断言检查协程交接完整性。
        const auto entry_ctx = current_;
        // 入口槽 [0] 原地替换为入口模块 dir_;dir_ 可空。
        source_roots_[0] = proto->module()->dir()->view();

        // 先清理上一轮 HALT 可能留下的帧，避免跨 run 叠帧。
        current_->reset();

        const auto guard          = gc_.make_guard(proto);
        const auto entry_function = new_function(gc_, proto);

        current_->push(Value::from_obj(entry_function)); // 入栈即根
        Eval::call_aria_fn(*current_, entry_function, 0);
        Result<Value, Error> result =
                Eval::dispatch_loop(*current_) ? current_->pop() : runtime_err(rebake_uncaught_error());
        ASSERT(current_ == entry_ctx, "current_ is not the run entry context (unbalanced context switch)");

        current_->reset();
        return result;
    }

    void AriaVM::enter_coroutine(ObjMovement* coroutine) {
        // 须先置 previous_ 链再换上下文，恢复者及其实参依此跨后续 GC 保活。
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

    Opt<String> AriaVM::resolve_module(const StringView specifier, const StringView current_module_path) const {
        const auto file_name = util::concat_string(stem_of(specifier), kAriaExtension);

        if (specifier.starts_with("./") || specifier.starts_with("../") || specifier == "." || specifier == "..") {
            if (current_module_path.empty()) {
                return std::nullopt;
            }
            List<stdfs::path> bases;
            bases.push_back(stdfs::path{String{current_module_path}}.parent_path());
            return probe_bases(bases, file_name);
        }

        List<stdfs::path> bases;
        for (const auto& root: source_roots_) {
            if (!root.empty()) {
                bases.emplace_back(root);
            }
        }
        return probe_bases(bases, file_name);
    }

    ObjModule* AriaVM::load_stdlib_module(ObjString* module_key) {
        static constexpr Pair<StringView, void (*)(AriaVM&, ObjModule*)> kStdlibModules[] = {
                {"std:math", stdlib::register_math_exports},
        };
        const auto name = module_key->view();
        for (const auto& [registered_name, register_exports]: kStdlibModules) {
            if (name == registered_name) {
                const auto module = new_module(gc_, registered_name, "");
                const auto guard  = gc_.make_guard(module);
                register_exports(*this, module);
                modules_.set(Value::from_obj(module_key), Value::from_obj(module));
                return module;
            }
        }
        return current_->fail(ErrorCode::ModuleNotFound, "module not found: '{}'", name);
    }

    ObjModule* AriaVM::load_file_module(ObjString* canonical_path, const StringView import_specifier) {
        const auto path       = canonical_path->view();
        auto       loaded_src = SourceFile::from_path(path);
        if (!loaded_src) {
            return current_->fail(ErrorCode::ModuleNotFound, "failed to load module '{}': read/decode error",
                                  import_specifier);
        }
        SourceFile source = std::move(*loaded_src);

        const auto [name, dir] = fs::module_name_and_dir(path);
        if (name.empty()) {
            return current_->fail(ErrorCode::ModuleNotFound, "module path has no valid name: '{}'", import_specifier);
        }

        const auto module = new_module(gc_, name, dir);
        const auto guard  = gc_.make_guard(module);
        if (const auto compiled = Compiler::compile(gc_, source, module, kModuleEntryName); !compiled) {
            current_->raise(Value::from_obj(new_exception(gc_, compiled.error())));
            return nullptr;
        }
        // 执行入口前入表，循环导入才能复用初始化中的对象。
        modules_.set(Value::from_obj(canonical_path), Value::from_obj(module));
        return module;
    }

    Pair<ErrorCode, String> AriaVM::take_uncaught_error() const {
        const auto payload = *current_->take_error();
        if (const auto ex = try_as_obj<ObjException>(payload)) {
            return {ex->code(), String{ex->message()->view()}};
        }
        const auto msg = std::format("uncaught exception: {}", value_repr(payload));
        return {ErrorCode::UncaughtException, Error::make_message(ErrorCode::UncaughtException, msg)};
    }

    Error AriaVM::rebake_uncaught_error() const {
        struct TraceEntry {
            ObjFnProto* proto;  // 函数原型(名字渲染)
            ObjModule*  module; // 帧模块(位置串渲染,ObjModule::format_location)
            u32         ip_off; // 行号经 proto->unit().line_for_offset 查
        };
        List<TraceEntry> trace;

        auto& frames = current_->frames();
        for (usize i = frames.size(); i > 0; --i) {
            auto& [function, unit, module, ip, slots, last_ip] = frames[i - 1];
            trace.emplace_back(function->proto(), module, static_cast<u32>(last_ip - unit->code.data()));
        }
        auto [code, msg] = take_uncaught_error();
        current_->reset();
        for (const auto& [proto, module, ip_off]: trace) {
            const u32 line = proto->unit().line_for_offset(ip_off);
            msg += std::format("\n  at {} ({})", proto->name()->view(), module->format_location(line));
        }
        return Error::from_baked(code, msg);
    }

} // namespace aria
