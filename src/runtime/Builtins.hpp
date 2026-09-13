#ifndef ARIA_BUILTINS_HPP
#define ARIA_BUILTINS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class AriaHashTable;

    // 内置函数注册机制:把 type/len/str/assert 等内建原生函数按名注册进 **VM 级只读
    // builtins_ 表**(全 VM 共享),用户代码经普通 LOAD_GLOBAL 解析 -- 先查模块 globals,
    // miss 回退 builtins_(Python 式查找链;不引入 LOAD_BUILTIN 指令)。不按模块注入:预填
    // globals 会在 REPL 逐行 run() 时重注册、覆写用户 shadow。shadow 语义:用户顶层 var 经
    // DEF_GLOBAL 写模块 globals 优先命中;内置不入编译期 defined_globals_,不触发
    // RedefinedVariable;STORE_GLOBAL 不回退 builtins(赋值不隐式创建)。
    namespace builtins {

        // 注册全部内置;由 AriaVM ctor 在 set_vm_roots 之后调用一次(在建对象经 make_guard
        // 双守卫,见定义)。
        void register_builtins(GC& gc, AriaHashTable& builtins);

    } // namespace builtins

} // namespace aria

#endif // ARIA_BUILTINS_HPP
