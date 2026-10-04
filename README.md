# aria

aria 是一门自研的动态类型脚本语言，解释器从零用 C++23 实现：**单遍编译器**把源码直接编成
**栈式字节码**，交给 **AriaVM** 执行，对象生命周期由 **mark-sweep GC** 管理（开发期即开 GC）。
目标平台是 Windows / Linux / macOS。

语言的设计取向偏“小而完整”：闭包捕获即引用、不做隐式类型转换、字符串以字节为一等索引域、
模块即文件、异常只有 `try/catch`。它没有想成为第二个 Python，只是把一门脚本语言该有的东西
（函数、闭包、类、容器、迭代协议、模块、异常、协程）按同一套语义收干净。

## 看一眼 aria

```aria
def Greeter {
    init(name) { this.name = name; }
    greet(loud = false) {
        var msg = "hello, " + this.name;
        return if (loud) { msg.upper() + "!" } else { msg + "!" };
    }
}

# 闭包捕获即引用：计数器状态跨调用保留
fun make_counter() {
    var n = 0;
    return fun() { n += 1; return n; };
}
var next = make_counter();
println(next()); println(next()); println(next());       # 1 2 3

var g = Greeter("aria");
println(g.greet());                                  # hello, aria!
println(g.greet(true));                              # HELLO, ARIA!

var xs = [3, 1, 2];
xs.sort();
var [head, ...tail] = xs;
println(head); println(tail);                          # 1 [2, 3]

var ages = {"ada": 36, "bob": 41};
for ([name, age] in ages) {
    println(name + " is " + str(age));               # 迭代序未规定
}

println(match (2) { 0 => "zero", 2 => "two", _ => "many" });   # two
println("  a,b,c  ".trim().split(",").join("-"));              # a-b-c
println("42".to_int() + 1);                                    # 43
```

`if` 与 `match` 既能作语句也能作表达式；`def` 体里 `init` 是构造器、`this` 指实例、方法可带
默认参数；`[a, b] = [b, a]`、`var [h, ...t] = xs`、`for ([k, v] in map)` 三种位置共用一套解构模式。

## 语言特性

- **值与类型**：`nil` / `bool` / `int`（48 位整数，值域 `[-2^47, 2^47)`）/ `float`（`f64`）/ `string`（UTF-8）/
  `list` / `map` / `range`，以及函数、闭包、类、实例、模块、迭代器等对象类型。
  相等分两级：`==` 内容相等（数值跨 `int`/`float` 成立），`===` 严格相等。
- **函数**：一等值、`fun(x) { ... }` lambda、默认参数、`...varargs`；闭包捕获即引用，同一变量
  的多个闭包共享同一份状态（`make_counter` 那个例子）。
- **类**：`def` 定义，`init` 构造器，`super` 单继承（`def Foo : Bar`），成员分静态变量、静态方法、
  实例方法三类；实例字段是动态的；根类 `Object` 收拢链式查找。运算符与调用可重载：在类上定义
  `__add__`、`__lt__` 等十个算子钩子或 `__call__`，实例出现在左操作数 / 被调用位置时按名取实现
  （仅左侧触发，无反射形态）。内置的 list/map/string/range 也是类，方法调用走同一套
  `recv.name(args)` 通路。
- **控制流**：`if`/`else`、`while`、C 风格 `for`、`for (pattern in expr)`、`break`/`continue`、
  `match`；`if` 与 `match` 均有表达式形态。
- **解构**：`var [a, b] = ...`、`[a, b] = [b, a]`、`for ([k, v] in ...)`，支持嵌套、`_` 占位与
  `...rest` 后缀。
- **异常**：`try`/`catch`（无 `finally`），`throw` 任意值且 `catch` 绑原值；未捕获时打印逐帧堆栈跟踪。
- **协程**：`coroutine.create/resume/yield/status`（非关键字，普通成员调用）。`resume` 首启的
  载荷即协程函数实参，返回 `yield` 交出值或完成值；`yield` 可从任意调用深度发起；协程内未捕获
  错误转投恢复者（外层 `try/catch` 可接住），未捕获跟踪截断在协程边界。
- **容器与迭代协议**：list/map/range 字面量与下标（负数从尾计数），range 作下标键即切片
  （`xs[1..3]`，倒序 range 得到倒序段）；`for-in` 由统一迭代协议驱动，也可显式取迭代器。
  方法面按类型分布：list 14 个、string 17 个、map 10 个。
- **字符串**：`+` 拼接只在两侧都是 String 时成立（无隐式转换，要转就用内置 `str()`）；比较按
  无符号字节序；下标与 `size()` 都是字节域，逐码点走 `chars()`。字符串即模板：`"n = ${x}"`
  的 `${...}` 档内是完整表达式、渲染与 `str()` 同源。方法面含 `split`/`trim`/`upper`/
  `substring`/`to_int` 等。
- **模块**：一个文件就是一个模块，`import "./utils" as U;`（别名必写）。模块顶层绑定即模块成员，
  `U.f()` 直调，成员只读。
- **内置函数**：`type` / `str` / `assert` / `println` / `clock` / `Error`（都可当一等值传递；`println` 是唯一的
  输出口，`clock` 返单调时钟秒数、只可相减，`Error(message[, code])` 造异常值）。另有内建模块变量
  `coroutine`（合成 `<coroutine>` 模块，协程原语载体，第 16 章）。

## 构建与运行

