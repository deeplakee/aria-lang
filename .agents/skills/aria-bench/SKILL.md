---
name: aria-bench
description: aria 性能测量纪律：跑基准、做 A/B 对比、写性能报告时使用。含两层基准分工（bench/lang 进程级墙钟与跨语言对照、lexer/vm/hashtable 进程内微基准、bench/profile 指令频度画像）、Release 原样构建要求、A/B 对照方法与报告表格规范、构建旗（ARIA_USE_TAGVALUE / ARIA_USE_MIMALLOC / ARIA_OPCODE_PROFILE）。
---

# aria 性能测量纪律

> 场景：评估一次改动的性能影响、跑基准出数字、写性能报告。清单与驱动用法见
> `bench/lang/README.md`；测量纪律全文与跨语言端口纪律见 `.agents/notes/bench/lang-bench-notes.md`。
> 本文件是入口速查，两者冲突时以 notes 为准。

## 1. 先选对层

| 想知道 | 去哪 |
| :--- | :--- |
| 一条指令/一个操作的单次成本 | `bench/vm_bench.cpp`（进程内，ns/次） |
| 词法吞吐 / 端到端编译 | `bench/lexer_bench.cpp`（进程内，ms/MB/s） |
| HashTable 吞吐 | `bench/hashtable_bench.cpp`（进程内，ns/op） |
| 真实程序体感、跨语言位次 | `bench/lang/`（进程级，整程序墙钟 + 对照端口） |
| 时间花在哪类指令上（非墙钟） | `bench/profile/`（`-DARIA_OPCODE_PROFILE=ON` 探针构建） |

两层的绝对数字不可互相换算（进程内不含启动/编译，进程级含）；进程内基准约定无 LTO 配方、
`bench/lang` 一律 Release 带 LTO，**两层配方别混用**。

## 2. 构建纪律

- **量实际发布的那份二进制**：原样 Release（`-DCMAKE_BUILD_TYPE=Release`，LTO 默认开 +
  `-O3 -DNDEBUG`），不加任何缩水开关。Debug 与 Release 读数可差一个数量级，
  `build/`（Debug）的数字不能进性能结论。
- 构建目录约定：`build/rel` = lang 基准 Release；`build/tagvalue` = TagValue 双配置验证
  （`-DARIA_USE_TAGVALUE=ON`）；`build/profile` = 画像探针（`-DARIA_OPCODE_PROFILE=ON`）。
- 分配器默认 mimalloc（vendored）；`-DARIA_USE_MIMALLOC=OFF` 退回 std::malloc，
  换分配器前后的对比须固定同一配方。

## 3. A/B 对照方法

- 跑法：`python3 bench/lang/lang_bench.py --aria=build/rel/aria`；**别为看格式跑全量**，
  验证性改动用 `--filter=<片段> --trials=2` 挑两三个脚本。
- 对照：`diff` 两次构建报告末尾的 `[summary]` 固定列宽块（mean / sd / min / inner 四列）。
- 抖动判读：跨构建与机器状态有百分之几量级的抖动；
  **小于 5% 的跨构建差异不当结论**。对照的两个构建同轮交叉重跑、取稳定读数，单点差异
  不下结论。
- 跨语言**只看内部列**（总耗时混入了各运行时启动成本，不可比）；Lua 行用 `os.clock()`
  是 CPU 时间，不做亚毫秒级跨语言比较。

## 4. 结果呈报

- 呈报表**每形态一列原始绝对值（ms）+ 相邻态 delta**，不能只给百分比。
- 结论**锚定版本对照**（工作区 vs commit 号）；过程代号、跨轮比较、被噪声放大的单点
  数字一律不进结论，效应收口为范围量级。
- 测量结果是当次评审的内容：数字是 bench 的输出、跑一次就有，不落任何仓库内文件、
  不进文档；确需文字载体时放仓库外（如 `/tmp`）。

## 5. 常见坑

- 0 可能是合法读数（如计数为 0）：探针与断言不要拿 0 当「未采集」哨兵。
- 读数里含 GC（开发期即开，设计使然）；`ASSERT` 在 Release 下被 NDEBUG 编空。
- 校验量不得依赖 map 迭代序（语言面 unspecified，跨构建不可复现）。
- 改基准脚本规模常量时所有语言端口同步改，`value` 键值表逐语言比对。
