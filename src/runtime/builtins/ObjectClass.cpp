#include "runtime/builtins/ObjectClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjInstance.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // init():no-op 不写槽,槽 0 原样返回实例;未写 init 的类沿链继承本实现。
        bool fn_init(AriaVM&, Span<Value>) { return true; }

        // is_a(klass) -> bool:接收者类链沿 super 与目标类同一比对;链根按接收者分派:实例自 class_,
        // 异常与内置容器自各自 bootstrap 类,类接收者自自身;纯读无分配,receiver/target 两根全程在槽。
        bool fn_is_a(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto target = try_as_obj<ObjClass>(slots[1]);
            if (target == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "argument must be a class, got {}", type_name(slots[1]));
            }
            ObjClass* chain = nullptr;
            switch (slots[0].as_obj()->type()) {
                case ObjType::INSTANCE:
                    chain = Object::as<ObjInstance>(slots[0].as_obj())->klass();
                    break;
                case ObjType::EXCEPTION:
                    chain = vm.exception_class();
                    break;
                case ObjType::LIST:
                    chain = vm.list_class();
                    break;
                case ObjType::MAP:
                    chain = vm.map_class();
                    break;
                case ObjType::STRING:
                    chain = vm.string_class();
                    break;
                case ObjType::RANGE:
                    chain = vm.range_class();
                    break;
                case ObjType::ITERATOR:
                    chain = vm.iterator_class();
                    break;
                case ObjType::CLASS:
                    chain = Object::as<ObjClass>(slots[0].as_obj());
                    break;
                default:
                    // 唯一可达 stray:类静态槽读出 is_a 后裸调(receiver 槽残留被调原生自身),响亮拒绝。
                    return vm.fail(ErrorCode::TypeMismatch, "type {} does not support 'is_a'", type_name(slots[0]));
            }
            for (auto node = chain; node != nullptr; node = node->superclass()) {
                if (node == target) {
                    slots[0] = Value::from_bool(true);
                    return true;
                }
            }
            slots[0] = Value::from_bool(false);
            return true;
        }

        // Object 根类方法表。
        constexpr BuiltinFnEntry kObjectBuiltins[] = {
                {"init", fn_init},
                {"is_a", fn_is_a},
        };

    } // namespace

    ObjClass* ObjectClass::make_class(GC& gc) {
        // Object 根类:唯一 super 为空的 bootstrap 类,全类链之根(各内建类以它作 super)。
        const auto klass = new_class(gc, "Object", nullptr);
        Builtin::register_class_methods(gc, klass, kObjectBuiltins);
        return klass;
    }

} // namespace aria
