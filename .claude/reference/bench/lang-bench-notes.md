# aria 源文件级性能基准:测量纪律与端口纪律

> 语料与清单:`bench/lang/`(每个基准量什么、怎么跑,见 `bench/lang/README.md`)
> 驱动:`bench/lang/lang_bench.py`
> 用途:改基准本身、或判断一次读数可不可信时读这篇。**数字不在这里**--跑一次驱动就有(README 里
> 有把输出落到临时文件的写法),本文只收**纪律**、**覆盖差异的理由**与**已实测排除项**。

## 1. 测量纪律

- 机器与构建:Apple M4,macOS 26.6.1;编译器随 CMake 默认(`/usr/bin/c++`,本机为 Apple clang
  21.0.0)。构建即原样的 Release 配置(`-DCMAKE_BUILD_TYPE=Release`:LTO 默认开、`-O3 -DNDEBUG`),
  全部 TU(含 isocline)都是这一套。对照语言:CPython(用 `sys.executable`,与驱动同解释器)、
  Node、Lua、OpenJDK。
- **读数里含 GC**:mark-sweep GC 在所有构建里都开(开发期即开,设计使然);`ASSERT` 则在 Release 下被
  `NDEBUG` 编译为空(`common.hpp`),即量的是没有断言开销的路径。
- **量的是实际发布的那份二进制**:这一层的数字要和其它语言的发行构建比,所以不加任何缩水开关--
  LTO / `-O3` 关掉会让每一行都慢一大截(两位数百分比,薄的派发热路径吃亏最多),那样比出来的位次没有
  意义。进程内基准(`lexer_bench` / `vm_bench`)相反,为了复现文档里的存档数字约定用无 LTO 配方,两层
  的配方别混用。跨构建对比前先固定 Debug/Release 与 LTO 两项。
- **进程级计时**:每个基准起一次子进程(等价手工 `./build/rel/aria xx.aria`),预热 1 轮丢弃后跑 5 轮,
  报**平均值 ± 标准差**并列最小值。平均是 CPython 官方的口径(pyperf 默认 mean ± stddev,并先标定到
  `--min-time`;MicroPython 的 run-perfbench 同样报平均),最小值是 CLBG / Ruby benchmark-driver 的口径,
  一并留着供对照。轮数用 `--trials` / `--warmup` 调。
- 每行两列数字:**总耗时** = 进程启动 + 词法/编译 + VM bootstrap + 负载;**内部** = 脚本用内置 `clock()`
  自测的纯负载段(经 stdout 的 `bench-time: <n> ms` 标记行报出)。两者之差即该脚本的启动+编译开销。
  **跨语言一律只看内部列**--总耗时把各运行时的启动成本混了进来(JVM 启动数十毫秒,CPython / Node /
  Lua 也各有几十毫秒),不可比。
- 抖动:同一二进制重复运行的总耗时标准差约 1%(个别行到 5%);负载段的标准差更小。跨构建另有 ±5-10%
  的差异,**小于 5% 的跨构建差异不要当结论**。
- **JVM 端口跑两遍、报第二遍**:`main` 里先跑一遍热身丢弃(触发 JIT),再计时跑第二遍。这是 JVM 测性能
  的通行做法,负载本身仍是同一份、同一规模,只是多跑一遍。Node 也有 JIT,但 V8 的分层编译在毫秒级
  完成,对这些长度的负载不构成系统性偏差,故不另跑热身。
- **Lua 无单调墙钟标准函数**,端口用 `os.clock()`(**CPU 时间**)。纯计算负载上它与本进程墙钟接近,
  但它不是墙钟--Lua 行不要与其它语言的行做亚毫秒级的比较。
- 想留一次运行的报告:`python3 bench/lang/lang_bench.py --aria=build/rel/aria > <文件>`--进度打
  stderr、报告打 stdout,故重定向得到的文件就是纯报告;报告里给人读的是前三节(速览 / aria 绝对值 /
  跨语言对照,倍数一律「该语言 ÷ aria」),拿来做 A/B 的是末尾固定列宽的 `[summary]` 块(mean / sd /
  min / inner 四列),`diff` 两次构建的该块即可。`--format=md` 把同一份数据渲染成 markdown 表
  (贴文档 / PR 用),渲染层只换格式,不动采样与口径。
- **别为看格式跑全量**:全量一次要几分钟(每脚本 6 轮 × 5 语言),验证格式/路径/解析这类改动用
  `--filter=<片段> --trials=2` 挑两三个脚本就够,全量留给真要做性能测量的时候。

