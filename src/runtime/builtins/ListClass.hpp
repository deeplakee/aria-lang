#ifndef ARIA_LIST_CLASS_HPP
#define ARIA_LIST_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class ListClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_LIST_CLASS_HPP
