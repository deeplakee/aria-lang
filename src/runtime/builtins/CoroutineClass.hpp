#ifndef ARIA_COROUTINE_CLASS_HPP
#define ARIA_COROUTINE_CLASS_HPP

#include "common.hpp"
#include "runtime/NativeCallInfo.hpp"
#include "runtime/builtins/Builtin.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Coroutine 单例类自持单元:协程句柄的类面载体 + 四原语静态方法面(create/resume/yield/
    // status);四原语为 NativeFn 签名静态方法,报错族与调用机器面经 ctx,切换对经 ctx.host()
    // 直调宿主私有面故为友元。唯一公有口是收尾的 make_class。
    class CoroutineClass {
        static bool create(const NativeCallInfo& info);

        static bool resume(const NativeCallInfo& info);

        static bool yield(const NativeCallInfo& info);

        static bool status(const NativeCallInfo& info);

        // 静态方法表;调用区 = [Coroutine 类(self 兼返回槽), co, payload..]。create/status 普通原生;
        // resume/yield 切换型:返 true 时 current_ 已切至对侧且不写返回槽(对侧预留结果槽,由
        // yield/RETURN 完成写;对侧死于未捕获错误时载荷走本侧挂起寄存器),false ⟺ 已 fail 未切换。
        static constexpr BuiltinFnEntry kCoroutineBuiltins[] = {
                {"create", create},
                {"resume", resume},
                {"yield", yield},
                {"status", status},
        };

    public:
        // 唯一构造口:建类并装载方法面;返回白色对象,发布进根由调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_COROUTINE_CLASS_HPP
