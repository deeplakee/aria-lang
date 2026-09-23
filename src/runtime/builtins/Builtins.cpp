#include "runtime/builtins/Builtins.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
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
                return vm.fail(ErrorCode::WrongArity, "type expects 1 argument, got {}", argc);
            }
            const auto name = new_string(vm.gc(), type_name(slots[1])); // 静态名 intern,无 GC 风险
            slots[0]        = Value::from_obj(name);
            return true;
        }

        // str(x) -> 字符串:值的可读渲染(复用 format_value)。
        bool fn_str(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "str expects 1 argument, got {}", argc);
            }
            const auto s = new_string(vm.gc(), format_value(slots[1])); // format_value 返 String,临时 view 跨调用有效
            slots[0]     = Value::from_obj(s);
            return true;
        }

        // println([x]) -> nil:值的可读渲染加换行(format_value + io::println),省参只输出换行;可作一等值传参。
        bool fn_println(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc > 1) {
                return vm.fail(ErrorCode::WrongArity, "println expects 0 or 1 arguments, got {}", argc);
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
                return vm.fail(ErrorCode::WrongArity, "assert expects 1 or 2 arguments, got {}", argc);
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

        // 内置表:按名注册进 VM 级 builtins 表。
        constexpr BuiltinEntry kBuiltins[] = {
                {"type", fn_type},
                {"str", fn_str},
                {"println", fn_println},
                {"assert", fn_assert},
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

    void register_builtin_methods(GC& gc, ObjClass* klass, const Span<const BuiltinEntry> methods) {
        for (const auto& [name, fn]: methods) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            klass->set_field(fn_obj->name(), Value::from_obj(fn_obj));
        }
    }

} // namespace aria::builtins
