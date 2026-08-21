---
paths:
  - "src/error/**"
---

# error 层模块参考

错误体系总览与四条错误通道的设计见 `CLAUDE.md`「错误处理」节（跨阶段通用规则，常驻上下文）。本文件仅收口 `src/error/` 下各头文件的结构参考。

- `error/ErrorCode.hpp`：`ErrorCode`/`ErrorCategory`（普通 `enum class : u8`，各 1 字节）。`ErrorCategory` 六值：`Ok`/`Syntax`/`Semantic`/`Runtime`/`Internal`/`Resource`；`ErrorCode` 枚举值分对应五层（外加 `Ok`=0），`category_of(ErrorCode)` 映射码->大类（`ErrorCategory`），`to_string(ErrorCode)`/`to_string(ErrorCategory)` 转可读名（自由函数，enum class 无成员函数故不挂类型上）。`using ErrCode = ErrorCode;` 别名（短写）。调用方直接写 `ErrorCode::X`。
- `error/Error.hpp`：`Error`（码+`SourceLoc`+`String` 消息；两构造 `Error(ErrorCode, String={})` 与 `Error(ErrorCode, SourceLoc, String={})`，`loc_` 默认构造空态 src=nullptr 表无位置），`format()` 渲染 `path:line:col: Category: Name 消息`（空态 loc 省略位置前缀）；仅 `code()/loc()/message()/format()` 四个接口，`loc()` 返 `const SourceLoc&`，分类/名称经 `code()` 再以 `to_string`/`category_of` 取）。`using src::SourceFile/SourceLoc/SourceSpan;` 把 `aria::src` 位置类型 re-export 进 `aria` 便于本层用。`fatal_error()`（`[[noreturn]]`，打印到 stderr 后 `std::exit(1)`）三重载：`(const Error&)`/`(ErrorCode, String={})`/`(ErrorCode, SourceLoc, String={})`；`errorf<Args...>(ErrorCode, std::format_string, Args&&...)` -> `Error` 格式化构造助手（命名取 error-format，类比 printf，避免 `Error{code, std::format(...)}` 嵌套样板）。
- `error/AriaException.hpp`：`AriaException : public std::exception`（持 `Error` + 预算的 `String what_`；三构造 `(Error)`/`(ErrorCode, String={})`/`(ErrorCode, SourceLoc, String={})`，`what_` 经私有 `make_what(error)` 预生成 `Category: Name` 串）；`error()` 取 `const Error&`、`what()` 返 `what_.c_str()`（指向内部缓冲，对象存活期有效）。派生 `AriaCompileException`/`AriaRuntimeException` 镜像同一构造集（仅类型不同，供编译期/运行期边界场景区分 catch）。

`Error` 尺寸：`String` + `SourceLoc` + `ErrorCode`，8 字节对齐；libstdc++ 下 64B（`String` 32 + `SourceLoc` 24 + `ErrorCode` 1）、libc++ 下 56B（`String` 24）；`SourceLoc` 24B = `SourceFile*` 8 + `LineCol` 16，默认构造空态 src=nullptr 表「无位置」。报错是冷路径，成功时 `Result` 的 `Error` 部分不构造，故「大」主要影响 `Result` 类型尺寸而非热路径性能。不要为压缩 `Error` 过早牺牲可读性或丢失结构化位置（source/span 支持多错误收集后统一渲染、按位置排序）；确需优化时再按收益处理（source 用 id 替指针、message 用 StringView、span 用 u32 等）。