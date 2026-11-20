#ifndef ARIA_VM_HPP
#define ARIA_VM_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/opcode_profile.hpp"
#include "runtime/str_table.hpp"
#include "runtime/value_register.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    class ObjBoundMethod;
    class ObjClass;
    class ObjFunction;
    class ObjFnProto;
    class ObjInstance;
    class ObjModule;
    class ObjString;

    // interpret 入口已将错误渲染到 stderr；需要值和错误详情时使用 run。
    enum class InterpretResult : u8 {
        Ok,           // 编译并执行成功
        CompileError, // 编译失败(词法/语法/语义)
        RuntimeError, // 运行期未捕获错误
        LoadError,    // 源文件加载失败(仅 interpret_from_path:I/O 或 UTF-8 编码)
    };

    // 持有 GC、共享服务表和当前执行上下文；字节码执行由 Eval 驱动。
    class AriaVM {
    public:
        AriaVM();

        AriaVM(const AriaVM&)            = delete;
        AriaVM& operator=(const AriaVM&) = delete;

        // 成员和 GC tracer 持有内部地址，VM 不可移动。
        AriaVM(AriaVM&&)            = delete;
        AriaVM& operator=(AriaVM&&) = delete;

        friend class Eval;

        friend class CoroutineClass;

#ifdef ARIA_OPCODE_PROFILE
        friend class OpcodeProfiler;
#endif

        // 执行良构的入口原型，前后重置上下文；未捕获错误以 Error 返回。
        Result<Value, Error> run(ObjFnProto* proto);

        // 编译并执行；source 须存活到返回，module 由内部保活。
        Result<Value, Error> run(SourceFile& source, ObjModule* module);

        InterpretResult interpret_from_src(StringView src);

        InterpretResult interpret_from_path(StringView path);

        [[nodiscard]]
        GC& gc() noexcept {
            return gc_;
        }

        [[nodiscard]]
        ObjMovement* current_context() const noexcept {
            return current_;
        }

        // 模块表键为驻留的文件规范路径或原生标准库完整名，值为模块对象。
        [[nodiscard]]
        AriaHashTable& modules() noexcept {
            return modules_;
        }

#define ARIA_VALUE_REGISTER_ACCESSOR_DECL(name, accessor, type) [[nodiscard]] type* accessor() const noexcept;
        ARIA_VALUE_REGISTER_SINGLETON_LIST(ARIA_VALUE_REGISTER_ACCESSOR_DECL)
        ARIA_VALUE_REGISTER_CLASS_LIST(ARIA_VALUE_REGISTER_ACCESSOR_DECL)
#undef ARIA_VALUE_REGISTER_ACCESSOR_DECL

        [[nodiscard]]
        ObjString* str(const usize index) const noexcept {
            return str_table_[index];
        }

        [[nodiscard]]
        ObjString* char_string(const u8 byte) const noexcept {
            ASSERT(char_cache_[byte] != nullptr, "single char cache slot unfilled");
            return char_cache_[byte];
        }

        [[nodiscard]]
        const List<String>& source_roots() const noexcept {
            return source_roots_;
        }

        // 替换配置根 [1..]，保留由 run 设置的入口目录 [0]。
        void set_source_roots(List<String> roots) noexcept;

    private:
        static constexpr usize kCharCacheSlots = 256;

        // 相对路径仅以当前模块目录为基，裸名沿 source_roots_ 搜索；未命中返回 nullopt。
        [[nodiscard]]
        Opt<String> resolve_module(StringView specifier, StringView current_module_path) const;

        // 仅在驱动期缓存未命中时调用，调用方须保活键。成功后入表，失败返回 nullptr 并挂起错误。
        ObjModule* load_file_module(ObjString* canonical_path, StringView import_specifier);

        ObjModule* load_stdlib_module(ObjString* module_key);

        InterpretResult interpret_run(SourceFile& source, ObjModule* module);

        void enter_coroutine(ObjMovement* coroutine);

        void leave_coroutine(ExecState departing_state);

        // 值寄存器组 bootstrap 编排(ctor 一次调用):逐格初始化全部 VM 单例对象;须在 ctor 构造
        // 临界区(GC 挂起)内调用,创建免守卫。
        void bootstrap_registers();

        // 常量串表 bootstrap(ctor 一次调用):按注册表逐条驻留填入 str_table_;须先于
        // bootstrap_registers -- bootstrap 期按名铸造钩子/方法名,靠本表锚保驻留命中。
        void bootstrap_str_table();

        // 单字符常量表 bootstrap(ctor 一次调用):字节域全值域 256 槽一次铸满与 str_table_
        // 同地位(运行期恒驻留常量,tracer 标根);须在 ctor 构造临界区内调用。
        void bootstrap_char_cache();

        // VM 根 tracer 挂接(ctor 一次调用):只标 current_ 一点,各上下文内部与 previous_ resume
        // 链经 ObjMovement::trace 级联。
        void hook_vm_roots();

        void init_source_roots();

        [[nodiscard]]
        Pair<ErrorCode, String> take_uncaught_error() const;

        // 仅在驱动失败后调用，须保留帧链和挂起错误以生成跟踪。
        [[nodiscard]]
        Error rebake_uncaught_error() const;

        GC gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)

        // 当前上下文及其恢复者链均由 tracer 标根。
        ObjMovement* current_;

        AriaHashTable modules_;  // 模块表(GC 根)
        AriaHashTable builtins_; // 只读 builtins 表(GC 根)

        // [0] 为入口目录，[1..] 为配置根。
        List<String> source_roots_;

        List<Object*> registers_;

        // 常量串须由 tracer 标根，驻留池不保活对象。
        List<ObjString*> str_table_;

        List<ObjString*> char_cache_;

#ifdef ARIA_OPCODE_PROFILE
        OpcodeProfiler opcode_profiler_;
#endif
    };

    static_assert(!std::is_move_constructible_v<AriaVM>);
    static_assert(!std::is_move_assignable_v<AriaVM>);

} // namespace aria

#endif // ARIA_VM_HPP
