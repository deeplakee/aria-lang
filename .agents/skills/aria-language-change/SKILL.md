---
name: aria-language-change
description: aria 语言面改动同步清单：改文法（docs/grammar.txt）、词法、语义或新增语言特性时使用。列出全部同步义务面：Parser/CodeGen 实现、tests/language 语料正负向、docs/guide 教程章节、README 特性表、.agents/rules 模块参考，及双配置全量验证要求。
---

# aria 语言面改动同步清单

> 场景：动语言面（新语法 / 新语义 / 行为变更 / 报错变更）时逐项过本清单。
> 语言面的单一事实源是 `docs/grammar.txt`；教程是可执行验收面，语料是行为安全网，
> 三者与实现同步走，缺一即回炉。

## 1. 同步义务面（按序过）

| # | 面 | 位置 | 义务 |
| :--- | :--- | :--- | :--- |
| 1 | 文法 | `docs/grammar.txt` | 产生式与实现保持一致；单点特判挂文末「说明」区的「上下文消歧」列表，**不为 parser 特判立非终结符** |
| 2 | 实现 | `src/compile/`（Lexer / Parser / CodeGen） | 词法 token、产生式、语义检查、字节码发射；报错文案走句式家族（技能 `aria-error-messages`） |
| 3 | 语料 | `tests/language/` | 正向脚本 `assert` 收口钉住新语义；行为报错变更同步负向用例与 `.err` 子串 |
| 4 | 教程 | `docs/guide/` 对应章 | 示例实跑验证（教程只写已落地特性，报错展示真实输出）；必要时更新章首目录与 `17-builtin-reference.md` |
| 5 | 特性表 | `README.md`「语言特性」节 | 新特性 / 行为变更须反映在读者面清单 |
| 6 | 模块参考 | `.agents/rules/` 对应模块 | compile / runtime / object 相关条目随代码同批更新 |

## 2. 验证要求

- **双配置全量**：NaN-boxing（默认）与 TagValue（`-DARIA_USE_TAGVALUE=ON` 独立
  build 目录）各跑一遍 build + ctest 全量（含语料）。
- 语料 runner 是 PRE_TEST 发现：新增 `.aria` 无需重链接，`ctest --test-dir build -R <片段>` 即收。
- 报错文案变更跑 `python3 tools/check_error_messages.py`。

## 3. 语料写作红线（详见 `tests/language/README.md`）

- 正向一律 `assert` 收口，期望值手算写死，不写同义反复。
- **不钉临时未实装行为**：「期待报错」的负向用例只收设计上永久如此的错误，
  未落地特性（见该 README「当前禁区」节）写了翻转日会变红。
- `.err` 子串只取消息稳定正文，禁绝对路径与随内容漂移的行号。
- 教程面：map 顺序未规定，示例一律先 sort 再输出。

## 4. 常见走偏

- 只改实现不改文法 / 只改文法不改语料 = 验收面缺口，review 直接打回。
- 语义决策（拍板点）记入对应设计文档 `.agents/design/` 或笔记 `.agents/notes/`，
  不散落在 commit 说明或会话里。
