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