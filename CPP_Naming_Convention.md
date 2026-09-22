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
void emit_expr(ExprNode& node);                                                 // AST 借用
```

------

# Variable & Parameter Names

> **名字承载语义：默认完整单词。** 单字母与缩写只允许来自下方豁免清单；清单是闭集，新条目先入表再用。

单字母（`n`/`m`/`b`）与臆造截断（`mod`）的问题：语义靠读者脑补、grep 不可及（搜 `n` 等于没搜）、截断可能与域内其他词相撞（`mod` 撞 `op_mod` 的 modulo）。大小写风格见「Naming Rules」表；指针/引用/按值的选择见「Parameter Passing」节。

| 规则 | 内容 |
| --- | --- |
| 参数实名 | 完整单词体现用途，零单字母：下标写 `index` 不写 `i`；纯数量参数可用 `n`（「n 个 xx」），语义更窄时写精确名（`capacity`/`new_len`/`count`） |
| 变量默认全称 | 完整单词；极小作用域的中转临时（可见范围几行内）可短，但豁免清单外的缩写仍不取 |
| 禁臆造截断 | `mod`/`tok`/`res`/`val`/`cond` 一类不取，用 `module`/`token`/`result`/`value`/`condition` |
| 同一概念全库同名 | 不一处 `node` 一处 `n`，一处 `module` 一处 `mod` |

豁免清单（领域标准缩写，仅限表内语义与范围使用）：

| 名字 | 语义 | 限用范围 |
| --- | --- | --- |
| `i`/`j`/`k` | 计数器/索引（嵌套依次取 j/k） | 局部，循环 |
| `n` | 数量（「n 个 xx」，std 惯用同款） | 纯数量语义的参数/局部（`advance(n)`/`drop(n)`）；语义不止数量（下标/字节操作数等）不适用 |
| `c` | 当前扫描字符 | 局部，仅逐字符扫描上下文（Lexer 扫描循环等） |
| `ch` | 单个字符 | 参数 |
| `lhs`/`rhs` | 二元操作数 | 参数/局部 |
| `loc` | 源位置（随 `SourceLoc` 类型短名） | `SourceLoc` 的参数/局部 |
| `cp` | UTF-8 码点 | `codepoint` 的参数/局部（utf8 层、Lexer 扫描） |
| `expr`/`stmt` | 表达式/语句 AST 节点 | AST 节点的参数/字段/局部 |
| `ctx` | 执行上下文 | `FunctionCtx`/`ModuleCtx`/`AriaVM` 等上下文的参数/局部（类型名 `XxxCtx` 不在此列） |

```cpp
// 不取
bool is_digit(const char c);        // 参数不用单字母（c 白名单仅限逐字符扫描局部）
usize p = pos_;                     // p 语义不明,应实名 saved_pos
void op_mod(AriaVM& vm, Value mod); // 截断撞 modulo

// 取
bool is_digit(const char ch);
void advance(usize n);              // 前进 n 个字符,数量语义
void op_mod(AriaVM& vm, Value rhs);
```

------

# Optional/Result 用法

> **`Opt<T>`/`Result<T,E>`（即 `std::optional`/`std::expected`）的判断、取值、移动按语境各定一式，全库一式。**
> 核心依据：`.value()` 是抛异常的访问器（`bad_optional_access`/`bad_expected_access`），解释器路径无异常语义；`*`/`->` 天然表达「此处已检查过」。

| 语境 | 规则 | 例 |
| --- | --- | --- |
| 布尔判断（`if`/`while`/三元条件/`&&`/`\|\|`/`!`） | 隐式转换，不写 `has_value()` | `if (!loaded)`、`if (rest && !check(x))` |
| 取值（检查后） | `*res` 解值 / `res->member` 取成员，禁 `.value()` | `run(*compiled)`、`out->as_int()` |
| 移出载荷 | 终局消费（返回/透传/装箱后不再用）**且**类型持堆资源才 move：`std::move(*res)` / `std::move(res).error()`；`Value`/标量等 trivially copyable 不 move | `SourceFile source = std::move(*loaded);` |
| bool 作为值产出（`return bool`、`ASSERT`/`EXPECT` 宏实参、存 bool 变量） | `has_value()` 显式 | `return pending_error_.has_value();`、`ASSERT_TRUE(r.has_value())` |

配套细则：

- 嵌套解引用过噪（`**parse` 一类）时先落局部变量再解：`auto program = std::move(*parse);`，随后 `*program`。
- `.error()` 透传给 `std::unexpected` / 装箱属终局消费，move 出：`return std::unexpected(std::move(res).error());`；仅读字段（`.message()` 等）不 move。
- `std::error_code::value()` 等 std 类型自有方法与本规则无关，不在约束范围。

```cpp
// 不取
if (!parsed.has_value()) { ... }        // 条件语境写 has_value()
run(compiled.value());                  // .value() 抛异常且不表达「已检查」
auto source = std::move(loaded.value()); // move 与取值分家用 .value()
return std::unexpected(compiled.error()); // 终局透传漏 move(Error 持 String)

