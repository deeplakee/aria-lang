#include "runtime/builtins/Builtin.hpp"

#include <chrono>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjString.hpp"
#include "runtime/Eval.hpp"
#include "runtime/ObjMovement.hpp"
#include "util/io.hpp"
#include "value/AriaHashTable.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 结构查询直接读取真实归属，不执行普通成员访问或用户钩子。
        [[nodiscard]]
        ObjClass* class_of(const AriaExeContext& ctx, const Value value) noexcept {
            if (!value.is_obj()) {
                return nullptr;
            }
            switch (value.as_obj()->type()) {
                case ObjType::INSTANCE:
                    return value.as_obj()->as<ObjInstance>()->klass();
                case ObjType::CLASS:
                    return ctx.class_class();
                case ObjType::MODULE:
                    return ctx.module_class();
                case ObjType::FUNCTION:
                    return ctx.function_class();
                case ObjType::BOUND_METHOD:
                    return ctx.bound_method_class();
                case ObjType::MOVEMENT:
                    return ctx.coroutine_class();
                case ObjType::EXCEPTION:
                    return ctx.exception_class();
                case ObjType::LIST:
                    return ctx.list_class();
                case ObjType::MAP:
                    return ctx.map_class();
                case ObjType::STRING:
                    return ctx.string_class();
                case ObjType::RANGE:
                    return ctx.range_class();
                case ObjType::ITERATOR:
                    return ctx.iterator_class();
                default:
                    UNREACHABLE();
            }
        }

        [[nodiscard]]
        bool is_subclass_of(const ObjClass* const derived, const ObjClass* const base) noexcept {
            for (auto klass = derived; klass != nullptr; klass = klass->superclass()) {
                if (klass == base) {
                    return true;
                }
            }
            return false;
        }

        bool fn_type(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto name = new_string(ctx.gc(), type_name(info[0])); // 静态名 intern,无 GC 风险
            info.set_return_val(Value::from_obj(name));
            return true;
        }

        bool fn_is_a(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 2) {
                return ctx.arity_error(argc, 2);
            }
            const auto target = try_as_obj<ObjClass>(info[1]);
            if (target == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "argument must be a class, got {}", type_name(info[1]));
            }
            info.set_return_val(Value::from_bool(is_subclass_of(class_of(ctx, info[0]), target)));
            return true;
        }

        bool fn_is_subclass(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 2) {
                return ctx.arity_error(argc, 2);
            }
            const auto derived = try_as_obj<ObjClass>(info[0]);
            if (derived == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "argument must be a class, got {}", type_name(info[0]));
            }
            const auto base = try_as_obj<ObjClass>(info[1]);
            if (base == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "argument must be a class, got {}", type_name(info[1]));
            }
            info.set_return_val(Value::from_bool(is_subclass_of(derived, base)));
            return true;
        }

        bool fn_str(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto str = Eval::stringify(ctx, info[0]);
            if (!str) {
                return false; // 载荷在寄存器,交外层续搜
            }
            info.set_return_val(Value::from_obj(str));
            return true;
        }

        bool fn_println(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc > 1) {
                return ctx.arity_error_range(argc, 0, 1);
            } else if (argc == 1) {
                const auto str = Eval::stringify(ctx, info[0]);
                if (!str) {
                    return false;
                }
                io::println("{}", str->view());
            } else {
                io::println();
            }
            info.set_return_val(Value::nil_val());
            return true;
        }

        // 非字符串消息参数按默认消息处理。
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

        // 单调时钟无固定纪元，只能用读数差计时。
        bool fn_clock(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto raw_now = std::chrono::steady_clock::now().time_since_epoch();
            info.set_return_val(Value::from_f64(std::chrono::duration<double>(raw_now).count()));
            return true;
        }

        // 用户消息和数字码原样携带，不加内部错误前缀。
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

        constexpr BuiltinFnEntry kBuiltinFns[] = {
                {"type", fn_type},   {"str", fn_str},     {"println", fn_println}, {"assert", fn_assert},
                {"clock", fn_clock}, {"Error", fn_Error}, {"is_a", fn_is_a},       {"is_subclass", fn_is_subclass},
        };

        void register_functions(GC& gc, AriaHashTable& table, const Span<const BuiltinFnEntry> functions) {
            for (const auto& [name, fn]: functions) {
                const auto function = new_function(gc, name, fn);
                table.set(Value::from_obj(function->name()), Value::from_obj(function));
            }
        }

    } // namespace

    void Builtin::register_class_methods(GC& gc, ObjClass* klass, const Span<const BuiltinFnEntry> methods) {
        for (const auto& [name, fn]: methods) {
            const auto method = new_function(gc, name, fn);
            method->set_defining_class(klass);
            klass->set_field(method->name(), Value::from_obj(method));
        }
    }

    void Builtin::register_class_functions(GC& gc, ObjClass* klass, const Span<const BuiltinFnEntry> functions) {
        for (const auto& [name, fn]: functions) {
            const auto function = new_function(gc, name, fn);
            klass->set_field(function->name(), Value::from_obj(function));
        }
    }

    void Builtin::register_builtins(GC& gc, AriaHashTable& builtins) { register_functions(gc, builtins, kBuiltinFns); }

} // namespace aria
