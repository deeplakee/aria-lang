#ifndef ARIA_BUILTINS_HPP
#define ARIA_BUILTINS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class AriaHashTable;

    // 内置函数注册机制:把 type/len/str/assert 等内建原生函数注册进 **VM 级只读 builtins 表**
    // (设计见 .claude/reference/runtime/vm-design.md §7「VM 级 builtins 表 + LOAD_GLOBAL 回退」)。
    //
    //   - 不引入 LOAD_BUILTIN 指令:内置经 new_native_fn 包成 ObjNativeFn 后按名写入
    //     AriaVM 的 builtins_ 表(全 VM 共享一份),用户代码经普通 LOAD_GLOBAL 解析 -- 先查当前
    //     模块 globals,miss 回退 builtins_(Python 式查找链)。不按模块注入:预填 globals 会在
    //     REPL 逐行 run() 时重注册、覆写用户 shadow,与「顶层 var 跨行保留」矛盾。
    //   - shadow 语义:用户顶层 `var type = ...` 经 DEF_GLOBAL 写模块 globals 优先命中,内置被
    //     遮蔽;内置不入编译期 defined_globals_,`var type` 不触发 RedefinedVariable。
    //     STORE_GLOBAL 不回退 builtins -- 裸名赋值(无 var)未命中即 UndefinedVariable,与
    //     grammar §205-206「赋值不隐式创建、必须先 var 声明」一致。
    //
    //   根安全(GC 已启用):由 VM ctor 在 set_vm_roots 之后调用,注册内触 GC 时已入表条目经
    //   tracer 标根,在建对象由 register_builtins 内 make_guard 双守卫承重(见其定义)。
    namespace builtins {

        // 把全部内置原生函数按名注册进 VM 级 builtins 表。由 AriaVM ctor 在 set_vm_roots 之后
        // 调用一次;每条的守卫次序见定义处注释。
        void register_builtins(GC& gc, AriaHashTable& builtins);

    } // namespace builtins

} // namespace aria

#endif // ARIA_BUILTINS_HPP
