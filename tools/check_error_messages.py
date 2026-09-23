#!/usr/bin/env python3
"""报错文案机械检查器(规范见 .claude/reference/error-message-style.md)。

用法:
    python3 tools/check_error_messages.py [路径...]     # 默认 src/

只查机器能判的项:
  1. ASCII-only  -- src/ 下所有字符串字面量不得含非 ASCII 字符(禁 CJK 与全角标点)。
  2. 报错入口格式串 -- 出现在报错入口调用上的首个字面量,不得以句号结尾、不得大写起首。
  3. ASSERT 文案 -- 不得以「封闭实体身份」前缀起首(宏已打 [file:line] 与 %s() 函数名)。

入口白名单(这些调用的字面量是用户可见文案):
    fail( / raise( / fatal_error( / Error::from_detail( / Error::make_message( /
    io::println(stderr, / io::print(stderr,

语义判断(句式家族是否用对、措辞是否漂移)脚本判不出,仍靠人读规范。有违规打印
file:line 与改法并返回 1。

实现要点:先剥注释、字符字面量与字符串字面量正文(三者都换成等长空格),再判:
字面量所在调用经「往回找配对左括号 + 读括号前的限定名」得到(故跨行实参也认得出),
行级模式作兜底(抓经 std::format 等中转的嵌套格式串)。不剥注释会把中文注释整片报成
违规(CJK 注释是本仓注释规范),不剥字面量正文会让文案里出现的入口名误命中。原始
字符串 R"(...)" 一并支持。
"""

import os
import re
import sys

# 报错入口调用名:命中即该字面量按「文案」标准判。
ENTRY_PATTERNS = (
    "fail(",
    "raise(",
    "fatal_error(",
    "Error::from_detail(",
    "Error::make_message(",
    "io::println(stderr,",
    "io::print(stderr,",
)

# 报错入口的「调用名」形态(经所在调用识别,覆盖跨行实参)。
MESSAGE_CALLEES = ("fail", "raise", "fatal_error", "from_detail", "make_message", "error")

# ASSERT 入口:其文案不得重复宏已打印的文件/行/函数名。
ASSERT_CALLEES = ("ASSERT",)
ASSERT_PATTERNS = ("ASSERT(",)

# 「封闭实体身份」前缀:Class:: / Class: / Class::method -- 正是宏的 %s() 与位置已给的。
IDENTITY_PREFIX = re.compile(r"^[A-Za-z_]\w*(::[A-Za-z_]\w*)?(: |::)")

# 允许的字面量首字符:小写字母、数字,以及非字母的包装符号(此时大小写规则不适用)。
LEAD_OK = set("{[(\"'%<*") | set("0123456789")

SOURCE_SUFFIXES = (".cpp", ".hpp")

# 转义序列在正文里的占位符:整体按一个非字母字符记(判定首尾字符用)。
ESCAPE_MARK = "\x00"


class Literal:
    """一个字符串字面量:所在行 + 正文(转义序列压成单字符占位)+ 净化文本里的起偏移。"""

    __slots__ = ("line", "pos", "raw", "body")

    def __init__(self, line, pos, raw, body):
        self.line = line
        self.pos = pos
        self.raw = raw
        self.body = body



def scan(src):
    """剥注释/字符字面量/字符串正文,返回 (净化文本, 字面量清单)。"""
    out = []
    literals = []
    index = 0
    line = 1
    size = len(src)
    while index < size:
        ch = src[index]
        if ch == "\n":
            line += 1
            out.append(ch)
            index += 1
            continue
        if src.startswith("//", index):
            while index < size and src[index] != "\n":
                out.append(" ")
                index += 1
            continue
        if src.startswith("/*", index):
            while index < size and not src.startswith("*/", index):
                out.append("\n" if src[index] == "\n" else " ")
                if src[index] == "\n":
                    line += 1
                index += 1
            out.append("  ")
            index += 2
            continue
        # 字符字面量(紧跟在字母数字后的是 C++14 数字分隔符,不在此列)。
        if ch == "'" and (index == 0 or not src[index - 1].isalnum()):
            out.append(" ")
            index += 1
            while index < size and src[index] != "'":
                if src[index] == "\\" and index + 1 < size:
                    out.append("  ")  # 转义占 2 字符:替换须等长(偏移可复用)
                    index += 2
                    continue
                if src[index] == "\n":
                    line += 1
                out.append("\n" if src[index] == "\n" else " ")
                index += 1
            out.append(" ")
            index += 1
            continue
        # 字符串字面量(含 u8/u/U/L/R 前缀与 R"..." 原始形态)。
        prefix_len = string_prefix_len(src, index)
        if prefix_len >= 0:
            start_index = index
            literal, index, line = read_literal(src, index, prefix_len, line)
            literals.append(literal)
            out.append(" " * (index - start_index))
            continue
        out.append(ch)
        index += 1
    return "".join(out), literals


