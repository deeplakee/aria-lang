#include "runtime/builtins/CoroutineModule.hpp"

#include "common.hpp"
#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    bool CoroutineModule::create(AriaVM& vm, Span<Value> slots) {
        const auto argc = slots.size() - 1;
        if (argc != 1) {
            return vm.arity_error(argc, 1);
        }
        const auto closure = try_as_obj<ObjClosure>(slots[1]);
        if (closure == nullptr) {
            return vm.fail(ErrorCode::TypeMismatch, "argument must be a function, got {}", type_name(slots[1]));
        }
        const auto co = new_movement(vm.gc());
        co->push(Value::from_obj(closure)); // 槽 0:被调闭包(首启进帧的 callee 槽;push 扩容不触 GC)
        slots[0] = Value::from_obj(co);     // 返回槽发布,建成即根化;自分配点至此无 GC 点,免守卫
        return true;
    }

    bool CoroutineModule::resume(AriaVM& vm, Span<Value> slots) {
        const auto argc = slots.size() - 1; // = 1(co) + 载荷数
        // 校验段只读可失败:元数、对象类型与状态检查,失败路径不得留下脏状态。
        if (argc == 0) {
            return vm.arity_error_at_least(argc, 1);
        }
        const auto co = try_as_obj<ObjMovement>(slots[1]);
        if (co == nullptr) {
            return vm.fail(ErrorCode::TypeMismatch, "argument must be a coroutine, got {}", type_name(slots[1]));
        }
        // 状态五态穷举:Suspended 是唯一可恢复态(状态是唯一分派依据),其余各归其码。
        switch (co->state()) {
            case ExecState::Suspended:
                break;
            case ExecState::Done:
            case ExecState::Failed:
                return vm.fail(ErrorCode::ResumeDeadCoroutine, "cannot resume a dead coroutine");
            case ExecState::Normal:
            case ExecState::Running:
                return vm.fail(ErrorCode::ResumeNonSuspendedCoroutine, "cannot resume a non-suspended coroutine");
        }
        // 两臂各自自含完整序列;臂内一切可失败检查都先于切换 -- 失败载荷须落 resumer 寄存器,
        // 切换之后只剩不可失败动作。
        const auto payload = static_cast<u8>(argc - 1);
        if (co->frames().empty()) {
            // 首启:载荷即被调函数实参(上界交 check_arity 报被调者元数,默认参数/varargs 照常)。
            const auto closure = try_as_obj<ObjClosure>(co->peek(0));
            ASSERT(closure != nullptr, "coroutine slot-0 is not a closure (create invariant)");
            const auto fn = closure->function();
            if (!vm.check_arity(fn, payload)) {
                return false;
            }
            vm.enter_coroutine(co);
            for (usize i = 0; i < payload; ++i) {
                co->push(slots[2 + i]); // push 扩容不触 GC(allocate/reallocate 不变式)
            }
            // 元数已预检、协程帧栈必空,直取 call_closure 的整形加进帧尾段;varargs 打包顶
            // maybe_collect 时恢复者经 previous_ 边可达。
            co->enter_frame(closure, vm.prepare_call_args(fn, payload));
        } else {
            // 已挂起:yield 是单值表达式,只收 0 或 1 个载荷。
            if (payload > 1) {
                return vm.arity_error_range(argc, 1, 2);
            }
            vm.enter_coroutine(co);
            // 协程栈顶即 yield 预留槽(call_native 事后 drop 停在它上面),载荷写入即 yield 表达式的值;
            // 载荷仍在恢复者调用区,跨换指读取安全。
            co->peek(0) = payload == 0 ? Value::nil_val() : slots[2];
        }
        return true;
    }

    bool CoroutineModule::yield(AriaVM& vm, Span<Value> slots) {
        const auto argc = slots.size() - 1;
        if (argc > 1) {
            return vm.arity_error_range(argc, 0, 1);
        }
        // yield 只可能由正在执行的上下文发起(current_);空 previous 即主上下文。
        const auto resumer = vm.current_->previous();
        if (resumer == nullptr) {
            return vm.fail(ErrorCode::YieldOutsideCoroutine, "cannot yield outside a coroutine");
        }
        // 提交段:载荷写 resumer 预留槽(resume 调用的槽 0),切回交 leave_coroutine(本侧置
        // Suspended 并解链、resumer 置 Running)。可从任意调用深度发起(挂起整个上下文,帧链原样
        // 保留);嵌套实参位下内层 yield 写的是自己 resumer 的预留槽,互不干扰。
        resumer->peek(0) = argc == 0 ? Value::nil_val() : slots[1];
        vm.leave_coroutine(ExecState::Suspended);
        return true;
    }

    bool CoroutineModule::status(AriaVM& vm, Span<Value> slots) {
        const auto argc = slots.size() - 1;
        if (argc != 1) {
            return vm.arity_error(argc, 1);
        }
        const auto co = try_as_obj<ObjMovement>(slots[1]);
        if (co == nullptr) {
            return vm.fail(ErrorCode::TypeMismatch, "argument must be a coroutine, got {}", type_name(slots[1]));
        }
        // 五状态串皆驻留表条目,new_string 驻留命中零分配,窗口内无 GC 点免守卫。
        slots[0] = Value::from_obj(new_string(vm.gc(), to_string(co->state())));
        return true;
    }

    void CoroutineModule::register_functions(GC& gc, ObjModule* module) {
        Builtin::register_module_functions(gc, module, kModuleFunctions);
    }

    Value CoroutineModule::make_module(GC& gc) {
        // <coroutine> 合成模块:语言面四原语的载体;dir 空 = 纯命名空间,无目录锚点。
        const auto module = new_module(gc, "<coroutine>", "");
        register_functions(gc, module);
        return Value::from_obj(module);
    }

} // namespace aria
