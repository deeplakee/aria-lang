#!/usr/bin/env python3
"""aria 源文件级性能基准驱动(语料与写作规范见同目录 README.md)。

用法:
    python3 bench/lang/lang_bench.py [--aria=PATH] [--trials=N] [--warmup=N] [--filter=SUBSTR]
                                     [--langs=python3,node,lua,java] [--list] [--timeout=SEC]

测什么:把 bench/lang/**.aria 当真实程序**逐个起进程**跑(等价手工 `./build/rel/aria xx.aria`),
每脚本预热 `--warmup` 次(丢)后重复 `--trials` 次,**报平均值 ± 标准差**并列出最小值(pyperf /
MicroPython run-perfbench 都用平均;CLBG / Ruby benchmark-driver 默认用最小值,故最小值并列供对照)。
脚本自身用内置 `clock()` 自测负载段,经 stdout 的 `bench-time: <n> ms` 标记行报出,于是每行有两列:

    总耗时   = 进程启动 + 词法/编译 + VM bootstrap + 负载(即"直接跑这个文件"的耗时)
    内部耗时 = 纯负载段(脚本 clock() 自测;地板行无此列)

两者之差是该脚本的启动+编译开销。**跨语言对照一律看内部列**:总耗时把各运行时的启动成本混了进来
(JVM 启动 60-100 ms、Node 与 CPython 也各有几十毫秒),只有内部列是同一份负载的读数。

对照语言按 PATH 探测(缺谁不出哪一列):CPython(当前解释器)、Node、Lua、Java(需 javac,编译产物
落在 build/bench-java/,每次运行编译一次)。**值一致性门禁**:每个脚本(含各语言端口)都要打印
`value <键>: <数>` 行,驱动逐语言比对,任一键的读数不同即判该行失败——这是"同算法同规模"的机械保证。

数字只在实际发布的那份二进制上有意义:按常规 `-DCMAKE_BUILD_TYPE=Release` 构建即可(LTO 默认开、
`-O3 -DNDEBUG`),别为跑基准关掉优化开关——对照语言也都在各自的发行配置下跑。同二进制重复运行抖动
约 ±1%(进程级另受调度影响,标准差会略大);跨构建 ±5~10%。任一行失败(编译/运行/断言/值不一致)
则本程序非零退出。
"""

import argparse
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

# 脚本自报内部耗时用的标记行(各语言同形,见同目录 README.md「写作约定」)。
MARKER_TIME = re.compile(r"bench-time:\s*([0-9]+(?:\.[0-9]+)?)")
# 脚本自报校验量用的标记行:驱动逐语言比对,是"同算法同规模"的机械保证。
MARKER_VALUE = re.compile(r"^value\s+([A-Za-z0-9_]+):\s*(-?[0-9]+)\s*$", re.MULTILINE)
# Java 端口声明的类名(文件名不必与类名一致:非 public 类合法,见同目录 README.md)。
JAVA_CLASS = re.compile(r"\bclass\s+(\w+)")

CORPUS_DIR = Path(__file__).resolve().parent  # features/ 与 workloads/ 就在驱动旁边
JAVA_CACHE = Path(__file__).resolve().parents[2] / "build" / "bench-java"
JAVA_LONG_OPT = "-Xss16m"  # 递归类基准的 JVM 栈余量
JAVA_HEAP_OPT = "-Xmx2g"  # word_count / sort_int 一类大表负载的堆余量


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


def format_span(pair):
    mean, sd = pair
    return "-" if mean is None else f"{mean:.1f} ± {sd:.1f}"


