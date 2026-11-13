# aria

aria 是一门动态类型脚本语言，解释器从零用 C++23 实现：**单遍编译器**把源码直接编译成
**字节码**，交给 **AriaVM** 执行，对象生命周期由 **GC** 管理。
目标平台是 Windows / Linux / macOS。

语言的设计取向偏“小而完整”：闭包捕获即引用、函数是一等公民、不做隐式类型转换、
模块即文件、异常只有 `try/catch`，每一项的语义都定义得明确、一致。

## 看一眼 aria

```aria
def Greeter {
    init(name) { this.name = name; }
    greet(loud = false) {
        var msg = "hello, ${this.name}";
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
println("n = ${1 + 1}, next = ${next()}");                     # n = 2, next = 4
```

`if` 与 `match` 既能作语句也能作表达式；`def` 体里 `init` 是构造器、`this` 指实例、方法可带
默认参数；`[a, b] = [b, a]`、`var [h, ...t] = xs`、`for ([k, v] in map)` 三种位置共用一套解构模式。

## 语言特性

- 动态类型：`nil` / `bool` / `int` / `float` / `string`（UTF-8）/ `list` / `map` / `range`，
  以及函数、类、模块等对象类型。
- 函数是一等值：`fun(x) { ... }` lambda、默认参数、`...varargs`；闭包捕获即引用。
- 类：`def` 定义，`init` 构造器，`super` 单继承；运算符、调用与显示均可重载。
- 控制流：`if` / `while` / C 风格 `for` / `for-in` / `break` / `continue` / `match`；
  `if` 与 `match` 也能作表达式。
- 解构：`var [a, b] = ...`、`[a, b] = [b, a]`、`for ([k, v] in map)`，支持嵌套、`_` 与 `...rest`。
- 异常：`try` / `catch`，`throw` 任意值；可继承内建 `Exception` 定义自己的异常类型。
- 协程：`Coroutine.create / resume / yield / status`，`yield` 可从任意调用深度发起。
- 容器：字面量、负数下标、range 切片（`xs[1..3]`）；`for-in` 走统一的迭代协议。
- 字符串插值：`"n = ${x}"`，`${...}` 里可以写完整表达式。
- 位运算：`&` `|` `^` `~` `<<` `>>` `>>>` 与复合赋值，整数域。
- 模块：一个文件一个模块，`import "./utils" as U;` 绑定别名访问，成员只读。

## 构建与运行

需要 CMake ≥ 3.20 与支持 C++23 的编译器，第三方依赖均 vendored 于 `external/`，构建零联网。

```sh
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target aria -j

build/aria script.aria        # 运行脚本
build/aria -e 'println(1 + 1);' # 求值一段源码
build/aria --repl             # 交互式 REPL（顶层 var 跨行保留）
```

Windows 配置（Ninja + clang）、构建选项（TagValue 值表示 / mimalloc / 指令频度探针）与
构建目录约定见 [docs/building.md](docs/building.md)。

## 测试与基准

```sh
cmake --build build --target aria_tests -j &&
ctest --test-dir build --output-on-failure --no-tests=error
```

测试分两层：`tests/<module>/` 是按模块分目录的 C++ 单元测试；`tests/language/` 是用 aria
语言写成的测试脚本，端到端运行解释器。

性能基准位于 `bench/`：进程内微基准与整程序基准 `bench/lang/`（带 CPython / Node / Lua / Java
对照端口），以 Release 构建测量。测试的组织与编写见 [docs/testing.md](docs/testing.md)。

## 实现

```
.
├── src/             解释器源码；入口 main.cpp / interpreter.cpp（CLI 分发），其余按层分目录
│   ├── compile/     Lexer / Parser / CodeGen / Compiler，单遍产出字节码
│   ├── bytecode/    指令集（code.hpp）、CodeUnit、Disassembler
│   ├── runtime/     AriaVM 主循环与执行上下文；builtins/ 是内建函数与各类型的方法
│   ├── object/      Object 基类与全部 Obj* 子类型（字符串 / 函数 / 类 / 容器 / 迭代器等）
│   ├── memory/      GC 与内存基础设施（Allocator / HashTable / InternPool / StringBuilder 等）
│   ├── value/       Value 值表示（NaN-boxing，可切换 TagValue）、AriaArray / AriaHashTable
│   ├── error/       ErrorCode / Error / AriaException
│   └── util/        fs / utf8 / source_file / io / cli 等工具（多为 header-only）
├── tests/           C++ 单元测试（tests/<module>/）+ aria 语言测试脚本（tests/language/）
├── bench/           进程内微基准与 lang/ 整程序基准（带 CPython / Node / Lua / Java 对照端口）
├── external/        isocline（REPL）/ googletest / mimalloc，vendored、构建时无需联网
├── docs/            grammar.txt（语言文法）、guide/（语言教程）、building / testing 等文档
└── tools/           开发脚本与 git 钩子（commit 说明 / 报错文案机械检查）
```

## 进一步阅读

- [语言教程](docs/guide/) -- 17 章，从值与变量讲到协程，示例全部可直接运行。
- [语言文法](docs/grammar.txt) -- 产生式规范。
- [构建与运行](docs/building.md) -- 三平台配置、构建选项与开发环境。
- [测试](docs/testing.md) -- 测试的组织与编写、性能基准与 CI。
