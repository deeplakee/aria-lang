#include "runtime/builtins/CoroutineClass.hpp"

#include "common.hpp"
#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/ObjMovement.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    bool CoroutineClass::create(const NativeCallInfo& info) {
        auto& ctx = info.ctx();
        if (const auto argc = info.size(); argc != 1) {
            return ctx.arity_error(argc, 1);
        }
        const auto function = try_as_obj<ObjFunction>(info[0]);
        if (function == nullptr || function->is_native()) {
            return ctx.fail(ErrorCode::TypeMismatch, "argument must be an aria function, got {}", type_name(info[0]));
        }
        const auto co = new_movement(ctx.gc(), ctx.host());
        co->push(Value::from_obj(function));      // 槽 0:入口函数(首启进帧的 callee 槽;push 扩容不触 GC)
        info.set_return_val(Value::from_obj(co)); // 返回槽发布,建成即根化;自分配点至此无 GC 点,免守卫
        return true;
    }

    bool CoroutineClass::resume(const NativeCallInfo& info) {
        auto& ctx = info.ctx();
        if (const auto argc = info.size(); argc == 0) {
            return ctx.arity_error_at_least(argc, 1);
        }
        const auto co = try_as_obj<ObjMovement>(info[0]);
        if (co == nullptr) {
            return ctx.fail(ErrorCode::TypeMismatch, "argument must be a coroutine, got {}", type_name(info[0]));
        }
        switch (co->state()) {
            case ExecState::Suspended:
                break;
            case ExecState::Done:
            case ExecState::Failed:
                return ctx.fail(ErrorCode::ResumeDeadCoroutine, "cannot resume a dead coroutine");
            case ExecState::Normal:
            case ExecState::Running:
                return ctx.fail(ErrorCode::ResumeNonSuspendedCoroutine, "cannot resume a non-suspended coroutine");
        }
        // 所有可失败检查须先于切换，使错误落在恢复者上下文。
        const auto payload = static_cast<u8>(info.size() - 1);
        if (co->frames().empty()) {
            const auto function = try_as_obj<ObjFunction>(co->peek(0));
            ASSERT(function != nullptr && !function->is_native(), "coroutine slot-0 must be an aria function");
            const auto proto = function->proto();
            if (!ctx.check_arity(proto, payload)) {
                return false;
            }
            ctx.host().enter_coroutine(co);
            for (usize i = 0; i < payload; ++i) {
                co->push(info[1 + i]); // push 扩容不触 GC(allocate/reallocate 不变式)
            }
            // 切换后 varargs 打包可能触发 GC，恢复者由 previous_ 链保活。
            co->enter_frame(function, co->prepare_call_args(proto, payload));
        } else {
            if (payload > 1) {
                return ctx.arity_error_range(info.size(), 1, 2);
            }
            ctx.host().enter_coroutine(co);
            // yield 的槽 0 是预留结果槽，由恢复者填入。
            co->peek(0) = payload == 0 ? Value::nil_val() : info[1];
        }
        return true;
    }

    bool CoroutineClass::yield(const NativeCallInfo& info) {
        auto& ctx = info.ctx();
        // 嵌套驱动等待同步结果，yield 会穿越返回边界，故禁止。
        if (ctx.ret_frame() != 0) {
            return ctx.fail(ErrorCode::YieldInNestedExecution, "cannot yield inside a nested execution");
        }
        const auto argc = info.size();
        if (argc > 1) {
            return ctx.arity_error_range(argc, 0, 1);
        }
        const auto resumer = ctx.previous();
        if (resumer == nullptr) {
            return ctx.fail(ErrorCode::YieldOutsideCoroutine, "cannot yield outside a coroutine");
        }
        // 结果写入恢复者的 resume 槽 0；当前协程帧链保留供下次继续。
        resumer->peek(0) = argc == 0 ? Value::nil_val() : info[0];
        ctx.host().leave_coroutine(ExecState::Suspended);
        return true;
    }

    bool CoroutineClass::status(const NativeCallInfo& info) {
        auto& ctx = info.ctx();
        if (const auto argc = info.size(); argc != 1) {
            return ctx.arity_error(argc, 1);
        }
        const auto co = try_as_obj<ObjMovement>(info[0]);
        if (co == nullptr) {
            return ctx.fail(ErrorCode::TypeMismatch, "argument must be a coroutine, got {}", type_name(info[0]));
        }
        info.set_return_val(Value::from_obj(new_string(ctx.gc(), to_string(co->state()))));
        return true;
    }

    ObjClass* CoroutineClass::make_class(GC& gc, ObjClass* super) {
        const auto klass = new_class(gc, "Coroutine", super);
        Builtin::register_class_functions(gc, klass, kCoroutineBuiltins);
        return klass;
    }

} // namespace aria
