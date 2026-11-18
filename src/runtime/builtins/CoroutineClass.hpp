#ifndef ARIA_COROUTINE_CLASS_HPP
#define ARIA_COROUTINE_CLASS_HPP

#include "common.hpp"
#include "runtime/NativeCallInfo.hpp"
#include "runtime/builtins/Builtin.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class CoroutineClass {
        static bool create(const NativeCallInfo& info);

        static bool resume(const NativeCallInfo& info);

        static bool yield(const NativeCallInfo& info);

        static bool status(const NativeCallInfo& info);

        // resume/yield 成功时已切换上下文，失败时须保持原上下文。
        static constexpr BuiltinFnEntry kCoroutineBuiltins[] = {
                {"create", create},
                {"resume", resume},
                {"yield", yield},
                {"status", status},
        };

    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_COROUTINE_CLASS_HPP
