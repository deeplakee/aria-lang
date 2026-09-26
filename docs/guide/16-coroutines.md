# 第 16 章 协程

协程是**可以挂起再继续的函数调用**。aria 的协程对标 Lua / Wren 的形态：单线程、协作式，
所有切换点都显式写在代码里（`yield` 与 `resume`），没有调度器也没有并行 -- 适合写生成器、
状态机、交错执行的协议逻辑，不是线程。语言面是一个内建模块的四函数：
`coroutine.create` / `resume` / `yield` / `status`。协程本体是普通值（一等公民），可存进
变量、当参数传、放进容器。

## 最小往返

```aria
var co = coroutine.create(fun() {
    println("running");
    coroutine.yield();           # 挂起，控制权交回恢复者
    println("resumed");
});
println(coroutine.status(co));   # create 只登记，不执行
coroutine.resume(co);            # 启动：执行到 yield 停下
println(coroutine.status(co));
coroutine.resume(co);            # 继续：从 yield 处接着跑
println(coroutine.status(co));   # 体执行完，协程死亡
```

输出：

```text
suspended
running
suspended
resumed
done
```

`create` 收一个函数作协程体，**不会执行它**；执行永远从 `resume` 开始。体执行完毕（自然
结束或 `return`）后协程进入 `done`，死了，不可再恢复。

## yield 与 resume 双向传值

协程与恢复者之间每次切换交换一个值：`yield(v)` 的 `v` 成为恢复者那边 `resume` 表达式的
值；反过来 `resume(co, w)` 的 `w` 成为协程内 `yield` 表达式的值（省略即 nil）。协程体
死亡时，`resume` 表达式的值是体的**返回值**：

```aria
var co = coroutine.create(fun(a, b) {
    var got = coroutine.yield(a + b);   # 把 a+b 交给恢复者，换回一个值
    println("got: " + str(got));
    return a * b;
});
println(coroutine.resume(co, 3, 4));    # 首启：3、4 就是协程体实参
println(coroutine.resume(co, "hi"));    # 唤醒："hi" 是 yield 表达式的值
println(coroutine.status(co));
```

输出：

```text
7
got: hi
12
done
```

**首启的载荷就是实参** -- 第一次 `resume` 在协程之外多带的每个实参都按普通调用的规则
交给协程体（元数照常检查，默认参数与 varargs 照常工作）；已挂起的协程则至多收 1 个
（`yield` 是单值表达式，多个报 `WrongArity`）。记法：`resume` 把值**推进去**，把挂起点
产出的值**带出来**，首启的「挂起点」就是函数入口。

## 生成器惯用法

`yield` 可以从**任意调用深度**发起 -- 挂起的是整个执行上下文，中间的帧链原样保留：

```aria
fun level2() {
    coroutine.yield("two levels down");
}
fun level1() {
    level2();
    coroutine.yield("back in level1");
}
var co = coroutine.create(level1);
println(coroutine.resume(co));
println(coroutine.resume(co));
```

输出：

```text
two levels down
back in level1
```

于是「循环里逐个产值」的生成器模式自然成形。有一个易错点：**收尾那次 `resume` 返回的是
体的返回值**（无 `return` 即 nil），不是 yield 值 -- 判断产没产出下一值要看 `status`：

```aria
fun drain(co) {
    var xs = [];
    while (coroutine.status(co) == "suspended") {
        var v = coroutine.resume(co);
        if (coroutine.status(co) == "suspended") {
            xs.push(v);           # 还是 suspended 说明 v 是 yield 产出
        }
    }
    return xs;
}
fun squares_gen(n) {              # 工厂：上界烘焙进闭包，体本身零参数
    return coroutine.create(fun() {
        for (var i = 1; i <= n; i = i + 1) {
            coroutine.yield(i * i);
        }
    });
}
println(drain(squares_gen(3)));
```

输出：

```text
[1, 4, 9]
```

另一条路是约定哨兵值（体结束时 `yield` 一个 `"end"` 一类的记号），消费侧见到即停 --
哪个合适看数据里会不会天然出现这个值。

## status 五态

| 态 | 含义 |
| :--- | :--- |
| `"suspended"` | 挂起或未启动。**唯一可 resume 的态** |
| `"running"` | 正在执行（当前执行者） |
| `"normal"` | 活着，正挂在自己 resume 的那个协程上 |
| `"done"` | 体执行完，正常死亡 |
| `"failed"` | 被未捕获错误杀死（见下节） |

resume 不是主上下文的专利 -- 协程也能恢复协程，恢复者进入 `"normal"`，链可以一直延长：

```aria
var inner = coroutine.create(fun(host) {
    println("host is " + coroutine.status(host));
});
var outer = coroutine.create(fun() {
    coroutine.resume(inner, outer);   # outer 自身作 inner 首启的实参
    println("back in outer");
});
coroutine.resume(outer);
println("outer: " + coroutine.status(outer));
println("inner: " + coroutine.status(inner));
```

