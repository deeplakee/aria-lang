# bench/lang -- aria 源文件级性能基准

`features/` 与 `workloads/` 里的每个文件都是一个**能独立运行的 aria 程序**。同目录的 `lang_bench.py`
把它们逐个当程序起进程跑(等价于手工 `./build/rel/aria <脚本>`),重复若干轮取平均,再跟同目录下同名
其它语言端口(Python / JavaScript / Lua / Java)的读数并列--量的是「直接跑一个 aria 文件要多久」,
以及 aria 在同类语言里的位次。

与 `bench/*.cpp` 那三个进程内基准的分工:那三个量解释器内部某条路径的单次成本(词法、VM 派发、
哈希表),这里量整程序的墙钟耗时。

## 快速开始

```sh
# 数字只在实际发布的那份二进制上有意义:原样的 Release 配置(LTO 默认开、-O3 -DNDEBUG)
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release
cmake --build build/rel --target aria -j

python3 bench/lang/lang_bench.py --aria=build/rel/aria        # 全部 48 个基准 × 各语言端口
python3 bench/lang/lang_bench.py --filter=workloads/sort_int  # 只跑几个(子串匹配)
python3 bench/lang/lang_bench.py --list                       # 只列清单,不跑
python3 bench/lang/lang_bench.py --help                       # 全部参数
```

全量一次要几分钟(48 个脚本 × 最多 5 门语言 × 每脚本 6 轮);只想看输出长什么样,`--filter` 加
`--trials=2` 挑两三个脚本就够了。

## 驱动用法

### 跑之前

- **aria 可执行**:默认依次找 `build/rel/aria`、`build/aria`,也可 `--aria=<路径>`;都找不到就退出
  (退出码 2)。
- **对照端口按 PATH 探测**:CPython(当前解释器)、`node`、`lua`、`javac` / `java`--缺谁不出哪一列,
  不报错;`--langs` 可只挑几门。
- **Java 端口的产物编译到 `build/bench-java/`**,每次运行编译一次。

### 参数

| 参数 | 默认 | 说明 |
| :--- | :--- | :--- |
| `--aria=PATH` | 自动探测 | aria 可执行路径 |
| `--trials=N` | 5 | 计时轮数,报平均 ± 标准差 |
| `--warmup=N` | 1 | 预热轮数,丢弃 |
| `--filter=SUBSTR` | 空 | 只跑相对路径含该子串的脚本 |
| `--langs=a,b` | python3,node,lua,java | 只跑这几门对照语言 |
| `--format=text\|md` | text | 报告格式:终端定宽表 / markdown 表 |
| `--timeout=SEC` | 600 | 单个进程的超时秒数 |
| `--list` | 关 | 只列脚本与可用端口,不跑 |

### 报告怎么读

进度打 stderr、报告打 stdout,所以 `> 文件` 存下来的是一份没有进度噪音的纯报告。报告分五节,按这个
顺序读:

- **一、速览** -- 结论先行:每门对照语言在两组语料上「aria 更快几行 / 更慢几行」与中位倍数(> 1 = 该
  语言整体比 aria 慢)。
- **二、aria 自身** -- 绝对值。**负载段**是脚本内 `clock()` 自测的纯负载,**进程总耗时**等价手工
  `./build/rel/aria xx.aria`;两者之差就是该脚本的启动+编译开销,抖动列 = 总耗时标准差 ÷ 平均。
- **三、跨语言对照** -- 只给比值(该语言负载段 ÷ aria 的同一值):> 1 = 比 aria 慢,< 1 = 比 aria 快。
  绝对 ms 与 sd 在第五节。
- **四、跨语言值一致性** -- 门禁:同一基准各语言自报的校验量必须逐位相同,对不上即判该行失败。
- **五、原始数字** -- 固定列宽的 `[summary]` 行(含 sd 与 min),`diff` 两次构建的这一块就是 A/B。

跨语言只看负载段那一列:进程总耗时把各运行时的启动成本混了进来(JVM 启动数十毫秒,CPython / Node /
Lua 也各有几十毫秒),不可比。`--format=md` 把同一份报告渲染成 markdown 表(采样与口径不变),
便于贴进文档或 PR:

```sh
# 两种格式都能直接重定向留档
python3 bench/lang/lang_bench.py --aria=build/rel/aria > /tmp/lang-bench-$(date +%F).txt
python3 bench/lang/lang_bench.py --aria=build/rel/aria --format=md > /tmp/lang-bench-$(date +%F).md
```

### 退出码

