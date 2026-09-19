#include "runtime/builtins/Builtins.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "value/AriaHashTable.hpp"
#include "value/ObjBridge.hpp" // try_obj<T>
#include "value/Value.hpp"

namespace aria::builtins {

    namespace {

        // ---- 内置原生函数实现(NativeFn 契约:读 slots[1..]、写 slots[0]、失败 return vm.fail(...)) ----

        // type(x) -> 字符串:值的精确类型名(PascalCase,如 "Int"/"String"/"Nil")。
        bool type_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "type expects 1 argument, got {}", argc);
            }
            const auto name = new_string(vm.gc(), type_name(slots[1])); // 静态名 intern,无 GC 风险
            slots[0]        = Value::from_obj(name);
            return true;
        }

        // len(x) -> 整数:String 返 UTF-8 字节数(ObjString::length()),List 返元素数,
        // Map 返键值对数。
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
            if (const auto list = try_obj<ObjList>(v)) {
                slots[0] = Value::from_int(static_cast<i64>(list->elements().size()));
                return true;
            }
            if (const auto map = try_obj<ObjMap>(v)) {
                slots[0] = Value::from_int(static_cast<i64>(map->table().size()));
                return true;
            }
            return vm.fail(ErrorCode::TypeMismatch, "len requires a string, list or map, got {}", type_name(v));
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

    // 把全部内置按名写入 VM 级 builtins 表。由 AriaVM ctor 在 tracer 挂接后于**构造临界区
    // (GC 挂起)内**调用一次:new_native_fn 的白色对象免逐个守卫(窗口内回收不可达),建成即
    // 入表、入表条目经 vm_roots tracer 的 builtins_.trace 标根;StringView 重载经 intern 池
    // 建名,保证 name 指针与 CodeGen 发射 LOAD_GLOBAL 所用同名常量同指。
    void register_builtins(GC& gc, AriaHashTable& builtins) {
        for (const auto& [name, fn]: kBuiltins) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            builtins.set(Value::from_obj(fn_obj->name()), Value::from_obj(fn_obj));
        }
    }

} // namespace aria::builtins
