#include "runtime/Builtins.hpp"

#include "error/ErrorCode.hpp"     // ErrorCode
#include "memory/GC.hpp"           // GC, make_guard
#include "object/ObjNativeFn.hpp"  // NativeFn, new_native_fn
#include "object/ObjString.hpp"    // ObjString, new_string
#include "runtime/AriaVM.hpp"      // AriaVM (vm.fail / vm.gc)
#include "value/AriaHashTable.hpp" // AriaHashTable
#include "value/ObjBridge.hpp"     // try_obj<T>（Value→对象子类型一步守卫）
#include "value/Value.hpp"         // Value, type_name, format_value, is_truthy

namespace aria::builtins {

    namespace {

        // ---- 内置原生函数实现(NativeFn 契约:读 slots[1..]、写 slots[0]、失败 return vm.fail(...)) ----

        // type(x) -> 字符串:值的精确类型名(PascalCase,如 "Int"/"String"/"Nil")。
        //   复用 value 层 type_name(Value)(原语走 Value::type_name() constexpr、Obj 走 obj->type_name())。
        bool type_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "type expects 1 argument, got {}", argc);
            }
            const auto name = new_string(vm.gc(), type_name(slots[1])); // 静态名 intern,无 GC 风险
            slots[0]        = Value::from_obj(name);
            return true;
        }

        // len(x) -> 整数:当前仅支持 String(返 UTF-8 字节数,即 ObjString::length());List/Map 随 M5。
        bool len_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "len expects 1 argument, got {}", argc);
            }
            const Value v = slots[1];
            if (const auto s = try_obj<ObjString>(v)) {
                slots[0] = Value::from_int(static_cast<i64>(s->length()));
                return true;
            }
            return vm.fail(ErrorCode::TypeMismatch, "len requires a string, got {}", type_name(v));
        }

        // str(x) -> 字符串:值的可读渲染(复用 format_value,与 PRINT 一致)。
        bool str_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "str expects 1 argument, got {}", argc);
            }
            const auto s = new_string(vm.gc(), format_value(slots[1])); // format_value 返 String,临时 view 跨调用有效
            slots[0]     = Value::from_obj(s);
            return true;
        }

        // assert(x[, msg]) -> nil:x 真值则成功返 nil;否则抛 AssertionFailed(带 msg 或默认消息)。
        bool assert_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1 && argc != 2) {
                return vm.fail(ErrorCode::WrongArity, "assert expects 1 or 2 arguments, got {}", argc);
            }
            if (is_truthy(slots[1])) {
                slots[0] = Value::nil_val();
                return true;
            }
            // 失败:msg 取第二参(须为 String),否则默认。
            StringView msg = "assertion failed";
            if (argc == 2) {
                if (const auto s = try_obj<ObjString>(slots[2])) {
                    msg = s->view();
                }
            }
            return vm.fail(ErrorCode::AssertionFailed, "{}", msg);
        }

        struct BuiltinEntry {
            StringView name;
            NativeFn   fn;
        };

        // 内置表:按名注册进 VM 级 builtins 表。`print` 是关键字/语句(走 PRINT 指令),不入此表。
        constexpr BuiltinEntry kBuiltins[] = {
                {"type", type_fn},
                {"len", len_fn},
                {"str", str_fn},
                {"assert", assert_fn},
        };

    } // namespace

    // 把全部内置按名 upsert 进 VM 级 builtins 表(给定 AriaHashTable)。每条:new_string(intern weak
    //   root)+ make_guard -> new_native_fn + make_guard -> builtins.upsert(rehash 触 GC,双守卫承重)
    //   -> 赋 value。intern 池保证此处 name 指针与 CodeGen 发射 LOAD_GLOBAL 所用同名常量同指。
    //   由 AriaVM ctor 在 set_vm_roots 之后调用一次:已入表条目经 vm_roots tracer 的 builtins_.trace
    //   标根,在建的 name/fn 经 make_guard 根化,故注册内触 GC 安全。
    void register_builtins(GC& gc, AriaHashTable& builtins) {
        for (const auto& [name, fn]: kBuiltins) {
            auto       guard    = gc.make_guard();
            const auto name_obj = new_string(gc, name);
            guard.push(name_obj);
            const auto fn_obj = new_native_fn(gc, name_obj, fn);
            guard.push(fn_obj);
            const auto entry = builtins.upsert(Value::from_obj(name_obj));
            entry->value     = Value::from_obj(fn_obj);
        }
    }

} // namespace aria::builtins
