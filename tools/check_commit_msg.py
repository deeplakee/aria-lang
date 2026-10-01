#!/usr/bin/env python3
"""commit 说明机械检查器(规范见 .zcode/skills/aria-commit/SKILL.md;检查项清单以本文件常量为准)。

用法:
    python3 tools/check_commit_msg.py <message-file>

只查机器能判的项;语义判断(正文是否在讲过程、subject 是否真的在说改动)仍靠人读
(规范第 5 步)。有硬违规时打印违规行与改法并返回 1。
"""

import re
import sys

# type 固定九类枚举(规范 §2)。
TYPES = ("feat", "fix", "refactor", "perf", "docs", "test", "build", "style", "chore")

# 列宽上限(规范 §1):说明 ASCII-only,故一字符一列。
WIDTH_MAX = 72
# 标题软目标(列)。
SUBJECT_WIDTH_SOFT = 50
# 正文上限:散文 1~3 句(规范 §1);超过即属该进 reference 文档的长内容。
BODY_LINES_MAX = 4

# 唯一允许的标签行。
TESTS_LABEL = "Tests:"

# 旧三段骨架的段标签:正文写散文,不用模板分段(规范 §1/§6)。
SECTION_LABELS = ("why", "what", "how", "motivation", "changes", "notes", "background", "impact", "summary")

# subject 祈使句要求:动词原形开头(规范 §1)。命中的是过去式/完成体常见形态。
PAST_TENSE_VERBS = (
    "added", "fixed", "removed", "renamed", "moved", "changed", "updated", "deleted", "implemented", "landed",
    "introduced", "replaced", "simplified", "refactored", "cleaned", "dropped", "made", "allowed", "improved",
    "reduced", "increased", "merged", "split", "reverted", "bumped", "aligned", "unified", "extracted", "migrated",
)

# 硬禁词:命中即违规。判据是「这段文字描述的是本次改动,还是得到它的过程」(规范 §3)。
BANNED_PATTERNS = (
    (r"as discussed|per review|per discussion|we decided|we chose|decision was|user decision|after review",
     "过程/决策注记:直述理由本身"),
    (r"this commit|this change|the purpose of this|this patch", "自我指涉旁白:直接说内容"),
    (r"full rationale|detailed rationale|see\s+\S+\.(md|txt)|for details see|refer to .*\.md",
     "文档指针:文档随批提交,说明里不指路"),
    (r"benchmark(ed|s)? (shows|says)|\d+(\.\d+)?% (faster|slower|fewer)|\d+ ns", "测量数据:归 reference 文档或单独 perf 提交"),
    (r"\bM[1-6]\b|phase \d|stage \d|batch \d|milestone", "规划词汇:按语言域名称描述改动"),
    (r"^\s*(improve|clean up|enhance|polish|optimize) (the )?(code|things|stuff)", "空泛评价:说机制变化"),
)


def is_section_label(line: str) -> bool:
    for label in SECTION_LABELS:
        if re.match(rf"^{label}\s*[:：]", line, re.IGNORECASE):
            return True
    return False


def main(argv) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    with open(argv[1], encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    while lines and not lines[-1].strip():
        lines.pop()

    violations = []
    advisories = []

    if not lines:
        print("违规: 说明为空")
        return 1

    subject = lines[0]

    # 机器生成的说明(Merge/Revert/fixup)不按本规范判。
    if subject.startswith(("Merge ", "Revert ", "fixup!", "squash!")):
        print(f"跳过(机器生成的说明): {subject[:40]}")
        return 0

    head = re.match(r"^([a-z]+)(\(([^)]*)\))?: (\S.*)$", subject)
    if head is None:
        violations.append((1, "标题行不合 <type>(<scope>): <subject>", "按规范 §1 结构重写"))
    else:
        if head.group(1) not in TYPES:
            violations.append((1, f"type `{head.group(1)}` 不在九类枚举内", "见规范 §2 取 type 表"))
        if subject.endswith("."):
            violations.append((1, "标题以句号收尾", "删句号"))
        first_word = head.group(4).split()[0].strip("`(,").lower()
        if first_word in PAST_TENSE_VERBS:
            violations.append((1, f"标题用了完成体 `{first_word}`", "改祈使句动词原形(规范 §1)"))
        subject_width = len(head.group(4))
        if subject_width > SUBJECT_WIDTH_SOFT:
            advisories.append(f"标题 {subject_width} 列,超软目标 {SUBJECT_WIDTH_SOFT},细节宜移入正文")

    body = lines[1:]
    if body and body[0].strip():
        violations.append((2, "标题行与正文之间缺空行", "加一个空行"))

    body_lines = [line for line in body if line.strip()]
    tests_lines = [line for line in body_lines if line.startswith(TESTS_LABEL)]
    prose_lines = [line for line in body_lines if not line.startswith(TESTS_LABEL)]

    if len(prose_lines) > BODY_LINES_MAX:
        violations.append((None, f"正文 {len(prose_lines)} 行 > {BODY_LINES_MAX} 行", "压到 1~3 句散文;长推导归 reference 文档"))
    if len(tests_lines) > 1:
        violations.append((None, f"`{TESTS_LABEL}` 行多于一行", "只留一行"))
    elif tests_lines and body_lines[-1] is not tests_lines[0]:
        violations.append((None, f"`{TESTS_LABEL}` 不在末行", "把该行移到说明末尾"))
    if tests_lines and not tests_lines[0][len(TESTS_LABEL):].strip():
        violations.append((None, f"`{TESTS_LABEL}` 行为空", "写实际跑过的命令与结果,或整行删掉"))

    # 逐行检查:ASCII-only、列宽、禁词、正文形态。
    for number, line in enumerate(lines, 1):
        bad_chars = sorted({ch for ch in line if ord(ch) > 0x7E})
        if bad_chars:
            shown = " ".join(f"{ch!r}(U+{ord(ch):04X})" for ch in bad_chars[:4])
            violations.append((number, f"含非 ASCII 字符 {shown}", "说明一律英文 ASCII-only:禁 CJK/全角标点/em-dash(规范 §1)"))
        if len(line) > WIDTH_MAX:
            violations.append((number, f"列宽 {len(line)} > {WIDTH_MAX}", "按 72 列折行"))
        for pattern, advice in BANNED_PATTERNS:
            hit = re.search(pattern, line, re.IGNORECASE)
            if hit is not None:
                violations.append((number, f"命中禁写内容 {hit.group(0)!r}", advice))
        if number > 1 and line.strip():
            if is_section_label(line):
                violations.append((number, "正文用了段标签(旧三段骨架)", "正文写散文,不分段标签(规范 §3)"))
            if re.match(r"^\s*[-*+]\s", line):
                violations.append((number, "正文出现列表项", "正文写散文,不列 bullet(规范 §1)"))

    for number, reason, advice in violations:
        where = f"第 {number} 行" if number else "整体"
        print(f"违规 {where}: {reason} -- {advice}")
    for note in advisories:
        print(f"提示: {note}")

    if violations:
        print(f"\n共 {len(violations)} 处违规,未通过。")
        return 1

    print(f"通过: {len(lines)} 行,无 ASCII/列宽/形状/禁写内容违规。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
