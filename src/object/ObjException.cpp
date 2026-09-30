#include "object/ObjException.hpp"

#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"

namespace aria {

    ObjException::ObjException(const i64 code, ObjString* message) :
        Object{ObjType::EXCEPTION}, code_{code}, message_{message} {
        // message_ 恒非空:空串 intern 兜底(无合法指针空态);构造期拦截非法 null。
        ASSERT(message != nullptr, "error message must not be null");
    }

    Error ObjException::to_error() const {
        // message_ 已是完整烘焙串 -- 经 Error::from_baked **原样**回传,跳过 make_message
        // 重烘(否则双重前缀)。
        return Error::from_baked(code(), message_->view());
    }

    void ObjException::trace(GC& gc) const noexcept {
        // message_ 恒非空(ctor ASSERT);mark_object 容 nullptr 仅防御。
        gc.mark_object(message_);
    }

    String ObjException::debug_repr() const {
        // 渲染完整烘焙消息(无引号);catch 的 println(e)/str(e) 即此文案。
        return String{message_->view()};
    }

    Opt<Value> ObjException::load_field(AriaVM& vm, ObjString* name) {
        // 两步形态同 ObjString(见 Object.hpp):委托 Exception bootstrap 类表(沿链达 Object
        // 根),命中自持 new_bound_method 恒绑 this(表条目全为原生方法)。
        const auto hit = vm.exception_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjException::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 不铸 ObjBoundMethod,命中直取类表原生值(契约见 Object.hpp);本体是纯透传。
        return vm.exception_class()->load_field(vm, name);
    }

    ObjException* new_exception(GC& gc, const ErrorCode code, const StringView message) {
        // VM 报错装箱:码 = 注册表序号。message 为调用方烘好的完整消息串(原样存,不经
        // make_message -- 否则双重前缀)。
        return new_exception(gc, std::to_underlying(code), message);
    }

    ObjException* new_exception(GC& gc, const i64 code, const StringView message) {
        // 基础实现:数字码直通(VM 注册表序号或 Error 内建码参)。msg 本函数内部新建(intern)
        // 并自守跨下方 new_object(真 GC 触发点)。
        const auto msg   = new_string(gc, message);
        const auto guard = gc.make_guard(msg);
        return gc.new_object<ObjException>(code, msg);
    }

    ObjException* new_exception(GC& gc, const Error& error) {
        // Error 公开构造面产物皆已烘焙完整消息,逐件转发原样装箱。
        return new_exception(gc, error.code(), error.message());
    }

} // namespace aria
