#ifndef ARIA_MODULECTX_HPP
#define ARIA_MODULECTX_HPP

// 模块编译上下文：收口每模块状态 -- 模块句柄 + 当前函数上下文游标（兼拥有入口 fn 上下文）+ 顶层
// 全局名注册表。与 FunctionCtx（每函数）对齐：模块 > 函数 > 作用域 三层各一席。「当前函数上下文」
// 游标 current_fn_ctx_ 寄存于此，「当前 CodeUnit」不再单独存--由 CodeGen 经
// cur_cu() = &current_fn_ctx_->fn_->unit() 派生，随游标自动切换，免两指针同步。
//
// current_fn_ctx_ 是普通 FunctionCtx*：构造函数就 m.entry() new 一个入口 FunctionCtx
// （enclosing_==nullptr = entry）并赋值给它。它既是「入口 fn 上下文的所有者」，又是「当前编译到哪个
// 函数」的游标。子函数上下文由 compile_function 用 `new` 分配、enclosing_ 回父、游标摆向子；**成功**
// 路径 compile_function 还原游标并手动 `delete` 子，**出错**路径不还原游标、不 delete 子、直接 return
// （出错即停 -- 见 CodeGen「出错即停」约定）。析构沿 enclosing_ 链从 current_fn_ctx_ 走到 entry 逐个
// delete：成功时游标 = entry 仅删 entry，出错时游标停在 deepest 未释放子，走链释放整条活动链 + entry。
// 故只需一个成员，无需 owner + cursor 两指针，也无需「游标必回入口」的不变式。
//
// **出错即停**（CodeGen 约定）：编译期深层 fail() 抛 AriaCompileException（[[noreturn]]），自动 unwind
// 跨 visit 递归栈，compile() 顶层 catch 翻译为 Result。无需 error_ 成员 / ok() / 各 visit 的
// if(!ok()) return 守卫--throw 即 unwind，首个错误自然即止；出错路径对所有权的影响即上段所述
// （游标还原与 delete 子被跳过，交 ~ModuleCtx 走链释放）。
//
// **一次性**：每个模块编译用一个新的 ModuleCtx（CodeGen::init_module 入口
// `mod_ctx_ = std::make_unique<ModuleCtx>(module)` 构造一个全新实例），用完即弃，不复用、无 reset。
//
// 轻封装：module_ / current_fn_ctx_ 公有（CodeGen 直接读写--后者在 compile_function 进出函数时摆动），
// defined_globals_ 私有（仅经内联 declare_global 访问，做顶层全局名重定义检查，按名字内容判等，
// HashSet<String> 内容哈希，不依赖 intern 指针）。一个内联方法 declare_global；构造函数就着已 set_entry
// 的模块创建入口 fn 上下文并就位游标。entry 判定 = current_fn_ctx_->enclosing_ == nullptr。
//
// 特殊成员：current_fn_ctx_ 是 new 分配的裸指针，析构沿链 delete（需 FunctionCtx 完整类型，故
// ~ModuleCtx() 声明于头、定义于 .cpp）；copy / move 删除（ModuleCtx 一次性、不可移动）--由调用方
// 按指针持有（CodeGen 持 UPtr<ModuleCtx>），无需 move。
//
// 设计上 gc_（共享引用）留 CodeGen：跨编译/运行共享，非模块状态。

#include "type.hpp"

namespace aria {

    class ObjModule;
    class ObjFunction;
    class FunctionCtx;

    // 模块编译上下文（一次性）。详见上方类注释。
    class ModuleCtx {
    public:
        ModuleCtx() = delete; // 必须绑定 module 构造（一次性实例，经 UPtr<ModuleCtx> 持有）

        // 一次性构造：绑定模块句柄 + 创建入口 fn 上下文（就 m.entry()，new 分配）并就位游标；
        // ASSERT m.entry() 非空（调用方须先 set_entry）。定义于 .cpp。
        explicit ModuleCtx(ObjModule* module);

        ~ModuleCtx(); // 定义于 .cpp：沿 enclosing_ 链从 current_fn_ctx_ 走到 entry 逐个 delete（无论游标在哪都对）

        ModuleCtx(const ModuleCtx&)            = delete;
        ModuleCtx& operator=(const ModuleCtx&) = delete;
        ModuleCtx(ModuleCtx&&)                 = delete;
        ModuleCtx& operator=(ModuleCtx&&)      = delete;

        // 登记顶层全局名（按内容判等）：true=新登记；false=已存在（调用方 fail RedefinedVariable）。
        // 内联：insert 已处理「存在则不插入」，.second 即「是否新插入」。
        bool declare_global(const StringView name) { return defined_globals_.insert(String{name}).second; }

        // 当前是否在模块顶层作用域（变量定义应作为模块全局而非局部）：当前函数为入口
        // （current_fn_ctx_->enclosing_ == nullptr）且处于第 0 层作用域（scope_depth_ == 0）。
        // 收口 CodeGen 中 `cur_fn_ctx()->enclosing_ == nullptr && cur_fn_ctx()->scope_depth_ == 0`
        // 判定，供 visitVarDeclNode / compile_function 复用。定义于 .cpp（需 FunctionCtx 完整类型）。
        bool is_global_scope() const noexcept;

        ObjModule* module_;

        // 当前函数上下文游标，兼拥有入口 fn 上下文（ctor new、dtor delete）。compile_function 进出函数
        // 时摆动；析构沿 enclosing_ 链走，无论游标在哪都对（成功仅 entry，出错整条活动链 + entry）。
        FunctionCtx* current_fn_ctx_;

    private:
        HashSet<String> defined_globals_; // 已登记顶层全局名（内容判等）
    };

} // namespace aria

#endif // ARIA_MODULECTX_HPP
