#ifndef ARIA_BUILTINS_HPP
#define ARIA_BUILTINS_HPP

#include "common.hpp"
#include "object/ObjNativeFn.hpp" // NativeFn

namespace aria {

    class GC;
    class ObjClass;
    class ObjModule;
    class AriaHashTable;

    // 内置函数注册机制:把内建原生函数按名注册进 **VM 级只读 builtins_ 表**(全 VM 共享),用户代码
    // 经普通 LOAD_GLOBAL 解析 -- 先查模块 globals,miss 回退 builtins_(Python 式查找链;不引入 LOAD_BUILTIN 指令)。不按
    // 模块注入:预填 globals 会在 REPL 逐行 run() 时重注册、覆写用户 shadow。shadow 语义:用户顶层 var 经 DEF_GLOBAL 写
    // 模块 globals 优先命中;内置不入编译期 defined_globals_,不触发 RedefinedVariable;STORE_GLOBAL 不回退 builtins(赋值
    // 不隐式创建)。本目录(runtime/builtins/)是语言内建面的统一收纳:命名规律 --**裸 Builtins = 全局自由函数表**(本文件
    // ,LOAD_GLOBAL 回退触达);**XXXClass = XXX 类型的内建方法面**(List/Map/Iterator/String/Range,恒经 bootstrap 类表
    // 分派、恒绑定 receiver;注册口 = 宿主类公有静态方法 XxxClass::register_methods,方法体与表仍住各 .cpp 匿名命名空间;
    // String 另有算子实现缓存清单 kOperatorFns(钩子名 -> 寄存器格),bootstrap_string_class
    // 据此拷实现进寄存器格);**XXXModule = 内建模块的方法面** (CoroutineModule,经模块 globals
    // 触达,方法面自持于友元宿主类 XXXModule -- 原语与表私有、唯一公有口 register_functions,resume 须访问切换私有面故为
    // AriaVM 友元,见该文件头)。前缀即机制区分; 三个底座(条目形态 BuiltinFnEntry、类型方法面装载
    // register_class_methods、模块方法面装载 register_module_functions)亦住本文件
    // --本文件即目录伞文件,先述底座再述全局表。
    namespace builtins {

        // NativeFn 方法调用形态(全部 XXXClass 方法共用,四个方法面文件不再复述):slots[0] =
        // receiver 兼返回槽,读 slots[1..] 为实参;失败 `return vm.fail(...)`(bool 契约
        // false ⟺ 已 raise)。receiver 在 slots[0] 于栈根,方法产出新对象须在覆写 slots[0] 前发布。

        // 内建表条目:名 + 原生函数指针。全局自由函数表(kBuiltinFns)、各类型方法表
        // (kListBuiltins / kStringBuiltins / kMapBuiltins / kRangeBuiltins / kIteratorBuiltins)
        // 与模块方法表(CoroutineModule::kModuleFunctions)同此一形态。
        struct BuiltinFnEntry {
            StringView name;
            NativeFn   fn;
        };

        // 按名把内建方法表逐条注册进**类字段表**,成为该类实例的内建方法面(receiver 恒绑定):
        // name 作字段键,经 new_native_fn 的 StringView 重载 intern,与 CodeGen LOAD_FIELD 发射
        // 的同名常量同指针,查表按指针命中。全局表不经此(只此一处填,循环就地写在定义里)。
        void register_class_methods(GC& gc, ObjClass* klass, Span<const BuiltinFnEntry> methods);

        // 按名把内建方法表逐条注册进**模块全局表**(内建模块的成员 = 模块全局绑定,load_field
        // 查表即命中):键 intern 同上;不绑定 receiver -- 方法调用区槽 0 恒模块值,原语不读它。
        // 与 register_builtin_methods 是类表/模块表两个装载面,签名平行。由 AriaVM ctor 构造
        // 临界区内的 bootstrap 调用。
        void register_module_functions(GC& gc, ObjModule* module, Span<const BuiltinFnEntry> fns);

        // 按名把实参函数表逐条注册进**指定 builtins 表**(全局面装载口)。
        void register_functions(GC& gc, AriaHashTable& table, Span<const BuiltinFnEntry> fns);

        // 注册全部内置;由 AriaVM ctor 在 set_vm_roots 之后调用一次(在建对象经 make_guard
        // 双守卫,见定义)。
        void register_builtin(GC& gc, AriaHashTable& builtins);

    } // namespace builtins

} // namespace aria

#endif // ARIA_BUILTINS_HPP
