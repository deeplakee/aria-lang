#ifndef ARIA_FUNCTION_CLASS_HPP
#define ARIA_FUNCTION_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class FunctionClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_FUNCTION_CLASS_HPP
