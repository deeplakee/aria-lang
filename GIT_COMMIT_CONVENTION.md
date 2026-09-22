---
name: aria-commit
description: aria 仓库 commit 说明规范：说明一律英文 ASCII，形状为 <type>(<scope>): <subject> + 可选散文正文（≤ 4 行，无标签无 bullet）+ 可选 `Tests:` 行。含 type 枚举、禁写内容清单（实现步骤/评审注记/被否方案推导/实测数字/文档指针/规划词汇）、落笔流程与机械检查。写 git commit message、--amend 改说明、判定提交粒度时使用；**每次 git commit 前必读**，配套 `tools/check_commit_msg.py`。
---

# Git Commit Convention

> **Subject 说改了什么，正文说为什么。正文默认不写，说明一律英文。**

本文件是仓库 commit 说明的规范，CLAUDE.md「Git 提交纪律」节为其摘要，细节以本文件为准。设计目标是**一眼读完**：多数提交只有一行标题，长内容归 `.claude/reference/` 文档（档案），commit 只当索引。说明用英文对齐主流规范（Conventional Commits 与 git 社区惯用的祈使句标题），也便于 `git log --oneline` 在任何终端与编码下稳定显示。本文件自身的中文叙述不受此限。本文件同时是 ZCode 技能 `aria-commit` 的本体（`.zcode/skills/aria-commit/SKILL.md` 软链到此），frontmatter 供技能注册、正文即规范。

## 1. 形状

```text
<type>(<scope>): <subject>

<optional body: 1-3 sentences of prose>

Tests: <optional single line>
```

| 段 | 必写 | 约束 |
| :--- | :--- | :--- |
| Subject | 是 | **祈使句**、动词原形开头（`add` 不取 `added`）、小写开头、无句号；软目标 ≤ 50 列，硬上限 72 列 |
| Body | 否 | 只在「标题与 diff 说不清 why」时写；1~3 句**散文**，≤ 4 行；不分段标签、不列 bullet；72 列折行 |
| `Tests:` 行 | 否 | 跑过测试/构建就带一行结果；纯文档与脚本类可省；须是说明末行 |

语言纪律：说明**一律英文且 ASCII-only**--禁 CJK、全角标点、Unicode em-dash（破折号语义用 ASCII `--`）；标识符、模块名、指令名原样引用（`MAKE_METHOD`、`ObjString::load_index`），不翻译。

## 2. type 与 scope

| type | 何时用 |
| :--- | :--- |
| `feat` | 行为从无到有（新特性 / 新机制 / 新 builtin） |
| `fix` | 行为从错到对 |
| `refactor` | 外部可观测行为不变的结构调整 |
| `perf` | 行为不变，动机就是性能（量化对比归 reference 文档，不进说明） |
| `docs` | 只改文档与注释 |
| `test` | 只改测试与语料基建（伴随 feat/fix 的测试随本批，不单拆） |
| `build` | 构建系统与依赖 |
| `style` | 不改语义的格式化 |
| `chore` | 其余杂项（`.gitignore`、脚本、agent 配置） |

scope 取仓库模块名（`core` / `util` / `value` / `error` / `compile` / `bytecode` / `runtime` / `object` / `memory` / `tests` / `docs` / `build`）；跨模块逗号分隔、主模块在前、至多三个；全库性改动省略 scope。

## 3. 正文写什么

正文只回答标题与 diff 答不出的两件事：**why**，以及**机制上不显然的那一点**（跨文件因果、被否方案的结论）。这两点都写不出来，就不要正文。

禁写内容，判据一句话：**这段文字描述的是本次改动，还是得到本次改动的过程**--后者删掉，信息无损。

