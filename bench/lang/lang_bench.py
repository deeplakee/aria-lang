#!/usr/bin/env python3
"""aria 源文件级性能基准驱动(语料与写作规范见同目录 README.md)。

用法:
    python3 bench/lang/lang_bench.py [--aria=PATH] [--trials=N] [--warmup=N] [--filter=SUBSTR]
                                     [--langs=python3,node,lua,java] [--format=text|md]
                                     [--list] [--timeout=SEC]

测什么:把 bench/lang/**.aria 当真实程序**逐个起进程**跑(等价手工 `./build/rel/aria xx.aria`),
每脚本预热 `--warmup` 次(丢)后重复 `--trials` 次,**报平均值 ± 标准差**并列出最小值(pyperf /
MicroPython run-perfbench 都用平均;CLBG / Ruby benchmark-driver 默认用最小值,故最小值并列供对照)。
脚本自身用内置 `clock()` 自测负载段,经 stdout 的 `bench-time: <n> ms` 标记行报出,于是每个读数分两列:

    负载段     = 纯负载(脚本 clock() 自测;地板行无此列)
    进程总耗时 = 进程启动 + 词法/编译 + VM bootstrap + 负载(即"直接跑这个文件"的耗时)

两者之差是该脚本的启动+编译开销。**跨语言对照一律看负载段**:总耗时把各运行时的启动成本混了进来
(各运行时启动成本量级见 .claude/reference/bench/lang-bench-notes.md §1),只有负载段是同一份负载的读数。

输出:测量期间进度打 stderr,测完在 stdout 打整份报告--速览(每门对照语言领先/落后多少行)、aria
自身的绝对值表、跨语言对照表(每门语言的绝对耗时 + 相对 aria 的倍数)、值一致性门禁,末尾是固定列宽
的 `[summary]` 原始数字块(含 sd 与 min,`diff` 两次构建即 A/B)。故 `> 文件` 存下来的是一份没有进度
噪音的报告。报告默认 text 格式(终端定宽表),`--format=md` 出 markdown 表(贴进文档 / PR 用),
两种格式是同一份数据的两种渲染。

对照语言按 PATH 探测(缺谁不出哪一列):CPython(当前解释器)、Node、Lua、Java(需 javac,编译产物
落在 build/bench-java/,每次运行编译一次)。**值一致性门禁**:每个脚本(含各语言端口)都要打印
`value <键>: <数>` 行,驱动逐语言比对,任一键的读数不同即判该行失败--这是"同算法同规模"的机械保证。

数字只在实际发布的那份二进制上有意义:按常规 `-DCMAKE_BUILD_TYPE=Release` 构建即可(LTO 默认开、
`-O3 -DNDEBUG`),别为跑基准关掉优化开关--对照语言也都在各自的发行配置下跑。重复运行/跨构建的抖动
量级见 lang-bench-notes.md §1。任一行失败(编译/运行/断言/值不一致)
则本程序非零退出。
"""

import argparse
import re
import shutil
import statistics
import subprocess
import sys
import time
import unicodedata
from pathlib import Path

# 脚本自报内部耗时用的标记行(各语言同形,见同目录 README.md「脚本写作约定」)。
MARKER_TIME = re.compile(r"bench-time:\s*([0-9]+(?:\.[0-9]+)?)")
# 脚本自报校验量用的标记行:驱动逐语言比对,是"同算法同规模"的机械保证。
MARKER_VALUE = re.compile(r"^value\s+([A-Za-z0-9_]+):\s*(-?[0-9]+)\s*$", re.MULTILINE)
# Java 端口声明的类名(文件名不必与类名一致:非 public 类合法,见同目录 README.md「脚本写作约定」)。
JAVA_CLASS = re.compile(r"\bclass\s+(\w+)")

CORPUS_DIR = Path(__file__).resolve().parent  # features/ 与 workloads/ 就在驱动旁边
JAVA_CACHE = Path(__file__).resolve().parents[2] / "build" / "bench-java"
JAVA_LONG_OPT = "-Xss16m"  # 递归类基准的 JVM 栈余量
JAVA_HEAP_OPT = "-Xmx2g"  # word_count / sort_int 一类大表负载的堆余量

