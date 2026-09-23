#ifndef ARIA_MODULECTX_HPP
#define ARIA_MODULECTX_HPP

// 模块编译上下文：收口每模块状态 -- 模块句柄 + 当前函数上下文游标（兼拥有入口 fn 上下文）+ 顶层
// 全局名注册表；与 FunctionCtx 对齐成「模块 > 函数 > 作用域」三层。current_fn_ctx_ 单成员兼两职：
// 既是入口 fn 上下文的所有者（ctor 就 m.entry() new、dtor 沿 enclosing_ 链 delete），又是当前编译
// 到哪个函数的游标--故无需 owner + cursor 两指针；子上下文由 compile_function new、成功路径还原游标
// 并 delete 子，出错路径交 ~ModuleCtx 走链释放。「当前 CodeUnit」由 CodeGen 经 cur_cu() 派生。
// 一次性实例：每个模块编译用全新一个，用完即弃，不复用、无 reset。

#include "type.hpp"

namespace aria {

    class ObjModule;
    class ObjFunction;
    class FunctionCtx;

    class ModuleCtx {
    public:
        ModuleCtx() = delete; // 必须绑定 module 构造（一次性实例，经 UPtr<ModuleCtx> 持有）

        // 一次性构造：绑定模块句柄 + 新建入口 fn 上下文（就 m.entry()）并就位游标；须先 set_entry（ASSERT）。
        explicit ModuleCtx(ObjModule* module);

        // 沿 enclosing_ 链从 current_fn_ctx_ 走到 entry 逐个 delete（无论游标在哪都对）；定义于 .cpp。
        ~ModuleCtx();

        ModuleCtx(const ModuleCtx&)            = delete;
        ModuleCtx& operator=(const ModuleCtx&) = delete;
        ModuleCtx(ModuleCtx&&)                 = delete;
        ModuleCtx& operator=(ModuleCtx&&)      = delete;

        // 登记顶层全局名（按内容判等）：true=新登记；false=已存在（调用方 fail RedefinedVariable）。
        bool declare_global(const StringView name) { return defined_globals_.insert(String{name}).second; }

        // 当前是否模块顶层作用域（入口函数 enclosing_ == nullptr 且第 0 层 scope_depth_ == 0）--变量
        // 定义为模块全局而非局部的判据，bind_stack_value 消费；定义于 .cpp（需 FunctionCtx 完整类型）。
        [[nodiscard]]
        bool is_global_scope() const noexcept;

        ObjModule* module_;

        // 当前函数上下文游标，兼拥有入口 fn 上下文（ctor new、dtor delete）；compile_function 进出函数时摆动。
        FunctionCtx* current_fn_ctx_;

    private:
        HashSet<String> defined_globals_; // 已登记顶层全局名（内容判等）
    };

} // namespace aria

#endif // ARIA_MODULECTX_HPP
