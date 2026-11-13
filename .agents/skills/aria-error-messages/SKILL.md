---
name: aria-error-messages
description: aria 报错文案规范速查：写或改任何面向用户的报错文案（编译期 / 运行期 / CLI / ASSERT）时使用。含 detail 通则、句式家族要点、元数族 arity_error 唯一口、被调者泛称、ASSERT 文案规则与机械门禁 tools/check_error_messages.py 的实际覆盖面。规范本体在 .agents/reference/specs/error-messages.md。
---

# aria 报错文案速查

> 场景：新增或修改报错点、写 ASSERT 文案、被机械门禁打回。通则与句式家族全文见
> `.agents/reference/specs/error-messages.md`（这里只留落笔判据，冲突时以本体为准）；四条错误通道的归属见
> `.agents/reference/error.md`。

## 1. 硬规则

- **英文 ASCII-only**：禁 CJK、禁全角标点。门禁扫的是 `src/` 下**全部字符串字面量**，不限报错点--加一个合法的非 ASCII 字面量也会被打回。
- **小写起首、无句尾标点**：detail 紧跟 `Code ` 之后；以 `{}` 占位或类型名开头的天然符合。
- 消息壳 `Code` 由 X-Macro `#` 派生（`src/error/ErrorCode.hpp`），`Category` 来自手写 `kCategoryNames`（六项含 `Ok`）；两者都**不另加中文名表**，故 detail 一律英文。

## 2. 门禁的真实覆盖面（`tools/check_error_messages.py`）

只查三类：① 所有字面量的非 ASCII 字符；② 报错入口的**每行首个**字面量--不以 `.` 结尾（`!`/`?` 不拦）、首字母小写；③ ASSERT 文案不带 `Class::method:` 这类实体身份前缀。

**它查不到**：消息壳是否真的由 X-Macro 派生；`std::format` 中转后再交给 `Error::from_detail` 的文案（入口与字面量不在同一调用链就漏检）；同一行第二个字面量的形状；非 `src/` 的文案（`docs/guide/17-builtin-reference.md` 逐字引用真实消息、语料 `.err` 也是逐字引用，改文案要手工同步）。

钩子是**本地配置**：`git config core.hooksPath tools/hooks` 才生效（`tools/hooks/pre-commit`）。没配时只有手跑 `python3 tools/check_error_messages.py` 才能兜住；脚本无参数时缺省扫 `src/`。

## 3. 高频句式族（完整家族表见本体）

| 语义 | 唯一形状 |
| :--- | :--- |
| 语法期望 | `expected <what>, got <spelling\|end of file>` |
| 元数 | `function expects <spec>, got <n>` |
| 类型 | `<what> must be <T>, got <actual>` |
| 越界 | `<what> <value\|range> out of range` |
| 空容器 | `<what> from empty <container>` |
| 缺键 | `map key not found: <value>` |
| 成员缺失 | `<recv repr> has no member '<name>'` |
| 协议不支持 | `type <T> does not support <op>` |

- **元数族唯一口 = `AriaExeContext::arity_error`**（三口：精确数 / 闭区间 / 仅下界，真身不在 `AriaVM`）。调用点只给数量，**不得手写格式串**；被调者一律泛称 `function`，不收名字。
- **类型族同理不带被调者名**：`<what>` 用泛称 `argument`，不写 `Coroutine.create` 一类全名。
- **算子钩子缺失不走协议族**：`x.__add__` 的 miss 是普通成员缺失（`<recv repr> has no member '__add__'`）；`type <T> does not support <op>` 只用于 `'__call__'`、`'__str__'` 与 field/subscript access。

## 4. ASSERT / static_assert 文案

- `ASSERT` 宏已打 `[file:line] Assert failed in <func>()`，断言文案**不写封闭实体身份**（`Class::method: ` 前缀是重复）；主语可用被检查的类型/量名。
- `static_assert` 不给函数名，类型/组件名可作主语（`InternPool: Alloc must satisfy TrivialAllocator`）。
- 两者的 ASCII-only 约束同 §1。

## 5. 位置与通道归属

- 编译期错误：位置 `path:line:col:` 前缀由 Lexer/Parser/CodeGen 烘进消息；detail 本身不带位置。
- 运行期错误：实体是 `ObjException`（携码 + 烘焙消息、**不含位置**），位置由未捕获出口的逐帧 `at <fn> (<mod>:line)` 行给出；**运行期报错点不就地构造 `Error`**。
