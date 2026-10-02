#ifndef ARIA_LIST_CLASS_HPP
#define ARIA_LIST_CLASS_HPP

#include "common.hpp"
#include "runtime/value_register.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // list 方法面(List bootstrap 类)自持单元:公有面 = make_class 加算子实现缓存清单,方法体
    // 与方法表住同名 .cpp。
    class ListClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面;返回白色对象,发布进根由调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);

        // 算子实现缓存清单:算子钩子名 -> List 实现格。
        static constexpr Pair<StringView, u8> kOperatorFns[] = {
                {"__add__", kListAddFnOffset},
                {"__mul__", kListMulFnOffset},
        };
    };

} // namespace aria

#endif // ARIA_LIST_CLASS_HPP
