#include "runtime/builtins/ExceptionClass.hpp"

#include <utility>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjInstance.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtins.hpp"
#include "runtime/string_constant.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // Exception 类方法实现(NativeFn 方法调用形态见 Builtins.hpp)。face 按槽 0 类型分派:
        // ObjException(VM 报错装箱与 Error 工厂产物)读 C++ 成员;ObjInstance(用户子类实例)经
        // load_field 读 _message/_code 字段;其余接收者(类静态槽裸调等 stray)响亮 TypeMismatch。

        // init():默认构造 -- 落 _code=Error 默认码、_message="" 两字段,保 Exception 链上实例
        // 恒有此二字段(face 实例腿读据前提)。用户子类未写 init 时沿链继承本实现;自有 init 须
        // 自行落同名字段(设默认值或传参由用户定),违约未落 -> face 读据走类措辞 UndefinedProperty。
        bool fn_init(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto inst = try_obj<ObjInstance>(slots[0]);
            if (inst == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "init requires an instance receiver, got {}",
                               type_name(slots[0]));
            }
            // GC 走查:receiver 经槽 0 栈根;键经常量串表强根零分配;_code 值无分配,_message 值
            // 建成即存(创建到入表窗口内零 GC 点,免守卫)。
            inst->store_field(vm, vm.string_constant(StringConstant::ExcCode),
                              Value::from_int(std::to_underlying(ErrorCode::Error)));
            inst->store_field(vm, vm.string_constant(StringConstant::ExcMessage),
                              Value::from_obj(new_string(vm.gc(), "")));
            return true;
        }

        // message() -> 消息原文:ObjException 读 message_(VM 报错为完整烘焙串含 "Category: Name"
        // 壳;Error 工厂产物为用户所给原串),实例读 _message 字段。
        bool fn_message(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            if (const auto ex = try_obj<ObjException>(slots[0])) {
                slots[0] = Value::from_obj(ex->message());
                return true;
            }
            if (const auto inst = try_obj<ObjInstance>(slots[0])) {
                if (const auto hit = inst->load_field(vm, vm.string_constant(StringConstant::ExcMessage))) {
                    slots[0] = *hit; // 建成即写槽:load_field 内绑定分配的新 bound 经此根化
                    return true;
                }
                return false;
            }
            return vm.fail(ErrorCode::TypeMismatch, "message requires an Exception receiver, got {}",
                           type_name(slots[0]));
        }

        // code() -> 错误码数字:ObjException 读 code_ 单一存储的 i64 视图(VM 报错 = 注册表序号,
        // Error(msg, code) 码参 = 用户所给 int 原样,见 ObjException 头注释),实例读 _code 字段。
        bool fn_code(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            if (const auto ex = try_obj<ObjException>(slots[0])) {
                slots[0] = Value::from_int(ex->numeric_code());
                return true;
            }
            if (const auto inst = try_obj<ObjInstance>(slots[0])) {
                if (const auto hit = inst->load_field(vm, vm.string_constant(StringConstant::ExcCode))) {
                    slots[0] = *hit; // 建成即写槽:load_field 内绑定分配的新 bound 经此根化
                    return true;
                }
                return false;
            }
            return vm.fail(ErrorCode::TypeMismatch, "code requires an Exception receiver, got {}", type_name(slots[0]));
        }

        // Exception 类方法表:注册进 Exception bootstrap 类(注册机制见 runtime/builtins/Builtins.hpp)。
        constexpr builtins::BuiltinEntry kExceptionBuiltins[] = {
                {"init", fn_init},
                {"message", fn_message},
                {"code", fn_code},
        };

    } // namespace

    void ExceptionClass::register_methods(GC& gc, ObjClass* klass) {
        register_class_methods(gc, klass, kExceptionBuiltins);
    }

} // namespace aria
