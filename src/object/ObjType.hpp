#ifndef ARIA_OBJ_TYPE_HPP
#define ARIA_OBJ_TYPE_HPP

#include "common.hpp"

namespace aria {

    // Object 子类型标识。
    enum class ObjType : u8 {
        BASE,
        STRING,
        FUNCTION,
        NATIVE_FN,
        UPVALUE,
        CLASS,
        INSTANCE,
        BOUND_METHOD,
        LIST,
        MAP,
        MODULE,
        RANGE,
        ITERATOR,
        EXCEPTION,
        CLOSURE,
        MOVEMENT,
    };

    // ObjType 的类型名映射(PascalCase):C++ 侧诊断与语言面(type() 内建/用户可见报错/debug_repr 默认)。
    [[nodiscard]]
    constexpr StringView to_string(const ObjType kind) noexcept {
        switch (kind) {
            case ObjType::BASE:
                return "Base";
            case ObjType::STRING:
                return "String";
            case ObjType::FUNCTION:
                return "Function";
            case ObjType::NATIVE_FN:
                return "NativeFn";
            case ObjType::UPVALUE:
                return "Upvalue";
            case ObjType::CLASS:
                return "Class";
            case ObjType::INSTANCE:
                return "Instance";
            case ObjType::BOUND_METHOD:
                return "BoundMethod";
            case ObjType::LIST:
                return "List";
            case ObjType::MAP:
                return "Map";
            case ObjType::MODULE:
                return "Module";
            case ObjType::RANGE:
                return "Range";
            case ObjType::ITERATOR:
                return "Iterator";
            case ObjType::EXCEPTION:
                return "Exception";
            case ObjType::CLOSURE:
                return "Closure";
            case ObjType::MOVEMENT:
                return "Coroutine"; // 语言概念是协程;MOVEMENT 只是 C++ 实现词根
            default:
                UNREACHABLE();
        }
    }

} // namespace aria

#endif // ARIA_OBJ_TYPE_HPP
