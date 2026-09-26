# aria 语言教程

面向有编程经验读者的 aria 语言教程：从构建运行讲到类、异常、模块与协程，附内建速查。行文是
**教学视角**（示例驱动、由浅入深）；语言语义的完整规范在
[docs/grammar.txt](../grammar.txt)，两份文档冲突时以文法为准。

## 怎么跑示例

教程所有示例都是可直接运行的完整程序。构建解释器（细节见仓库根 `README.md`）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target aria -j
```

三种运行形态：

```sh
build/aria example.aria     # 运行脚本文件
build/aria -e 'println(1);' # 一行求值
build/aria                  # REPL（顶层 var 跨行保留）
```

跟着章节做练习建议直接进 REPL；多文件示例（第 15 章）按章内目录树摆好文件再跑入口。

## 目录

| 章 | 内容 | 一句话 |
| :--- | :--- | :--- |
| 1 | [开始](01-getting-started.md) | 构建、运行、REPL、报错的两种形态 |
| 2 | [值与变量](02-values-and-variables.md) | 七种基础值、`var`、运算符全集、`==` 与 `===` |
| 3 | [控制流](03-control-flow.md) | if（含表达式）、while、三段 for、break / continue |
| 4 | [函数](04-functions.md) | 声明与 lambda、默认参数、varargs、一等函数 |
| 5 | [闭包](05-closures.md) | 捕获即引用、计数器与工厂、高阶函数 |
| 6 | [列表](06-lists.md) | 下标与负数、增删查、排序、区间切片 |
| 7 | [映射](07-maps.md) | `===` 键语义、读写口径、快照与顺序 |
| 8 | [字符串](08-strings.md) | 字节域与码点域、方法面、安全解析 |
| 9 | [区间与迭代](09-ranges-and-iteration.md) | range 三形态、for-in、显式迭代器与协议 |
| 10 | [解构](10-destructuring.md) | 模式语法、三种位置、rest、交换 |
| 11 | [match](11-match.md) | 语句与表达式形态、`==` 匹配、`_` 通配 |
| 12 | [类与对象](12-classes.md) | `def`、三类成员、继承与 super、自定义可迭代 |
| 13 | [运算符重载](13-operator-overloading.md) | 11 个钩子、仅左侧触发、`__call__` |
| 14 | [异常](14-exceptions.md) | throw 任意值、catch 保类型、无 finally |
| 15 | [模块](15-modules.md) | 一文件一模块、导入即执行、成员只读 |
| 16 | [协程](16-coroutines.md) | create/resume/yield/status、生成器、跨协程错误 |
| 17 | [附录：内建参考](17-builtin-reference.md) | 全局内建、各类型方法面、错误消息速查 |

前五章是基础（值 / 控制流 / 函数 / 闭包），六到十一章是数据与表达式（容器 / 迭代 /
解构 / match），十二到十六章是组织手段（类 / 重载 / 异常 / 模块 / 协程）。赶时间的话，读完
第 2、4、6 章就能上手写脚本。

## 约定

- 示例代码块标 `aria`，注释用 `#`（`//` 等价）；`text` 块是程序的期望输出或报错原文。
- 报错展示的都是真实输出（编译期带 `文件:行:列:`，运行期带堆栈行；文件名以示例自身
  命名为准）。
- 涉及 map 顺序的示例一律先 sort 再输出 -- map 的一切顺序都未规定。
- 练习不给答案；题面里「不运行先推理」的题目先写下预测再验证。
- 教程只写已落地的语言面；语言层尚未提供的特性（详见附录末尾）不预先书写。
