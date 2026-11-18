#ifndef ARIA_BOUND_METHOD_CLASS_HPP
#define ARIA_BOUND_METHOD_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class BoundMethodClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_BOUND_METHOD_CLASS_HPP