## 2. 端口纪律(对照语言怎么写才算数)

- **同算法、同规模、同校验和**:端口必须与 aria 版做同一份工作、断出同一批读数,否则对比表在比两件
  不同的事。规模常量写在各脚本头部,改规模时所有语言同步改。
- **值一致性门禁**:每个脚本(含各语言端口)都要打印 `value <键>: <整数>` 行,驱动逐语言比对同一基准的
  键值表,任一键不同即判该行失败。这把「同算法」从口头承诺变成机械保证--批量写端口时它是唯一安全网
  (实测抓到过凭记忆写错的断言值)。因此**只有各语言读数一致的量才进 `value` 行**:反例是字符串的字节
  长度(UTF-8 语言与 UTF-16 语言不同),故 `string_codepoint` 只收码点域读数。
- 容器与写法按**各语言自然形态**取,不追求逐 op 同构:`sieve` 用 Python `bytearray` / JS `Uint8Array` /
  Java `boolean[]` / Lua 表 / aria `list`;排序用各语言的原地排序。下标按各语言的基址换算(Lua 1-based
  的位置换算、0-based 语言上界 +1),但断言出来的**读数必须相同**。
- 校验和一律取**整数**:浮点结果的十进制渲染各语言不同(aria `format_value` / Python `repr` /
  JS `Number` / Lua `%.14g` / Java `Double`),整数才逐位可比。浮点负载(`mandelbrot`)因此把校验和定义
  为「迭代数滚动取模」,另钉内部点个数。
- **校验量不得吃 map 迭代序**:aria 的 map 迭代序是开放寻址的槽位序且语言面 unspecified
  (`HashTable` 契约),依赖它的读数跨语言不可复现。`huffman` 的建树队列因此按「词首次出现序」取
  确定序(2026-09-28 从 freq 表迭代序改出,烘死值恰好未变);`json_codec` 的 canonical 序列化
  (键排序输出)同理。新基准写校验量时遵守同一约束。
- 端口自带断言(Python `assert` / Node 与 Java `check()` / Lua `assert`),漂移即非零退出。
- 计时函数:aria `clock()`(单调秒)、Python `time.perf_counter()`、Node `performance.now()`、
  Java `System.nanoTime()`、Lua `os.clock()`(见上)。

## 3. 跨语言覆盖差异(哪些行缺某语言,以及为什么)

| 行 | 缺哪个语言 | 原因 |
| --- | --- | --- |
| `iterator_manual` | 全部端口(aria-only) | 各语言的显式迭代写法不对齐:Python / JS 的 `next()` 以抛异常报尽,Java 的 `Iterator` 次序语义不同,故只测 aria 自身协议 |
| `import_module` | 全部端口(aria-only) | 模块机制不可对齐:Python 的 import 在进程启动期执行、Java 全在编译期,故只测 aria 的 IMPORT + 跨模块调用 |
| `operator_overload` | JS / Java | 两门语言都没有算子重载(`a + b` 恒为内置语义) |
| `call_hook` | JS / Java | 没有可调用对象:JS 里「调用」本就是函数调用(与 `plain_call` 无区别),Java 无 `__call__` |
| `exception_hot` | Java | aria / JS / Lua 可 throw **任意值**(零分配),Java 只能 throw Throwable、必须建异常对象,机制不同量级 |
| `destructure_assign` / `destructure_slice` | Java | Java 无解构绑定 |
| `forin_destructure` | Java | Java 无 `for ([k, v] of ...)` 这种解构式迭代 |
| `call_default_params` | Java | Java 无默认参数(惯用做法是重载,那是另一个构造) |

以下几行**有端口,但是「构造不存在、用本语言惯用写法近似」**(读数仍逐位一致):

| 行 | 近似写法 |
| --- | --- |
| `match_dispatch` | Lua 用 `if/elseif` 链;JS / Java 用 `switch`;Python 用 `match` -- 各用本语言的多路派发 |
| `bound_method_read` | Lua 读方法值不绑定(函数是一等值,self 显式传),与 aria 读路径现场绑定机制不同 |
| `closure_upvalue` | Java 的 lambda 只能捕获 effectively-final 局部,可变捕获用单元素数组当持有槽 |
| `string_concat_compare` | Java 无字符串比较算子,用 `compareTo`;拼接用非 final 局部以避免 javac 常量折叠 |
| `list_methods` / `list_slice` / `string_ops` / `string_methods`(Lua) | Lua 的表与字符串库没有 `find` / 切片 / `split`,这些操作只能手写循环 -- 这些行比的是**实现形态**而不是派发速度,读 Lua 列时要留意 |
| `json_codec` | Java 以 `RuntimeException` 表达解析失败(aria 可 throw 任意值,零分配);Lua 以唯一哨兵表表示 JSON null(表槽存不了 nil),空表只出现在校验段被丢弃的残缺值里 |

