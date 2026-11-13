#!/usr/bin/env python3
"""agent 文档防漂移检查器。

检查 Markdown 相对链接的目标路径，以及 agent 文档的仓库路径、标识符和禁词。
`Foo::bar` 只检查两个标识符是否分别出现在源码中，不验证成员归属。

用法：python3 tools/check_agent_docs.py [--staged] [路径...]
默认检查工作树里的已跟踪文档（AGENTS.md、.agents、README.md、docs 与语料 README）。
--staged 从 Git 暂存区读取文件和链接目标，供 pre-commit 使用。
退出码：0 = 通过；1 = 有违规；2 = 用法错误。
"""
import argparse
import os
import posixpath
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent.parent
DOC_PATHS = ("AGENTS.md", ".agents", "README.md", "docs", "tests/language/README.md")

# 文档正文里可以出现的仓库路径（不含通配与占位符）。
PATH_PATTERN = re.compile(
    r"(?<![A-Za-z0-9_./-])(?:\.agents|docs|tests|tools|bench|src)/"
    r"[A-Za-z0-9_./-]*\.(?:md|txt|py|sh|hpp|cpp|yml|toml)(?![A-Za-z0-9_./-])"
)
# 常用的行内 Markdown 链接，支持尖括号包住带空格的目标。不校验锚点或外链。
LINK_PATTERN = re.compile(r"(?<!\\)\[[^\]\n]*\]\(\s*(?:<([^>\n]+)>|([^\s()]+))(?:\s|\))")
INLINE_CODE = re.compile(r"(`+)[^`].*?\1(?!`)")
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")
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


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, stdout=subprocess.PIPE, check=True).stdout


def tracked_paths():
    return {os.fsdecode(path) for path in git("ls-files", "-z").split(b"\0") if path}


def under(path, base):
    return base == "." or path == base or path.startswith(base + "/")


def repo_relative(path):
    # 只做词法归一化；工作树中的软链不能改变暂存区里的路径。
    return Path(os.path.abspath(path)).relative_to(ROOT).as_posix()


def relative_links(line, path):
    for match in LINK_PATTERN.finditer(INLINE_CODE.sub("", line)):
        target = match.group(1) or match.group(2)
        if target.startswith(("/", "#", "?")) or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
            continue
        link_path = unquote(urlsplit(target).path)
        if link_path:
            yield target, posixpath.normpath(posixpath.join(posixpath.dirname(path), link_path))


def check(path, source, identifiers, exists):
    problems = []
    is_agent_doc = under(path, ".agents") or path == "AGENTS.md"
    fence = ""
    for lineno, line in enumerate(source.splitlines(), start=1):
        marker = FENCE.match(line)
        if marker:
            run, tail = marker.groups()
            if not fence:
                fence = run
            elif run.startswith(fence) and not tail.strip():
                fence = ""
        elif not fence:
            for target, resolved in relative_links(line, path):
                if not exists(resolved):
                    problems.append((path, lineno, "missing-link", target))
        if not is_agent_doc:
            continue
        for match in PATH_PATTERN.finditer(line):
            target = match.group(0)
            if not exists(target):
                problems.append((path, lineno, "missing-path", target))
        for match in SYMBOL_PATTERN.finditer(line):
            qualifier, member = match.groups()
            if qualifier in SKIP_QUALIFIERS or qualifier.startswith("Xxx"):
                continue
            if qualifier not in identifiers or member not in identifiers:
                problems.append((path, lineno, "missing-identifier", f"{qualifier}::{member}"))
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--staged", action="store_true", help="检查 Git 暂存快照")
    parser.add_argument("paths", nargs="*", help="Markdown 文件或目录")
    args = parser.parse_args()
    tracked = tracked_paths()
    indexed_paths = tracked | {parent.as_posix() for path in tracked for parent in PurePosixPath(path).parents}

    def exists(path):
        if args.staged:
            return path in indexed_paths
        return path != ".." and not path.startswith(("../", "/")) and (ROOT / path).exists()

    def read(path):
        if args.staged:
            return git("show", ":" + path).decode("utf-8", errors="replace")
        return (ROOT / path).read_text(encoding="utf-8", errors="replace")

    targets = set()
    if args.paths:
        for arg in args.paths:
            try:
                path = repo_relative(arg)
            except ValueError:
                parser.error(f"路径不在仓库内：{arg}")
            if not exists(path):
                parser.error(f"路径不存在：{arg}")
            if args.staged:
                targets.update(p for p in tracked if under(p, path))
            elif (ROOT / path).is_dir():
                targets.update(repo_relative(p) for p in (ROOT / path).rglob("*.md"))
            else:
                targets.add(path)
    else:
        targets.update(path for path in tracked if any(under(path, base) for base in DOC_PATHS))
    targets = sorted(path for path in targets if path.endswith(".md") and exists(path))

    identifiers = set()
    for path in tracked:
        if path.startswith(("src/", "tests/")) and path.endswith((".hpp", ".cpp")) and exists(path):
            identifiers.update(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", read(path)))
    all_problems = [problem for path in targets for problem in check(path, read(path), identifiers, exists)]

    for path, lineno, kind, detail in all_problems:
        print(f"{path}:{lineno}: [{kind}] {detail}")
    if all_problems:
        print(f"\n共 {len(all_problems)} 处违规")
        return 1
    print(f"agent 文档检查通过（{len(targets)} 个文件）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
