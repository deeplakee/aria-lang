# 第 14 章 异常

aria 的异常机制刻意收窄：`throw` 抛**任意值**，`try` / `catch` 接住，`catch` 绑定的就是
被抛的那个原值。没有 `finally`。用户 `throw` 与 VM 检测到的运行期错误走同一条通道 -- 
所以「接住一个除零错误」和「接住一个自己抛的字符串」写法完全一样。

## throw 与 catch

```aria
try {
    throw "boom";
} catch (e) {
    println(e);
    println(type(e));
}
```

输出：

```text
boom
String
```

`throw` 后面是任意表达式，**catch 绑原值、保类型**：抛字符串接到的就是字符串，抛整数
接到的就是整数：

```aria
try {
    throw 42;
} catch (e) {
    println(type(e));
    println(e + 1);
}
try {
    throw [1, 2];
} catch (e) {
    println(e.size());
}
```

输出：

```text
Int
43
2
```

实践里最常见的抛值是字符串（消息）与专门的「错误对象」 -- 比如定义一个 `AppError` 类，
带 `code` 字段，catch 侧按 `type(e) == "Instance"` 加字段判断分流。

## 运行期错误同样可接

VM 抛的运行期错误装箱成异常对象，`type(e)` 是 `"Exception"`，打印形态是
`类别: 码 消息`：

```aria
try {
    var x = 1 / 0;
} catch (e) {
    println(e);
    println(type(e));
}
try {
    var xs = [1];
    println(xs[5]);
} catch (e) {
    println(e);
}
```

输出：

```text
Runtime: DivisionByZero integer division by zero
Exception
Runtime: IndexOutOfBounds list index 5 out of range
```

于是一个惯用法成形：**把「会报错的取值」包进 try，用 catch 做兜底**。比如 map 下标读
miss 报 `KeyError`（第 7 章），要么用 `has` 预判，要么 catch：

```aria
fun safe_get(m, key) {
    try {
        return m[key];
    } catch (e) {
        return nil;
    }
}
println(safe_get({"a": 1}, "a"));
println(safe_get({"a": 1}, "z"));
```

输出：

```text
1
nil
```

catch 里 `return` 没有任何限制，函数正常返回。

## 嵌套与跨帧展开

嵌套 try 取**最近的** handler；异常沿调用链向外展开（unwind），途中任何一层都能接：

```aria
fun level3() { throw "deep"; }
fun level2() {
    try {
        level3();
    } catch (e) {
        throw "from level2: " + e;   # 接住后加工再抛（rethrow）
    }
}
try {
    level2();
} catch (e) {
    println(e);
}
```

输出：

```text
from level2: deep
```

原值穿透也是同理 -- catch 到什么就还能再 throw 什么，类型不变：

```aria
try {
    try {
        throw "original";
    } catch (e) {
        throw e;
    }
} catch (e2) {
    println(e2 == "original");
}
```

输出：

```text
true
```

## 未捕获时

异常一路没遇到 handler 才落到程序出口：打印错误与逐帧堆栈跟踪后退出（第 1 章见过完整
形态，从出错位置逐帧列回 `<main>`）。开发期这是排查利器；交付期想兜一切意外，就在
程序最外层包一个总 try。

## 没有 finally

`try` **必须**带 `catch`（编译期强制）：

<!-- expect-error: TryWithoutHandler -->
```aria
try { var x = 1; }
```

```text
finally.aria:1:1: Semantic: TryWithoutHandler 'try' requires a catch clause
```

没有 `finally` 子句 -- 这是有意的取舍：aria 的语言层目前没有制造操作系统资源（文件、锁
等）的内建，清理场景有限；参照实现 Lua / Wren 也都没有 finally。需要「无论成败都执行」
的逻辑，现在的写法是在 try 块正常路径末尾与 catch 里各写一次（或抽成函数调用两次）。

## assert 也是抛

第 1 章用的 `assert(cond)` 失败时抛的就是异常 -- `AssertionFailed`，可 catch：

```aria
try {
    assert(false, "custom message");
} catch (e) {
    println(type(e));
    println(e);
}
```

输出：

```text
Exception
Internal: AssertionFailed custom message
```

`assert(cond, msg)` 第二参是消息（省略则默认 `assertion failed`）。未捕获的 assert 失败
以 `Internal: AssertionFailed ...` 形态打印退出 -- 它的类别前缀是 `Internal`，与
`Runtime:` 区分开。

## 常见运行期错误码

教程各章已经零散见过，这里集中列一次（完整消息见[附录](17-builtin-reference.md)）：

| 码 | 典型触发 |
| :--- | :--- |
| `DivisionByZero` | 整数除零 / 模零 |
| `IndexOutOfBounds` | 列表 / 字符串下标越界、空表 `pop`、切片端点越界 |
| `KeyError` | map 下标读 miss |
| `TypeMismatch` | 算子两域不符（数值 / 字符串 / 钩子签名） |
| `UndefinedVariable` | 读 / 写未声明的名字 |
| `UndefinedProperty` | 对象缺成员（含缺运算符钩子、不可迭代值） |
| `WrongArity` | 实参个数不符 |
| `MatchNoArm` | match 无匹配且无 `_` |
| `IterationExhausted` | 迭代器耗尽后再 `next()` |
| `StackOverflow` | 递归超 256 帧 |
| `CallNonCallable` | 调用不可调用的值 |
| `EmptyPattern` | `split("")` / `replace("", ...)` 空模式 |
| `AssertionFailed` | `assert` 失败 |

## 小结

- `throw` 任意值，`catch` 绑原值保类型；运行期错误装箱为 `type(e) == "Exception"`。
- 嵌套取最近 handler，沿调用链展开；catch 内可 `return`、可再 `throw`。
- `try` 必带 `catch`；没有 `finally`（设计取舍，当前无 OS 资源类内建）。
- `assert` 失败即抛 `AssertionFailed`（`Internal:` 前缀），同样可 catch。

## 练习

1. 写 `fun parse_int(s)`：基于 `to_int()` 的 nil 语义返回整数或 nil；再写 `fun parse_int_or(s, fallback)`
   用 try / catch 把「s 不是数字」兜住（先故意在 try 里 `throw s`？ --  不，体会一下：
   nil 判空与 try / catch 各适合什么场景，两个函数分别选一种并说明理由）。
2. 写 `fun div(a, b)`：`b == 0` 时 `throw "division by zero"`；调用处接住并打印错误。
3. 写 `fun retry(f, n)`：调用 `f()`，抛异常就重试，最多 `n` 次后把最后的异常原样
   rethrow（提示：循环里嵌 try / catch，计数在 catch 里做）。
4. 不运行先推理：`try { throw nil; } catch (e) { println(type(e)); println(e == nil); }`
   输出什么？验证后想一想「抛 nil」会不会和「没抛」混淆。

---

[上一章：运算符重载](13-operator-overloading.md) · [下一章：模块](15-modules.md)
