#include "runtime/builtins/ObjectClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjInstance.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtins.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // Object 根类方法实现(NativeFn 方法调用形态见 Builtins.hpp)。

        // init():no-op -- 返回 true 不写槽,槽 0 原样即返回实例(不合成 ObjFunction,保
        // 「module 恒非空」不变式);未写 init 的类沿链继承本实现。
        bool fn_init(AriaVM&, Span<Value>) { return true; }

        // is_a(klass) -> bool:接收者的类链沿 super 指针与目标类做同一比对,命中即 true。
        // 链根按接收者表示分派:实例自 class_、异常与内置容器自各自 bootstrap 类(vm 访问器,
        // 对象不持类指针)、类接收者自自身。纯读无分配无 GC 点,receiver/target 两根全程在槽。
        bool fn_is_a(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto target = try_obj<ObjClass>(slots[1]);
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
                    // 类链外类型的成员读取在基类默认即 fail,此处唯一可达 stray = 类静态槽读出
                    // is_a 后裸调(receiver 槽残留被调原生自身),响亮拒绝。
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

        // Object 根类方法表:注册进 Object bootstrap 类(注册机制见 runtime/builtins/Builtins.hpp)。
        constexpr BuiltinFnEntry kObjectBuiltins[] = {
                {"init", fn_init},
                {"is_a", fn_is_a},
        };

    } // namespace

    void ObjectClass::register_methods(GC& gc, ObjClass* klass) { Builtin::register_class_methods(gc, klass, kObjectBuiltins); }

} // namespace aria
