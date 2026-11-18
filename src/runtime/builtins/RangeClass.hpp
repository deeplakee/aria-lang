#ifndef ARIA_RANGE_CLASS_HPP
#define ARIA_RANGE_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class RangeClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_RANGE_CLASS_HPP
