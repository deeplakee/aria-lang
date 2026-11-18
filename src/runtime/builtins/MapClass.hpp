#ifndef ARIA_MAP_CLASS_HPP
#define ARIA_MAP_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class MapClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_MAP_CLASS_HPP
