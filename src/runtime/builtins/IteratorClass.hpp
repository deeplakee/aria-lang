#ifndef ARIA_ITERATOR_CLASS_HPP
#define ARIA_ITERATOR_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class IteratorClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_ITERATOR_CLASS_HPP
