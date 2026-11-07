#include "runtime/builtins/ObjectClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // init():no-op 不写槽,槽 0 原样返回实例;未写 init 的类沿链继承本实现。
        bool fn_init(const NativeCallInfo&) { return true; }

        // __str__():默认显示渲染,体 = 接收者对象自身类型的 repr(非重入、不执行用户代码):
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
            info.set_return_val(Value::from_obj(new_string(ctx.gc(), self.as_obj()->repr())));
            return true;
        }

        // Object 根类方法表。
        constexpr BuiltinFnEntry kObjectBuiltins[] = {
                {"init", fn_init},
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
