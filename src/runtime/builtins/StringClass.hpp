#ifndef ARIA_STRING_CLASS_HPP
#define ARIA_STRING_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // string 方法面(String bootstrap 类表条目)的注册入口,住 runtime/builtins/(方法面是 VM 侧
    // 语言面,object 层保持纯表示;对标 Builtins.hpp 的 register_builtin_functions)。实现与方法体
    // 在 StringClass.cpp,VM 只在 bootstrap_string_class 编排调用。须在 ctor 构造临界区
    // 内调用(创建免守卫,入表即根)。
    void register_string_builtins(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_STRING_CLASS_HPP
