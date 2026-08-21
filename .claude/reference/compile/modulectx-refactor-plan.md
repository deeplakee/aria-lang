# CodeGen 重构计划：引入 ModuleCtx + 去掉 ctx_stack_

> 状态：**已实现**（2026-08-20 落地，414/414 测试全绿）。
> 来源：会话讨论结论。范围已用 grep 核实（见末尾「使用面核实」）。

## 目标

1. 把 `CodeGen` 的**模块级**状态收口进新类 `ModuleCtx`，与 `FunctionContext`（每函数状态）对齐，形成「模块 > 函数 > 作用域」三层。
2. **去掉 `ctx_stack_`**：不再用 `List<UPtr<FunctionContext>>` 持有所有上下文，改为 `ctx_` 单指针 + `enclosing_` 链 + 局部 `UPtr` 持有。
3. 顺带去掉 `is_entry_`：`enclosing_ == nullptr` 即 entry，`is_entry_` 冗余。
4. 顺带两项改进：`defined_globals_` 值类型 `String` → `ObjString*`（intern 指针判等，免拷贝）；import 别名入表（补 `import "x" as U; var U = 1;` 漏检）。

## 不动的边界

- `error_`（首错）**留 CodeGen**：它是 pass 级状态。将来一次 pass 编译多模块时，要的是「整个 pass 的第一个错」停住所有模块，而非每模块各持一个错。放进 `ModuleCtx` 会与该将来冲突。
- `gc_`（共享引用）**留 CodeGen**：跨编译/运行共享，非模块状态。
- `ctx_stack_`/`ctx_`/`is_entry_`：`ctx_` 迁入 `ModuleCtx`；`ctx_stack_`、`is_entry_` 删除。

## 设计

### ModuleCtx（新：`src/compile/ModuleCtx.hpp` / `.cpp`）

```cpp
namespace aria {
    class ObjModule;
    class ObjString;
    class FunctionContext;

    // 模块编译上下文：收口每模块状态 -- 模块句柄 + 顶层全局名注册表 + 当前函数上下文指针。
    // 与 FunctionContext（每函数）对齐：模块 > 函数 > 作用域 三层各一席。
    // 只负责「登记」：declare_global 做顶层全局名重定义检查（intern 指针判等）。
    class ModuleCtx {
    public:
        ModuleCtx() = default;            // CodeGen 值成员空态（module_=nullptr, ctx_=nullptr）
        void reset(ObjModule& m);         // 每次 compile() 复用前：module_=&m, defined_globals_.clear(), ctx_=nullptr

        ObjModule* module() const noexcept;

        // 登记顶层全局名（intern 指针判等）：true=新登记；false=已存在（调用方 fail RedefinedVariable）。
        bool declare_global(ObjString* name);

        FunctionContext* ctx_ = nullptr;  // 当前函数上下文；nullptr=尚未开始；ctx_->enclosing_==nullptr=entry

    private:
        ObjModule*          module_ = nullptr;
        HashSet<ObjString*> defined_globals_; // intern -> 同名同指针，免 String 拷贝
    };
}
```

- `declare_global`：`if (defined_globals_.contains(name)) return false; defined_globals_.insert(name); return true;`
- `reset`：`module_ = &m; defined_globals_.clear(); ctx_ = nullptr;`
- `module()`：`return module_;`

### FunctionContext 简化（改 `src/compile/FunctionContext.hpp`/`.cpp`）

- **删 `is_entry_` 成员**与构造参数。
- 入口构造改为 `FunctionContext(ObjFunction& fn)`（`enclosing_{nullptr}`）；嵌套构造 `FunctionContext(FunctionContext& enclosing, ObjFunction& fn)` 不变。
- entry 判定由调用方用 `ctx_->enclosing_ == nullptr` 替代 `ctx_->is_entry_`。
- 更新头注释：去掉 `is_entry_` 描述；`enclosing_` 链说明改为「所有权由调用方局部 UPtr 持有，父函数编译期长于子函数，故 enclosing_ 裸指针在子生命期内稳定（不再依赖 vector 固定地址）」。