def string_prefix_len(src, index):
    """若 index 处是字符串字面量的起首,返回其前缀长度(无前缀为 0);否则返回 -1。"""
    for prefix in ("u8R", "uR", "UR", "LR", "u8", "u", "U", "L", "R", ""):
        end = index + len(prefix)
        if src.startswith(prefix, index) and end < len(src) and src[end] == '"':
            return len(prefix)
    return -1


def read_literal(src, index, prefix_len, line):
    """读出一个字符串字面量,返回 (Literal, 新的 index, 新的行号)。"""
    start_line = line
    start_pos = index
    index += prefix_len
    if prefix_len > 0 and src[index - 1] == "R":  # 原始字符串 R"delim(...)delim"
        cursor = index + 1
        delim_end = src.index("(", cursor)
        close = ")" + src[cursor:delim_end] + '"'
        body_start = delim_end + 1
        body_end = src.index(close, body_start)
        line += src.count("\n", index, body_end + len(close))
        raw = src[body_start:body_end]
        return Literal(start_line, start_pos, raw, raw), body_end + len(close), line
    index += 1  # 跳过开引号
    raw = []
    body = []
    while index < len(src):
        ch = src[index]
        if ch == "\\":
            raw.append(ch)
            raw.append(src[index + 1])
            body.append(ESCAPE_MARK)
            index += 2
            continue
        if ch == '"':
            index += 1
            break
        if ch == "\n":
            line += 1
        raw.append(ch)
        body.append(ch)
        index += 1
    return Literal(start_line, start_pos, "".join(raw), "".join(body)), index, line


def enclosing_callee(clean, pos):
    """经「往回找配对左括号 + 读括号前的限定名」得到字面量所在调用的名字(跨行实参也认得出)。

    找不到(如裸字面量、宏体)返回空串。只取末段(如 `io::println` -> `println`)。
    """
    depth = 0
    index = pos - 1
    while index >= 0:
        ch = clean[index]
        if ch == ")":
            depth += 1
        elif ch == "(":
            if depth == 0:
                break
            depth -= 1
        index -= 1
    if index < 0:
        return ""
    end = index
    index -= 1  # 配对 '(' 停在 index,限定名在其左侧
    while index >= 0 and (clean[index].isalnum() or clean[index] in "_:"):
        index -= 1
    return clean[index + 1:end].rsplit("::", 1)[-1]


def check_file(path):
    """返回该文件的违规清单 [(line, rule, detail)]。"""
    with open(path, encoding="utf-8") as handle:
        src = handle.read()
    clean, literals = scan(src)
    assert len(clean) == len(src), "净化文本须与原文等长(偏移才可复用)"
    clean_lines = clean.split("\n")
    first_on_line = {}
    for literal in literals:
        first_on_line.setdefault(literal.line, literal)
    problems = []
    for literal in literals:
        bad = sorted({ch for ch in literal.raw if ord(ch) > 0x7E})
        if bad:
            shown = "".join("U+%04X " % ord(ch) for ch in bad[:6]).strip()
            problems.append((literal.line, "non-ascii", "含非 ASCII 字符(%s): %s" % (shown, "".join(bad[:6]))))
        callee = enclosing_callee(clean, literal.pos)
        if first_on_line.get(literal.line) is not literal:
            continue
        source_line = clean_lines[literal.line - 1] if literal.line - 1 < len(clean_lines) else ""
        # 规则 3:ASSERT 文案不重复宏已打的位置与函数名。
        is_assert = callee in ASSERT_CALLEES or any(pattern in source_line for pattern in ASSERT_PATTERNS)
        if is_assert:
            if IDENTITY_PREFIX.match(literal.body):
                problems.append((literal.line, "assert-identity-prefix",
                                 "ASSERT 文案不得重复宏已打的文件/行/函数名: \"%s\"" % literal.raw))
            continue
        # 规则 2:报错入口格式串。
        is_message_entry = callee in MESSAGE_CALLEES or any(pattern in source_line for pattern in ENTRY_PATTERNS)
        if not is_message_entry:
            continue
        if literal.body.endswith("."):
            problems.append((literal.line, "trailing-period", "文案不得以 '.' 结尾: \"%s\"" % literal.raw))
        if literal.body and literal.body[0] not in LEAD_OK and literal.body[0].isupper():
            problems.append((literal.line, "leading-uppercase", "文案首字母须小写: \"%s\"" % literal.raw))
    return problems


def main(argv):
    roots = argv[1:] or ["src"]
    targets = []
    for root in roots:
        if os.path.isfile(root):
            targets.append(root)
            continue
        for base, _, names in os.walk(root):
            targets.extend(os.path.join(base, name) for name in sorted(names) if name.endswith(SOURCE_SUFFIXES))
    targets.sort()
    total = 0
    for path in targets:
        for line, rule, detail in check_file(path):
            print("%s:%d: %s: %s" % (path, line, rule, detail))
            total += 1
    if total:
        print("\n共 %d 处违规 -- 规范见 .claude/reference/error-message-style.md" % total)
        return 1
    print("报错文案机械检查通过(%d 个文件)" % len(targets))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
