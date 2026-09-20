#include "object/ObjBoundMethod.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjNativeFn.hpp"
#include "value/ObjBridge.hpp"

namespace aria {

    ObjBoundMethod::ObjBoundMethod(const Value method, const Value receiver) :
        Object{ObjType::BOUND_METHOD}, method_{method}, receiver_{receiver} {
        // method_ 须为可调用值(ctor ASSERT):闭包或原生函数 -- 非可调用值没有绑定语义,
        // 静态值直读不走本类型。判定收口 is_callable_value。
        ASSERT(is_callable_value(method), "ObjBoundMethod: method must be a callable (closure or native fn)");
    }

    bool ObjBoundMethod::equals(const Object* other) const noexcept {
        // 先比指针(同一对象恒等),再比「绑的是同一个方法值、且 receiver 同一」--绑定语义就是
        // 「同一实现 + 同一接收者」。method_ 为闭包/原生对象、receiver_ 为任意 Value,皆按身份
        // (value_identical)比较(无 GC 分配,GC-pure 契约保持)。
        if (this == other) {
            return true;
        }
        if (const auto rhs = try_as<ObjBoundMethod>(other)) {
            return value_identical(method_, rhs->method_) && value_identical(receiver_, rhs->receiver_);
        }
        return false;
    }

    void ObjBoundMethod::trace(GC& gc) const noexcept {
        gc.mark_value(method_);   // 方法值装箱任意对象(闭包/原生),mark_value 分派
        gc.mark_value(receiver_); // receiver 装箱任意值,mark_value 分派
    }

    ObjString* ObjBoundMethod::name() const noexcept {
        // 非虚读取:闭包取 fn 名、原生取 name_(两者皆 intern 驻留恒非空,指针随宿主对象保命)。
        const auto m = method_.as_obj();
        if (m->type() == ObjType::CLOSURE) {
            return as<ObjClosure>(m)->name();
        }
        return as<ObjNativeFn>(m)->name();
    }

    String ObjBoundMethod::debug_repr() const {
        // 渲染方法名,与 <fn m> 渲染族同源(经非虚 name() 取名)。
        return std::format("<bound method {}>", name()->view());
    }

    ObjBoundMethod* new_bound_method(GC& gc, const Value method, const Value receiver) {
        // 工厂不替调用方守卫入参:只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 method 与 receiver 中的对象。
        return gc.new_object<ObjBoundMethod>(method, receiver);
    }

} // namespace aria