# 报告格式:终端定宽表 / markdown 表(--format)。
TEXT = "text"
MARKDOWN = "md"


def build_lang_specs():
    """对照语言表:探测可执行 -> 端口扩展名 -> 该端口的运行命令。"""
    return {
        "python3": {"probe": sys.executable, "ext": ".py",
                    "command": lambda path: [sys.executable, str(path)]},
        "node": {"probe": "node", "ext": ".js",
                 "command": lambda path: ["node", str(path)]},
        "lua": {"probe": "lua", "ext": ".lua",
                "command": lambda path: ["lua", str(path)]},
        # Java 是编译型:先 javac 到缓存目录,再 java 起类名;命令在 prepare 阶段才拿得到。
        "java": {"probe": "javac", "ext": ".java", "command": None},
    }


def prepare_java(path):
    """编译 Java 端口到缓存目录,返回 (运行命令, 失败说明)。文件名与类名不必一致(非 public 类)。"""
    text = path.read_text(encoding="utf-8", errors="replace")
    found = JAVA_CLASS.search(text)
    if not found:
        return None, "端口里找不到 class 声明"
    class_name = found.group(1)
    JAVA_CACHE.mkdir(parents=True, exist_ok=True)
    compiled = subprocess.run(["javac", "-d", str(JAVA_CACHE), str(path)],
                              capture_output=True, text=True, timeout=300)
    if compiled.returncode != 0:
        tail = (compiled.stderr or compiled.stdout).strip().splitlines()
        return None, f"javac 失败: {tail[0] if tail else '(无输出)'}"
    return ["java", JAVA_LONG_OPT, JAVA_HEAP_OPT, "-cp", str(JAVA_CACHE), class_name], None


def find_aria(explicit):
    """定位 aria 可执行:命令行优先,其次仓库约定的两个构建目录,最后 PATH。"""
    if explicit:
        return Path(explicit)
    for candidate in ("build/rel/aria", "build/aria"):
        if Path(candidate).exists():
            return Path(candidate)
    found = shutil.which("aria")
    return Path(found) if found else None


def discover(filter_text):
    """递归收集语料并按相对路径排序(用例集确定;lib 目录只作辅助模块,不单独成行)。"""
    cases = []
    for path in sorted(CORPUS_DIR.rglob("*.aria")):
        if "lib" in path.parts:
            continue
        rel = path.relative_to(CORPUS_DIR).as_posix()
        if filter_text and filter_text not in rel:
            continue
        cases.append((rel, path))
    return cases


def ports_for(script_path, lang_names, specs):
    """返回该脚本可用的对照端口 [(语言名, 端口路径)]:文件存在且工具链在 PATH 上。"""
    ports = []
    for name in lang_names:
        sidecar = script_path.with_suffix(specs[name]["ext"])
        if sidecar.exists() and shutil.which(specs[name]["probe"]):
            ports.append((name, sidecar))
    return ports


def run_trials(command, trials, warmup, timeout_seconds):
    """预热 warmup 次(丢)后跑 trials 次:返回 (总耗时样本, 内部耗时样本, 值表, 失败说明)。

    两列都保留全部样本,由调用方算平均值 ± 标准差。值表取任一轮的 value 行(各轮相同)。
    """
    totals, inners, values = [], [], {}
    for index in range(warmup + trials):
        begin = time.perf_counter()
        try:
            proc = subprocess.run(command, capture_output=True, text=True, timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            return None, None, None, f"超时(>{timeout_seconds}s)"
        elapsed = (time.perf_counter() - begin) * 1000.0
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout).strip().splitlines()
            tail = detail[-1] if detail else "(无输出)"
            return None, None, None, f"退出码 {proc.returncode}: {tail}"
        if index < warmup:
            continue
        totals.append(elapsed)
        found = MARKER_TIME.search(proc.stdout)
        if found:
            inners.append(float(found.group(1)))
        values = {key: int(number) for key, number in MARKER_VALUE.findall(proc.stdout)}
    return totals, inners, values, None


