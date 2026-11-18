#ifndef ARIA_MODULE_CLASS_HPP
#define ARIA_MODULE_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    class ModuleClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_MODULE_CLASS_HPP
