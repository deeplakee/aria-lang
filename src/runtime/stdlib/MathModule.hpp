#ifndef ARIA_MATH_MODULE_HPP
#define ARIA_MATH_MODULE_HPP

namespace aria {

    class GC;
    class ObjModule;

    namespace stdlib {

        // module 须在注册期间保持可达。
        void register_math_exports(GC& gc, ObjModule* module);

    } // namespace stdlib

} // namespace aria

#endif // ARIA_MATH_MODULE_HPP
