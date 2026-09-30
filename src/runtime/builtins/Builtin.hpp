#ifndef ARIA_BUILTIN_HPP
#define ARIA_BUILTIN_HPP

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
    // 不隐式创建)。本目录(runtime/builtins/)是语言内建面的统一收纳:命名规律 --
    // **Builtin 类 = 全局内建面宿主 + 两装载底座**(type/str/println/assert/clock/Error 六原生、两张表
    // kBuiltinFns/kBuiltinVars 与全局面两个装载口 register_functions/register_variables 私有于同名 .cpp;
    // 公有面 = 类表/模块表装载底座 register_class_methods/register_module_functions 加编排口
    // register_builtins,见类注释);**XXXClass = XXX 类型的内建方法面**
    // (List/Map/Iterator/String/Range/Exception/Object,恒经 bootstrap 类表
    // 分派、恒绑定 receiver;注册口 = 宿主类公有静态方法 XxxClass::register_methods,方法体与表仍住各 .cpp 匿名命名空间;
    // String 另有算子实现缓存清单 kOperatorFns(钩子名 -> 寄存器格),bootstrap_string_class
    // 据此拷实现进寄存器格);**XXXModule = 内建模块的方法面** (CoroutineModule,经模块 globals
    // 触达,构造与方法面自持于友元宿主类 XXXModule -- 原语与表私有、唯一公有口 make_module,resume 须访问切换私有面故为
    // AriaVM 友元,见该文件头)。前缀即机制区分;条目形态两式(BuiltinFnEntry/BuiltinVarEntry)亦住本文件
    // --本文件即目录伞文件。

    // 内建表条目:名 + 原生函数指针。全局自由函数表(kBuiltinFns)、各类型方法表
    // (kListBuiltins / kStringBuiltins / kMapBuiltins / kRangeBuiltins / kIteratorBuiltins)
    // 与模块方法表(CoroutineModule::kModuleFunctions)同此一形态。
    struct BuiltinFnEntry {
        StringView name;
        NativeFn   fn;
    };

    // 内建变量条目的初始化函数:收 GC 引用自含构造变量值,复合 bootstrap(如 <coroutine>
    // 模块)同在此能力圈内;registers_ 寄存器单例够不着 -- Exception 类腿须写 registers_,不经此。
    using VarInitFn = Value (*)(GC&);

    // 内建变量条目:名 + 初始化函数指针。仅全局内建变量表(kBuiltinVars)用此形态。
    struct BuiltinVarEntry {
        StringView name;
        VarInitFn  fn;
    };

    // 全局内建面宿主类:六原生实现、两张表(kBuiltinFns / kBuiltinVars)与全局面两个装载口
    // (register_functions / register_variables)私有于同名 .cpp,公有面 = 类表/模块表两个装载
    // 底座加把全局两表一次装齐的编排口 register_builtins(AriaVM ctor 于 set_vm_roots 之后、
    // 构造临界区内调用一次)。静态类无实例(与 ObjModule 的模块对象概念经此消歧,同
    // CoroutineModule 先例)。
    class Builtin {
    public:
        // 按名把内建方法表逐条注册进**类字段表**,成为该类实例的内建方法面(receiver 恒绑定):
        // name 作字段键,经 new_native_fn 的 StringView 重载 intern,与 CodeGen LOAD_FIELD 发射
        // 的同名常量同指针,查表按指针命中。全局表不经此(全局面装载口私有于 Builtin.cpp)。
        static void register_class_methods(GC& gc, ObjClass* klass, Span<const BuiltinFnEntry> methods);

        // 按名把内建方法表逐条注册进**模块全局表**(内建模块的成员 = 模块全局绑定,load_field
        // 查表即命中):键 intern 同上;不绑定 receiver -- 方法调用区槽 0 恒模块值,原语不读它。
        // 与 register_class_methods 是类表/模块表两个装载底座,签名平行。由 AriaVM ctor 构造
        // 临界区内的 bootstrap 调用。
        static void register_module_functions(GC& gc, ObjModule* module, Span<const BuiltinFnEntry> fns);

        // 注册全部内置;由 AriaVM ctor 在 set_vm_roots 之后调用一次(在建对象经 make_guard
        // 双守卫,见定义)。
        static void register_builtins(GC& gc, AriaHashTable& builtins);
    };

} // namespace aria

#endif // ARIA_BUILTIN_HPP