输出：

```text
host is normal
back in outer
outer: done
inner: done
```

挂在链中间的协程不能被恢复（`"normal"` / `"running"` 都不是 suspended），这个形态只能
出现在协程内部，错误码见下节。

## 错误穿过协程边界

协程内未捕获的错误不会就地终止程序 -- 协程以 `"failed"` 死去，**错误原样转投恢复者的
`resume` 调用点**。接法与第 14 章完全一样：try / catch 兜住，`catch` 绑原值保类型：

```aria
var co = coroutine.create(fun() { var x = 1 / 0; });
try {
    coroutine.resume(co);
} catch (e) {
    println(str(e));
    println(coroutine.status(co));
}
var co2 = coroutine.create(fun() { throw "boom"; });
try {
    coroutine.resume(co2);
} catch (e) {
    println(type(e));
    println(e);
}
```

输出：

```text
Runtime: DivisionByZero integer division by zero
failed
String
boom
```

若恢复者也一路不接，错误沿链继续跳、最终在主上下文物化，程序以未捕获错误终止。此时
**堆栈跟踪截断在协程边界** -- 死掉的协程内帧不并入，最内一行是恢复侧的 `resume` 调用点：

<!-- expect-error: UncaughtException -->
```aria
fun leaf() {
    throw "boom in coroutine";
}
var co = coroutine.create(fun() { leaf(); });
coroutine.resume(co);
```

```text
Runtime: UncaughtException uncaught exception: boom in coroutine
  at <main> (cor.aria:5)
```

## 死协程与误用形态

三种运行期错误钉住误用（`resume` 已死协程不区分 done / failed，同码同文案）：

<!-- expect-error: ResumeDeadCoroutine -->
```aria
var co = coroutine.create(fun() { coroutine.yield(1); });
coroutine.resume(co);
coroutine.resume(co);
coroutine.resume(co);   # 第二次 yield 没等到，体已完成
```

```text
Runtime: ResumeDeadCoroutine cannot resume a dead coroutine
  at <main> (dead.aria:4)
```

<!-- expect-error: YieldOutsideCoroutine -->
```aria
coroutine.yield(1);     # 主上下文没有恢复者
```

```text
Runtime: YieldOutsideCoroutine cannot yield outside a coroutine
  at <main> (yieldmain.aria:1)
```

此外 `resume` 非协程对象报 `TypeMismatch argument must be a coroutine, got Int`（按实参
实际类型报），`create` 非函数报 `TypeMismatch argument must be a function, got Int`；
恢复链中间的协程（`"normal"` / `"running"`）报 `ResumeNonSuspendedCoroutine`。

## 语义边界

- **单线程协作式**：切换只发生在 `yield`、体完成、错误转投三处，此外代码原子执行。没有
  抢占、没有超时、没有数据竞争，也不存在任何并行收益 -- 协程的价值在**以线性代码写交错
  逻辑**（生成器、状态机、按需生产）。
- 协程的 `type()` 名是 `"Coroutine"`；`println` 渲染为 `<coroutine suspended>` 形态、
  带当前状态。
- 协程是 GC 对象：持有它的变量、容器是根，不可达的挂起协程（连同其整套帧链与捕获）
  会被回收。

## 小结

- `create(fn)` 登记不执行；`resume` 启动 / 继续，返回 yield 值或体返回值；首启载荷即
  协程体实参。
- `yield` 从任意调用深度挂起整个上下文，与 `resume` 双向各传一个值。
- `status` 五态，只有 `"suspended"` 可恢复；协程可恢复协程（`"normal"`），链可延长。
- 协程内未捕获错误转投恢复者的 `resume` 调用点，接法同第 14 章；无人接则主上下文物化，
  跟踪截断在协程边界。
- 误用三码：`ResumeDeadCoroutine` / `ResumeNonSuspendedCoroutine` /
  `YieldOutsideCoroutine`。

## 练习

1. 写 `fun counter_gen(start, step)`：返回一个协程，每次被 resume 产出一个数
   （start、start+step、……）。用 `drain` 的思路取前 10 项。
2. 写 `fun take(co, n)`：从生成器协程取前 n 个产出为列表；不足 n 个时取到协程死亡为止
   （体会：收尾那次 resume 的返回值不是产出）。
3. 把第 8 章 `split` 的逐段扫描改写成协程：写 `fun words(src)` 返回一个每次 `yield`
   一个空白分隔单词的协程，主循环 drain。对比一下它和「先 split 成列表再遍历」在代码
   形态上的差别。
4. 不运行先推理：协程体 `yield(1)` 后 `throw "late"`，主上下文不接 -- 程序输出什么、
   退出码形态如何（对照「错误穿过协程边界」一节）？验证后再想：若主上下文 try 住了，
   `catch` 里 `coroutine.status(co)` 是什么？

---

[上一章：模块](15-modules.md) · [下一章：附录：内建参考](17-builtin-reference.md)
