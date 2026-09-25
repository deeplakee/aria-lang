# 第 8 章 字符串

字符串是 UTF-8 字节序列，**不可变** -- 所有变换方法都产出新串。aria 把「字节」定为一等
索引域：`size()`、`s[i]`、`find()` 都按字节计数，逐「字符」处理要走 `chars()`。这个设计
换来了下标 O(1) 与干净的切片语义，代价是处理多字节文本时必须分清两个域。本章先立住
字节域 / 码点域的区分，再过方法面。

字面量与转义规则在第 2 章讲过：单双引号、不能跨行、`\u{...}` 码点转义、无插值。

## 字节域与码点域

```aria
var s = "héllo";
println(s.size());            # 6：é 占 2 个字节
println(s.chars().size());    # 5：码点数
println(s.codepoint_at(1));   # 233：é 的 Unicode 码点值
println("中文".size());        # 6
println("\u{1F600}".size());  # 4：emoji 占 4 字节
```

输出：

```text
6
5
233
6
4
```

三个入口的分工：

| 需求 | 用法 |
| :--- | :--- |
| 存储长度（字节数） | `s.size()` |
| 字符个数 | `s.chars().size()`（`chars()` 把串切成单字符字符串的列表） |
| 第 i 个字符的码点值 | `s.codepoint_at(i)`（按码点序号，不是字节位置） |

## 下标与切片：与列表同口径

`s[i]` 切出**第 i 个字节**（产出一个 1 字节的新串），负数从尾计数；`s[range]` 切片与
列表切片同一套规则（闭 / 半开 / 无上界 / 负端点 / 倒序段）：

```aria
var s = "hello";
println(s[0]);
println(s[-1]);
println(s[1..3]);
println(s[1...-1]);
println(s[-3..]);
```

输出：

```text
h
o
ell
ell
llo
```

对多字节字符用字节下标会切出「半个字符」 -- 一个不构成合法 UTF-8 的单字节串。程序不会
拦你（字节域是明文契约），但输出端多半显示乱码。**处理含中文 / emoji 的文本，请统一走
`chars()` / `codepoint_at()`**：

```aria
var s = "中文";
var cs = s.chars();
println(cs.size());
println(cs[0]);
println(cs[1]);
```

输出：

```text
2
中
文
```

## 拼接与转换

`+` 只在两侧都是字符串时拼接（第 2 章），任意值转字符串用 `str()`。列表侧的 `join(sep)`
把元素串起来：

```aria
var parts = ["aria", "is", "small"];
println(parts.join(" "));
println("n = " + str(42));
```

输出：

```text
aria is small
n = 42
```

字符串到数字用 `to_int()` / `to_float()`，**失败返 `nil` 而不报错** -- 配合 `if` 判空就是
解析惯用法：

```aria
println("42".to_int());
println("42px".to_int());
println(" 42".to_int());
println("3.5".to_float());
println("1e3".to_float());
println("3.5x".to_float());
```

输出：

```text
42
nil
nil
3.5
1000.0
nil
```

`to_int()` 是「整串是十进制整数」的严格判定：不跳空白、不认进制前缀与下划线，收前导
正负号。`to_float()` 额外收小数点与指数形态。

## 方法面一览

全部变换方法产新串、原串不变：

```aria
println("aBc1".upper());
println("aBc1".lower());
println("\t\n hi \r\n".trim());
println("a,,b".split(","));
println("  a  b\tc ".split());
println("hello".find("l"));
println("hello".find("z"));
println("hello".contains("ell"));
println("aaa".replace("a", "bb"));
println("hello".substring(1, 3));
println("hello".starts_with("he"));
println("hello".ends_with("lo"));
println("hello".is_empty());
println("".is_empty());
```

输出：

```text
ABC1
abc1
hi
["a", "", "b"]
["a", "b", "c"]
2
nil
true
bbbbbb
el
true
true
false
true
```

几个口径要点：

- `upper()` / `lower()` 只转 ASCII 字母，`é` 不动。
- `split(sep)` 按分隔符切、**保留空段**（`"a,,b"` 三段，空串切出 `[""]`）；`split()` 无参
  按连续 ASCII 空白切、**丢空段**。分隔符为空串是 `EmptyPattern` 错误。
- `find(sub)` 返**字节下标**，未命中返 `nil`（不是 -1）。
- `substring(start, end)` 取字节区间 `[start, end)`，end 可省；**不收负数、不静默钳制**，
  越界直接报错 -- 与列表下标的负数口径不同，别混：

<!-- expect-error: IndexOutOfBounds -->
```aria
println("hello".substring(3, 9));
```

```text
Runtime: IndexOutOfBounds substring range 3..9 out of range
  at <main> (sub.aria:1)
```

- `replace(old, new)` 替换全部命中，`old` 为空串报 `EmptyPattern`。

完整的方法签名表见[附录](16-builtin-reference.md)。

## 小结

- 字符串不可变；字节是一等索引域，字符处理走 `chars()` / `codepoint_at()`。
- 下标与切片与列表同口径（负数、倒序段）；多字节文本别用字节下标切字符。
- `find` 未命中返 `nil`；`substring` 不收负数、越界报错；`upper/lower` 只管 ASCII。
- `to_int` / `to_float` 失败返 `nil`，配条件判断做安全解析。

## 练习

1. 写 `is_palindrome(s)`：忽略大小写判断回文（提示：`lower()` + `chars()`，双指针比较）。
   `"Level"` 与 `"aria"` 各验证一次。
2. 写 `count_vowels(s)`：统计小写元音字母个数（用 `chars()` 遍历）。
3. 写 `parse_pair(line)`：解析 `"key=value"` 形态的一行，返回 `[key, value]` 二元列表；
   不含 `=` 时返回 `nil`（提示：`find("=")` 判 nil 后用 `substring`）。
4. 不运行先推理：`var s = "中文"; println(s.chars()[0].size());` 输出什么？验证后用一句话
   解释（`chars()` 的元素是什么、它的 `size()` 又是什么域）。

---

[上一章：映射](07-maps.md) · [下一章：区间与迭代](09-ranges-and-iteration.md)
