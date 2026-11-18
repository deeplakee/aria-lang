#ifndef ARIA_OBJECT_CLASS_HPP
#define ARIA_OBJECT_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class ObjectClass {
    public:
        static ObjClass* make_class(GC& gc);
    };

} // namespace aria

#endif // ARIA_OBJECT_CLASS_HPP