// 取
if (!parsed) { ... }
run(*compiled);
auto source = std::move(*loaded);
return std::unexpected(std::move(compiled).error());
```

------

# Nodiscard 与结果丢弃

> **`[[nodiscard]]` 说的是「这个返回值必须被处置」：要值就接住，不要值就写 `std::ignore = f(...)`。**
> 被调函数没标 `[[nodiscard]]` 时，调用语句前不留 `(void)`--那里没有需要压制的诊断，转换是纯装饰。

| 语境 | 规则 | 例 |
| --- | --- | --- |
| 丢弃 `[[nodiscard]]` 函数的返回值 | `std::ignore = f(...)` | `std::ignore = define_local_or_fail(name, loc);` |
| 丢弃未标 `[[nodiscard]]` 函数的返回值 | 裸调用，不加任何转换 | `make_class(gc, "orphan");` |
| 压制未用**参数** | 照常带类型带参数名，签名不做任何压缩 | `void op_add(AriaVM& vm, Value lhs, Value rhs)`，`rhs` 未用也照写 |
| 压制未用**变量** | `(void) x;` | `(void) cp;`（结构化绑定里只用一半时） |

细则：

- `(void) f(...)` 是 C 遗留写法，且同一个 `(void)` 语法在别处承担压制未用参数/变量的职责，两种语义同形不可区分；`std::ignore` 是标准库的显式丢弃设施，不产生转换。
- `std::ignore` 定义在 `<tuple>`：使用的 TU 显式 `#include <tuple>`，不靠其它头传递引入。
- `std::ignore =` 只写在有属性约束处。给未标 `[[nodiscard]]` 的调用补 `std::ignore =`，与给它补 `(void)` 同样是噪音，两者都删。
- 裸丢弃 `[[nodiscard]]` 返回值（不写任何转换）编译器会报 `-Wunused-result`（clang 默认开启、无需 `-Wall`；gcc 侧挂在 `-Wall` 下），故清扫既有站点时以「全量重建 + grep warning」为准，只 grep `(void)` 会漏。
- `AriaVM::fail` 的惯用出口是 `return vm.fail(...)`（`FailSignal` 按调用点返回类型转 false/nullptr/nullopt）；确需不返回地调用时同样写 `std::ignore = vm.fail(...)`。

```cpp
// 不取
(void) define_local_or_fail(name, loc);    // nodiscard 返值被 (void) 压制
(void) make_class(gc, "orphan");           // 该函数未标 nodiscard，(void) 空转
gc.new_object<ObjDummy>();                 // 裸丢弃 nodiscard 返值，仅编译器警告可查
void op_add(AriaVM& vm, Value lhs, Value); // 省略参数名压制未用参数

// 取
std::ignore = define_local_or_fail(name, loc);
make_class(gc, "orphan");
std::ignore = gc.new_object<ObjDummy>();
void op_add(AriaVM& vm, Value lhs, Value rhs);
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

当 AI 自动生成 C++ 代码时，默认遵循上文「Naming Rules」表。**不要混用 Google 风格、LLVM 风格和 Unreal 风格**--整个项目只采用这一套命名规则，并保持一致。