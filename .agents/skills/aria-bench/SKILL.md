---
name: aria-bench
description: aria 性能测量纪律：跑基准、做 A/B 对比、写性能报告时使用。含三层分工（bench/lang 进程级墙钟与跨语言对照、lexer/vm/hashtable 进程内微基准、bench/profile 指令频度画像）、各自的构建配方与目录、A/B 对照方法与报告规范、构建旗（ARIA_USE_TAGVALUE / ARIA_USE_MIMALLOC / ARIA_OPCODE_PROFILE）。
---

# aria 性能测量纪律

> 场景：评估一次改动的性能影响、跑基准出数字、写性能报告。清单与驱动用法见 `bench/lang/README.md`；
> 测量纪律与端口纪律全文见 `.agents/notes/bench/lang.md`（冲突时以 notes 为准）。

## 1. 先选对层

| 想知道 | 去哪 | 配方 |
| :--- | :--- | :--- |
| 一条指令/一个操作的单次成本 | `bench/vm_bench.cpp` | 进程内，`build/micro` |
| 词法吞吐 / 端到端编译 | `bench/lexer_bench.cpp` | 进程内，`build/micro` |
| HashTable 吞吐 | `bench/hashtable_bench.cpp` | 进程内，`build/micro` |
| 真实程序体感、跨语言位次 | `bench/lang/` + `lang_bench.py` | 进程级，`build/rel` |
| 时间花在哪类指令上（非墙钟） | `bench/profile/` | 探针构建，`build/profile` |

**两层的配方互斥，别混用**：进程内是 `-O2 -DNDEBUG` + **关 LTO**（复现存档数字的基线），lang 是原样 Release（**LTO 开**）。两处绝对数字不可换算（进程内不含启动/编译）。

## 2. 构建纪律

- **lang 层量的是实际发布的那份二进制**：原样 `-DCMAKE_BUILD_TYPE=Release`（LTO 默认开、`-O3 -DNDEBUG`），不加缩水开关。Debug 与 Release 可差一个数量级，`build/`（Debug）的数字不能进性能结论。
- 目录各管一事：`build/rel` = lang Release；`build/micro` = 进程内（`-DARIA_ENABLE_LTO=OFF -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"`）；`build/profile` = 画像探针（`-DARIA_OPCODE_PROFILE=ON`）。
- **在同一目录反复重配要小心缓存**：CMake 缓存钉住的旧 `-D` 值会让「新配置」臂实为旧产物（特征：两臂读数逐毫秒一致）。跨配置对照前看 `CMakeFiles/<target>.dir/flags.make` 实证旗标。
- 分配器默认 mimalloc（vendored）；`-DARIA_USE_MIMALLOC=OFF` 退回 `std::malloc`，换分配器前后的对比须固定同一配方。
- `bench/profile` 的 `--static` 模式（静态发射计数）要另配 `-DARIA_DEBUG_PRINT_CODE=ON` 的构建，不是 `ARIA_OPCODE_PROFILE`。

## 3. A/B 对照方法

- 跑法：`python3 bench/lang/lang_bench.py --aria=build/rel/aria`。**别为看格式跑全量**（每脚本 6 轮 × 5 语言，几分钟），验证性改动用 `--filter=<片段> --trials=2`。
- 对照：`diff` 两次构建报告末尾的 `[summary]` 固定列宽块（mean / sd / min / inner 四列）。
- 抖动判读：跨构建与机器状态有百分之几量级的抖动，**小于 5% 的跨构建差异不当结论**；两个构建同轮交叉重跑、取稳定读数，单点差异不下结论。
- 跨语言**只看内部列**（总耗时混入各运行时启动成本）；Lua 行用 `os.clock()` 是 CPU 时间，不做亚毫秒级跨语言比较。

## 4. 结果呈报

- 呈报表**每形态一列原始绝对值（ms）+ 相邻态 delta**，不能只给百分比。
- 结论**锚定版本对照**（工作区 vs commit 号）；过程代号、跨轮比较、被噪声放大的单点数字一律不进结论，效应收口为范围量级。
- 测量数字**不入库、不进文档**：需要文字载体时用 `local-` 前缀的 markdown（已被 `.gitignore` 忽略）或 `/tmp`。

## 5. 常见坑

- 0 可能是合法读数（如计数为 0）：探针与断言不要拿 0 当「未采集」哨兵。
- 读数里含 GC（开发期即开，设计使然）；`ASSERT` 在 Release 下被 NDEBUG 编空。
- 校验量不得依赖 map 迭代序（语言面 unspecified，跨构建不可复现）。
- 改基准脚本规模常量时所有语言端口同步改，`value` 键值表逐语言比对；`startup_floor` 是唯一不打印 `value` 行的脚本。
- 某语言的 `value` 行为空时驱动是**跳过该语言**而非报错，新增端口后手工核对一次。