| 类别 | 反例 | 改法 |
| :--- | :--- | :--- |
| 实现步骤流水 | "first added a method to ObjString, then moved it into util" | 只写最终落点 |
| 评审/决策过程 | "(user decision)"、"as discussed, we picked option B" | 直述理由本身 |
| 被否方案推导 | "deliberately not shared with the Lexer parser (which accepts `_`)" | 一行结论或不写 |
| 实测数据 | "extracting the helper measured 9% slower" | 删；数字属 reference 文档或 `perf` 提交 |
| 文档指针 | "full rationale in collections-builtin-methods-plan.md" | 删；文档随批提交，`git show --stat` 即可见 |
| 项目状态叙述 | "there was previously nowhere to put this" | 删；只写现在的行为与机制 |
| 规划词汇 | "M5 phase 3"、"batch 9"、"finalize" | 按语言域名称描述改动 |
| 空泛评价与完成体 | "improve"、"clean up"、"fixed X" | 说机制变化；动词用原形 |

## 4. 落笔（每次提交强制）

1. `git add -u` 后 `git diff --cached --stat` 核范围，只含本批文件。
2. 说明写进临时文件（如 `/tmp/commit_msg.txt`），不用 `-m` 随手一句。
3. `python3 tools/check_commit_msg.py <file>` 必须返回 0；`commit-msg` 钩子（`git config core.hooksPath tools/hooks` 启用）会在落库时自动再跑一遍，违规拒提交。
4. `git commit -F <file>`；未推送时改说明用 `git commit --amend -F <file>`。
5. `git log -1 --format=%B` 复读一遍--脚本判不出「正文是不是在讲过程」。

## 5. 粒度与流程

一个 commit 一个主题（一个特性 / 一个修复 / 一次重构 / 一轮文档收口），每批独立可编译、可 review、可回滚。与 review 流程咬合：改动完成 → 验证全绿 → 报告并停手 → 用户逐批 review → 点头后落一批 commit，**一次 commit = 一次 review 通过的批**。main 直推，保持线性历史，避免 merge commit。说明在落库时点当场写，不套用上一批的话术。

## 6. 例子

```text
style(tests): drop decorative (void) casts
```

```text
feat(runtime): add string to_int/to_float

Strings could only be converted out, so reading numeric data meant
hand-rolled parsing. Both methods parse the whole string in decimal via
util::parse_int_text/parse_float_text and return nil on failure; to_int
additionally gates the i48 domain.
Tests: ctest 1158/1158 (default + TagValue); clang-format clean
```

```text
fix(memory): iterate HashTable benchmark via const_iterator

The benchmark still called for_each_occupied after the slot-scan
primitive was retired, and that target is part of default ALL, so the
default build failed to compile.
```

反例与改法：

| 反例 | 错在哪 |
| :--- | :--- |
| 20 行 `动机:` / `改动:`（三条 bullet）/ `验证:` 三段骨架 | 标签与清单是模板声，改完只剩 2~4 行散文；读者要的是一眼读完 |
| 正文写 "deliberately not shared with the Lexer parser" + "measured 9% slower" + "full rationale in <doc>" | 三段都是「得到本次改动的过程」，对「本批改了哪些代码」零信息 |
| 正文夹 "there was previously nowhere to put this" / "first added a method, then moved it" | 叙述开发进程与实现步骤，读者要的是最终落点与理由 |
| 说明用中文或全角标点 | 与主流规范不一致，且 `git log --oneline` 在窄终端/异编码下不稳 |

## 7. 落笔前自检

- [ ] 一批一主题，只含本批文件，type / scope 准确
- [ ] Subject 是英文祈使句、动词原形、无句号、≤ 72 列
- [ ] 正文 ≤ 4 行英文散文（无标签、无 bullet）；没有过程叙述、被否推导、实测数字、文档指针、规划词汇
- [ ] 全文 ASCII-only（无 CJK、全角标点、em-dash）
- [ ] 写了 `Tests:` 就确实跑过，且结果与说明一致
- [ ] 已按 §4 五步走过（脚本返回 0），并复读一遍全文
