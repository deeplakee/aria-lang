# C++ Naming Convention (Recommended)

## Core Principle

> **Types use PascalCase. Everything else uses snake_case.**
>
> 这是最容易被人和 AI 同时遵守的规则。

------

## Naming Rules

| Entity                                            | Style                      | Example                                         |
| ------------------------------------------------- | -------------------------- | ----------------------------------------------- |
| Class / Struct / Enum Type / Concept / Type Alias | `PascalCase`               | `ThreadPool`, `StringView`, `Color`, `Sortable` |
| Function / Method                                 | `snake_case`               | `calculate_total()`, `load_file()`              |
| Local Variable                                    | `snake_case`               | `file_name`, `item_count`                       |
| Function Parameter                                | `snake_case`               | `max_size`, `timeout_ms`                        |
| Global Variable                                   | `snake_case`               | `default_config`                                |
| Static Variable                                   | `snake_case`               | `instance_count`                                |
| Non-static Member Variable                        | `snake_case_`              | `name_`, `buffer_size_`                         |
| Constant (`constexpr`, static const)              | `kPascalCase`              | `kMaxSize`, `kDefaultTimeout`                   |
| Enum Value (`enum class`)                         | `PascalCase`               | `Color::Red`, `State::Running`                  |
| Namespace                                         | `lowercase` / `snake_case` | `utils`, `network_io`                           |
| Template Type Parameter                           | `PascalCase`               | `T`, `ValueType`, `Allocator`                   |
| Template Non-type Parameter                       | `snake_case`               | `buffer_size`                                   |
| Macro                                             | `ALL_CAPS`                 | `LOG_ERROR`, `PROJECT_VERSION`                  |

------

# Examples

## Types

```cpp
class HttpClient;
struct Point3D;

enum class FileState {
    Ready,
    Loading,
    Failed
};

using TimePoint = std::chrono::system_clock::time_point;

template<typename T>
concept Sortable = true;
```

------

## Functions

```cpp
void process_file();

int calculate_total_price();

std::string get_file_name();
```

------

## Variables

```cpp
int retry_count = 3;

std::string file_name;

auto current_time = std::chrono::steady_clock::now();
```

------

## Member Variables

```cpp
class Buffer {
public:
    bool is_full() const {
        return data_.size() >= capacity_;
    }

private:
    std::vector<char> data_;
    size_t capacity_;
};
```

------

## Constants

```cpp
constexpr int kMaxConnections = 128;

constexpr std::chrono::seconds kDefaultTimeout{30};

static constexpr double kPi = 3.141592653589793;
```

------

## Boolean Naming

布尔变量和布尔函数应体现状态或能力。

```cpp
bool is_empty;

bool is_valid;

bool has_permission;

bool can_retry();

bool should_reload();
```

推荐前缀：

```text
is_
has_
can_
should_
was_
needs_
```

------

## Template Parameters

```cpp
template<typename T>
class Vector;

template<typename ValueType, typename Allocator>
class HashMap;

template<typename T, size_t buffer_size>
class RingBuffer;
```

------

# Parameter Passing

> **参数形态表达「借用契约与可空性」，不表达所有权。**
> 所有权只在 `UPtr` 成员/返回中出现；函数参数永远是非拥有借用。

| 参数类别 | 形态 | 说明与例子 |
| --- | --- | --- |
| GC 对象（object 层 `Object`/`Obj*`） | 恒 `T*` | 族级约定：成员/`trace`/`is`/`as` 全指针化，不为个别非空参数引入 `&`（`ObjClass(GC&, ObjString* name, ObjClass* super)`，`super` 可空） |
| 服务/宿主（`GC`/`AriaVM`/`SourceFile` 等长寿命非 GC 宿主） | `T&` | 借用期必非空；成员需要指针存法时在**成员侧**取址解决，参数仍传 `T&`（`Lexer::tokenize(SourceFile& src)`，成员 `source_` 存 `&src`） |
| AST 节点 | `T&` | 非空借用：visitor 双分派 `accept(AstVisitor&)` / `visitXxxNode(XxxNode&)`；仅语义上可缺省的子节点入参才 `T*`（如 for 的可省 init/condition） |
| 容器分配器注入 | `T*` | 豁免：`Buffer`/`Array`/`HashTable` 的 `Alloc*`、`Movement`/`GC::Guard` 的 `GC*`，与 `TrivialAllocator` concept 的 `A*` 形态绑定 |
| 位置/槽位 | `T*` | `Value*` 槽位、`u8*` 内存块：以地址身份参与（算术/写穿/身份比较） |
| dyn_cast 查询家族 | `T*` | `Object::is/as/try_as`、`equals(const Object* other)`，与 `dynamic_cast` 惯例对齐恒指针 |
| 递归链/重绑 | `T*` | `nullptr` 是合法状态或递归终止（`FunctionCtx* enclosing_`、`Movement* previous_`） |
| 字符串/缓冲 | `StringView`/`Span<T>` | 不建串；要 GC 串对象走工厂 `StringView` 重载，`ObjString*` 只表示已持有对象的身份 |
| 小值 | 按值，`const` 写在定义处 | `Value`、`i8..i64`/`u8..u64`、`f32`/`f64`、`bool`、`SourceLoc`、`StringView`、`Span`；range-for 与容器内联回调 lambda 的元素绑定不属参数规则 |
| 出参 | 无 | 返回值优先（`Result`/`Opt`/聚合）；仅「往已有缓冲追加」类允许 `T&` 入出参（如 `String& out`） |

判定顺序：所有权？→ 不许（`UPtr` 只在成员/返回）。可空 / 位置 / dyn_cast 查询？→ 指针。object 层 GC 对象？→ 指针。其余必非空借用 → 引用；小值 → 按值（`const` 写在**定义**处--.cpp 实现或头内 inline 定义体，纯声明不写，const 是「不改参」契约）。

```cpp
ObjClass(GC& gc, ObjString* name, ObjClass* super);                          // 服务 &，GC 对象 *
Result<List<Token>, List<Error>> tokenize(SourceFile& src);                  // 宿主借用 &（成员空态存 &src）
void visitBlockNode(BlockNode& node) override;                               // AST 非空借用
Opt<u8> resolve_upvalue(FunctionCtx* ctx, StringView name, SourceLoc loc);   // 可空递归链 + 小值按值
void emit_expr(ExprNode& n);                                                 // AST 借用
```

------

# Naming Priorities

当规则冲突时：

```text
可读性 > 一致性 > 简洁性
```

例如：

```cpp
get_user_profile()
```

优于：

```cpp
get_usr_prof()
```

------

# Forbidden Patterns

## Reserved Identifiers

禁止：

```cpp
_Foo
__bar
foo__bar
```

这些名称可能与编译器或标准库冲突。

------

## Hungarian Notation

禁止：

```cpp
m_name
m_count

str_name
i_count
p_buffer
```

改为：

```cpp
name_
count_

user_name
buffer
```

------

## Macro-style Constants

禁止：

```cpp
const int MAX_SIZE = 1024;
```

改为：

```cpp
constexpr int kMaxSize = 1024;
```

------

# AI Agent Guidelines

当 AI 自动生成 C++ 代码时，默认遵循：

```text
Type              -> PascalCase
Function          -> snake_case
Variable          -> snake_case
Member Variable   -> snake_case_
Constant          -> kPascalCase
Enum Value        -> PascalCase
Namespace         -> lowercase
Macro             -> ALL_CAPS
Boolean           -> is_/has_/can_/should_
```

**不要混用 Google 风格、LLVM 风格和 Unreal 风格。**
整个项目只采用这一套命名规则，并保持一致。