需要 CMake ≥ 3.20 与支持 C++23 的编译器（`clang++` / `clang-format` / `clangd` 建议在 PATH 中）。
第三方依赖（Google Test v1.14.0 / isocline / mimalloc）均 vendored 于 `external/`，configure 零网络依赖。

```sh
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# Windows（Ninja + clang；clang++ 为 MSVC target 时链接器走本机 Visual Studio）
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug

cmake --build build --target aria -j
```

解释器 CLI：

```sh
build/aria script.aria        # 运行脚本
build/aria -e 'println(1 + 1);' # 求值一段源码
build/aria --repl             # 交互式 REPL（顶层 var 跨行保留）
build/aria --version
```

单文件语法检查须带 `-I src`，否则 `common.hpp` / `type.hpp` 找不到：

```sh
clang++ -std=c++23 -I src -fsyntax-only <file>
```

## 测试与基准

```sh
cmake --build build --target aria_tests -j
ctest --test-dir build --output-on-failure
```

测试分两层：`tests/<module>/` 下的 C++ GTest 白盒用例（按模块分目录），以及 `tests/language/`
下用 aria 自己写的**脚本语料**（正向脚本以 `assert` 收口，负向脚本钉错误码，端到端驱动解释器，
兼作 GC 压力安全网），详见 `tests/language/README.md`。

`bench/` 下有独立的性能基准。进程内微项有三个可执行（`lexer_bench` / `hashtable_bench` /
`vm_bench`，量单条路径的单次成本），另有**源文件级**基准 `bench/lang/`：里面的 `.aria` 都是能直接
运行的完整程序（40 个语言功能细项 + 16 个真实负载，负载与多数功能项带 CPython / Node / Lua / Java
对照端口），同目录的 `lang_bench.py` 逐个起进程计时，报多轮**平均值 ± 标准差**，并逐语言比对脚本自报
的校验量：

```sh
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release    # 原样 Release 即可（LTO 默认开）
cmake --build build/rel --target aria -j
python3 bench/lang/lang_bench.py --aria=build/rel/aria
```

数字只在实际发布的那份二进制上有意义：Release 默认开 LTO、带 `-O3 -DNDEBUG`，别为跑基准关掉优化
开关，Debug / Release 之差足以量级性地改变读数。清单与写法见 `bench/lang/README.md`；本次运行的数字
就是驱动的输出（要留档就重定向到文件），测量纪律与端口纪律见
`.claude/reference/bench/lang-bench-notes.md`。

值表示默认取 NaN-boxing；想验证等价的 TagValue 路径，另配一个构建目录：

```sh
cmake -S . -B build/tagvalue -DARIA_USE_TAGVALUE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tagvalue --target aria_tests -j
ctest --test-dir build/tagvalue --output-on-failure
```

## 实现

源码到执行的路径是 `Lexer` → `Parser` → `AstVisitor`（`CodeGen` 单遍产出 `CodeUnit`）→ `AriaVM` 主循环。
运行期错误与用户 `throw` 走同一条 VM 自管异常通道：错误装箱成 `ObjException` 存入当前执行上下文的
挂起寄存器，`unwind` 由内向外查字节码里的异常记录表定位 handler，全未命中才物化 `Error` 并附带
堆栈跟踪。协程走单循环切换模型：`resume`/`yield` 是原生函数、在 CALL 善后点换执行上下文，主循环
永不重入；协程内未捕获错误沿 resume 链逐跳转投恢复者。

```
src/
  main.cpp / interpreter.hpp / interpreter.cpp   CLI 入口与分发
  util/      fs / utf8 / source_file / io / util / cli 工具（多为 header-only）
  value/     Value（NaN-boxing，可切 TagValue）/ AriaArray / AriaHashTable
  error/     ErrorCode / Error / AriaException
  compile/   Token / Lexer / ast / Parser / AstVisitor / FunctionCtx / ModuleCtx / CodeGen / Compiler
  bytecode/  code.hpp（OpCode）/ CodeUnit / Disassembler
  object/    Object 及 Obj* 子类型（string / function / native / module / exception /
             closure / upvalue / class / instance / bound-method / list / map / range / iterator）
  memory/    Buffer / Array / Allocator / HashTable / InternPool / GC
  runtime/   FrameStack / ObjMovement / AriaVM / value_register；builtins/ 内建函数与各类型方法面
tests/       C++ GTest（tests/<module>/）与 aria 脚本语料（tests/language/）
bench/       性能基准（lexer/vm/hashtable 三个进程内可执行 + lang/ 源文件级基准与对照端口）
external/    isocline（REPL）+ googletest v1.14.0 + mimalloc（GC 分配器），均 vendored、configure 零网络
docs/        grammar.txt（语言文法）与 guide/（语言教程）
tools/       check_commit_msg.py 等开发脚本
```

## 进一步阅读

- `docs/guide/` -- 语言教程（面向有编程经验读者，17 章入门到全景，含内建速查附录）。
- `docs/grammar.txt` -- 语言文法规范（语法面单一事实源；语义设计见 `.claude/reference/`）。
- `AGENTS.md` -- 项目规则与进度，以及构建 / 命名 / 类型 / 错误处理等通用约定。
- `.claude/rules/` -- 按源码目录拆分的模块参考（带 `paths:` frontmatter，读对应源码时自动加载）。
- `.claude/reference/` -- 深度设计文档（VM 设计、指令集、import、lowering 与各模块踩坑归档等，按需阅读）。
- `CPP_Naming_Convention.md` -- C++ 命名与参数传递规范。
- `.zcode/skills/aria-commit/SKILL.md` -- commit 说明规范与落笔流程。
