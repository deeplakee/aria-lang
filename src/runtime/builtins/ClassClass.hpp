#ifndef ARIA_CLASS_CLASS_HPP
#define ARIA_CLASS_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class ClassClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_CLASS_CLASS_HPP
