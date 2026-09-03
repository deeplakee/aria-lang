#ifndef ARIA_BUILTINS_HPP
#define ARIA_BUILTINS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class AriaHashTable;

    // 内置函数注册机制(M2 收尾):把 type/len/str/assert 等内建原生函数注册进 **VM 级只读 builtins 表**。
    //
    //   设计(见 .claude/reference/runtime/vm-design.md §7,方案 B「VM 级 builtins 表 + LOAD_GLOBAL 回退」):
    //   - 不引入 LOAD_BUILTIN 指令。内置经 new_native_fn 包成 ObjNativeFn 后按名 upsert 进 AriaVM
    //     的 builtins_ 表(VM 级 AriaHashTable,全 VM 共享一份);用户代码经普通 LOAD_GLOBAL 解析 --
    //     先查当前模块 globals,miss 再回退 builtins_(Python 式 globals -> builtins 查找链)。
    //     intern 池保证 CodeGen 发射的名字指针与注册所用 new_string 同指,value_identical 命中。
    //   - 由 AriaVM 构造期调用一次(在 set_vm_roots 之后),全 VM 生命周期共享,不再每模块注入。
    //     旧方案 A「按模块预填 globals」会在 REPL 逐行 run() 时重注册、覆写用户 shadow,与「顶层 var
    //     跨行保留」矛盾;方案 B 一份只读表彻底回避,并省掉每模块 4 个 ObjNativeFn 分配。
    //   - shadow 语义:用户顶层 `var type = ...` 经 DEF_GLOBAL 写入模块 globals,LOAD_GLOBAL 模块
    //     globals 优先命中即返回用户值,内置被遮蔽。内置仅入运行期 builtins_ 表,不入编译期
    //     defined_globals_,故 `var type` 不触发 RedefinedVariable(允许 shadow)。STORE_GLOBAL 不回退
    //     builtins -- 裸名赋值 `type = 5`(无 var)模块 globals 未命中即 UndefinedVariable,与 grammar
    //     §205-206「赋值不隐式创建、必须先 var 声明」一致。
    //
    //   根安全(GC 已启用):由 VM ctor 在 set_vm_roots 之后调用,故注册内 new_string/new_native_fn
    //   (各一次 new_object 顶 maybe_collect)触 GC 时:已入表的条目经 vm_roots tracer 的 builtins_.trace
    //   标根;在建的 name 串(intern weak root)与 ObjNativeFn 跨 new_native_fn 与 upsert 须守 -- 每条用
    //   make_guard 双守卫承重。builtins_ 表本身是 VM 值成员,tracer 持 [this] 标其 key+value。
    namespace builtins {

        // 把全部内置原生函数按名注册进 VM 级 builtins 表(给定 AriaHashTable)。由 AriaVM ctor 在
        // set_vm_roots 之后调用一次。每条:new_string(intern weak root)+ make_guard -> new_native_fn +
        // make_guard -> builtins.upsert(rehash 触 GC,双守卫承重) -> 赋 value。intern 池保证 name 指针
        // 与 CodeGen 发射 LOAD_GLOBAL 所用同名常量同指。
        void register_builtins(GC& gc, AriaHashTable& builtins);

    } // namespace builtins

} // namespace aria

#endif // ARIA_BUILTINS_HPP