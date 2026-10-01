---
name: aria-error
description: aria 解释器 error 层模块参考：ErrorCode/ErrorCategory、Error（静态工厂构造面）、AriaException、fatal_error。读写 src/error/** 或涉及四条错误通道（Result 返回/VM 自管异常/AriaException 边界/fatal_error）、错误消息烘焙时使用。报错文案的语言与句式家族见 `.claude/reference/error-message-style.md`；通道总览见 AGENTS.md「错误处理」节。
paths:
  - "src/error/**"
---

# error 层模块参考

> **文案**（语言 / 通则 / 句式家族）见 `.claude/reference/error-message-style.md`，改任何报错点措辞前先读那份规范。各 API 的构造与烘焙语义以代码头注为准（`Error.hpp` 类头注与工厂注、`ErrorCode.hpp` 注册表头注已逐条收口），本文件只述通道归属与跨文件设计边界。

## 四条错误通道（原则常驻 `AGENTS.md`「错误处理」节，此处收口实例）

- ① **`Result<T, Error>` 返回**（编译期通道 + VM 边界返回类型）：Lexer / Parser 恢复式收集，CodeGen / Compiler 首错即止；各 `Result` 签名见各头文件。`AriaVM::run()` 的 `Result` 仅为未捕获出口的边界返回类型，运行期错误不以 Result 逐站传播。
- ② **VM 自管异常（运行期主通道）**：错误实体为 `ObjException`，存当前执行上下文挂起错误寄存器 `Movement::pending_error_`；装箱点 `AriaVM::raise(code, detail)` 一步烘齐（`vm.fail`/`call_value` 族 bool 契约共用），消息**不含位置前缀**--位置由未捕获出口的 at 跟踪行给出。`Error` 仅在 `dispatch_loop` 未捕获出口经 `AriaVM::take_uncaught_error` 反提拆件 + `from_baked` 物化构造。接 unwind（`THROW` + CodeUnit 异常记录表）：落地状态见 `.claude/rules/runtime.md`，设计见 vm-design.md §4.5。
- ③ **`AriaException`**（C++ 异常）：仅跨 C++ 调用栈边界（Parser/CodeGen 深层 `fail()`）。
- ④ **`fatal_error()`**：Internal/Resource 不可恢复（生产调用点仅 OOM 一族）。

## `error/ErrorCode.hpp`

全量错误码注册表（X-Macro `ARIA_ERROR_LIST` 单一事实源，加一行 `X(名字, 大类)` 即收口）；注册表机制、五层分层与查表函数见该头头注。跨文件事实两条：

- `kCategoryNames` 的哨兵 `static_assert` **只拦表长漂移**：枚举增删而表未跟会炸；行序错位不炸断言，靠增删时自查。
- 死面登记（grep 实测零引用，2026-10-01）：`ErrorCode::Ok`（仅测试哨兵消费）、`NotIterable`、`CircularImport`。

## `error/Error.hpp`

构造面 = 私有 raw 构造 + 静态工厂（`from_detail` ×2 / `from_baked` / `make_message` ×2）+ `fatal_error` ×2，逐条语义见 `Error.hpp` 头注。跨文件事实两条：

- **`from_baked` 的全部合法调用点**（防双重前缀不变式的完整名单）：`ObjException::to_error()`、`AriaVM::unwind` 未捕获出口、`AriaVM::run_closure` 进帧失败分支。
- **`Error` 不保留结构化位置**：位置在构造期烘进 `message_` 串即丢弃；解释器无需多错误按位置排序/去重，结构化位置只会徒增复杂度与生命期约束。尺寸（`String` + `ErrorCode`）随之不再是关注点：运行期错误不走返回类型，`Result` 里的 `Error` 只出现在编译期通道与未捕获出口，两条都是冷路径。

## `error/AriaException.hpp`

纯 Error 载体，不挂 `std::exception` 协议面；唯一公开构造收成品 `Error`（镜像工厂会随 `Error` 构造面漂移），构造面见头注。`AriaCompileException`/`AriaRuntimeException` 仅类型标签、不校验所持 Error 的类别；**`AriaRuntimeException` 现无使用点**（grep 实测，2026-10-01）。