### CodeGen 改动（`src/compile/CodeGen.hpp`/`.cpp`）

**成员**：
- 删 `ObjModule* module_`、`HashSet<String> defined_globals_`、`List<UPtr<FunctionContext>> ctx_stack_`、`FunctionContext* ctx_`。
- 增 `ModuleCtx mod_ctx_;`。
- 增访问辅助（减少 `ctx_->` → `mod_ctx_.ctx_->` 的满屏改动）：
  ```cpp
  FunctionContext*& ctx() noexcept { return mod_ctx_.ctx_; } // 读 ctx()->... / 写 ctx() = ...
  ```
  → `ctx_->` 全部 sed 替换为 `ctx()->`（读），`ctx_ = ...` 替换为 `ctx() = ...`（写，仅 3 处）。

**`compile()`**：入口上下文改由**局部 UPtr** 持有（不再入 ctx_stack_）：
```cpp
mod_ctx_.reset(module);
auto entry_ctx = std::make_unique<FunctionContext>(*entry);  // enclosing_=nullptr = entry
ctx() = entry_ctx.get();
// ... 遍历声明、隐式 return ...
ctx() = nullptr;          // （entry_ctx 在函数末析构，entry ObjFunction 归 GC/module 存活）
```
（`module_ = &module; defined_globals_.clear(); ctx_stack_.clear();` 三行删除，由 `mod_ctx_.reset(module)` 取代。）

**`compile_function()`**：子上下文改由**局部 UPtr** 持有，靠 `enclosing_` 回父：
```cpp
auto child = std::make_unique<FunctionContext>(*ctx(), *fn);  // child->enclosing_ = 当前 ctx()
ctx() = child.get();
// 形参 add_local、emit_stmt(body)、隐式 LOAD_NIL/RETURN ...
ctx() = child->enclosing_;   // = 父
// child 在函数末析构（其局部 UPtr 出作用域）
```
（原 `push_back`/`pop_back`/`back().get()` 三组删除。）

**顶层全局登记**（`visitVarDeclNode` + `compile_function` 顶层分支）：intern 一次、复用给常量池：
```cpp
// visitVarDeclNode 顶层分支
auto* name_str = new_string(gc_, id->name);          // intern
if (!mod_ctx_.declare_global(name_str)) {
    fail(ErrorCode::RedefinedVariable, id->loc(), "重复定义全局: {}", id->name);
    continue;
}
// ... init ...
const u16 name_idx = add_constant(Value::from_obj(name_str)); // 复用，免 add_name 二次 intern
emit_op(OpCode::DEF_GLOBAL, line);
emit_word(name_idx, line);
```
```cpp
// compile_function 顶层 fun 分支（name 已是 intern ObjString*）
if (!mod_ctx_.declare_global(name)) {
    fail(ErrorCode::RedefinedVariable, loc, "重复定义全局: {}", name->view());
}
emit_op(OpCode::LOAD_CONST, line);
emit_word(fn_idx, line);
const u16 name_idx = add_constant(Value::from_obj(name)); // 复用
emit_op(OpCode::DEF_GLOBAL, line);
emit_word(name_idx, line);
```
- entry 判定 `ctx_->is_entry_ && ctx_->scope_depth_ == 0` → `ctx()->enclosing_ == nullptr && ctx()->scope_depth_ == 0`（2 处：visitVarDeclNode、compile_function）。

