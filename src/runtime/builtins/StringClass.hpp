#ifndef ARIA_STRING_CLASS_HPP
#define ARIA_STRING_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class StringClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_STRING_CLASS_HPP