## 4. 已实测排除项

- **不含需要数学内建的负载**:语言面暂无 `sqrt`,故 classic 的 `nbody` / `spectral-norm` 无法表达;
  `pidigits` 需任意精度整数,与 i48 域冲突。将来若加数学内建,这三项可直接补进 `workloads/`。
- **fannkuch_redux 钉的不是公开那个 checksum**:公开的带符号约定复原不出来--用「置换奇偶」「枚举
  序号」「flips 奇偶」三种符号约定分别算 N=10 得 -78 / -292742 / -78158,都不等于公开的 73196。
  翻面语义本身确凿(`max_flips` 在 N=9 / N=10 得 30 / 38,与公开 `Pfannkuchen` 值一致),故该脚本改钉
  `max_flips`(公开可比)+ `flips_sum`(本语料自定义)+ `perm_count`(闭式)三个序无关量。
- **aria 整数是 i48**:脚本的中间量与校验和须留在 ±2^47 内(Debug 下 `Value::from_int` 有断言)。现存
  脚本的 LCG 中间积约 1.0e14,贴着上限但安全;改规模时先算一遍。
- **内建 `clock()` 返 f64 秒、单调、起点未定**(`src/runtime/builtins/Builtins.cpp`):只能相减,别当
  时间戳用。返 f64 而非常量级整数,是因为自开机起的微秒计数会越过 i48(2^47 µs ≈ 51 天)。

## 5. 与进程内基准的分工

| 基准 | 量什么 | 数字口径 |
| --- | --- | --- |
| `bench/lexer_bench.cpp` | 词法吞吐、端到端编译、位置派生、utf8 解码 | 进程内紧循环,ms / MB/s / ns 每次调用 |
| `bench/vm_bench.cpp` | 方法派发、迭代协议、调用开销、算子钩子取名路径 | 进程内编译一次 + 跑 N 轮取最小,ns/次 + 确定性分配/次 |
| `bench/hashtable_bench.cpp` | HashTable 的 set/find/erase 吞吐(好/弱哈希对照) | 进程内 ns/op |
| `bench/lang/lang_bench.py` + `bench/lang/` | **整程序**(直接跑 `.aria` 文件)与跨语言对照 | 进程级 wall time(预热 1 轮 + 5 轮平均 ± 标准差)+ 脚本内 `clock()` 自测负载段 |

改一条指令想看它的单次成本去 `vm_bench`;想看它在一个真实程序里的体感、或想看 aria 相对其它语言的
位次,来这一层。两处的绝对数字不可互相换算(进程内不含启动/编译,进程级含)。

## 6. 读表时的四条注意

跑一次驱动得到当前数字后,按这四条读(它们是历次读数里稳定成立的模式,不是某一次的结论):

1. **按「派发 vs 原语」分组看**:凡是一轮里做很多次字节码派发的行(算术循环、方法派发、类字段、异常
   热路径),aria 对 CPython 领先;凡是 CPython 用 C 原语一步做掉的行(varargs 打包、切片、`dict` / `map`
   迭代、解构绑定、字符串方法面),CPython 领先。前者说明派发侧已经占优,后者指出下一步该把哪些常用
   操作下沉成原语。
2. **分配密集的行看 `binary_trees`**:它是纯对象分配 + 字段访问,CPython 在这类负载上仍占优,故这一行
   是 GC / 对象分配的观测点。
3. **JIT 语言(Node / Java)是另一个量级**,只作参照:可被 JIT 完全消除的循环差距最大(如类字段读写),
   库实现主导的行差距最小(如 `sort_int`,aria 的原生排序反而稳定胜过 V8 的)。
4. **Lua 是最接近的解释器同侪**,多数行落在 aria 的零点几倍;但凡是 Lua 只能手写、aria 有原生实现的
   行(第 3 节末表),差距会拉到数倍到数十倍 -- 那些行比的是实现形态,不是解释器速度。
