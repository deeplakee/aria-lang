# C++ 代码风格规范

本规范对项目 C++ 代码强制生效。优先级：可读性 > 一致性 > 简洁性；不混用其它命名体系。
注释用中文，注释取舍见 [AGENTS.md](../AGENTS.md#代码注释)。

## Naming Rules

| 实体 | 形式 | 示例 |
| --- | --- | --- |
| 类型、别名、concept、模板类型参数、枚举值 | `PascalCase` | `SourceFile`、`ValueType`、`FnKind::Method` |
| 函数、变量、参数、模板非类型参数 | `snake_case` | `parse_source`、`buffer_size` |
| 非静态成员 | `snake_case_` | `scope_depth_` |
| `constexpr` / `static const` 常量 | `kPascalCase` | `kMaxSize` |
| 命名空间 | `lowercase` | `aria::utf8` |
| 宏 | `ALL_CAPS` | `ARIA_NOINLINE` |

- 布尔变量和函数使用 `is_` / `has_` / `can_` / `should_` / `was_` / `needs_` 前缀。
- 禁止 Hungarian 前缀（`m_` / `p_` 等）、保留标识符（`_Foo` / `__bar` / `foo__bar`）和宏风格常量（`MAX_SIZE`）。
- 使用 [src/type.hpp](../src/type.hpp) 的类型别名，不直接使用 `std::string` / `int` / `size_t` 等。
  数值、字符串、容器、智能指针与结果类型的映射见该头及 [core.md](../.agents/reference/core.md)。

## Variable & Parameter Names

默认使用完整单词，同一概念全库同名。参数不得用单字母，纯数量参数 `n` 除外；下标写
`index`，不写 `i`。禁止臆造截断（`mod` / `tok` / `res` / `val` / `cond`），使用
`module` / `token` / `result` / `value` / `condition`。

缩写和单字母仅允许来自下表；清单是闭集，新增条目须先更新本表。

| 名字 | 语义 | 限用范围 |
| --- | --- | --- |
| `i` / `j` / `k` | 循环计数或索引 | 局部循环变量 |
| `n` | 数量（n 个对象） | 纯数量参数或局部；索引、偏移等须用精确名称 |
| `c` | 当前扫描字符 | 逐字符扫描的局部变量 |
| `ch` | 字符 | 参数 |
| `lhs` / `rhs` | 操作数 | 参数或局部 |
| `loc` | 源位置 | `SourceLoc` 参数或局部 |
| `src` | 源文件或源码文本 | 随 `SourceFile` 的参数、局部或成员；与 `source_` 并存时须各有明确含义 |
| `cp` | 码点 | UTF-8 处理或词法扫描中的参数、局部 |
| `expr` / `stmt` | 表达式或语句节点 | AST 节点的参数、字段或局部 |
| `ctx` | 上下文 | 编译或执行上下文的参数、局部；类型名中的 `XxxCtx` 不受此表约束 |

## Parameter Passing

参数形态表达借用契约与可空性，不表达所有权；所有权只经 `UPtr` 成员或返回值出现。

| 参数类别 | 形态 | 约束 |
| --- | --- | --- |
| GC 对象（`Object` / `Obj*`） | `T*` | 统一按指针，不为个别非空参数改用引用 |
| 服务或宿主（`GC` / `AriaVM` / `SourceFile`） | `T&` | 必非空借用；成员需存指针时在成员侧取址 |
| AST 节点 | `T&` | 非空借用；语义上可缺省的子节点才用 `T*` |
| 容器分配器注入 | `Alloc*` / `GC*` | `Buffer` / `Array` / `HashTable`、`AriaExeContext` / `GC::Guard` 的分配器参数沿用指针形态 |
| 位置或槽位 | 指针 | 如 `Value*` / `u8*`，以地址身份参与寻址和写入 |
| 动态类型查询 | 指针 | `Object::is/as/try_as` 与 `equals(const Object* other)` |
| 可空或递归终止 | 指针 | 如 `FunctionCtx* enclosing_`、`ObjMovement* previous_` |
| 字符串或缓冲视图 | `StringView` / `Span<T>` | 不为传参构造拥有内容的字符串；需要 GC 字符串身份时用 `ObjString*` |
| 小值 | 按值 | `Value`、标量、`SourceLoc`、`StringView`、`Span` |
| 结果 | 返回值 | 仅向已有缓冲追加等场景允许 `T&` 入出参 |

判定时先看可空性、位置与动态类型查询，再区分 GC 对象、必非空宿主和小值。
按值参数的 `const` 只写在定义处，纯声明不写；容器循环中的元素绑定按读写需求选择。

```cpp
ObjClass(GC& gc, ObjString* name, ObjClass* super);
Result<List<Token>, Error> tokenize(SourceFile& src);
void visitBlockNode(BlockNode& node);
Opt<u8> resolve_upvalue(FunctionCtx* ctx, StringView name, SourceLoc loc);
```

## Class Initialization

- 简单聚合（字段少、无逻辑或初始化依赖）使用类内默认成员初始化。
- 带逻辑、多步或有依赖的复杂类统一在构造函数中初始化，不散落到字段声明处；无争议的
  空态（`nullptr` / `0`）可留在成员声明处。
- 构造函数初始化列表统一使用大括号：`Foo : first_{...}, second_{...} {}`。

## Variable Declarations

构造使用 `{...}`，从表达式或已有值赋值使用 `=`。

| 场景 | 写法 | 判据 |
| --- | --- | --- |
| RAII guard 或作用域标记 | `T guard{...}` | 类型表达行为契约 |
| 短名基础类型 | `usize count = expr` / `usize count{...}` | 显式类型便于阅读 |
| 普通或类型名很长的数据对象 | `auto value = T{...}` | 名字在前，构造类型在右 |
| 工厂或推导结果 | `auto value = expr` | 类型由表达式决定 |
| `static_cast` / `reinterpret_cast` 等转换结果 | `auto value = static_cast<T>(expr)` | 转换已写明类型，即使 T 是基础类型也不重复 |
| 公共 API 或接口边界 | `T value{...}` | 类型是契约的一部分 |

只有大括号触发 `initializer_list` 歧义或窄化时才使用小括号，例如 `String(size, '\0')`。
显式构造使用 `auto value = T{...}`，不省略右侧类型。

## Optional/Result 用法

`Opt<T>` / `Result<T,E>` 的判断、取值与移动按语境统一：

| 语境 | 规则 | 示例 |
| --- | --- | --- |
| `if` / `while` / 三元条件 / 逻辑运算 | 隐式 bool，不写 `has_value()` | `if (!loaded)` |
| 已检查后取值 | `*` / `->`，禁止 `.value()` | `run(*compiled)`、`result->as_int()` |
| 终局移出持堆资源的载荷 | move 出值或错误 | `std::move(*loaded)`、`std::move(compiled).error()` |
| bool 作为结果产出或宏实参 | 显式 `has_value()` | `return pending_error_.has_value();`、`ASSERT_TRUE(parsed.has_value())` |

- `.value()` 会抛 C++ 异常，不用于解释器的受检访问路径。
- `Value` / 标量等平凡可拷贝载荷不 move；仅读错误字段时也不 move。
- `.error()` 透传给 `std::unexpected` 或装箱后不再使用，且错误持有资源时，须 move。
- 嵌套解引用影响可读性时可先命名中间值；指针载荷直接复制，不为解引用添加无意义的 move。
- `std::error_code::value()` 等标准类型自身接口不在禁止范围内。

```cpp
if (!compiled) {
    return std::unexpected(std::move(compiled).error());
}
run(*compiled);
```

## Nodiscard 与结果丢弃

| 语境 | 写法 |
| --- | --- |
| 丢弃 `[[nodiscard]]` 函数的返回值 | `std::ignore = f(...)` |
| 丢弃未标 `[[nodiscard]]` 函数的返回值 | 裸调用，不加转换或 `std::ignore` |
| 未用参数 | 签名照常保留类型和名称，不加压制语句 |
| 未用变量 | 允许 `(void) variable;` |

使用 `std::ignore` 的 TU 显式包含 `<tuple>`。禁止 `(void) f(...)`，避免将结果丢弃与
未用变量压制混为一谈。`AriaExeContext::fail` 的惯用出口仍是 `return ctx.fail(...)`；
确需丢弃失败信号时写 `std::ignore = ctx.fail(...)`。

裸丢弃 `[[nodiscard]]` 的诊断须以编译器警告核对；仅搜索 `(void)` 找不全违规站点。
