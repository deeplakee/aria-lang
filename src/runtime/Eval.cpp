#include "runtime/Eval.hpp"

#include <cmath>
#include <format>
#include <limits>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "bytecode/Disassembler.hpp"
#include "bytecode/code.hpp"
#include "memory/GC.hpp"
#include "memory/StringBuilder.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjRange.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/NativeCallInfo.hpp"
#include "runtime/opcode_profile.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {
    namespace {

        u8 read_u8(CallFrame* frame) noexcept { return *frame->ip++; }

        u16 read_u16(CallFrame* frame) noexcept {
            const u16 value = util::make_u16(frame->ip[0], frame->ip[1]);
            frame->ip += 2;
            return value;
        }

        // 编译器保证名字常量为 intern 串。
        ObjString* read_name(CallFrame* frame) noexcept {
            const auto idx = read_u16(frame);
            return frame->unit->constants[idx].as_obj()->as<ObjString>();
        }

        // 跟踪只能用 value_repr，避免显示钩子重入 VM。
        [[maybe_unused]] void trace_execution(AriaExeContext& ctx) {
            const auto& frames = ctx.frames();
            const auto& frame  = frames.top();
            const auto  ip_off = static_cast<u32>(frame.ip - frame.unit->code.data());
            const auto  instr  = Disassembler::disassembleInstruction(frame.unit, ip_off);

            io::print(stderr, "[trace] ctx {:p}  {}  {} @{:04X}  {}\n", static_cast<const void*>(&ctx),
                      frame.module->repr(), frame.function->proto()->repr(), ip_off, instr);

            // 累计当前帧之前的渲染宽度，使 ^ 对齐帧槽 0。
            const String prefix = std::format("        stack[{}]: ", ctx.stack_size());
            usize        col    = prefix.size();
            String       stack_str;
            for (auto p = ctx.stack_base(); p < ctx.stack_top(); ++p) {
                const auto s = std::format("[ {} ]", value_repr(*p));
                if (p < frame.slots) {
                    col += s.size();
                }
                stack_str += s;
            }
            if (stack_str.empty()) {
                stack_str = "(empty)";
            }

            io::print(stderr, "{}{}\n", prefix, stack_str);

            io::print(stderr, "{}^ frame[{}]\n", String(col, ' '), static_cast<u32>(frames.size() - 1));
        }

        void push_call_area(AriaExeContext& ctx, const Value slot0, ArgList args) {
            List<Value> saved_args;
            // push 写满最后一槽就会扩容，须在压入 slot0 前复制可能借自值栈的实参。
            if (1 + args.size() >= ctx.stack_capacity() - ctx.stack_size()) {
                saved_args.assign(args.begin(), args.end());
                args = saved_args;
            }
            ctx.push(slot0);
            for (const auto& arg: args) {
                ctx.push(arg);
            }
        }

        template<OpCode Op>
        Opt<Value> load_operator_hook(AriaExeContext& ctx, Object* receiver) {
            if constexpr (Op == OpCode::ADD) {
                return receiver->load_field_from_class(ctx, ctx.str<"__add__">());
            } else if constexpr (Op == OpCode::SUBTRACT) {
                return receiver->load_field_from_class(ctx, ctx.str<"__sub__">());
            } else if constexpr (Op == OpCode::MULTIPLY) {
                return receiver->load_field_from_class(ctx, ctx.str<"__mul__">());
            } else if constexpr (Op == OpCode::DIVIDE) {
                return receiver->load_field_from_class(ctx, ctx.str<"__div__">());
            } else if constexpr (Op == OpCode::MOD) {
                return receiver->load_field_from_class(ctx, ctx.str<"__mod__">());
            } else if constexpr (Op == OpCode::GREATER) {
                return receiver->load_field_from_class(ctx, ctx.str<"__gt__">());
            } else if constexpr (Op == OpCode::GREATER_EQUAL) {
                return receiver->load_field_from_class(ctx, ctx.str<"__ge__">());
            } else if constexpr (Op == OpCode::LESS) {
                return receiver->load_field_from_class(ctx, ctx.str<"__lt__">());
            } else if constexpr (Op == OpCode::LESS_EQUAL) {
                return receiver->load_field_from_class(ctx, ctx.str<"__le__">());
            } else if constexpr (Op == OpCode::NEGATE) {
                return receiver->load_field_from_class(ctx, ctx.str<"__neg__">());
            } else if constexpr (Op == OpCode::BIT_AND) {
                return receiver->load_field_from_class(ctx, ctx.str<"__and__">());
            } else if constexpr (Op == OpCode::BIT_OR) {
                return receiver->load_field_from_class(ctx, ctx.str<"__or__">());
            } else if constexpr (Op == OpCode::BIT_XOR) {
                return receiver->load_field_from_class(ctx, ctx.str<"__xor__">());
            } else if constexpr (Op == OpCode::SHIFT_LEFT) {
                return receiver->load_field_from_class(ctx, ctx.str<"__lshift__">());
            } else if constexpr (Op == OpCode::SHIFT_RIGHT) {
                return receiver->load_field_from_class(ctx, ctx.str<"__rshift__">());
            } else if constexpr (Op == OpCode::SHIFT_RIGHT_LOGICAL) {
                return receiver->load_field_from_class(ctx, ctx.str<"__urshift__">());
            } else if constexpr (Op == OpCode::BIT_NOT) {
                return receiver->load_field_from_class(ctx, ctx.str<"__invert__">());
            } else {
                STATIC_UNREACHABLE("unsupported opcode in load_operator_hook");
            }
        }

    } // namespace

    bool Eval::call_value(AriaExeContext& ctx, const Value callee, const u8 argc) {
        if (!callee.is_obj()) {
            return ctx.fail(ErrorCode::CallNonCallable, "type {} does not support '__call__'", type_name(callee));
        }

        switch (Object* obj = callee.as_obj(); obj->type()) {
            case ObjType::FUNCTION:
                return call_function(ctx, obj->as<ObjFunction>(), argc);
            case ObjType::CLASS:
                return call_class(ctx, obj->as<ObjClass>(), argc);
            case ObjType::BOUND_METHOD:
                return call_bound_method(ctx, obj->as<ObjBoundMethod>(), argc);
            default: {
                // 槽 0 须换为实际被调对象，不能沿用成员宿主。
                ctx.peek(argc) = callee;
                if (const auto target = obj->load_field_from_class(ctx, ctx.str<"__call__">())) {
                    return call_value(ctx, *target, argc);
                }
                return false;
            }
        }
    }

    bool Eval::call_class(AriaExeContext& ctx, ObjClass* klass, const u8 argc) {
        // 成员调用的槽 0 可能还放着成员宿主。先写入类,跨 new_instance 的 GC 点保活。
        ctx.peek(argc)      = Value::from_obj(klass);
        const auto instance = new_instance(ctx.gc(), klass);
        ctx.peek(argc)      = Value::from_obj(instance); // 实例接手保活类,同时成为 init 的 this
        return call_value(ctx, klass->init(), argc);
    }

    bool Eval::call_bound_method(AriaExeContext& ctx, const ObjBoundMethod* bound, const u8 argc) {
        const auto method = bound->method();
        ctx.peek(argc)    = bound->receiver(); // 槽 0:bound -> this(实参槽位不动)
        return call_function(ctx, method, argc);
    }

    bool Eval::call_function(AriaExeContext& ctx, ObjFunction* function, const u8 argc) {
        return function->is_native() ? call_native_fn(ctx, function, argc) : call_aria_fn(ctx, function, argc);
    }

    bool Eval::call_aria_fn(AriaExeContext& ctx, ObjFunction* function, const u8 argc) {
        ASSERT(!function->is_native(), "function must have an aria payload");
        // 进帧前由临时根保活函数对象;进帧后由 frame.function 接管。
        const auto guard = ctx.gc().make_guard(function);
        const auto proto = function->proto();
        if (!ctx.check_arity(proto, argc)) {
            return false;
        }
        if (ctx.frames_full()) {
            return ctx.fail(ErrorCode::StackOverflow, "call frame stack overflow");
        }
        const auto prepared_argc = ctx.prepare_call_args(proto, argc);
        ctx.enter_frame(function, prepared_argc);
        return true;
    }

    bool Eval::call_native_fn(AriaExeContext& ctx, const ObjFunction* function, const u8 argc) {
        // 成功时可能已切换上下文，drop 仍须落在进场 ctx，使槽 0 留作结果槽。
        // 失败不得切换，否则调用方无法消费错误。
        const NativeCallInfo info{ctx, argc};
        ASSERT(!ctx.has_error(), "pending error not cleared before native call");
        if (function->fn()(info)) {
            ASSERT(!ctx.has_error(), "native fn returned true but raised error");
            ctx.drop(argc);
            return true;
        }
        ASSERT(ctx.host().current_ == &ctx, "native fn returned false after switching vm.current_");
        ASSERT(ctx.has_error(), "native fn returned false but raised no error");
        return false;
    }

    Opt<Value> Eval::execute_aria_fn(AriaExeContext& ctx, const Value slot0, ObjFunction* function,
                                     const ArgList args) {
        ASSERT(&ctx == ctx.host().current_, "reentry must target the executing context");
        if (ctx.recursion_depth() >= AriaExeContext::kMaxRecursionDepth) {
            return ctx.fail(ErrorCode::StackOverflow, "nested execution too deep");
        }
        const auto argc = args.size();
        if (argc > std::numeric_limits<u8>::max()) {
            return ctx.fail(ErrorCode::WrongArity, "too many arguments: {}", argc);
        }
        push_call_area(ctx, slot0, args);
        const auto saved_ret = ctx.enter_recursion(); // 换防本循环出口边界,取走外层值
        if (call_aria_fn(ctx, function, argc) && dispatch_loop(ctx)) {
            ctx.leave_recursion(saved_ret);
            return ctx.pop();
        }
        ctx.leave_recursion(saved_ret);
        return std::nullopt;
    }

    Opt<Value> Eval::execute_native_fn(AriaExeContext& ctx, const Value slot0, const ObjFunction* function,
                                       const ArgList args) {
        const auto argc = args.size();
        if (argc > std::numeric_limits<u8>::max()) {
            return ctx.fail(ErrorCode::WrongArity, "too many arguments: {}", argc);
        }
        push_call_area(ctx, slot0, args);
        if (call_native_fn(ctx, function, argc)) {
            return ctx.pop();
        }
        return std::nullopt;
    }

    Opt<Value> Eval::execute_function(AriaExeContext& ctx, const Value slot0, ObjFunction* function,
                                      const ArgList args) {
        return function->is_native() ? execute_native_fn(ctx, slot0, function, args)
                                     : execute_aria_fn(ctx, slot0, function, args);
    }

    Opt<Value> Eval::execute_callable(AriaExeContext& ctx, Value slot0, const Value callee, const ArgList args) {
        if (!callee.is_obj()) {
            return ctx.fail(ErrorCode::CallNonCallable, "type {} is not callable", type_name(callee));
        }
        switch (Object* obj = callee.as_obj(); obj->type()) {
            case ObjType::BOUND_METHOD: {
                const auto bound = obj->as<ObjBoundMethod>();
                return execute_function(ctx, bound->receiver(), bound->method(), args);
            }
            case ObjType::FUNCTION:
                return execute_function(ctx, slot0, obj->as<ObjFunction>(), args);
            default:
                return ctx.fail(ErrorCode::CallNonCallable, "type {} is not callable", type_name(callee));
        }
    }

    ObjString* Eval::execute_str_hook(AriaExeContext& ctx, const Value value, const Value hook) {
        const auto rendered = execute_callable(ctx, value, hook, {});
        if (!rendered) {
            return nullptr;
        }
        if (const auto string = try_as_obj<ObjString>(*rendered)) {
            return string;
        }
        ctx.raise(ErrorCode::TypeMismatch, "'__str__' must return a string, got {}", type_name(*rendered));
        return nullptr;
    }

    ObjString* Eval::stringify(AriaExeContext& ctx, const Value value) {
        if (!value.is_obj()) {
            return new_string(ctx.gc(), value_repr(value));
        }
        const auto hook = value.as_obj()->load_field_from_class(ctx, ctx.str<"__str__">());
        if (!hook) {
            return nullptr;
        }
        return execute_str_hook(ctx, value, *hook);
    }

    ObjString* Eval::concat_values(AriaExeContext& ctx, const Span<const Value> values, const StringView sep) {
        // 逐段渲染时须临时根化返回串，跨后续渲染保活；驻留池仅为弱根。
        auto             guard = ctx.gc().make_guard();
        List<StringView> parts;
        usize            total = 0;
        parts.reserve(values.size());
        for (const auto& element: values) {
            const auto rendered = stringify(ctx, element);
            if (!rendered) {
                return nullptr;
            }
            guard.push(rendered);
            const auto view = rendered->view();
            total += view.size();
            parts.push_back(view);
        }

        if (!values.empty()) {
            total += (values.size() - 1) * sep.size();
        }

        auto builder = StringBuilder{ctx.gc()};
        builder.reserve(total);
        for (usize index = 0; index < parts.size(); ++index) {
            if (index > 0) {
                builder.append(sep);
            }
            builder.append(parts[index]);
        }
        return builder.take_string();
    }

    template<OpCode Op>
    bool Eval::run_binary_operator(AriaExeContext& ctx) {
        if (const auto lhs = ctx.peek(1); lhs.is_obj()) {
            if (const auto target = load_operator_hook<Op>(ctx, lhs.as_obj())) {
                return call_value(ctx, *target, 1);
            }
            return false;
        }
        if constexpr (Op == OpCode::BIT_AND || Op == OpCode::BIT_OR || Op == OpCode::BIT_XOR ||
                      Op == OpCode::SHIFT_LEFT || Op == OpCode::SHIFT_RIGHT || Op == OpCode::SHIFT_RIGHT_LOGICAL) {
            return run_binary_bitwise<Op>(ctx);
        } else {
            return run_binary_numeric<Op>(ctx);
        }
    }

    bool Eval::run_negate(AriaExeContext& ctx) {
        const Value operand = ctx.peek(0);
        if (operand.is_int()) {
            if (operand.as_int() == kIntMin) {
                return ctx.fail(ErrorCode::IntegerOverflow, "integer result out of range");
            }
            ctx.peek(0) = Value::from_int(-operand.as_int());
            return true;
        }
        if (operand.is_f64()) {
            ctx.peek(0) = Value::from_f64(-operand.as_f64());
            return true;
        }
        if (operand.is_obj()) {
            if (const auto target = load_operator_hook<OpCode::NEGATE>(ctx, operand.as_obj())) {
                return call_value(ctx, *target, 0);
            }
            return false;
        }
        return ctx.fail(ErrorCode::TypeMismatch, "negate requires a number, got {}", type_name(operand));
    }

    template<OpCode Op>
    bool Eval::run_binary_bitwise(AriaExeContext& ctx) {
        // 左移须在计算前检查 i48 溢出，避免中间值回绕。
        const Value rhs_value = ctx.pop();
        const Value lhs_value = ctx.pop();
        if (!(lhs_value.is_int() && rhs_value.is_int())) {
            return ctx.fail(ErrorCode::TypeMismatch, "operator '{}' requires integers, got {} and {}", op_symbol(Op),
                            type_name(lhs_value), type_name(rhs_value));
        }
        const i64 lhs = lhs_value.as_int();
        const i64 rhs = rhs_value.as_int();
        if constexpr (Op == OpCode::SHIFT_LEFT || Op == OpCode::SHIFT_RIGHT || Op == OpCode::SHIFT_RIGHT_LOGICAL) {
            if (rhs < 0 || rhs > kMaxShiftCount) {
                return ctx.fail(ErrorCode::IntegerOverflow, "shift count must be between 0 and {}, got {}",
                                kMaxShiftCount, rhs);
            }
        }
        if constexpr (Op == OpCode::BIT_AND) {
            ctx.push(Value::from_int(lhs & rhs));
        } else if constexpr (Op == OpCode::BIT_OR) {
            ctx.push(Value::from_int(lhs | rhs));
        } else if constexpr (Op == OpCode::BIT_XOR) {
            ctx.push(Value::from_int(lhs ^ rhs));
        } else if constexpr (Op == OpCode::SHIFT_LEFT) {
            if (lhs != 0 && (lhs > (kIntMax >> rhs) || lhs < (kIntMin >> rhs))) {
                return ctx.fail(ErrorCode::IntegerOverflow, "integer result out of range");
            }
            ctx.push(Value::from_int(lhs << rhs));
        } else if constexpr (Op == OpCode::SHIFT_RIGHT) {
            ctx.push(Value::from_int(lhs >> rhs)); // C++20 有符号右移为算术右移,结果恒在域内
        } else {
            static_assert(Op == OpCode::SHIFT_RIGHT_LOGICAL);
            // 逻辑右移 0 位时，负数的无符号视图会超出 i48 正数上界。
            const auto result = static_cast<i64>((static_cast<u64>(lhs) & 0xFFFFFFFFFFFF) >> rhs);
            if (result > kIntMax) {
                return ctx.fail(ErrorCode::IntegerOverflow, "integer result out of range");
            }
            ctx.push(Value::from_int(result));
        }
        return true;
    }

    bool Eval::run_bit_not(AriaExeContext& ctx) {
        const Value operand = ctx.peek(0);
        if (operand.is_int()) {
            ctx.peek(0) = Value::from_int(~operand.as_int());
            return true;
        }
        if (operand.is_obj()) {
            if (const auto target = load_operator_hook<OpCode::BIT_NOT>(ctx, operand.as_obj())) {
                return call_value(ctx, *target, 0);
            }
            return false;
        }
        return ctx.fail(ErrorCode::TypeMismatch, "bitwise not requires an integer, got {}", type_name(operand));
    }

    template<OpCode Op>
    bool Eval::run_binary_numeric(AriaExeContext& ctx) {
        const Value b = ctx.pop();
        const Value a = ctx.pop();
        if (!(is_num(a) && is_num(b))) {
            return ctx.fail(ErrorCode::TypeMismatch, "operator '{}' requires numbers, got {} and {}", op_symbol(Op),
                            type_name(a), type_name(b));
        }
        if (a.is_int() && b.is_int()) {
            return run_binary_int<Op>(ctx, a.as_int(), b.as_int());
        }
        return run_binary_f64<Op>(ctx, as_num(a), as_num(b));
    }

    template<OpCode Op>
    bool Eval::run_binary_int(AriaExeContext& ctx, const i64 lhs, const i64 rhs) {
        // 加减除的中间值可用 i64 容纳；乘法须预检，避免 i64 溢出。
        i64 result = 0;
        if constexpr (Op == OpCode::ADD) {
            result = lhs + rhs;
        } else if constexpr (Op == OpCode::SUBTRACT) {
            result = lhs - rhs;
        } else if constexpr (Op == OpCode::MULTIPLY) {
            const i64  lhs_magnitude = lhs < 0 ? -lhs : lhs;
            const i64  rhs_magnitude = rhs < 0 ? -rhs : rhs;
            const bool is_negative   = (lhs < 0) != (rhs < 0);
            const i64  limit         = is_negative ? -kIntMin : kIntMax;
            if (lhs_magnitude != 0 && rhs_magnitude > limit / lhs_magnitude) {
                return ctx.fail(ErrorCode::IntegerOverflow, "integer result out of range");
            }
            result = lhs * rhs;
        } else if constexpr (Op == OpCode::DIVIDE) {
            if (rhs == 0) {
                return ctx.fail(ErrorCode::DivisionByZero, "integer division by zero");
            }
            result = lhs / rhs;
        } else if constexpr (Op == OpCode::MOD) {
            if (rhs == 0) {
                return ctx.fail(ErrorCode::ModuloByZero, "integer modulo by zero");
            }
            result = lhs % rhs;
        } else if constexpr (Op == OpCode::GREATER) {
            ctx.push(Value::from_bool(lhs > rhs));
            return true;
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            ctx.push(Value::from_bool(lhs >= rhs));
            return true;
        } else if constexpr (Op == OpCode::LESS) {
            ctx.push(Value::from_bool(lhs < rhs));
            return true;
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            ctx.push(Value::from_bool(lhs <= rhs));
            return true;
        } else {
            STATIC_UNREACHABLE("run_binary_int: no arm for this Op");
        }
        if (result < kIntMin || result > kIntMax) {
            return ctx.fail(ErrorCode::IntegerOverflow, "integer result out of range");
        }
        ctx.push(Value::from_int(result));
        return true;
    }

    template<OpCode Op>
    bool Eval::run_binary_f64(AriaExeContext& ctx, const f64 lhs, const f64 rhs) {
        // 浮点运算遵循 IEEE，除零产生 inf/nan。
        if constexpr (Op == OpCode::ADD) {
            ctx.push(Value::from_f64(lhs + rhs));
        } else if constexpr (Op == OpCode::SUBTRACT) {
            ctx.push(Value::from_f64(lhs - rhs));
        } else if constexpr (Op == OpCode::MULTIPLY) {
            ctx.push(Value::from_f64(lhs * rhs));
        } else if constexpr (Op == OpCode::DIVIDE) {
            ctx.push(Value::from_f64(lhs / rhs));
        } else if constexpr (Op == OpCode::MOD) {
            ctx.push(Value::from_f64(std::fmod(lhs, rhs)));
        } else if constexpr (Op == OpCode::GREATER) {
            ctx.push(Value::from_bool(lhs > rhs));
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            ctx.push(Value::from_bool(lhs >= rhs));
        } else if constexpr (Op == OpCode::LESS) {
            ctx.push(Value::from_bool(lhs < rhs));
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            ctx.push(Value::from_bool(lhs <= rhs));
        } else {
            STATIC_UNREACHABLE("run_binary_f64: no arm for this Op");
        }
        return true;
    }

    bool Eval::run_load_field(AriaExeContext& ctx, ObjString* name) {
        const Value obj = ctx.peek(0);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (const auto result = obj.as_obj()->load_field_bound(ctx, name)) {
            ctx.peek(0) = *result; // 写回原槽:[obj] -> [v]
            return true;
        }
        return false;
    }

    bool Eval::run_store_field(AriaExeContext& ctx, ObjString* name) {
        const Value obj = ctx.peek(1);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (!obj.as_obj()->store_field(ctx, name, ctx.peek(0))) {
            return false;
        }
        ctx.peek(1) = ctx.peek(0); // 单槽下移:弹 obj 留 v,[obj, v] -> [v]
        ctx.drop(1);
        return true;
    }

    bool Eval::run_load_this_field(AriaExeContext& ctx, ObjString* name) {
        const Value this_value = ctx.frames().top().slots[0];
        if (const auto inst = try_as_obj<ObjInstance>(this_value)) {
            if (const auto result = inst->load_field_bound(ctx, name)) {
                ctx.push(*result);
                return true;
            }
            return false;
        }
        return ctx.fail(ErrorCode::TypeMismatch, "field access requires an instance, got {}", type_name(this_value));
    }

    bool Eval::run_store_this_field(AriaExeContext& ctx, ObjString* name) {
        const Value this_value = ctx.frames().top().slots[0];
        if (const auto inst = try_as_obj<ObjInstance>(this_value)) {
            if (inst->store_field(ctx, name, ctx.peek(0))) {
                return true; // 值留栈(peek-store)
            }
            return false;
        }
        return ctx.fail(ErrorCode::TypeMismatch, "field assignment requires an instance, got {}",
                        type_name(this_value));
    }

    bool Eval::run_prepare_method(AriaExeContext& ctx, ObjString* name) {
        // 须先解析方法再求值实参，接收者留栈保活。
        const Value recv = ctx.peek(0);
        if (!recv.is_obj()) {
            return ctx.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(recv));
        }
        if (const auto target = recv.as_obj()->load_field(ctx, name)) {
            ctx.push(*target); // 调用目标留栈，跨实参求值保活。
            return true;
        }
        return false;
    }

    bool Eval::run_call_method(AriaExeContext& ctx, const u8 argc) {
        // 移除待调值，留下 [receiver, a1..aN]；离栈的调用目标须在 GC 点前保活。
        const Value target = ctx.peek(argc);
        for (u8 i = argc; i >= 1; --i) {
            ctx.peek(i) = ctx.peek(i - 1);
        }
        ctx.drop(1);
        return call_value(ctx, target, argc);
    }

    bool Eval::try_list_load_fast_dispatch(AriaExeContext& ctx, Object* target, const Value idx) {
        if (target->is<ObjList>() && idx.is_int()) {
            const auto list = target->as<ObjList>();
            if (const auto slot = util::resolve_index(idx.as_int(), list->elements().size())) {
                ctx.peek(1) = list->elements()[*slot];
                ctx.drop(1);
                return true;
            }
        }
        return false;
    }

    bool Eval::try_list_store_fast_dispatch(AriaExeContext& ctx, Object* target, const Value idx, const Value value) {
        if (target->is<ObjList>() && idx.is_int()) {
            const auto list = target->as<ObjList>();
            if (const auto slot = util::resolve_index(idx.as_int(), list->elements().size())) {
                list->elements()[*slot] = value;
                ctx.peek(2)             = value;
                ctx.drop(2);
                return true;
            }
        }
        return false;
    }

    bool Eval::run_load_index(AriaExeContext& ctx) {
        const Value idx = ctx.peek(0);
        const Value obj = ctx.peek(1);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        const auto target = obj.as_obj();
        if (try_list_load_fast_dispatch(ctx, target, idx)) {
            return true;
        }
        if (const auto result = target->load_index(ctx, idx)) {
            ctx.peek(1) = *result; // 写回 obj 槽再弹 idx:[obj, idx] -> [v]
            ctx.drop(1);
            return true;
        }
        return false;
    }

    bool Eval::run_store_index(AriaExeContext& ctx) {
        const Value value = ctx.peek(0);
        const Value idx   = ctx.peek(1);
        const Value obj   = ctx.peek(2);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        const auto target = obj.as_obj();
        if (try_list_store_fast_dispatch(ctx, target, idx, value)) {
            return true;
        }
        if (!target->store_index(ctx, idx, value)) {
            return false;
        }
        ctx.peek(2) = value; // 值下移两格:弹 obj、idx 留 v,[obj, idx, v] -> [v]
        ctx.drop(2);
        return true;
    }

    bool Eval::run_make_range(AriaExeContext& ctx, const u8 flags) {
        if ((flags & kRangeFlagUnbounded) != 0) {
            const Value from = ctx.peek(0);
            if (!from.is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "range bound must be an integer, got {}", type_name(from));
            }
            const auto range = new_range(ctx.gc(), from.as_int());
            ctx.drop(1);
            ctx.push(Value::from_obj(range));
            return true;
        }
        const bool  exclusive = (flags & kRangeFlagExclusive) != 0;
        const Value to        = ctx.peek(0);
        const Value from      = ctx.peek(1);
        if (!from.is_int() || !to.is_int()) {
            return ctx.fail(ErrorCode::TypeMismatch, "range bounds must be integers, got {} and {}", type_name(from),
                            type_name(to));
        }
        const auto range = new_range(ctx.gc(), from.as_int(), to.as_int(), exclusive);
        ctx.drop(2);
        ctx.push(Value::from_obj(range));
        return true;
    }

    bool Eval::run_build_string(AriaExeContext& ctx, const u8 count) {
        // 每段渲染后原槽换装为串，跨后续渲染保活。
        // stringify 可能搬迁值栈，段址须在每次访问时重新获取。
        usize total = 0;
        for (isize index = count; index > 0; --index) {
            const auto rendered = stringify(ctx, ctx.peek(index - 1));
            if (!rendered) {
                return false;
            }
            ctx.peek(index - 1) = Value::from_obj(rendered);
            total += rendered->length();
        }
        auto builder = StringBuilder{ctx.gc()};
        builder.reserve(total);
        for (isize index = count; index > 0; --index) {
            builder.append(ctx.peek(index - 1).as_obj()->as<ObjString>()->view());
        }
        const auto result = builder.take_string();
        ctx.drop(count);
        ctx.push(Value::from_obj(result));
        return true;
    }

    bool Eval::run_make_class(AriaExeContext& ctx, ObjString* name) {
        if (const auto super = try_as_obj<ObjClass>(ctx.peek(0))) {
            // 内建容器方法要求具体内建对象，普通实例不能满足，故禁止继承这些类。
            if (super == ctx.list_class() || super == ctx.map_class() || super == ctx.string_class() ||
                super == ctx.range_class() || super == ctx.iterator_class()) {
                return ctx.fail(ErrorCode::TypeMismatch, "cannot inherit from built-in class '{}'",
                                super->name()->view());
            }
            const auto klass = new_class(ctx.gc(), name, super);
            ctx.peek(0)      = Value::from_obj(klass);
            return true;
        }
        return ctx.fail(ErrorCode::TypeMismatch, "superclass must be a class, got {}", type_name(ctx.peek(0)));
    }

    bool Eval::run_load_super_field(AriaExeContext& ctx, ObjString* name) {
        const auto& frame    = ctx.frames().top();
        const auto  defining = frame.function->defining_class();
        ASSERT(defining != nullptr, "closure has no defining class (compiler invariant)");
        const auto super = defining->superclass();
        ASSERT(super != nullptr, "method class has no superclass (compiler invariant)");
        const auto hit = super->load_field(ctx, name); // 从父类起读穿透(类协议)
        if (!hit) {
            return false;
        }
        if (const auto method = try_as_method(*hit)) {
            ctx.push(Value::from_obj(new_bound_method(ctx.gc(), method, frame.slots[0])));
        } else {
            ctx.push(*hit); // 非方法值原样返回
        }
        return true;
    }

    bool Eval::run_import(AriaExeContext& ctx, const ObjString* path) {
        const auto& frame              = ctx.frames().top(); // 导入方帧:abs_path 作相对解析基;进帧后不再引用
        const auto  canonical_path_str = ctx.host().resolve_module(path->view(), frame.module->abs_path());
        if (!canonical_path_str) {
            return ctx.fail(ErrorCode::ModuleNotFound, "module not found: '{}'", path->view());
        }
        const auto canonical_path = new_string(ctx.gc(), *canonical_path_str);
        // 驻留路径须跨模块编译的分配保活。
        auto guard = ctx.gc().make_guard(canonical_path);
        if (const auto module_entry = ctx.host().modules().find(Value::from_obj(canonical_path));
            module_entry != nullptr) {
            ctx.push(module_entry->value); // 命中:体执行中即循环导入,复用半初始化对象
            return true;
        }
        const auto module = ctx.host().load_module(canonical_path, path->view());
        if (module == nullptr) {
            return false;
        }
        const auto proto = module->entry();
        ASSERT(proto != nullptr, "load_module returned a module whose entry is not set");
        const auto function = new_function(ctx.gc(), proto);
        ctx.push(Value::from_obj(function));
        return call_aria_fn(ctx, function, 0);
    }

    bool Eval::run_return(AriaExeContext& ctx) {
        const Value ret = ctx.pop();
        ctx.exit_frame();
        if (ctx.frames().size() != ctx.ret_frame()) {
            ctx.push(ret); // 帧间返回:写回 callee 槽
            return false;
        }
        if (ctx.ret_frame() != 0) {
            ctx.push(ret); // VM 重入出口:返回值留在栈顶
            return true;
        }
        if (const auto resumer = ctx.previous()) {
            resumer->peek(0) = ret;
            ctx.reset();
            ctx.host().leave_coroutine(ExecState::Done);
            return false;
        }
        ctx.push(ret); // 主顶层返回:返回值留在栈顶(主模块对象)
        return true;
    }

    bool Eval::unwind(AriaVM& vm) {
        ASSERT(vm.current_->has_error(), "no pending payload");

        while (true) {
            auto& frames = vm.current_->frames();
            // 无符号下溢即终止
            for (usize i = frames.size() - 1; i >= vm.current_->ret_frame() && i < frames.size(); --i) {
                auto& [function, unit, module, ip, slots, last_ip] = frames[i];
                const u32 ip_off                                   = static_cast<u32>(last_ip - unit->code.data());
                if (const auto record = unit->find_try_handler(ip_off)) {
                    vm.current_->unwind_to_handler(i, **record);
                    return true;
                }
            }

            // 只清理当前重入边界内的帧，保留错误供外层继续搜索。
            if (vm.current_->ret_frame() != 0) {
                vm.current_->truncate_frames_to(vm.current_->ret_frame());
                return false;
            }

            if (const auto caller = vm.current_->previous()) {
                // 须先取错误再 reset；reset 会清寄存器。取出到重新 raise 之间不得触发 GC。
                const auto payload = vm.current_->take_error();
                vm.current_->reset();
                vm.leave_coroutine(ExecState::Failed);
                caller->raise(*payload);
            } else {
                return false; // 全链未命中:载荷在寄存器、帧栈活着,善后归调用方
            }
        }
    }

    bool Eval::dispatch_loop(AriaExeContext& ctx) {
        // CALL、RETURN、IMPORT 等可能使 current/frame 失效，之后须跳到 sync_now 重取。
        auto& vm      = ctx.host();
        auto  current = &ctx;
        auto  frame   = &current->frames().top();

        while (true) {
            // 取指前记录故障指令起点，供行号和异常查表使用。
            ASSERT(frame == &current->frames().top(), "stale frame after a frame-changing call");
            frame->last_ip = frame->ip;
#ifdef DEBUG_TRACE_EXECUTION
            trace_execution(*current);
#endif
            // 错误统一跳 unwind_check；switch 之后不得使用可能已失效的 frame。
            switch (ARIA_FETCH_OPCODE(current, frame)) {
                case OpCode::HALT:
                    // HALT 仅供手写字节码使用，不允许协程经此退出。
                    ASSERT(current->previous() == nullptr, "HALT reached inside a coroutine");
                    current->push(Value::nil_val()); // 停机值留栈顶(形态与 RETURN 顶层出口一致)
                    return true;

                case OpCode::LOAD_CONST: {
                    const auto idx = read_u16(frame);
                    current->push(frame->unit->constants[idx]);
                    break;
                }
                case OpCode::LOAD_NIL:
                    current->push(Value::nil_val());
                    break;
                case OpCode::LOAD_TRUE:
                    current->push(Value::true_val());
                    break;
                case OpCode::LOAD_FALSE:
                    current->push(Value::false_val());
                    break;
                case OpCode::LOAD_IMM: {
                    const u8 raw = read_u8(frame);
                    current->push(Value::from_i32(std::bit_cast<i8>(raw)));
                    break;
                }
                case OpCode::LOAD_REG: {
                    current->push(Value::from_obj(vm.registers_[read_u8(frame)]));
                    break;
                }
                case OpCode::LOAD_LOCAL: {
                    const auto slot = read_u16(frame);
                    current->push(frame->slots[slot]);
                    break;
                }
                case OpCode::LOAD_LOCAL_1:
                    current->push(frame->slots[1]);
                    break;
                case OpCode::LOAD_LOCAL_2:
                    current->push(frame->slots[2]);
                    break;
                case OpCode::LOAD_LOCAL_3:
                    current->push(frame->slots[3]);
                    break;
                case OpCode::LOAD_LOCAL_4:
                    current->push(frame->slots[4]);
                    break;
                case OpCode::LOAD_LOCAL_5:
                    current->push(frame->slots[5]);
                    break;
                case OpCode::LOAD_LOCAL_6:
                    current->push(frame->slots[6]);
                    break;
                case OpCode::LOAD_LOCAL_7:
                    current->push(frame->slots[7]);
                    break;
                case OpCode::LOAD_LOCAL_8:
                    current->push(frame->slots[8]);
                    break;
                case OpCode::STORE_LOCAL: {
                    const auto slot    = read_u16(frame);
                    frame->slots[slot] = current->peek(0);
                    break;
                }
                case OpCode::STORE_LOCAL_1:
                    frame->slots[1] = current->peek(0);
                    break;
                case OpCode::STORE_LOCAL_2:
                    frame->slots[2] = current->peek(0);
                    break;
                case OpCode::STORE_LOCAL_3:
                    frame->slots[3] = current->peek(0);
                    break;
                case OpCode::STORE_LOCAL_4:
                    frame->slots[4] = current->peek(0);
                    break;
                case OpCode::STORE_LOCAL_5:
                    frame->slots[5] = current->peek(0);
                    break;
                case OpCode::STORE_LOCAL_6:
                    frame->slots[6] = current->peek(0);
                    break;
                case OpCode::STORE_LOCAL_7:
                    frame->slots[7] = current->peek(0);
                    break;
                case OpCode::STORE_LOCAL_8:
                    frame->slots[8] = current->peek(0);
                    break;
                case OpCode::LOAD_UPVALUE: {
                    const u8 idx = read_u8(frame);
                    current->push(*frame->function->upvalues()[idx]->value_slot());
                    break;
                }
                case OpCode::STORE_UPVALUE: {
                    const u8 idx                                    = read_u8(frame);
                    *frame->function->upvalues()[idx]->value_slot() = current->peek(0);
                    break;
                }
                case OpCode::CLOSE_UPVALUE: {
                    // POP_N 已弹槽但未覆写；此处只关闭 upvalue，不再弹栈。
                    current->close_upvalues(current->stack_top());
                    break;
                }
                case OpCode::DEF_GLOBAL: {
                    ObjString* name = read_name(frame);
                    frame->module->globals().set(Value::from_obj(name), current->peek(0));
                    current->drop(1); // 写完才弹,栈效应仍为 [v] -> []
                    break;
                }
                case OpCode::LOAD_GLOBAL: {
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    auto        entry = frame->module->globals().find(key);
                    if (entry == nullptr) {
                        entry = vm.builtins_.find(key); // 回退宿主 builtins 表
                        if (entry == nullptr) {
                            current->raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                            goto unwind_check;
                        }
                    }
                    current->push(entry->value);
                    break;
                }
                case OpCode::STORE_GLOBAL: {
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame->module->globals().find(key);
                    if (entry == nullptr) {
                        current->raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                        goto unwind_check;
                    }
                    entry->value = current->peek(0);
                    break;
                }
                case OpCode::LOAD_FIELD:
                    if (!run_load_field(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_FIELD:
                    if (!run_store_field(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LOAD_INDEX:
                    if (!run_load_index(*current)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_INDEX:
                    if (!run_store_index(*current)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LOAD_THIS_FIELD:
                    if (!run_load_this_field(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_THIS_FIELD:
                    if (!run_store_this_field(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;

                case OpCode::EQUAL: {
                    const Value b = current->pop();
                    const Value a = current->pop();
                    current->push(Value::from_bool(value_equal(a, b)));
                    break;
                }
                case OpCode::NOT_EQUAL: {
                    const Value b = current->pop();
                    const Value a = current->pop();
                    current->push(Value::from_bool(!value_equal(a, b)));
                    break;
                }
                case OpCode::STRICT_EQUAL: {
                    const Value b = current->pop();
                    const Value a = current->pop();
                    current->push(Value::from_bool(value_identical(a, b)));
                    break;
                }
                case OpCode::STRICT_NOT_EQUAL: {
                    const Value b = current->pop();
                    const Value a = current->pop();
                    current->push(Value::from_bool(!value_identical(a, b)));
                    break;
                }
                case OpCode::GREATER:
                    if (!run_binary_operator<OpCode::GREATER>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::GREATER_EQUAL:
                    if (!run_binary_operator<OpCode::GREATER_EQUAL>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::LESS:
                    if (!run_binary_operator<OpCode::LESS>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::LESS_EQUAL:
                    if (!run_binary_operator<OpCode::LESS_EQUAL>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::ADD:
                    if (!run_binary_operator<OpCode::ADD>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::SUBTRACT:
                    if (!run_binary_operator<OpCode::SUBTRACT>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::MULTIPLY:
                    if (!run_binary_operator<OpCode::MULTIPLY>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::DIVIDE:
                    if (!run_binary_operator<OpCode::DIVIDE>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::MOD:
                    if (!run_binary_operator<OpCode::MOD>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::BIT_AND:
                    if (!run_binary_operator<OpCode::BIT_AND>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::BIT_OR:
                    if (!run_binary_operator<OpCode::BIT_OR>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::BIT_XOR:
                    if (!run_binary_operator<OpCode::BIT_XOR>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::SHIFT_LEFT:
                    if (!run_binary_operator<OpCode::SHIFT_LEFT>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::SHIFT_RIGHT:
                    if (!run_binary_operator<OpCode::SHIFT_RIGHT>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::SHIFT_RIGHT_LOGICAL:
                    if (!run_binary_operator<OpCode::SHIFT_RIGHT_LOGICAL>(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::NOT:
                    current->push(Value::from_bool(!is_truthy(current->pop())));
                    break;
                case OpCode::NEGATE:
                    if (!run_negate(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::BIT_NOT:
                    if (!run_bit_not(*current)) {
                        goto unwind_check;
                    }
                    goto sync_now;

                case OpCode::POP:
                    current->drop(1);
                    break;
                case OpCode::POP_N: {
                    const u8 n = read_u8(frame);
                    current->drop(n);
                    break;
                }
                case OpCode::DUP:
                    current->push(current->peek(0));
                    break;
                case OpCode::DUP2: {
                    const Value b = current->peek(0);
                    const Value a = current->peek(1);
                    current->push(a);
                    current->push(b);
                    break;
                }

                case OpCode::NOP:
                    break;

                case OpCode::JUMP: {
                    const u16 off = read_u16(frame);
                    frame->ip += off;
                    break;
                }
                case OpCode::JUMP_TRUE: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(current->pop())) {
                        frame->ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_TRUE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(current->peek(0))) {
                        frame->ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_FALSE: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(current->pop())) {
                        frame->ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_FALSE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(current->peek(0))) {
                        frame->ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_BACK: {
                    const u16 off = read_u16(frame);
                    frame->ip -= off;
                    vm.gc_.maybe_collect();
                    break;
                }
                case OpCode::JUMP_NE: {
                    // 仅融合不重入的 EQUAL；LESS 等比较可能调用用户方法，不能在单条指令内完成跳转。
                    const u16   off = read_u16(frame);
                    const Value b   = current->pop();
                    const Value a   = current->pop();
                    if (!value_equal(a, b)) {
                        frame->ip += off;
                    }
                    break;
                }

                case OpCode::CALL: {
                    const u8 argc = read_u8(frame);
                    ASSERT(current->stack_size() >= static_cast<usize>(argc) + 1, "malformed stack");
                    if (const Value callee = current->peek(argc); !call_value(*current, callee, argc)) {
                        goto unwind_check;
                    }
                    goto sync_now;
                }
                case OpCode::CLOSURE: {
                    // 先将闭包压栈保活，再逐项捕获 upvalue；捕获过程可能触发 GC。
                    const auto proto_index = read_u16(frame);
                    const auto proto       = frame->unit->constants[proto_index].as_obj()->as<ObjFnProto>();
                    const auto closure     = new_function(vm.gc_, proto);
                    current->push(Value::from_obj(closure)); // 立即入栈:值栈即根,跨 desc 循环免守卫
                    for (const auto& [is_local, index]: proto->upvalue_descs()) {
                        if (is_local) {
                            closure->add_upvalue(current->capture_upvalue(frame->slots + index));
                        } else {
                            closure->add_upvalue(frame->function->upvalues()[index]);
                        }
                    }
                    break;
                }

                case OpCode::MAKE_CLASS:
                    if (!run_make_class(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MAKE_METHOD: {
                    // defining_class 同时标识方法性和 super 的查找起点。
                    const auto klass        = try_as_obj<ObjClass>(current->peek(1));
                    const auto method_value = current->peek(0);
                    ASSERT(klass != nullptr, "slot-1 is not a class (malformed stack)");
                    const auto method = try_as_obj<ObjFunction>(method_value);
                    ASSERT(method != nullptr && !method->is_native(), "slot-0 must be an aria function");
                    klass->set_field(read_name(frame), method_value);
                    method->set_defining_class(klass);
                    current->drop(1);
                    break;
                }
                case OpCode::MAKE_STATIC: {
                    // 静态成员不设置 defining_class，读取时保持原值。
                    const auto klass = try_as_obj<ObjClass>(current->peek(1));
                    ASSERT(klass != nullptr, "slot-1 is not a class (malformed stack)");
                    klass->set_field(read_name(frame), current->peek(0));
                    current->drop(1); // 弹 value 留 class:[class, value] -> [class]
                    break;
                }
                case OpCode::LOAD_SUPER_FIELD:
                    if (!run_load_super_field(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::PREPARE_METHOD:
                    if (!run_prepare_method(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::CALL_METHOD:
                    if (!run_call_method(*current, read_u8(frame))) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::MAKE_LIST: {
                    const u16  count = read_u16(frame);
                    const auto list  = new_list(vm.gc_);
                    list->elements().copy_from({current->stack_top() - count, count});
                    current->drop(count);
                    current->push(Value::from_obj(list));
                    break;
                }
                case OpCode::MAKE_MAP: {
                    const u16  count = read_u16(frame);
                    const auto map   = new_map(vm.gc_);
                    const auto base  = current->stack_top() - count * 2;
                    for (usize i = 0; i < count; ++i) {
                        map->table().set(base[i * 2], base[i * 2 + 1]);
                    }
                    current->drop(count * 2);
                    current->push(Value::from_obj(map));
                    break;
                }
                case OpCode::MAKE_RANGE:
                    if (!run_make_range(*current, read_u8(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::BUILD_STRING:
                    if (!run_build_string(*current, read_u8(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::IMPORT:
                    if (!run_import(*current, read_name(frame))) {
                        goto unwind_check;
                    }
                    goto sync_now;
                case OpCode::THROW: {
                    current->raise(current->pop());
                    goto unwind_check;
                }
                case OpCode::RETURN:
                    if (run_return(*current)) {
                        return true;
                    }
                    goto sync_now;
                default:
                    UNREACHABLE();
            }
            continue; // 状态未变的正常路径:break 出 switch 后回循环顶,不落善后标签
        unwind_check:
            if (!unwind(vm)) {
                return false;
            }
        sync_now:
            current = vm.current_;
            frame   = &current->frames().top();
        }
    }

} // namespace aria
