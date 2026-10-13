#include "runtime/builtins/ObjectClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // init():no-op 不写槽,槽 0 原样返回实例;未写 init 的类沿链继承本实现。
        bool fn_init(const NativeCallInfo&) { return true; }

        // is_a(klass) -> bool:接收者类链沿 super 与目标类同一比对;链根按接收者分派:实例自 class_,
        // 其余承载值自各自 bootstrap 类,类值自 Class 元类单例(类值的所属类,模板链不参与判定);
        // 纯读无分配,receiver/target 两根全程在槽。
        bool fn_is_a(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto target = try_as_obj<ObjClass>(info[0]);
            if (target == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "argument must be a class, got {}", type_name(info[0]));
            }
            ObjClass* chain = nullptr;
            switch (info.self().as_obj()->type()) {
                case ObjType::INSTANCE:
                    chain = info.self().as_obj()->as<ObjInstance>()->klass();
                    break;
                case ObjType::EXCEPTION:
                    chain = ctx.exception_class();
                    break;
                case ObjType::LIST:
                    chain = ctx.list_class();
                    break;
                case ObjType::MAP:
                    chain = ctx.map_class();
                    break;
                case ObjType::STRING:
                    chain = ctx.string_class();
                    break;
                case ObjType::RANGE:
                    chain = ctx.range_class();
                    break;
                case ObjType::ITERATOR:
                    chain = ctx.iterator_class();
                    break;
                case ObjType::CLOSURE:
                    chain = ctx.closure_class();
                    break;
                case ObjType::NATIVE_FN:
                    chain = ctx.native_fn_class();
                    break;
                case ObjType::BOUND_METHOD:
                    chain = ctx.bound_method_class();
                    break;
                case ObjType::MODULE:
                    chain = ctx.module_class();
                    break;
                case ObjType::MOVEMENT:
                    chain = ctx.coroutine_class();
                    break;
                case ObjType::CLASS:
                    chain = ctx.class_class(); // 类值是 Class 元类的"实例",归属沿元类链
                    break;
                default:
                    // 唯一可达 stray:静态槽读出 is_a 后裸调(receiver 槽残留被调原生自身),响亮拒绝。
                    return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support 'is_a'", type_name(info.self()));
            }
            for (auto node = chain; node != nullptr; node = node->superclass()) {
                if (node == target) {
                    info.set_return_val(Value::from_bool(true));
                    return true;
                }
            }
            info.set_return_val(Value::from_bool(false));
            return true;
        }

        // __str__():默认显示渲染,体 = 接收者对象自身类型的 debug_repr(非重入、不执行用户代码):
        // <Foo instance>/<class Foo>/<fn f>/协程形各归其位;凡类链抵达根的承载值缺省继承本实现,
        // 子类在表上覆写即沿链生效。
        bool fn___str__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.self();
            if (!self.is_obj()) {
                return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support '__str__'", type_name(self));
            }
            info.set_return_val(Value::from_obj(new_string(ctx.gc(), self.as_obj()->debug_repr())));
            return true;
        }

        // Object 根类方法表。
        constexpr BuiltinFnEntry kObjectBuiltins[] = {
                {"init", fn_init},
                {"is_a", fn_is_a},
                {"__str__", fn___str__},
        };

    } // namespace

    ObjClass* ObjectClass::make_class(GC& gc) {
        // Object 根类:唯一 super 为空的 bootstrap 类,全类链之根(各内建类以它作 super)。
        const auto klass = new_class(gc, "Object", nullptr);
        Builtin::register_class_methods(gc, klass, kObjectBuiltins);
        return klass;
    }

} // namespace aria