**import 别名入表**（`visitImportStmtNode`，补漏检）：
```cpp
auto* alias_str = new_string(gc_, node->alias);
if (!mod_ctx_.declare_global(alias_str)) {
    fail(ErrorCode::RedefinedVariable, node->loc(), "重复定义全局: {}", node->alias);
    return;
}
const u16 path_idx  = add_constant(Value::from_obj(new_string(gc_, node->path)));
const u16 alias_idx = add_constant(Value::from_obj(alias_str));
emit_op(OpCode::IMPORT, line);
emit_word(path_idx, line);
emit_word(alias_idx, line);
```
（新行为：`import "x" as U; var U = 1;` 现报 `RedefinedVariable`。需加测试。）

## 登记

- `CMakeLists.txt`：`aria_core` 源列表加 `src/compile/ModuleCtx.cpp`（`ModuleCtx.hpp` 仿 `FunctionContext.hpp` 登记位置加）。
- `.claude/rules/compile.md`：
  - 新增 `compile/ModuleCtx.hpp/.cpp` 条目（职责：模块句柄 + 顶层全局名注册表 declare_global + 当前函数 ctx_ 指针；intern 指针判等；与 FunctionContext 对齐）。
  - 改 `FunctionContext` 条目：去掉 `is_entry_`，注明 entry 由 `enclosing_==nullptr` 判定。
  - 改 `CodeGen` 条目：`module_`/`defined_globals_`/`ctx_stack_`/`ctx_` → `ModuleCtx mod_ctx_`（经 `ctx()` 访问当前函数）；ctx_stack_ 去除，靠 enclosing_ 链 + 局部 UPtr 持有；import 别名入表。

## 测试（`tests/test_codegen.cpp`）

- 新增 `ErrRedefinedImportAlias`：`import "lib/u" as U; var U = 1;` → `RedefinedVariable`（新行为）。
- 现有 413 测试须全绿（含 `ErrRedefinedVariable`/`ErrRedefinedGlobalFun`/`ErrBreakInNestedFunDoesNotBindOuterLoop`，后者验证 enclosing_ 链下循环上下文仍函数隔离）。
- 重点回归：嵌套函数 / 递归 / break-continue / 前向引用全局（`resolve_name` 走 enclosing_ 链不变）。

## 验证

```sh
clang-format -i src/compile/ModuleCtx.hpp src/compile/ModuleCtx.cpp src/compile/CodeGen.hpp src/compile/CodeGen.cpp src/compile/FunctionContext.hpp src/compile/FunctionContext.cpp
clang++ -std=c++23 -I src -fsyntax-only src/compile/ModuleCtx.cpp
clang++ -std=c++23 -I src -fsyntax-only src/compile/CodeGen.cpp
cmake --build build --target aria_tests -j
ctest --test-dir build --output-on-failure
```
验收：414/414（413 + 新 import 别名测试）全绿；语法干净；clangd 无新真错（`common.hpp not used directly` 为已知误报）。

## 风险与注意

- **enclosing_ 链生命期**：父函数 `compile_function` 帧包住子函数 `compile_function` 帧，父 `FunctionContext`（局部 UPtr in 父帧 / entry UPtr in `compile`）必长于子。子回父用 `ctx() = child->enclosing_`。**实现时务必核对**：子编译体 `emit_stmt(body)` 递归期间 `ctx()` 始终指向最内层；每次 `compile_function` 退出都把 `ctx()` 还原为其入参时的父。entry_ctx 生命期贯穿整个 `compile()`。
- **`resolve_name` 不受影响**：已沿 `ctx_->enclosing_` 链查外层局部，模型一致。
- **sed 替换 `ctx_->` → `ctx()->`**：`ctx_->` 为成员名，唯一；替换后检查 `ctx_ =`（3 处）改 `ctx() =`。注意不要误伤注释里的 `ctx_stack_` 文字（先改代码后改注释）。
- **`is_entry_` 删除**：确认无其它引用（grep 已核实仅 2 处 CodeGen + FunctionContext 定义/构造）。
- **`ObjString*` 判等依赖 intern**：`new_string` 已 intern（Phase 2 InternPool），同名同指针。`declare_global` 与 `add_constant` 复用同一 intern 串。