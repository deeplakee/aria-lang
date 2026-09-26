#include "runtime/builtins/Builtins.hpp"

#include <chrono>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "util/io.hpp"
#include "value/AriaHashTable.hpp"
#include "value/ObjBridge.hpp" // try_obj<T>
#include "value/Value.hpp"

namespace aria::builtins {

    namespace {

        // 内置原生函数实现(NativeFn 契约:读 slots[1..]、写 slots[0]、失败 return vm.fail(...))

        // type(x) -> 字符串:值的精确类型名(PascalCase,如 "Int"/"String"/"Nil")。
        bool fn_type(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto name = new_string(vm.gc(), type_name(slots[1])); // 静态名 intern,无 GC 风险
            slots[0]        = Value::from_obj(name);
            return true;
        }

        // str(x) -> 字符串:值的可读渲染(复用 format_value)。
        bool fn_str(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto s = new_string(vm.gc(), format_value(slots[1])); // format_value 返 String,临时 view 跨调用有效
            slots[0]     = Value::from_obj(s);
            return true;
        }

        // println([x]) -> nil:值的可读渲染加换行(format_value + io::println),省参只输出换行;可作一等值传参。
        bool fn_println(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc > 1) {
                return vm.arity_error_range(argc, 0, 1);
            }
            if (argc == 0) {
                io::println();
            } else {
                io::println("{}", format_value(slots[1]));
            }
            slots[0] = Value::nil_val();
            return true;
        }

        // assert(x[, msg]) -> nil:x 真值则成功返 nil;否则抛 AssertionFailed(msg 为 string
        // 时用之,非 string 静默忽略落默认消息)。
        bool fn_assert(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1 && argc != 2) {
                return vm.arity_error_range(argc, 1, 2);
            }
            if (is_truthy(slots[1])) {
                slots[0] = Value::nil_val();
                return true;
            }
            StringView msg = "assertion failed";
            if (argc == 2) {
                if (const auto s = try_obj<ObjString>(slots[2])) {
                    msg = s->view();
                }
            }
            return vm.fail(ErrorCode::AssertionFailed, "{}", msg);
        }

        // clock() -> f64:单调时钟当前读数(秒)。起点未定(非 Unix 纪元),只有两次读数**相减**才有
        // 意义;同进程内跨调用单调不减,不受系统调时影响。返 f64 秒而非微秒整数:自开机起的微秒
        // 计数会超出 i48 值域(2^47 微秒约 51 天),而秒制下 f64 精度仍在微秒级。基准脚本靠它把
        // 启动/编译成本从进程总耗时里剥出来(aria 脚本无 argv,耗时只能语言内自测)。
        bool fn_clock(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto raw_now = std::chrono::steady_clock::now().time_since_epoch();
            slots[0]           = Value::from_f64(std::chrono::duration<double>(raw_now).count());
            return true;
        }

        // 内置表:按名注册进 VM 级 builtins 表。
        constexpr BuiltinEntry kBuiltins[] = {
                {"type", fn_type}, {"str", fn_str}, {"println", fn_println}, {"assert", fn_assert}, {"clock", fn_clock},
        };

    } // namespace

    // 把全部内置按名写入 VM 级 builtins 表。由 AriaVM ctor 在 tracer 挂接后于**构造临界区
    // (GC 挂起)内**调用一次:new_native_fn 的白色对象免逐个守卫,建成即入表、入表条目经
    // vm_roots tracer 标根;StringView 重载经 intern 池建名,保证 name 指针与 CodeGen 发射
    // LOAD_GLOBAL 所用同名常量同指。
    void register_builtin_functions(GC& gc, AriaHashTable& builtins) {
        for (const auto& [name, fn]: kBuiltins) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            builtins.set(Value::from_obj(fn_obj->name()), Value::from_obj(fn_obj));
        }
    }

    void register_class_methods(GC& gc, ObjClass* klass, const Span<const BuiltinEntry> methods) {
        for (const auto& [name, fn]: methods) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            klass->set_field(fn_obj->name(), Value::from_obj(fn_obj));
        }
    }

    void register_module_functions(GC& gc, ObjModule* module, const Span<const BuiltinEntry> fns) {
        for (const auto& [name, fn]: fns) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            module->globals().set(Value::from_obj(fn_obj->name()), Value::from_obj(fn_obj));
        }
    }

} // namespace aria::builtins
