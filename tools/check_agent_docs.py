#!/usr/bin/env python3
"""agent 文档防漂移检查器。

三件事：
  1. 文档里写出的仓库路径必须存在（`src/...`、`.agents/...`、`docs/...` 等）。
  2. 反引号里的 `Foo::bar` 必须能在 src/ 或 tests/ 里找到（防类/函数改名后文档残留）。
  3. reference/ 与 notes/ 不得出现落地记录与计划词汇（已落地 / 待实现 / 日期等）。

用法：python3 tools/check_agent_docs.py [根目录...]   # 缺省扫 .agents 与 AGENTS.md
退出码：0 = 通过；1 = 有违规；2 = 用法错误。
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 文档正文里可以出现的仓库路径（不含通配与占位符）。
PATH_PATTERN = re.compile(
    r"(?:\.agents|docs|tests|tools|bench|src)/[A-Za-z0-9_./-]*\.(?:md|txt|py|sh|hpp|cpp|yml|toml)"
)
# `Qualifier::member` 形式的符号引用。
SYMBOL_PATTERN = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)`")
# 不需要在 src/ 里核对的限定名（标准库 / 平台 / 局部命名空间）。
SKIP_QUALIFIERS = {
    "std", "fs", "utf8", "io", "str", "mem", "util", "aria", "detail",
    "filesystem", "chrono", "ranges", "views", "numbers", "format", "fmt", "gtest", "testing",
}
# 落地记录与计划词汇：只扫 reference/ 与 notes/（skills 会引用这些词作反例）。
BANNED = [
    ("已落地", "落地记录"),
    ("已加入", "落地记录"),
    ("已启用", "落地记录"),
    ("已采纳", "落地记录"),
    ("待实现", "计划词汇"),
    ("留待后续", "计划词汇"),
    ("留作后续", "计划词汇"),
    ("将来若", "计划词汇"),
    ("本批", "批次词汇"),
]
BANNED_DATE = re.compile(r"\b20\d\d-\d\d(-\d\d)?\b")
BANNED_BATCH = re.compile(r"\b(?:batch|phase|批)\s*#?\d+\b|\b[MPBT]\d-[A-Z]?\d+\b")


def tracked_text_files():
    """返回纳入检查的文档清单（相对 ROOT）。"""
    out = subprocess.run(
        ["git", "ls-files", ".agents", "AGENTS.md"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.split()
    return [path for path in out if path.endswith(".md")]


def symbol_corpus():
    """把 src/ 与 tests/ 的源码拼成一个可搜索的大串（小写无关，只做存在性判断）。"""
    files = subprocess.run(
        ["git", "ls-files", "src", "tests"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.split()
    chunks = []
    for path in files:
        if path.endswith((".hpp", ".cpp")):
            chunks.append((ROOT / path).read_text(encoding="utf-8", errors="ignore"))
    return "\n".join(chunks)


def _has_token(corpus, name):
    return re.search(r"\b" + re.escape(name) + r"\b", corpus) is not None


def check(path, source, corpus):
    problems = []
    for lineno, line in enumerate(source.split("\n"), start=1):
        for match in PATH_PATTERN.finditer(line):
            target = match.group(0)
            if any(ch in target for ch in "*<>") or target.endswith("/"):
                continue
            if not (ROOT / target).exists():
                problems.append((path, lineno, "missing-path", target))
        for match in SYMBOL_PATTERN.finditer(line):
            qualifier, member = match.groups()
            if qualifier in SKIP_QUALIFIERS or qualifier.startswith("Xxx"):
                continue
            # 只判「限定名与成员名各自还存在于源码」：足以抓改名/删类，且不误报字段与模板实例。
            if not _has_token(corpus, qualifier) or not _has_token(corpus, member):
                problems.append((path, lineno, "unknown-symbol", f"{qualifier}::{member}"))
        if not path.startswith((".agents/reference/", ".agents/notes/")):
            continue
        for word, kind in BANNED:
            if word in line:
                problems.append((path, lineno, kind, word))
        for match in BANNED_DATE.finditer(line):
            problems.append((path, lineno, "落地日期", match.group(0)))
        for match in BANNED_BATCH.finditer(line):
            problems.append((path, lineno, "批次编号", match.group(0)))
    return problems


def main(argv):
    if argv[1:]:
        targets = []
        for arg in argv[1:]:
            path = Path(arg)
            if path.is_dir():
                targets.extend(str(p.relative_to(ROOT)) for p in path.rglob("*.md"))
            else:
                targets.append(str(path.relative_to(ROOT)) if path.is_absolute() else arg)
    else:
        targets = tracked_text_files()

    corpus = symbol_corpus()
    all_problems = []
    for path in targets:
        source = (ROOT / path).read_text(encoding="utf-8", errors="ignore")
        all_problems.extend(check(path, source, corpus))

    for path, lineno, kind, detail in all_problems:
        print(f"{path}:{lineno}: [{kind}] {detail}")
    if all_problems:
        print(f"\n共 {len(all_problems)} 处违规")
        return 1
    print(f"agent 文档检查通过（{len(targets)} 个文件）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