def mean_and_sd(samples):
    if not samples:
        return None, None
    mean = statistics.fmean(samples)
    sd = statistics.stdev(samples) if len(samples) > 1 else 0.0
    return mean, sd


def progress(text):
    """测量进度打 stderr:报告在测量结束后整体输出,stdout 重定向到文件即得一份干净的表。"""
    print(text, file=sys.stderr, flush=True)


class Sample:
    """一个脚本在一门语言下的读数(ms):总耗时与负载段各自的平均 ± 标准差,加总耗时最小值。"""

    def __init__(self, totals, inners):
        self.total_mean, self.total_sd = mean_and_sd(totals)
        self.total_min = min(totals)
        self.inner_mean, self.inner_sd = mean_and_sd(inners)

    def startup(self):
        """启动+编译 = 进程总耗时 - 负载段;无负载段(地板行)时为 None。"""
        return None if self.inner_mean is None else self.total_mean - self.inner_mean

    def jitter(self):
        """重复运行的相对抖动(总耗时标准差 / 平均),百分比。"""
        return self.total_sd / self.total_mean * 100.0


def display_name(rel):
    """表里的基准名:去掉组前缀与 .aria 后缀(组由小节标题区分)。"""
    stem = rel.split("/", 1)[1] if "/" in rel else rel
    return stem[:-len(".aria")] if stem.endswith(".aria") else stem


def group_rows(cases, results):
    """[(组名, [(显示名, 相对路径, {语言: Sample})])],保持语料顺序,只收测到结果的脚本。"""
    groups = []
    for rel, _path in cases:
        if rel not in results:
            continue
        label = "功能细项" if rel.startswith("features/") else "真实负载"
        if not groups or groups[-1][0] != label:
            groups.append((label, []))
        groups[-1][1].append((display_name(rel), rel, results[rel]))
    return groups


def comparable_ms(sample, base):
    """该样本在这一行里的读数(ms):两边都有负载段就用负载段,否则退回进程总耗时(与 ratio 同口径)。"""
    if base.inner_mean and sample.inner_mean:
        return sample.inner_mean
    return sample.total_mean


def ratio(port, base):
    """该语言 ÷ aria:优先按负载段(跨语言唯一可比的一列),整行无负载段时退回进程总耗时。"""
    return comparable_ms(port, base) / comparable_ms(base, base)


def display_width(text):
    """终端里的显示宽度:CJK / 全角字符占两列。表头是中文,按 len() 对齐会整体歪掉。"""
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in text)


def print_table(headers, rows, aligns):
    """定宽表:列宽取表头与各行的最大显示宽度,列间两空格;aligns 每列 'l' 或 'r'。"""
    widths = [display_width(text) for text in headers]
    for row in rows:
        for index, text in enumerate(row):
            widths[index] = max(widths[index], display_width(text))

    def line(cells):
        padded = []
        for index, text in enumerate(cells):
            gap = widths[index] - display_width(text)
            padded.append(" " * gap + text if aligns[index] == "r" else text + " " * gap)
        print("  ".join(padded).rstrip())

    line(headers)
    for row in rows:
        line(row)


def strip_code(text):
    """text 模式去掉行内代码的反引号(markdown 模式保留):终端里反引号只是噪音。"""
    return text.replace("`", "")


