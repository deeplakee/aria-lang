---
paths:
  - "src/error/**"
---

# error 层模块参考

错误体系总览与四条错误通道的设计见 `CLAUDE.md`「错误处理」节（跨阶段通用规则，常驻上下文）。本文件仅收口 `src/error/` 下各头文件的结构参考。

- `error/ErrorCode.hpp`：`ErrorCode`/`ErrorCategory`（普通 `enum class : u8`，各 1 字节）。`ErrorCategory` 六值：`Ok`/`Syntax`/`Semantic`/`Runtime`/`Internal`/`Resource`；`ErrorCode` 枚举值分对应五层（外加 `Ok`=0），`category_of(ErrorCode)` 映射码->大类（`ErrorCategory`），`to_string(ErrorCode)`/`to_string(ErrorCategory)` 转可读名（自由函数，enum class 无成员函数故不挂类型上）。`using ErrCode = ErrorCode;` 别名（短写）。调用方直接写 `ErrorCode::X`。
- `error/Error.hpp`：`Error`（仅 `ErrorCode code_` + `String message_` 两字段；**不持 `SourceFile*` 裸指针**--`message_` 在构造期一次性烘焙成型为完整可读串，构造后与 `SourceFile` 完全解耦，无悬空风险、可任意拷贝移动跨流程传递）。两构造 `Error(ErrorCode, String detail={})`（无位置）与 `Error(ErrorCode, SourceLoc, String detail={})`（带位置）；构造期经私有 `make_message` 把 `SourceLoc` 的 `"path:line:col"`、分类名、码名、细节拼成 `message_`：带位置为 `"path:line:col: Category: Name[ detail]"`、无位置为 `"Category: Name[ detail]"`（空态 loc 按 `source()==nullptr` 判无位置、细节空则无尾）。接口仅 `code()`/`message()` 两个：`code()` 取机器标识（分类/名称经此再以 `to_string`/`category_of` 取），`message()` 返 `const String&`（构造期烘焙的完整串，直接用于显示，无 `format()`/`location()`）。`using src::SourceFile/SourceLoc/SourceSpan;` 把 `aria::src` 位置类型 re-export 进 `aria` 便于本层用（`SourceLoc` 仅作 `Error` 构造参数类型，`Error` 不长期持有）。`fatal_error()`（`[[noreturn]]`，打印 stderr 后 `std::exit(1)`）三重载：`(const Error&)`（打印 `error.message()`）/`(ErrorCode, String={})`/`(ErrorCode, SourceLoc, String={})`；`errorf<Args...>(ErrorCode, std::format_string, Args&&...)` -> `Error` 格式化构造助手（命名取 error-format，类比 printf，避免 `Error{code, std::format(...)}` 嵌套样板）。
- `error/AriaException.hpp`：`AriaException : public std::exception`（持 `Error` + 预算的 `String what_`；三构造 `(Error)`/`(ErrorCode, String={})`/`(ErrorCode, SourceLoc, String={})`，`what_` 经私有 `make_what(error)` 预生成 `Category: Name` 串）；`error()` 取 `const Error&`、`what()` 返 `what_.c_str()`（指向内部缓冲，对象存活期有效）。派生 `AriaCompileException`/`AriaRuntimeException` 镜像同一构造集（仅类型不同，供编译期/运行期边界场景区分 catch）。

`Error` 尺寸：`String`（`message_`）+ `ErrorCode`，8 字节对齐；libstdc++ 下 40B（`String` 32 + `ErrorCode` 1，对齐填充）、libc++ 下 32B（`String` 24 + `ErrorCode` 1，对齐填充）。报错是冷路径，成功时 `Result` 的 `Error` 部分不构造，故「大」主要影响 `Result` 类型尺寸而非热路径性能。**不保留结构化位置**：`Error` 不存 `SourceLoc`/`LineCol`，位置在构造期烘进 `message_` 串即丢弃；解释器无需多错误按位置排序/去重等能力，结构化位置只会徒增复杂度与生命期约束（先前「保留结构化位置供排序」属前期设计，已弃）。