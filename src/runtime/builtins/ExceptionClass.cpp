#include "runtime/builtins/ExceptionClass.hpp"

#include <utility>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjInstance.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // face 按槽 0 类型分派:ObjException 读 C++ 成员,ObjInstance(用户子类实例)经 load_field
        // 读 _message/_code 字段,其余接收者(stray)响亮 TypeMismatch。

        // init():默认构造落 _code/_message 两字段(链上实例恒有此二字段);子类自有 init 未落
        // 同名字段时,face 读据走 UndefinedProperty 类措辞。
        bool fn_init(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto inst = try_as_obj<ObjInstance>(slots[0]);
            if (inst == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "init requires an instance receiver, got {}",
                               type_name(slots[0]));
            }
            // GC 走查:receiver 经槽 0 栈根,键串零分配,_message 建成即存,窗口内零 GC 点免守卫。
            inst->store_field(vm, vm.str<"_code">(), Value::from_int(std::to_underlying(ErrorCode::Error)));
            inst->store_field(vm, vm.str<"_message">(), Value::from_obj(new_string(vm.gc(), "")));
            return true;
        }

        // message() -> 消息原文:ObjException 读 message_(VM 报错为烘焙整串),实例读 _message 字段。
        bool fn_message(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            if (const auto ex = try_as_obj<ObjException>(slots[0])) {
                slots[0] = Value::from_obj(ex->message());
                return true;
            }
            if (const auto inst = try_as_obj<ObjInstance>(slots[0])) {
                if (const auto hit = inst->load_field(vm, vm.str<"_message">())) {
                    slots[0] = *hit; // 建成即写槽:命中值经此根化
                    return true;
                }
                return false;
            }
            return vm.fail(ErrorCode::TypeMismatch, "message requires an Exception receiver, got {}",
                           type_name(slots[0]));
        }

        // code() -> 错误码数字:ObjException 读 code_ 的 i64 视图,实例读 _code 字段。
        bool fn_code(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            if (const auto ex = try_as_obj<ObjException>(slots[0])) {
                slots[0] = Value::from_int(ex->numeric_code());
                return true;
            }
            if (const auto inst = try_as_obj<ObjInstance>(slots[0])) {
                if (const auto hit = inst->load_field(vm, vm.str<"_code">())) {
                    slots[0] = *hit; // 建成即写槽:命中值经此根化
                    return true;
                }
                return false;
            }
            return vm.fail(ErrorCode::TypeMismatch, "code requires an Exception receiver, got {}", type_name(slots[0]));
        }

        // Exception 类方法表。
        constexpr BuiltinFnEntry kExceptionBuiltins[] = {
                {"init", fn_init},
                {"message", fn_message},
                {"code", fn_code},
        };

    } // namespace

    ObjClass* ExceptionClass::make_class(GC& gc, ObjClass* super) {
        // Exception bootstrap 类:用户异常基类(继承它定义自己的异常类型)。
        const auto klass = new_class(gc, "Exception", super);
        Builtin::register_class_methods(gc, klass, kExceptionBuiltins);
        return klass;
    }

} // namespace aria