def render(blocks, style):
    """把块渲染成终端文本或 markdown:块由下面各 report_* 生成,两种风格共用同一份数据。

    markdown 各块之间一律留空行(表必须与上一段隔开才会被解析成表),故 md 模式忽略 blank 块;
    text 模式的空行由 blank 块显式给出。
    """
    previous = None
    for kind, payload in blocks:
        if kind == "blank":
            if style != MARKDOWN:
                print()
            continue
        if style == MARKDOWN:
            if previous is not None and not (previous == "bullet" and kind == "bullet"):
                print()
        previous = kind
        if kind == "title":
            print(f"# {payload}" if style == MARKDOWN else payload)
        elif kind == "bullet":
            print(f"- {payload}" if style == MARKDOWN else strip_code(payload))
        elif kind == "heading":
            print(f"## {payload}" if style == MARKDOWN else f"-- {payload} --")
        elif kind == "subheading":
            print(f"### {payload}" if style == MARKDOWN else f"{payload}:")
        elif kind == "paragraph":
            print(payload if style == MARKDOWN else strip_code(payload))
        elif kind == "table":
            headers, rows, aligns = payload
            if style == MARKDOWN:
                print("| " + " | ".join(headers) + " |")
                print("| " + " | ".join("---:" if align == "r" else "---" for align in aligns) + " |")
                for row in rows:
                    print("| " + " | ".join(row) + " |")
            else:
                print_table(headers, rows, aligns)
        elif kind == "code":
            if style == MARKDOWN:
                print("```text")
            for line in payload:
                print(line)
            if style == MARKDOWN:
                print("```")


def report_meta(aria, stamp, cases, args, lang_names):
    return [
        ("title", "aria 源文件级性能基准(整程序进程级计时)"),
        ("bullet", f"aria: `{aria}`(构建于 {stamp});语料 `bench/lang`,{len(cases)} 个脚本。"),
        ("bullet", f"对照语言: {'/'.join(lang_names)}。"),
        ("bullet", f"口径:每脚本预热 {args.warmup} 轮后跑 {args.trials} 轮取平均;负载段 = 脚本内 "
                   f"`clock()` 自测的纯负载,进程总耗时 = 直接跑该文件(启动 + 编译 + 负载)。"),
        ("blank", None),
    ]


def report_verdict(groups, lang_names):
    """速览:先给结论--每门对照语言在两组语料上分别领先/落后多少行,中位倍数多少。"""
    headers = ["对照语言"] + [label for label, _entries in groups]
    rows = {name: [name] for name in lang_names}
    for _label, entries in groups:
        comparable = []
        for _name, _rel, samples in entries:
            if "aria" not in samples or not samples["aria"].inner_mean:
                continue
            for lang in lang_names:
                if lang in samples and samples[lang].inner_mean:
                    comparable.append((lang, ratio(samples[lang], samples["aria"])))
        for lang in lang_names:
            values = [value for name, value in comparable if name == lang]
            if not values:
                rows[lang].append("-")
                continue
            # 比值 = 该语言 ÷ aria:> 1 才是 aria 更快(对方花的时间更多)。
            aria_faster = sum(1 for value in values if value > 1.0)
            median = statistics.median(values)
            rows[lang].append(f"aria 更快 {aria_faster} / 更慢 {len(values) - aria_faster} 行,"
                              f"中位 {median:.2f}x")
    return [
        ("heading", "一、速览"),
        ("paragraph", "只统计有该语言端口、且双方都有负载段的行;中位倍数 =「该语言负载段 ÷ aria 负载段」"
                      "在可比行上的中位数,> 1 表示该语言整体比 aria 慢。"),
        ("table", (headers, [rows[name] for name in lang_names], ["l"] * len(headers))),
        ("blank", None),
    ]


def report_aria_table(groups):
    """aria 自身:绝对值表(负载段 / 进程总耗时 / 启动+编译 / 抖动),这是「直接跑一个 .aria」的读数。"""
    blocks = [("heading", "二、aria 自身")]
    headers = ["基准", "负载段 ms", "进程总耗时 ms", "启动+编译 ms", "抖动"]
    aligns = ["l", "r", "r", "r", "r"]
    for label, entries in groups:
        rows = []
        for name, _rel, samples in entries:
            aria = samples["aria"]
            inner = "-" if aria.inner_mean is None else f"{aria.inner_mean:.1f}"
            startup = "-" if aria.startup() is None else f"{aria.startup():.1f}"
            rows.append([name, inner, f"{aria.total_mean:.1f}", startup, f"{aria.jitter():.1f}%"])
        blocks += [("subheading", label), ("table", (headers, rows, aligns))]
    blocks.append(("blank", None))
    return blocks


