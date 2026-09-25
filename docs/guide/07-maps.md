# 第 7 章 映射

映射（`map`）是哈希表：任意值做键，任意值做值。与多数脚本语言最不一样的一点是**键的判
等走 `===` 严格相等** -- `1` 和 `1.0` 是两个不同的键，对象做键按身份判定。本章讲键语义、
读写口径、方法面与计数惯用法。

## 字面量与基本性质

`{}` 空映射，`{"k": v}` 逐对书写，键值都是任意表达式（运行时求值）：

```aria
var ages = {"ada": 36, "bob": 41};
println(type(ages));
println(ages.size());
println(ages["ada"]);
```

输出：

```text
Map
2
36
```

注意 `{` 的身份是上下文相关的：语句位置是块（第 3 章），表达式位置才是映射字面量。

## 键语义：严格相等

键的匹配一律走 `===`：类型严格一致、对象按身份。三个直接推论：

```aria
var m = {};
m[1] = "int one";
m[1.0] = "float one";
println(m.size());
m["key"] = 1;
m["ke" + "y"] = 2;
println(m.size());
println(m["key"]);
var xs = [1];
var ys = [1];
m[xs] = "list xs";
println(m.get(ys));
println(m[xs]);
```

输出：

```text
2
2
2
nil
list xs
```

对照着读：

- `m[1]` 与 `m[1.0]` 是两个键（`Int` 与 `F64` 类型不同），size 是 2。
- 字符串按内容折叠：`"ke" + "y"` 拼出的串与 `"key"` 命中同一个键（字符串驻留使内容相同的
  字符串就是同一对象）。
- **list 做键按身份**：`xs` 与 `ys` 内容相等但是两个对象，`m.get(ys)` 未命中得 `nil`
  （`get` 未命中返 nil，见下节），`m[xs]` 命中。可变对象做键前先想清楚：改了它，键的
  身份不变，但它作为「值」的可读性可能就乱了。

range 也是合法键（`m[1..3]`），同样按 `===`。

## 读写口径

**写**：命中覆写、未命中新增，恒成功：

```aria
var m = {"a": 1};
m["a"] = 10;
m["b"] = 2;
println(m.size());
println(m["a"]);
```

输出：

```text
2
10
```

**读**：未命中的下标读是运行期错误：

<!-- expect-error: KeyError -->
```aria
var m = {"a": 1};
println(m["nope"]);
```

```text
Runtime: KeyError map key not found: "nope"
  at <main> (key.aria:2)
```

「探查 + 读取」的正规姿势是 `has` / `get`：

```aria
var m = {"a": 1};
println(m.has("a"));
println(m.has("b"));
println(m.get("b"));
```

输出：

```text
true
false
nil
```

`get` 未命中返 `nil` 不报错 -- 代价是「键不存在」与「键存在但值就是 `nil`」无法区分，
能区分这两者的只有 `has`。存 `nil` 值的映射请配 `has` 读取。

## 方法面

| 方法 | 行为 |
| :--- | :--- |
| `size()` | 键值对数 |
| `is_empty()` | 空表谓词 |
| `has(key)` | 键存在判定 |
| `get(key)` | 读值，未命中返 `nil` |
| `keys()` / `values()` / `pairs()` | 各铸**新 list 快照**；`pairs()` 元素是 `[k, v]` |
| `remove(key)` | 移除命中键，返回是否命中（Bool） |
| `clear()` | 原地清空 |
| `iter()` | 迭代器（每步产出 `[k, v]`） |

`remove` 与 list 的 `remove` 一样返回 Bool：

```aria
var m = {"a": 1, "b": 2};
println(m.remove("a"));
println(m.remove("zz"));
println(m.size());
```

输出：

```text
true
false
1
```

**顺序未规定**：`keys()` / `values()` / `pairs()` 与直接打印映射、for-in 遍历，顺序都是
哈希序，官方不承诺任何顺序，别在程序里依赖它（两次调用之间也不保证一致）。要稳定顺序，
取出 keys 后 `sort()` 再用：

```aria
var m = {"bob": 41, "ada": 36, "carol": 28};
var names = m.keys();
names.sort();
for (name in names) {
    println(name + ": " + str(m[name]));
}
```

输出：

```text
ada: 36
bob: 41
carol: 28
```

`pairs()` 的三个快照在同一调用轮次内槽位对齐（`pairs()[i]` 就是 `[keys()[i], values()[i]]`），
但这只是「同一快照内」的保证。

## 遍历与计数惯用法

for-in 遍历映射，每轮产出一个 `[key, value]` 二元列表，配合第 10 章的解构正好拆开：

```aria
var m = {"bob": 41, "ada": 36};
var total = 0;
for ([name, age] in m) {
    total += age;
}
println(total);
```

输出：

```text
77
```

（顺序不定但求和无所谓。）词频统计是映射的标准用例 -- `has` 分流「首次出现 / 再次出现」：

```aria
var text = "the quick brown fox jumps over the lazy dog the end";
var counts = {};
for (w in text.split(" ")) {
    if (counts.has(w)) {
        counts[w] += 1;
    } else {
        counts[w] = 1;
    }
}
println(counts.get("the"));
println(counts.get("fox"));
```

输出：

```text
3
1
```

## 小结

- 键判等一律 `===`：`1` 与 `1.0` 异键、字符串按内容折叠、对象按身份。
- 写恒成功（miss 即新增）；下标读 miss 报 `KeyError`，探查用 `has` / `get`。
- `remove` 返 Bool；`keys` / `values` / `pairs` 铸新快照；**一切顺序都未规定**，要序先 sort。
- for-in 每轮给 `[k, v]`，配解构最顺手。

## 练习

1. 写 `invert(m)`：键值反转。对 `{"a": 1, "b": 2}` 得 `{1: "a", 2: "b"}`（用 `pairs()`
   遍历构造新映射，打印 `result.get(1)` 与 `result.get(2)` 验证）。
2. 写 `histogram(xs)`：统计列表中各元素出现次数，返回映射；打印 `histogram([1, 2, 2, 3,
   3, 3]).get(3)` 应得 3。
3. 给定 `var scores = {"ada": [90, 85], "bob": [70, 95]};`，写循环求每人平均分，打印
   `名字: 均分`（按名字排序输出）。
4. 预测：`var m = {}; m[true] = 1; m[1] = 2; println(m.size());` 输出几？为什么 `true`
   与 `1` 不是同一个键？

---

[上一章：列表](06-lists.md) · [下一章：字符串](08-strings.md)