def main():
    parser = argparse.ArgumentParser(description="aria 源文件级性能基准(进程级计时)")
    parser.add_argument("--aria", help="aria 可执行路径(默认 build/rel/aria 或 build/aria)")
    parser.add_argument("--trials", type=int, default=5, help="计时轮数,报平均(默认 5)")
    parser.add_argument("--warmup", type=int, default=1, help="预热轮数,丢弃(默认 1)")
    parser.add_argument("--filter", default="", help="只跑相对路径含该子串的脚本")
    parser.add_argument("--langs", default="python3,node,lua,java", help="对照语言(默认全部)")
    parser.add_argument("--timeout", type=float, default=600.0, help="单次运行的超时秒数(默认 600)")
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
    print(f"aria 源文件级性能基准:进程级 wall time,预热 {args.warmup} 轮后 {args.trials} 轮取平均 ± 标准差")
    print(f"aria: {aria}(构建时间 {stamp})")
    print(f"语料: {CORPUS_DIR}({len(cases)} 个脚本);对照语言: {' '.join(lang_names)}")
    print("列义:内部 = 脚本 clock() 自测的负载段(跨语言只看这一列);总耗时含启动+编译,min 为最小值")
    print()

    failures = []
    rows = []          # (相对路径, 语言, 总耗时 mean/sd, 总耗时 min, 内部 min, 值表)
    values_by_case = {}  # 相对路径 -> {语言: 值表}

    def measure_row(rel, lang, command):
        totals, inners, values, failure = run_trials(command, args.trials, args.warmup, args.timeout)
        if failure:
            failures.append(f"{rel} [{lang}] {failure}")
            print(f"  !! {rel} [{lang}] 失败: {failure}")
            if lang == "aria":
                try:
                    detail = subprocess.run(command, capture_output=True, text=True,
                                            timeout=args.timeout).stderr.strip()
                except subprocess.TimeoutExpired:
                    detail = ""
                for line in detail.splitlines():
                    print(f"     {line}")
            return None
        cells = (mean_and_sd(totals), mean_and_sd(inners), min(totals))
        rows.append((rel, lang, cells[0], cells[1], cells[2], values))
        values_by_case.setdefault(rel, {})[lang] = values
        return cells

    def command_for(lang, sidecar):
        if lang == "java":
            command, failure = prepare_java(sidecar)
            if failure:
                failures.append(f"{sidecar.name} [java] {failure}")
                print(f"  !! {sidecar.name} [java] 失败: {failure}")
                return None
            return command
        return specs[lang]["command"](sidecar)

    for rel, path in cases:
        ports = ports_for(path, lang_names, specs)
        measured = measure_row(rel, "aria", [str(aria), str(path)])
        if measured is None:
            continue
        total_cell, inner_cell, best = measured
        overhead = "-" if inner_cell[0] is None else f"{total_cell[0] - inner_cell[0]:.1f}"
        note = "" if inner_cell[0] is not None else "  (无标记行:地板行/无负载段)"
        print(f"{rel:<34} 内部 {format_span(inner_cell):>15} ms  总耗时 {format_span(total_cell):>15} ms  "
              f"min {best:>8.2f}  启动+编译 {overhead:>7}{note}")
        for lang, sidecar in ports:
            command = command_for(lang, sidecar)
            if command is None:
                continue
            port_measured = measure_row(rel, lang, command)
            if port_measured is None:
                continue
            port_total, port_inner, _ = port_measured
            print(f"{'':<34}   └ {lang:<8} 内部 {format_span(port_inner):>15} ms  "
                  f"总耗时 {format_span(port_total):>15} ms")

    # 值一致性门禁:同一基准各语言打出的 value 行必须一致(缺失不判错:地板行无校验量)。
    print()
    print("-- 跨语言值一致性 --")
    mismatched = 0
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
                print(f"  !! {rel} [{lang}] 值不一致")
                print(f"     {lang:<8} {values}")
                print(f"     aria     {reference}")
    if not mismatched:
        print(f"全部一致({sum(1 for r in cases if values_by_case.get(r[0], {}).get('aria'))} 个基准有校验量)")

    # 跨语言内部耗时对照(总耗时对 JVM/Node 无意义,只列内部)。
    print()
    print("-- 跨语言内部耗时对照(ms,mean ± sd;总耗时把各运行时启动成本混了进来,故不比) --")
    present = ["aria"] + [name for name in lang_names if name in seen_languages(rows)]
    print(f"{'基准':<42}" + "".join(f"{name:>18}" for name in present))
    for rel, _path in cases:
        cells = {row[1]: row for row in rows if row[0] == rel}
        if not cells:
            continue
        line = f"{rel:<42}"
        for name in present:
            inner_cell = cells[name][3] if name in cells else None
            line += f"{format_span(inner_cell):>18}" if inner_cell and inner_cell[0] is not None else f"{'-':>18}"
        print(line)

    print()
    print("-- 跨构建对照摘要(diff 本块可比两次构建;mean 为主口径,inner/min 并列) --")
    for rel, lang, total_cell, inner_cell, best, _values in rows:
        inner_text = "-" if inner_cell[0] is None else f"{inner_cell[0]:.1f}"
        print(f"[summary] {rel:<42} {lang:<8} mean {total_cell[0]:>9.2f} sd {total_cell[1]:>6.2f} "
              f"min {best:>9.2f} inner {inner_text:>9}")

    if failures:
        print()
        print(f"失败 {len(failures)} 项:")
        for item in failures:
            print(f"  {item}")
        return 1
    print()
    print(f"完成:{len(rows)} 行,全部通过。")
    return 0


def seen_languages(rows):
    return {row[1] for row in rows}


if __name__ == "__main__":
    sys.exit(main())
