#ifndef ARIA_EXCEPTION_CLASS_HPP
#define ARIA_EXCEPTION_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class ExceptionClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_EXCEPTION_CLASS_HPP
