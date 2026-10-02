# tests/language -- aria 语言脚本级端到端语料

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
    08_builtins/             # println 格式（配 .out）/type/str/assert
    09_modules/              # import 各族；每用例一个子目录（main.aria + lib/）
    10_integration/          # 多特性组合的综合小程序
    11_classes/              # def 类：init/this/super/继承/静态与实例成员/bound
    12_collections/          # list/map/range 字面量、下标与切片、方法面、迭代协议
    13_strings/              # string 方法面/字节下标/码点迭代
    14_coroutines/           # create/resume/yield/status、嵌套 resume、跨协程错误各族
  negative/
    compile_errors/          # compile_*.aria -> 期待 CompileError
    runtime_errors/          # runtime_*.aria -> 期待 RuntimeError（可配 .err）
```

runner 经 CMake 注入的 `ARIA_LANG_CORPUS_DIR` 递归发现全部 `.aria` 脚本，**跳过任何
路径分量名为 `lib` 的目录**（辅助模块只经 import 驱动，不单独成用例），每脚本一个
ctest 条目（相对路径 `/` 换 `_`，`ctest -N` 可读）。每个 VM 实例开 stress GC
（`set_stress(true)`，与 test_interpret 同法）--语料兼当 GC 压力安全网。

判定规则按相对路径：

| 位置 | 判定 |
| :--- | :--- |
| `positive/**` | `interpret_from_path` 须 `Ok`；脚本内以 `assert(...)` 自检（不依赖返回值/打印） |
| `positive/**` + 同名 `.out` | 另捕获 stdout，与 `.out` **逐字节精确比对**（含末尾换行） |
| `negative/compile_errors/compile_*.aria` | 须 `CompileError` |
| `negative/runtime_errors/runtime_*.aria` | 须 `RuntimeError` |
| 两类负向 + 同名 `.err` | 捕获 stderr，`.err` **每行一个子串**，全部须出现（编译期与运行期同规则；编译错误无堆栈跟踪行，故只钉消息正文，不钉位置串） |

`09_modules` 每个用例一个子目录：`用例名/main.aria`（入口固定叫 main.aria）+
`用例名/lib/*.aria`（辅助模块，被 runner 跳过）。`negative/runtime_errors/` 需要
辅助模块时，`lib/` 直接与脚本同级（相对导入相对**脚本所在目录**解析）。

## 语料脚本写作规范

- 每脚本聚焦一个语义组，文件名表意（snake_case），单脚本建议 < 80 行。
- **正向脚本一律以 `assert` 收口**：期望值手算写死，不写同义反复（`assert(a == a)`
  无效）；`println` 仅用于配 `.out` 的输出格式用例。
- 头部一行中文注释说明本脚本钉住什么语义；脚本内注释用 `//` 或 `#`。
- 负向用例只收**设计上永久如此**的错误（整除零、读未声明名、非法左值等）；
  「临时未实装」的行为（见下节禁区）不写负向用例--翻转日会变红。
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

1. **不要钉临时未实装行为**：`range` 方法面（现仅 `iter`）、**判等（`==`/`!=`）与下标的重载**
   （算子与调用的重载已落地，见下）、`NotIterable`/`IteratorProtocol` 专用码接线、defer
   未落地--写「期待报错」的负向用例会在翻转日变红。协程全路径已落地
   （`coroutine.create/resume/yield/status` 与跨协程错误，见 14_coroutines），无禁区。
2. **不钉拿不准的消息全文**：如 assert 失败消息、异常烘焙消息里的路径/行号；对不可迭代值
   for-in 目前报 `UndefinedProperty`（降糖为 `.iter` 方法调用 miss，非对象落原语统一文案
   `type Int does not support field access`），`NotIterable` 专用码已预留未接线，措辞会变
   --负向用例只断言 RuntimeError、不配 `.err`。

### 可正常写用例的语言特性

- **运算符重载**：用户类按 dunder 方法名定义（`+ - * / %`、四个比较、一元 `-`，拼写注册于
  `src/runtime/str_table.hpp`），实例参与运算时按名从实例 fields（可遮蔽）再类链取实现；实例取不到钩子即成员
  缺席（`<class Box> has no member '__add__'`），内置类型没实现该算子报「本类型不支持」
  （`type Map does not support '__add__'`），两者都不是数值路径的旧文案。**调用重载同款**：
  `obj(args)` 按 `__call__` 取实现（与算子同一个「取实现再调用」协议）；非对象值同码报
  `type Int does not support '__call__'`，类本身仍走实例化。
- **字符串 `+`/`*` 与四个比较算子**（`<`/`<=`/`>`/`>=`）：`+` 与比较要求两侧皆 `String`、不做
  隐式转换（显式转换走 `str()`），比较按**无符号字节序**（与 `size`/`s[i]` 同字节域，大小写敏感、
  无 collation）；`*` 按整数次数重复出新串，乘数严格 Int（F64 一律拒）、负数报 `TypeMismatch`
  不静默得空，结果经驻留池（`===` 同内容同真），见 13_strings/string_repeat。
- **list `+`/`*` 算子**（拼接/重复）：`+` 两侧皆 `List` 产浅拷新表（嵌套容器按元素共享）、源表
  不动；`*` 按整数次数重复，乘数校验同 string（严格 Int、负数报错）；`+=`/`*=` 复合赋值重绑
  新值不原地改（与 push 的就地语义相对照），见 12_collections/list_concat、list_repeat。
- **模块成员访问**：模块的顶层绑定即成员（`H.x` 读原值、`H.f(args)` 直调），成员只读、写报
  `TypeMismatch`；09_modules 直接经 `H.x` 观察辅助模块。
- **list 方法面**（push/pop/insert/remove/remove_at/clear/sort/reverse/find/contains/size/is_empty/
  join/iter）：变更方法一律就地改、返 nil；`remove` 例外（移除**全部** `==` 命中元素、返命中布尔，
  miss 走返回值不报错）；`remove_at(i)` 按位置移除返元素、负数从尾计数；`sort` 就地升序，域为全数值
  或全字符串（与比较算子同源），NaN 排最前；`find` 未命中返 nil（aria 有负下标，故「未命中返 -1」
  是 Python 式坑）、`find`/`contains`/`remove` 走 `==` 内容判定；`size`/`is_empty` 为元素数与空表谓词。
- **map 方法面**（size/is_empty/has/get/keys/values/pairs/remove/clear/iter）：**键判定一律走表内
  判等 `===`、与下标读同域**（int 1 与 f64 1.0 是不同键、可变对象作键按身份；判键方法不做 `==`
  内容相等，那是 list 的域）；`has(key)` 取 `has` 不取 `contains`（后者在 map 上「判键还是判值」
  二义）；`get(key)` 单参、未命中返 nil 不报错（下标读 `m[k]` 未命中仍报 KeyError；map 可合法存
  nil，故 `get` 的 nil 与「键存在而值为 nil」不可分，分清用 `has`）；`remove(key)` 命中 true 未命中
  false（与 list `remove` 同口径，不做 Python `pop` 式返被删值）；`clear()` 原地清空；
  `keys()`/`values()`/`pairs()` 各铸新 list 快照（与源 map 解耦，序 unspecified、与 for-in 同，
  用例不得依赖具体序；三者同槽位序，故同一次快照内 `keys()[i]`、`values()[i]` 与 `pairs()[i]`
  三元对齐）；`pairs()` 每元素是 `[k, v]` 二元 list（与 for-in 每轮产出、`iter().next()` 同一形状）。
- **内建类工厂构造**（12_collections/builtin_class_constructors、13_strings/string_constructor）：
  类调用即构造（init 为原生工厂，覆盖 call_class 预置的临时 instance 产出真值）--
  `List(...)` 实参即元素（0+ 元）、`Map()` 只收 0 元、`String(v)` 与 `str()` 同域、
  `Range(a)` / `Range(a, b)` 等价 `a...` / `a...b`、`Iterator(src)` 恰 1 元只收
  string/list/map；产物与字面量产物同表示，方法面/下标/is_a 全部可用。
- **内建类继承守卫**：内建容器五类（iterator/list/map/string/range）不可作 superclass，
  `def` 执行时报 `TypeMismatch`（MAKE_CLASS 运行期守卫；Exception 可继承、Object 亦可
  显式写出），负向见 runtime_inherit_builtin_class 与 runtime_ctor_* 三例。
- **Exception 面**（07_exceptions）：运行期错误装箱对象与 `Error(msg[, code])` 工厂产物有
  `message()`/`code()` 方法（完整烘焙消息 / 错误码数字：VM 报错 = 注册表序号，Error 码参
  收整数原样携带，非整数报 `TypeMismatch`）；裸名 `Exception` 可被子类化（实例是普通
  ObjInstance，字段自管，`type` 恒 `"Instance"`）；根类方法 `x.is_a(类)` 沿继承链判归属
  （实例/异常/内置容器/类接收者同走一路，目标位须类值非类报 `TypeMismatch`）。

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
  语料是文件驱动参数化，新增 `.aria` 不会触发重链接，构建期枚举会漏新脚本--
  切 PRE_TEST 后无需任何额外步骤。
- 失败消息含脚本相对路径、期望类别与 golden/.err 差异摘要；`LanguageCorpusDiscovery.
  CorpusTreeIsHealthy` 钉住语料树非空、无违反命名约定的脚本、用例名不撞车。
