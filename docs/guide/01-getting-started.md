# 第 1 章 开始

本章帮你把 aria 解释器跑起来：从源码构建、运行第一个脚本、用一行求值与交互式 REPL 快速试语句，
最后认识 aria 报错的两种形态。读完本章你就能跟着后续章节敲例子了。

## aria 是什么

aria 是一门动态类型脚本语言，解释器从零用 C++23 实现：单遍编译器把源码直接编成栈式字节码，
交给 AriaVM 执行，对象生命周期由 mark-sweep GC 管理。目标平台是 Windows / Linux / macOS。

语言的设计取向是「小而完整」：闭包捕获即引用、不做隐式类型转换、字符串以字节为一等索引域、
模块即文件、异常只有 `try/catch`。它没有想成为第二个 Python，只是把一门脚本语言该有的东西
（函数、闭包、类、容器、迭代协议、模块、异常）按同一套语义收干净。

本教程面向有编程经验的读者：不再解释「什么是变量」「什么是循环」，把篇幅留给 aria 自己的
写法与差异点（无隐式转换、字节域字符串、`===` 键语义这些）。

## 获取与构建

aria 目前以源码分发。构建需要 CMake >= 3.20 与支持 C++23 的编译器；构建过程会经 FetchContent
联网拉取 Google Test（仅测试需要，不联网时跳过测试构建即可）。

```sh
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# Windows（MinGW Makefiles + clang）
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang

cmake --build build --target aria -j
```

构建完成后，解释器二进制位于 `build/aria`。后续章节统一用它举例。

## 第一个脚本

aria 源文件的扩展名是 `.aria`。新建 `hello.aria`：

```aria
# hello.aria
println("hello, aria");

var names = ["ada", "grace", "linus"];
for (name in names) {
    println("hi, " + name);
}
```

输出：

```text
hello, aria
hi, ada
hi, grace
hi, linus
```

运行：

```sh
build/aria hello.aria
```

这几行已经用到了不少语言面：`println` 是唯一的输出口（内建函数，可省参数只输出换行）；
`//` 与 `#` 都是行注释（没有块注释）；`var` 声明变量（aria 没有 `let`，一切 `var` 可变）；
`for (x in xs)` 遍历列表元素；`+` 在两侧都是字符串时拼接。这些都会在后续章节展开。

## 一行求值

不改文件也能跑代码：`-e`（长名 `--eval`）把一段源码当程序执行，适合验证单个表达式：

```sh
build/aria -e 'println(1 + 1);'        # 输出 2
build/aria -e 'println(type("aria"));' # 输出 String
```

注意 `-e` 的参数是一段完整的 aria 源码，语句要带分号。

## REPL

不带参数运行（或显式 `--repl`）进入交互式 REPL。REPL 带行编辑与历史（500 条），
两个习惯用法：

- 输入 `exit` 或 `quit`，或按 Ctrl-D 退出。
- 顶层 `var` 声明跨行保留 -- 像 Python 交互环境一样，先 `var x = 10;`，
  下一行还能用 `x`：

```text
$ build/aria
aria> var x = 10;
aria> println(x * 2);
20
aria> exit
```

REPL 适合跟着教程随手敲例子；本章末的练习也建议在 REPL 里做。

## 报错长什么样

aria 的报错分两大类，读懂它们是后续章节调试例子的基础。

**编译期错误**（词法 / 语法 / 语义检查，程序不执行），带 `文件:行:列` 位置与
`Syntax:` / `Semantic:` 前缀：

<!-- expect-error: UnterminatedString -->
```aria
var s = "line1
line2";
```

```text
hello.aria:1:9: Syntax: UnterminatedString unterminated string: line break in literal
```

字符串字面量不能跨行（第 2 章讲字符串时细说）。再如把保留字拼错、`break` 写在循环外，
都是编译期拦下。

**运行期错误**（执行中才暴露），`Runtime:` 前缀 + 逐帧堆栈跟踪（从 `<main>` 逐帧列到出错位置）：

<!-- expect-error: DivisionByZero -->
```aria
fun inner(n) { return n / 0; }
fun outer(n) { return inner(n) + 1; }

println(outer(5));
```

```text
Runtime: DivisionByZero integer division by zero
  at <main> (divzero.aria:4)
  at outer (divzero.aria:2)
  at inner (divzero.aria:1)
```

堆栈跟踪里的 `<main>` 是模块顶层。运行期错误同样可以被 `try/catch` 接住（第 14 章），
未接住时才像上面这样打印并退出。

## 小结

- aria 用 C++23 解释器执行，`cmake` 构建出 `build/aria`。
- 三种运行形态：脚本文件、`-e` 一行求值、REPL（顶层 `var` 跨行保留）。
- `println` 是输出口；`//` 与 `#` 是行注释；语句以 `;` 结尾。
- 编译期错误带 `文件:行:列` 与 `Syntax:`/`Semantic:` 前缀；运行期错误带 `Runtime:`
  前缀与逐帧堆栈跟踪。

## 练习

1. 把 `hello.aria` 改成用一行 `println` 输出三行文字（提示：`\n` 转义）。
2. 用 `-e` 求值 `1 + 2 * 3` 与 `(1 + 2) * 3`，验证优先级。
3. 进 REPL，声明 `var xs = [3, 1, 2];`，下一行调用 `xs.sort()` 后再打印 `xs` 看结果
   （方法调用第 6 章细讲，先混个脸熟）。

---

[下一章：值与变量](02-values-and-variables.md)
