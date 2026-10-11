#include "runtime/builtins/Builtin.hpp"

#include <chrono>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/CoroutineModule.hpp"
#include "util/io.hpp"
#include "value/AriaHashTable.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 内置原生函数实现(NativeFn 契约:实参经 info 下标读、返回值 set_return_val 写、失败 return ctx.fail(...))

        // type(x) -> 字符串:值的精确类型名(PascalCase,如 "Int"/"String"/"Nil")。
        bool fn_type(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto name = new_string(ctx.gc(), type_name(info[0])); // 静态名 intern,无 GC 风险
            info.set_return_val(Value::from_obj(name));
            return true;
        }

        // str(x) -> 字符串:值的可读渲染(复用 stringify)。
        bool fn_str(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto s = new_string(ctx.gc(), stringify(info[0])); // stringify 返 String,临时 view 跨调用有效
            info.set_return_val(Value::from_obj(s));
            return true;
        }

        // println([x]) -> nil:值的可读渲染加换行(stringify + io::println),省参只输出换行;可作一等值传参。
        bool fn_println(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc > 1) {
                return ctx.arity_error_range(argc, 0, 1);
            } else if (argc == 1) {
                io::println("{}", stringify(info[0]));
            } else {
                io::println();
            }
            info.set_return_val(Value::nil_val());
            return true;
        }

        // assert(x[, msg]) -> nil:真值返 nil,否则抛 AssertionFailed(msg 非 string 静默忽略落默认消息)。
        bool fn_assert(const NativeCallInfo& info) {
            auto&      ctx  = info.ctx();
            const auto argc = info.size();
            if (argc != 1 && argc != 2) {
                return ctx.arity_error_range(argc, 1, 2);
            }
            if (is_truthy(info[0])) {
                info.set_return_val(Value::nil_val());
                return true;
            }
            StringView msg = "assertion failed";
            if (argc == 2) {
                if (const auto s = try_as_obj<ObjString>(info[1])) {
                    msg = s->view();
                }
            }
            return ctx.fail(ErrorCode::AssertionFailed, "{}", msg);
        }

        // clock() -> f64:单调时钟当前读数(秒)。起点未定(非 Unix 纪元),只有两次读数相减才有意义;
        // 同进程内跨调用单调不减,不受系统调时影响。
        bool fn_clock(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto raw_now = std::chrono::steady_clock::now().time_since_epoch();
            info.set_return_val(Value::from_f64(std::chrono::duration<double>(raw_now).count()));
            return true;
        }

        // Error(message[, code]) -> ObjException:用户异常工厂,直接产出 VM 同款异常对象;code 可选
        // int 原样携带(未设 = 默认 Error 码)。message 原样入库不烘 "Category: Name" 前缀,catch 侧原文取回。
        bool fn_Error(const NativeCallInfo& info) {
            auto&      ctx  = info.ctx();
            const auto argc = info.size();
            if (argc != 1 && argc != 2) {
                return ctx.arity_error_range(argc, 1, 2);
            }
            const auto msg = try_as_obj<ObjString>(info[0]);
            if (msg == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "argument must be a string, got {}", type_name(info[0]));
            }
            auto code = static_cast<i64>(std::to_underlying(ErrorCode::Error));
            if (argc == 2) {
                if (!info[1].is_int()) {
                    return ctx.fail(ErrorCode::TypeMismatch, "argument must be an integer, got {}", type_name(info[1]));
                }
                code = info[1].as_int();
            }
            info.set_return_val(Value::from_obj(new_exception(ctx.gc(), code, msg->view())));
            return true;
        }

        // 内置全局函数表。
        constexpr BuiltinFnEntry kBuiltinFns[] = {
                {"type", fn_type},     {"str", fn_str},     {"println", fn_println},
                {"assert", fn_assert}, {"clock", fn_clock}, {"Error", fn_Error},
        };

        // 内建变量表:名 + 自含构造的变量值。
        constexpr BuiltinVarEntry kBuiltinVars[] = {
                {"coroutine", CoroutineModule::make_module}, // <coroutine> 合成模块
        };

        // 全局面函数装载口:按名写入给定 builtins 表;须在构造临界区(GC 挂起)内调用 -- 白色对象
        // 免逐个守卫,建成即入表、入表条目经 vm_roots tracer 标根。
        void register_functions(GC& gc, AriaHashTable& table, const Span<const BuiltinFnEntry> fns) {
            for (const auto& [name, fn]: fns) {
                const auto fn_obj = new_native_fn(gc, name, fn);
                table.set(Value::from_obj(fn_obj->name()), Value::from_obj(fn_obj));
            }
        }

        // 全局面变量装载口:GC 纪律同 register_functions(构造临界区内,init 产出即时入表不落中间)。
        void register_variables(GC& gc, AriaHashTable& table, const Span<const BuiltinVarEntry> vars) {
            for (const auto& [name, init_fn]: vars) {
                const auto key   = new_string(gc, name);
                const auto value = init_fn(gc);
                table.set(Value::from_obj(key), value);
            }
        }

    } // namespace

    void Builtin::register_class_methods(GC& gc, ObjClass* klass, const Span<const BuiltinFnEntry> methods) {
        for (const auto& [name, fn]: methods) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            klass->set_field(fn_obj->name(), Value::from_obj(fn_obj));
        }
    }

    void Builtin::register_module_functions(GC& gc, ObjModule* module, const Span<const BuiltinFnEntry> fns) {
        for (const auto& [name, fn]: fns) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            module->globals().set(Value::from_obj(fn_obj->name()), Value::from_obj(fn_obj));
        }
    }

    void Builtin::register_builtins(GC& gc, AriaHashTable& builtins) {
        register_functions(gc, builtins, kBuiltinFns);
        register_variables(gc, builtins, kBuiltinVars);
    }

} // namespace aria
