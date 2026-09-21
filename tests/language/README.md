# tests/language —— aria 语言脚本级端到端语料

用 aria 语言本身写测试脚本（语料），端到端驱动解释器验证**语言行为**，作为 C++
GTest 白盒测试（util / value / compile / bytecode / runtime / object / memory 七模块）
之上的行为级安全网。本模块唯一的 C++ 文件是参数化 runner（`test_language_corpus.cpp`），
其余全部是 `.aria` 脚本与旁挂的 golden / 期望文件。

## 目录结构与判定规则

```
tests/language/
  README.md                  # 本文件
  test_language_corpus.cpp   # runner（唯一 C++ 文件，参数化 GTest）
  positive/                  # 应 Ok 的程序
    01_lexical/              # 词法与字面量
    02_arith_compare/        # 算术/除模/比较/== ===/短路/一元/复合赋值
    03_vars_scope/           # var 多绑定/作用域/遮蔽/模块全局
    04_control_flow/         # if/while/for/for-in/break/continue/match
    05_functions/            # 声明/递归/一等值/lambda/if 表达式/默认参数/varargs
    06_closures/             # 捕获即引用各族
    07_exceptions/           # try/catch/throw 各族
    08_builtins/             # print 格式（配 .out）/type/len/str/assert
    09_modules/              # import 各族；每用例一个子目录（main.aria + lib/）
    10_integration/          # 多特性组合的综合小程序
    11_classes/              # def 类：init/this/super/继承/静态与实例成员/bound
    12_collections/          # list/map/range 字面量、下标与切片、方法面、迭代协议
    13_strings/              # string 方法面/字节下标/码点迭代
  negative/
    compile_errors/          # compile_*.aria -> 期待 CompileError
    runtime_errors/          # runtime_*.aria -> 期待 RuntimeError（可配 .err）
```

runner 经 CMake 注入的 `ARIA_LANG_CORPUS_DIR` 递归发现全部 `.aria` 脚本，**跳过任何
路径分量名为 `lib` 的目录**（辅助模块只经 import 驱动，不单独成用例），每脚本一个
ctest 条目（相对路径 `/` 换 `_`，`ctest -N` 可读）。每个 VM 实例开 stress GC
（`set_stress(true)`，与 test_interpret 同法）——语料兼当 GC 压力安全网。

判定规则按相对路径：

| 位置 | 判定 |
| :--- | :--- |
| `positive/**` | `interpret_from_path` 须 `Ok`；脚本内以 `assert(...)` 自检（不依赖返回值/打印） |
| `positive/**` + 同名 `.out` | 另捕获 stdout，与 `.out` **逐字节精确比对**（含末尾换行） |
| `negative/compile_errors/compile_*.aria` | 须 `CompileError` |
| `negative/runtime_errors/runtime_*.aria` | 须 `RuntimeError` |
| 同名 `.err` | 捕获 stderr，`.err` **每行一个子串**，全部须出现 |

`09_modules` 每个用例一个子目录：`用例名/main.aria`（入口固定叫 main.aria）+
`用例名/lib/*.aria`（辅助模块，被 runner 跳过）。`negative/runtime_errors/` 需要
辅助模块时，`lib/` 直接与脚本同级（相对导入相对**脚本所在目录**解析）。

## 语料脚本写作规范

- 每脚本聚焦一个语义组，文件名表意（snake_case），单脚本建议 < 80 行。
- **正向脚本一律以 `assert` 收口**：期望值手算写死，不写同义反复（`assert(a == a)`
  无效）；`print` 仅用于配 `.out` 的输出格式用例。
- 头部一行中文注释说明本脚本钉住什么语义；脚本内注释用 `//` 或 `#`。
- 负向用例只收**设计上永久如此**的错误（整除零、读未声明名、非法左值等）；
  「临时未实装」的行为（见下节禁区）不写负向用例——翻转日会变红。
- `.err` 子串只取错误消息稳定正文（如 `Runtime: DivisionByZero integer division by zero`、
  `at <main> (`），**禁止含绝对路径**与随内容漂移的行号；多写不稳定的子串 = 给未来埋雷。
- `.out` golden 必须从**当前构建产物实测**生成（`./build/aria script.aria > script.out`），
  生成前人工核对输出与特性清单一致，不要盲冻结。

## interpret 的分类语义（按失败阶段）

`interpret_run` 按**失败阶段**分类，不按错误码大类：编译期失败 -> CompileError（意为
「主入口编译失败，程序从未开始执行」）；run 期浮现的一切错误 -> RuntimeError，含运行期
才抛的 `UndefinedVariable`（`LOAD/STORE_GLOBAL` miss，码已归 Runtime 类），与经 IMPORT
站点异常通道传播的**被导入模块编译期错误**（主模块已在执行、错误可被 try/catch 捕获，
「可 catch 的错误」不构成 CompileError）。因此「读未定义全局 / 未声明赋值 / 块局部出
作用域 / 被导入模块编译错」都归 `runtime_errors/`。

## 当前禁区（写新用例前必读）

1. **字符串没有排序比较**（`<`/`>`/`<=`/`>=` 仅数值）：语料不要写字符串排序。
   `+` 拼接已落地（两侧皆 String 才成立、无隐式转字符串、显式转换走 `str()`），
   故「两侧皆 String 拼接」可写正向、「String + 非 String」可写负向。
2. **模块成员访问已落地**（模块的顶层绑定即成员，`H.x` 读原值、`H.f(args)` 直调；成员
   只读，写报 `TypeMismatch`）。09_modules 可直接经 `H.x` 观察辅助模块，不再只能靠模块体
   副作用（旧的「模块全局未导出」注记已失效）。仍未实装的行为照旧不要写负向用例钉死。
3. **不钉拿不准的消息全文**：如 assert 失败消息、异常烘焙消息里的路径/行号；对
   不可迭代值 for-in 目前报 `UndefinedProperty`（降糖为 `.iter` 方法调用 miss），
   `NotIterable` 专用码已预留未接线，措辞会变——负向用例只断言 RuntimeError、
   不配 `.err`。

## 如何新增用例

1. 选层：正向放 `positive/<层>/`；负向按错误类别放 `negative/<子目录>/` 并以
   `compile_`/`runtime_` 前缀命名。文件名 snake_case 表意。
2. 正向脚本写 assert 自检；需要钉输出格式时加 `.out` golden（实测生成）。
   运行期负向需要核对消息时加 `.err`（每行一个稳定子串）。
3. 直接 `ctest --test-dir build -R <用例名片段>` 验证（发现是 PRE_TEST 模式，
   新增脚本无需重链接/重新 configure 即被 ctest 收到）。
4. 09_modules 新用例：建 `用例名/` 子目录放 `main.aria`，辅助模块放 `用例名/lib/`。

## runner 机制备忘

- 发现：`ARIA_LANG_CORPUS_DIR`（CMake 注入，见 `tests/CMakeLists.txt`）为根递归收集
  `*.aria`，跳过 `lib` 目录分量，排序注册（用例集确定）。
- `gtest_discover_tests(aria_tests DISCOVERY_MODE PRE_TEST)`：ctest 启动时枚举测试。
  语料是文件驱动参数化，新增 `.aria` 不会触发重链接，构建期枚举会漏新脚本——
  切 PRE_TEST 后无需任何额外步骤。
- 失败消息含脚本相对路径、期望类别与 golden/.err 差异摘要；`LanguageCorpusDiscovery.
  CorpusTreeIsHealthy` 钉住语料树非空、无违反命名约定的脚本、用例名不撞车。