| 码 | 含义 |
| :--- | :--- |
| 0 | 全部通过(含值一致性门禁) |
| 1 | 有失败行:脚本非零退出、超时、断言不过,或跨语言校验量对不上 |
| 2 | 跑不起来:参数错,或 `--aria` 指向的可执行不存在 |

## 目录

```
bench/lang/
  lang_bench.py   驱动(起进程计时 + 值一致性门禁 + 跨语言对照表)
  features/       40 个语言功能细项,一行一条路径(下表逐个说明)
  workloads/       8 个真实负载(整程序,各语言端口齐全)
  README.md       本文件
```

- 每个基准的规模常量写在自己头部(`// 规模:`),改规模只改该处和它的端口。
- `features/import_module/` 是唯一的多文件用例(`main.aria` + `lib/helper.aria`,后者是 import 目标)。
- 对照端口 = **同目录同 stem 的旁挂文件**(`mandelbrot.aria` + `mandelbrot.py` + `.js` + `.lua` +
  `.java`),和 `tests/language` 的 `.out` / `.err` 旁挂惯例一致。驱动按 PATH 探测工具链,缺谁不出哪列。
  「对照端口」列里的 `aria-only` 表示该行只测 aria 自己(原因见下表后的说明)。

## 功能细项(features/,40 个)

一行一个路径,规模都调到负载段约 0.35-0.45 s,故列间的耗时可以直接相互比较。

| 基准 | 量什么 | 对照端口 |
| --- | --- | --- |
| `arith_f64_loop` | 浮点域算术(加/减/乘/除) | Python JS Lua Java |
| `arith_int_loop` | 整数域算术与复合赋值 | Python JS Lua Java |
| `bound_method_read` | 读方法值 obj.method(读路径现场绑定) | Python JS Lua Java |
| `call_default_params` | 默认参数的缺省垫充 | Python JS Lua |
| `call_hook` | __call__ 钩子派发 | Python Lua |
| `call_varargs` | varargs 打包 rest | Python JS Lua Java |
| `class_field_rw` | 实例字段读写 | Python JS Lua Java |
| `class_inheritance_super` | 继承 + super 调父实现 | Python JS Lua Java |
| `class_static_member` | 静态方法与静态 var | Python JS Lua Java |
| `closure_creation` | 循环内建闭包(upvalue 装箱) | Python JS Lua Java |
| `closure_upvalue` | 捕获变量的读 / 写 / 调用 | Python JS Lua Java |
| `compare_branch` | 比较算子 + if / else-if 分支 | Python JS Lua Java |
| `destructure_assign` | 解构赋值(交换式 + rest) | Python JS Lua |
| `destructure_slice` | 解构绑定 + range 切片 | Python JS Lua |
| `exception_deep_unwind` | 跨帧 unwind 到顶层 catch | Python JS Lua Java |
| `exception_hot` | throw / catch 热路径 | Python JS Lua |
| `forin_destructure` | for ([k, v] in m) 解构迭代 | Python JS Lua |
| import_module/main | 跨模块调用(IMPORT + 模块成员) | aria-only |
| `iter_protocol` | for-in 四种源(list / range / string / map) | Python JS Lua Java |
| `iterator_manual` | 显式迭代器循环(iter / has_next / next) | aria-only |
| `lambda_higher_order` | 一等函数组合子(三次闭包调用/轮) | Python JS Lua Java |
| `list_methods` | list 方法面(push / insert / find / pop / join) | Python JS Lua Java |
| `list_ops` | list 下标读写 | Python JS Lua Java |
| `list_slice` | 正 / 负 / 倒序切片 | Python JS Lua Java |
| `list_sort_reverse` | 就地 sort + reverse | Python JS Lua Java |
| `logic_short_circuit` | && / || 短路 | Python JS Lua Java |
| `loop_forms` | C 风格 for + continue / break | Python JS Lua Java |
| `map_methods` | map 方法面(含 keys / values / pairs 快照) | Python JS Lua Java |
| `map_ops` | map 建表 + has / get 查表 | Python JS Lua Java |
| `match_dispatch` | match 多路值派发 | Python JS Lua Java |
| `method_dispatch` | 实例方法调用(两段式派发) | Python JS Lua Java |
| `operator_overload` | 用户类 __add__ 钩子 | Python Lua |
| `plain_call` | 普通函数调用(进帧 / 返回下界) | Python JS Lua Java |
| `recursion_fib` | 递归调用(朴素 fib) | Python JS Lua Java |
| `scope_locals` | 局部槽读写 + 一层块作用域 | Python JS Lua Java |
| `startup_floor` | 地板行:空负载(即启动 + 编译成本) | Python JS Lua Java |
| `string_codepoint` | 码点域(chars / codepoint_at) | Python JS Lua Java |
| `string_concat_compare` | 字符串 + 拼接与四个比较算子 | Python JS Lua Java |
| `string_methods` | string 方法面(trim / split / join / replace / …) | Python JS Lua Java |
| `string_ops` | string 方法面(contains / find / split / substring) | Python JS Lua Java |

