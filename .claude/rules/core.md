---
paths:
  - "src/common.hpp"
  - "src/type.hpp"
  - "src/sys.hpp"
  - "src/main.cpp"
  - "src/interpreter.hpp"
---

# 顶层公共头模块参考

- `common.hpp`：项目级公共头（被几乎所有 .hpp/.cpp 间接 include），置于 `namespace aria`。`#define USING_NANBOXING`（当前始终开启，`value/Value.hpp` 据此选 NanBoxing/TagValue 实现）。含项目级宏：`UNREACHABLE()`（NDEBUG 下 `std::unreachable()`，否则打印 `__FILE__`/`__LINE__`/`__func__` 后 `std::abort()`）、`ASSERT(condition, message)`（NDEBUG 下空操作，否则失败打印 + abort）。`static_assert(sizeof(void*) == 8)` 钉死 64 位平台。include `<cstdio>`/`<cstdlib>`/`<cstring>`/`<utility>` + `sys.hpp` + `type.hpp`，是各模块共享的「前置依赖收口」。
- `type.hpp`：项目类型别名层（`namespace aria`），**禁直接用 `std::string`/`int`/`size_t` 等**，统一走本头别名。整数：`i8`/`u8`/`i16`/`u16`/`i32`/`u32`/`i64`/`u64`（映射 `int8_t`..`uint64_t`）、`f32`=`float`/`f64`=`double`、`isize`=`std::ptrdiff_t`/`usize`=`std::size_t`。字符串：`String`=`std::string`、`StringView`=`std::string_view`。容器：`List<T>`=`std::vector<T>`、`Vector<T,N>`=`std::array<T,N>`、`Stack<T>`=`std::stack<T>`、`HashMap<K,V>`=`std::unordered_map<K,V>`、`HashSet<T>`=`std::unordered_set<T>`、`Pair<T1,T2>`=`std::pair`、`Tuple<...>`=`std::tuple`、`Span<T>`=`std::span<T>`。智能/结果：`UPtr<T>`=`std::unique_ptr<T>`、`SPtr<T>`=`std::shared_ptr<T>`、`Result<T,E>`=`std::expected<T,E>`（项目默认错误返回通道）、`Opt<T>`=`std::optional<T>`。全部为别名（无约束/无平台分流），与 `CLAUDE.md`「类型」节一致。
- `sys.hpp`：平台检测宏（`namespace aria`，无依赖，被 `common.hpp` 最先 include）。按编译器预定义宏分流：`__linux__`+`__ANDROID__`->`SYS_ANDROID`、`__linux__` 非 Android->`SYS_LINUX`、`__APPLE__` 经 `TargetConditionals.h` 分 `SYS_IOS`/`SYS_MACOS`（未知 Apple 平台 `#error`）、`_WIN32`/`_WIN64`->`SYS_WINDOWS`、`__FreeBSD__`->`SYS_FREEBSD`，未知平台 `#error`。Windows 下额外保证 `WIN32_LEAN_AND_MEAN` 与 `NOMINMAX` 已定义（未定义则补 `#define`，避免 `<windows.h>` 拖入多余符号与 `min`/`max` 宏污染）--按 `CLAUDE.md`「代码组织」要求，含 `<windows.h>` 前本头会先就位，无需再手动加守卫。
- `main.cpp`：**正式 aria 解释器入口**。`int main(argc, argv)` 设 isocline 历史、把 `argv[1..]` 透传 `List<StringView>` 后调 `cli_dispatch`（见下 `interpreter.hpp`），传入 isocline 行读取器（`ic_readline("aria> ")` + `free`，`exit`/`quit`/EOF 结束 REPL）。本身不含分发/编译/执行逻辑，仅负责 isocline 行读取与 argv 透传。退出码 0 成功 / 1 任何错误。
- `interpreter.hpp`（header-only，属 `aria_core`）：解释器 CLI 入口分发核心，供 `main.cpp` 转调与测试驱动。`build_cli()` 建 `util::Cli`（`--repl`/`--eval`/`-e`/可选 `<file>` + 内置 `--help`/`-h`）；`cli_dispatch(Span<const StringView>, LineReader)` 按优先级 `--help > --eval > --repl > <file> > 默认 REPL`（无参即进 REPL）派发，返回退出码 0/1。`--eval`/`<file>` 走 turnkey `AriaVM::interpret`/`interpret_from_path`（错误内部渲染 stderr）；REPL 经 `repl_run` 复用单个 `<repl>` 模块逐行 `run(SourceFile&, ObjModule&)` 编译执行，模块经 `gc.make_guard` 跨行保活使顶层 `var` 经 `DEF_GLOBAL` 落 `globals_` 跨行持久，逐行错误渲染 stderr 后循环不中断（容错继续）。`LineReader = std::function<bool(String&)>`：main 用 isocline 实现，测试注入流读取器 lambda（见 `tests/runtime/test_interpreter.cpp`）。