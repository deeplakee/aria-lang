---
name: aria-language-change
description: aria 语言面改动同步清单：改文法（docs/grammar.txt）、词法、语义或新增语言特性时使用。列出全部同步义务面：Parser/CodeGen 实现、tests/language 语料正负向、docs/guide 教程章节、README 特性表、.agents/reference 模块参考，及双配置全量验证要求。
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
| 3 | 语料 | `tests/language/` | 正向脚本 `assert` 收口钉住新语义；行为报错变更同步负向用例与 `.err` 子串；**显示/输出语义变更要重生成受影响的 `.out` golden**（人工核对后冻结） |
| 4 | 教程 | `docs/guide/` 对应章 | 示例实跑验证（教程只写已落地特性，报错展示真实输出）；必要时更新章首目录与 `17-builtin-reference.md` |
| 5 | 特性表 | `README.md`「语言特性」节 | 新特性 / 行为变更须反映在读者面清单 |
| 6 | 模块参考与规格 | `.agents/reference/` | 模块参考随代码同批更新；新增/改动指令还要同步 `reference/specs/instruction-set.md` 与 `lowering.md` |

## 2. 验证要求

- **双配置全量**：NaN-boxing（默认）与 TagValue（`-DARIA_USE_TAGVALUE=ON` 独立
  build 目录）各跑一遍 build + ctest 全量（含语料）。
- 语料 runner 是 PRE_TEST 发现：新增 `.aria` 无需重链接，`ctest --test-dir build -R <片段>` 即收。
- 报错文案变更跑 `python3 tools/check_error_messages.py`，并手工同步 `docs/guide/17-builtin-reference.md`「常见错误消息速查」里逐字引用的消息（门禁只扫 `src/`）。

## 3. 语料写作红线（详见 `tests/language/README.md`）

- 正向一律 `assert` 收口，期望值手算写死，不写同义反复。
- **不钉临时未实装行为**：「期待报错」的负向用例只收设计上永久如此的错误，
  未落地特性（见该 README「当前禁区」节）写了翻转日会变红。
- `.err` 子串只取消息稳定正文，禁绝对路径与随内容漂移的行号。
- 教程面：map 顺序未规定，示例一律先 sort 再输出。

## 4. 常见走偏

- 只改实现不改文法 / 只改文法不改语料 = 验收面缺口（三者都是验收面，缺一项即未完成）。
- 特性落地时要**回头删** `tests/language/README.md`「当前禁区」里已翻转的条目并补负向用例--禁区不清理会继续指挥后续用例。
- 语义决策（拍板点）记入 `.agents/reference/` 或 `.agents/notes/` 的对应文档，
  不散落在 commit 说明或会话里。