## 真实负载(workloads/,8 个)

整程序,各语言端口齐全;规模约 0.4-0.9 s。其中 `mandelbrot` / `binary_trees` / `fannkuch_redux` 是
Computer Language Benchmarks Game 的经典项(算法与规模与公开版本一致者,数字可与公开结果对照),
其余是本地定义的等价负载。

| 基准 | 量什么 | 对照端口 |
| --- | --- | --- |
| `binary_trees` | 建满二叉树再数节点(分配 + 字段访问密集) | Python JS Lua Java |
| `fannkuch_redux` | 枚举全部置换算 pancake 翻面次数 | Python JS Lua Java |
| `fasta` | LCG 造碱基序列后按字符滚动校验 | Python JS Lua Java |
| `mandelbrot` | 复平面迭代(浮点 + 嵌套循环) | Python JS Lua Java |
| `matmul_int` | 嵌套 list 的整数矩阵乘法 | Python JS Lua Java |
| `sieve` | 埃拉托斯特尼筛(三段规模) | Python JS Lua Java |
| `sort_int` | LCG 造数 -> 就地排序 -> 滚动校验和 | Python JS Lua Java |
| `word_count` | 造词表文本 -> join -> split -> map 计数 | Python JS Lua Java |

## 为什么有些行只有 aria、有些行缺某个语言

`aria-only` 与缺列的原因逐行记在 `.claude/reference/bench/lang-bench-notes.md`「跨语言覆盖差异」一节
(总览:JS / Java 没有算子重载与可调用对象,Java 没有解构与默认参数、也做不到 throw 任意值,而
`iterator_manual` / `import_module` 这类行的各语言机制根本不可对齐)。那份文档同时还写清了测量纪律
(为什么用平均、进程级计时含哪些成本、各语言计时函数的口径差异)与各行的近似写法--这些是维护基准
本身时需要知道的事,不是用这份语料的人需要读的。

## 怎么加一个基准

1. 选层:功能细项进 `features/`,整程序进 `workloads/`。写 aria 版:头部规模常量、跑完 `assert` 校验和、
   末尾两行标记(`value <键>: <整数>` 与 `bench-time: <数> ms`)。
2. 算出期望校验和(写一份 Python 参照实现跑一遍最快),把数字烘进 `assert` 与 `value` 行。
3. 写其它语言端口(能对齐的那些),`python3 bench/lang/lang_bench.py --filter=<片段> --trials=2` 跑一遍
   --值一致性检查会报出任何一处对不上。迭代期间都用子集,全量留到收尾。
4. 若某门语言连等价写法都不成立,就不写该端口,并在 notes 的覆盖差异表里登记原因。

### 脚本写作约定

一个脚本就是一份普通程序,只多两处约定:头部写规模常量,末尾打两行标记。aria 版的样子:

```aria
// 量什么(一句话);规模常量写这里,端口同步改。
var iterations = 5000000;

var t0 = clock();
...负载...
var dt = clock() - t0;

assert(acc == 109608);                                // 校验和:自己先算准
println("value checksum: " + str(acc));               // 驱动逐语言比对的门禁
println("bench-time: " + str(dt * 1000.0) + " ms");   // 驱动解析成「负载段」列
```

- 规模调到负载段约 0.4 s:太短则启动成本占比过高、抖动也大,太长则全量跑不动。
- 校验和一律取整数(浮点渲染各语言不同);同一基准各语言断出的键值必须逐位相同。
- `startup_floor` 是唯一不打印标记行的脚本(它量的就是启动本身),驱动那一行显示 `-`。
- 数值留在 i48 域内、递归不超过帧上限(256 层);除 `features/import_module/` 外都是单文件。
- Java 端口:文件名与类名不必一致(非 public 类合法),驱动按源码里的 `class` 名起进程。

端口怎么写才算「对得上」、Java 端口为什么要跑两遍、Lua 的计时函数是什么口径,见
`.claude/reference/bench/lang-bench-notes.md`。
