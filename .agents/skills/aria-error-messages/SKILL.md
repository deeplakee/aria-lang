---
name: aria-error-messages
description: aria 报错文案规范速查：写或改任何面向用户的报错文案（编译期 / 运行期 / CLI / ASSERT）时使用。含 detail 通则、句式家族表要点、元数族 arity_error 唯一口、被调者泛称、ASSERT 文案规则与机械门禁 tools/check_error_messages.py。规范本体在 .agents/reference/specs/error-messages.md。
---

# aria 报错文案速查

> 场景：新增或修改报错点、写 ASSERT 文案、报错被机械门禁打回。通则 8 条、句式家族表
> 全文与裁定背景见 `.agents/reference/specs/error-messages.md`；四条错误通道的归属见 `.agents/reference/error.md`。
> 本文件是落笔前的速查，两者冲突时以规范本体为准。

## 1. 硬规则（机械门禁可查）

- **英文 ASCII-only**：禁 CJK、禁全角标点；适用面含编译期报错、运行期报错、CLI help/usage、
  ASSERT 文案（注释与文档不受约束）。
- **小写起首、无句尾标点**：detail 紧跟 `Code ` 之后；以 `{}` 占位或类型名开头的天然符合。
- 消息壳 `Category: Code` 由枚举名经 X-Macro `#` 派生（`src/error/ErrorCode.hpp`），
  **不另加名表**，故 detail 也一律英文。
- 门禁：`python3 tools/check_error_messages.py`（pre-commit 钩子壳同目录 `hooks/`）。

## 2. detail 写法（通则摘要）

1. 只写「上下文 + 期望 + 实际」：不写解释性从句、不堆规则、不写教程。
2. 名字单引号（`'name'`）、被渲染的值走 `repr`（字符串自带双引号，不与单引号混）。
3. 不暴露内部术语：token 类型名吐源码拼写，不出现字节码概念 / 编码宽度 / C++ 类型名。
4. 上限类说人话：`too many arguments (max 255)`，不写「跳转偏移超过 64KB」。
5. 码名 + detail 语义重叠保留：detail 须能脱离码名独立读懂。
6. **每语义只许一种形状**：措辞漂移即缺陷源，不因「就近完整」另起措辞。

## 3. 高频句式族（完整家族表见规范本体）

| 语义 | 唯一形状 |
| :--- | :--- |
| 语法期望 | `expected <what>, got <spelling\|end of file>` |
| 元数 | `function expects <spec>, got <n>` |
| 类型 | `<what> must be <T>, got <actual>` |
| 越界 | `<what> <value\|range> out of range` |
| 空容器 | `<what> from empty <container>` |
| 成员缺失 | `<recv repr> has no member '<name>'` |
| 协议不支持 | `type <T> does not support <op>` |

- **元数族唯一口**：措辞经 `AriaVM::arity_error` 三口构造（精确数 / 闭区间 / 仅下界），
  调用点只给数量，**不得手写格式串**；被调者一律泛称 `function`，不收名字。
- **类型族同理不带被调者名**：`<what>` 用泛称 `argument`，不写 `Coroutine.create` 一类全名
  --`at` 行已锚定调用点，detail 重复归属零增益。

## 4. ASSERT / static_assert 文案

- `ASSERT` 宏已打 `[file:line] Assert failed in <func>()`，故断言文案**不写封闭实体身份**
  （`Class::method: ` 前缀是重复）；主语可用被检查的类型/量名。
- 只写「被检查的对象 + 违反的不变式」；`static_assert` 不给函数名，类型/组件名可作主语。

## 5. 位置与通道归属

- 编译期错误：位置串 `path:line:col:` 前缀由 Lexer/Parser/CodeGen 烘焙，detail 不带位置。
- 运行期错误：实体是 `ObjException`（携码 + 烘焙消息、不含位置），位置由未捕获出口的
  逐帧 `at <fn> (<mod>:line)` 行给出；**运行期报错点不就地构造 Error**。