def report_cross_table(groups, lang_names):
    """跨语言:每门语言给绝对耗时 + 相对 aria 的倍数,一行看全(比值 > 1 = 比 aria 慢)。"""
    blocks = [
        ("heading", "三、跨语言对照"),
        ("paragraph", "每格 = 该语言负载段耗时 ms(该语言 ÷ aria 的倍数);> 1 表示比 aria 慢,< 1 表示比 aria 快。"
                      "sd / min 见文末;带 * 的行无负载段(地板行),整行按进程总耗时算。"),
    ]
    aligns = ["l", "r"] + ["r"] * len(lang_names)
    for label, entries in groups:
        rows = []
        for name, _rel, samples in entries:
            aria = samples["aria"]
            mark = "*" if aria.inner_mean is None else ""
            cells = [name, f"{comparable_ms(aria, aria):.1f}{mark}"]
            for lang in lang_names:
                if lang not in samples:
                    cells.append("-")
                    continue
                cells.append(f"{comparable_ms(samples[lang], aria):.1f}{mark} "
                             f"({ratio(samples[lang], aria):.2f}x)")
            rows.append(cells)
        blocks += [("subheading", label), ("table", (["基准", "aria ms"] + lang_names, rows, aligns))]
    blocks.append(("blank", None))
    return blocks


def report_values(cases, values_by_case, failures):
    """值一致性门禁:同一基准各语言打出的 value 行必须逐位相同(缺失不判错:地板行无校验量)。"""
    blocks = [("heading", "四、跨语言值一致性")]
    mismatched = 0
    detail = []
    for rel, _path in cases:
        seen = values_by_case.get(rel, {})
        reference = seen.get("aria", {})
        if not reference:
            continue
        for lang, values in seen.items():
            if lang == "aria" or not values:
                continue
            if values != reference:
                mismatched += 1
                failures.append(f"{rel} [{lang}] 值不一致: {values} != {reference}")
                detail += [f"  !! {rel} [{lang}] 值不一致", f"     {lang:<8} {values}",
                           f"     aria     {reference}"]
    if mismatched:
        blocks += [("paragraph", line) for line in detail]
    else:
        checked = sum(1 for rel, _path in cases if values_by_case.get(rel, {}).get("aria"))
        blocks.append(("paragraph", f"全部一致({checked} 个基准有校验量)。"))
    blocks.append(("blank", None))
    return blocks


def report_summary(results, cases, lang_names):
    """原始数字:固定列宽(含 sd 与 min),`diff` 两次构建的该块即 A/B。"""
    lines = []
    for rel, _path in cases:
        samples = results.get(rel, {})
        for lang in ["aria"] + lang_names:
            if lang not in samples:
                continue
            sample = samples[lang]
            inner = "-" if sample.inner_mean is None else f"{sample.inner_mean:.1f}"
            lines.append(f"[summary] {rel:<42} {lang:<8} mean {sample.total_mean:>9.2f} "
                         f"sd {sample.total_sd:>6.2f} min {sample.total_min:>9.2f} inner {inner:>9}")
    return [
        ("heading", "五、原始数字"),
        ("paragraph", "固定列宽,`diff` 两次构建的本块即 A/B。"),
        ("code", lines),
        ("blank", None),
    ]


def report_outcome(cases, failures):
    if failures:
        return [("paragraph", f"失败 {len(failures)} 项:")] + [
            ("paragraph", f"  {item}") for item in failures]
    return [("paragraph", f"完成:{len(cases)} 个脚本,全部通过。")]


