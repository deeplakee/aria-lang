#ifndef ARIA_BUILTINS_HPP
#define ARIA_BUILTINS_HPP

#include "common.hpp"
#include "object/ObjNativeFn.hpp" // NativeFn

namespace aria {

    class GC;
    class ObjClass;
    class AriaHashTable;

    // 内置函数注册机制:把内建原生函数按名注册进 **VM 级只读 builtins_ 表**(全 VM 共享),用户代码
    // 经普通 LOAD_GLOBAL 解析 -- 先查模块 globals,miss 回退 builtins_(Python 式查找链;不引入 LOAD_BUILTIN 指令)。不按
    // 模块注入:预填 globals 会在 REPL 逐行 run() 时重注册、覆写用户 shadow。shadow 语义:用户顶层 var 经 DEF_GLOBAL 写
    // 模块 globals 优先命中;内置不入编译期 defined_globals_,不触发 RedefinedVariable;STORE_GLOBAL 不回退 builtins(赋值
    // 不隐式创建)。本目录(runtime/builtins/)是语言内建面的统一收纳:命名规律 --**裸 Builtins = 全局自由函数表**(本文件
    // ,LOAD_GLOBAL 回退触达);**XXXBuiltins = XXX 类型的内建方法面**(List/Map/Iterator/String/Range,恒经 bootstrap 类表
    // 分派、恒绑定 receiver)。前缀有无即两类机制的区分;两者共用的底座(条目形态 BuiltinEntry、类型方法面装载
    // register_builtin_methods)亦住本文件 --本文件即目录伞文件,先述底座再述全局表。
    namespace builtins {

        // NativeFn 方法调用形态(全部 XXXBuiltins 方法共用,四个方法面文件不再复述):slots[0] =
        // receiver 兼返回槽,读 slots[1..] 为实参;失败 `return vm.fail(...)`(bool 契约
        // false ⟺ 已 raise)。receiver 在 slots[0] 于栈根,方法产出新对象须在覆写 slots[0] 前发布。

        // 内建表条目:名 + 原生函数指针。全局自由函数表(kBuiltins)与各类型方法表
        // (kListBuiltins / kStringBuiltins / kMapBuiltins / kRangeBuiltins / kIteratorBuiltins)
        // 同此一形态。
        struct BuiltinEntry {
            StringView name;
            NativeFn   fn;
        };

        // 按名把内建方法表逐条注册进**类字段表**,成为该类实例的内建方法面(receiver 恒绑定):
        // name 作字段键,经 new_native_fn 的 StringView 重载 intern,与 CodeGen LOAD_FIELD 发射
        // 的同名常量同指针,查表按指针命中。全局表不经此(只此一处填,循环就地写在定义里)。
        void register_builtin_methods(GC& gc, ObjClass* klass, Span<const BuiltinEntry> methods);

        // 注册全部内置;由 AriaVM ctor 在 set_vm_roots 之后调用一次(在建对象经 make_guard
        // 双守卫,见定义)。
        void register_builtin_functions(GC& gc, AriaHashTable& builtins);

    } // namespace builtins

} // namespace aria

#endif // ARIA_BUILTINS_HPP