def main():
    parser = argparse.ArgumentParser(description="aria 源文件级性能基准(进程级计时)")
    parser.add_argument("--aria", help="aria 可执行路径(默认 build/rel/aria 或 build/aria)")
    parser.add_argument("--trials", type=int, default=5, help="计时轮数,报平均(默认 5)")
    parser.add_argument("--warmup", type=int, default=1, help="预热轮数,丢弃(默认 1)")
    parser.add_argument("--filter", default="", help="只跑相对路径含该子串的脚本")
    parser.add_argument("--langs", default="python3,node,lua,java", help="对照语言(默认全部)")
    parser.add_argument("--timeout", type=float, default=600.0, help="单次运行的超时秒数(默认 600)")
    parser.add_argument("--format", default=TEXT, choices=(TEXT, MARKDOWN),
                        help="报告格式:text(终端定宽表,默认)或 md(markdown 表,便于贴文档)")
    parser.add_argument("--list", action="store_true", help="只列出发现的脚本与可用端口")
    args = parser.parse_args()

    if args.trials < 1:
        parser.error("--trials 须 >= 1")
    specs = build_lang_specs()
    lang_names = [name for name in args.langs.split(",") if name]
    unknown = [name for name in lang_names if name not in specs]
    if unknown:
        parser.error(f"未知对照语言 {'/'.join(unknown)}(可选:{'/'.join(specs)})")

    cases = discover(args.filter)
    aria = find_aria(args.aria)
    if not args.list and (aria is None or not aria.exists()):
        shown = aria if aria else "(未找到)"
        print(f"找不到 aria 可执行: {shown};用 --aria=PATH 指定,或先构建 build/rel/aria", file=sys.stderr)
        return 2

    if args.list:
        print(f"语料根: {CORPUS_DIR}")
        for rel, path in cases:
            ports = ",".join(name for name, _ in ports_for(path, lang_names, specs)) or "-"
            print(f"  {rel:<44} 端口: {ports}")
        available = [f"{name}({'有' if shutil.which(specs[name]['probe']) else '缺'})" for name in lang_names]
        print(f"共 {len(cases)} 个脚本;aria: {aria if aria else '(未找到)'};对照语言: {' '.join(available)}")
        return 0

    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(aria.stat().st_mtime))
    failures = []
    results = {}         # 相对路径 -> {语言: Sample}
    values_by_case = {}  # 相对路径 -> {语言: 值表}

    def measure_row(rel, lang, command):
        totals, inners, values, failure = run_trials(command, args.trials, args.warmup, args.timeout)
        if failure:
            failures.append(f"{rel} [{lang}] {failure}")
            progress(f"  !! {rel} [{lang}] 失败: {failure}")
            if lang == "aria":
                try:
                    detail = subprocess.run(command, capture_output=True, text=True,
                                            timeout=args.timeout).stderr.strip()
                except subprocess.TimeoutExpired:
                    detail = ""
                for line in detail.splitlines():
                    progress(f"     {line}")
            return False
        results.setdefault(rel, {})[lang] = Sample(totals, inners)
        values_by_case.setdefault(rel, {})[lang] = values
        return True

    def command_for(lang, sidecar):
        if lang == "java":
            command, failure = prepare_java(sidecar)
            if failure:
                failures.append(f"{sidecar.name} [java] {failure}")
                progress(f"  !! {sidecar.name} [java] 失败: {failure}")
                return None
            return command
        return specs[lang]["command"](sidecar)

    # 测量:进度只在 stderr,报告留到全部测完再整体打印。
    for index, (rel, path) in enumerate(cases, 1):
        progress(f"[{index}/{len(cases)}] {rel}")
        if not measure_row(rel, "aria", [str(aria), str(path)]):
            continue
        for lang, sidecar in ports_for(path, lang_names, specs):
            command = command_for(lang, sidecar)
            if command is not None:
                measure_row(rel, lang, command)

    # 报告:速览 -> aria 绝对值 -> 跨语言比值 -> 值一致性门禁 -> 原始数字;两种格式同一份数据。
    progress("测量结束,输出报告")
    groups = group_rows(cases, results)
    blocks = []
    blocks += report_meta(aria, stamp, cases, args, lang_names)
    blocks += report_verdict(groups, lang_names)
    blocks += report_aria_table(groups)
    blocks += report_cross_table(groups, lang_names)
    blocks += report_values(cases, values_by_case, failures)
    blocks += report_summary(results, cases, lang_names)
    blocks += report_outcome(cases, failures)
    render(blocks, args.format)

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
