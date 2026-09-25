#!/usr/bin/env python3
"""opcode_profile.py -- aria 指令频度画像驱动。

跑 workload 集合(默认 bench/lang/workloads 与 bench/profile/workloads),每个负载独立起一次
aria 进程(ARIA_OPCODE_STATS=1,探针构建),解析 stderr 的 [opprofile] 行块,聚合成 markdown
报告(stdout)并可选存 JSON(--json)。静态发射计数用 --static --aria=<print-code 构建>:
该构建以 -DARIA_DEBUG_PRINT_CODE=ON 配置,每函数反汇编打 stderr,按行解析 opcode 记静态数。

用法:
  python3 bench/profile/opcode_profile.py --aria=build/profile/aria
  python3 bench/profile/opcode_profile.py --aria=build/profile/aria --json=/tmp/opprof.json
  python3 bench/profile/opcode_profile.py --static --aria=build/disasm/aria
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_WORKLOAD_DIRS = ["bench/lang/workloads", "bench/profile/workloads"]

INSTR_RE = re.compile(r"^\s*[0-9A-Fa-f]{4}\s+(?:\d+\s+)?\|?\s*([A-Z][A-Z0-9_]*)")


def find_workloads(dirs):
    scripts = []
    for d in dirs:
        path = os.path.join(ROOT, d) if not os.path.isabs(d) else d
        for name in sorted(os.listdir(path)):
            if name.endswith(".aria"):
                scripts.append(os.path.join(path, name))
    return scripts


def parse_dynamic(stderr_text):
    data = {"total": 0, "op": {}, "pair": {}, "jt": {}, "tr": {}, "operand": {},
            "branch": {}, "mcall": {}, "ncall": {}, "call": {}}
    inside = False
    for line in stderr_text.splitlines():
        if line == "[opprofile] begin":
            inside = True
            continue
        if line == "[opprofile] end":
            inside = False
            continue
        if not inside or not line.startswith("[opprofile] "):
            continue
        parts = line[len("[opprofile] "):].split(" ")
        kind = parts[0]
        if kind == "total":
            data["total"] = int(parts[1])
        elif kind in ("op", "branch"):
            data[kind][parts[2]] = int(parts[1])
        elif kind in ("pair", "jt", "tr"):
            a, b = parts[2].split("|")
            data[kind][(a, b)] = int(parts[1])
        elif kind == "operand":
            data[kind][parts[1]] = [int(v) for v in parts[2:]]
        elif kind in ("mcall", "ncall", "call"):
            data[kind][" ".join(parts[2:])] = int(parts[1])
    return data


def parse_static(stderr_text):
    counts = {}
    for line in stderr_text.splitlines():
        m = INSTR_RE.match(line)
        if m:
            counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    return {"total": sum(counts.values()), "op": counts}


def run_dynamic(aria, scripts):
    env = dict(os.environ)
    env["ARIA_OPCODE_STATS"] = "1"
    result = {}
    for script in scripts:
        name = os.path.splitext(os.path.basename(script))[0]
        proc = subprocess.run([aria, script], capture_output=True, text=True, env=env)
        if proc.returncode != 0:
            sys.exit(f"[fail] {script}: exit {proc.returncode}\n{proc.stderr[:2000]}")
        data = parse_dynamic(proc.stderr)
        if data["total"] == 0:
            sys.exit(f"[fail] {script}: no [opprofile] block (probe build?)")
        # 值一致性:stdout 的 value 行有就校验与重跑一致(确定性负载),顺带透出
        result[name] = {"script": script, **data}
        print(f"[run] {name}: {data['total']:,} instr", file=sys.stderr)
    return result


def run_static(aria, scripts):
    result = {}
    for script in scripts:
        name = os.path.splitext(os.path.basename(script))[0]
        proc = subprocess.run([aria, script], capture_output=True, text=True)
        if proc.returncode != 0:
            sys.exit(f"[fail] {script}: exit {proc.returncode}\n{proc.stderr[:2000]}")
        data = parse_static(proc.stderr)
        if data["total"] == 0:
            sys.exit(f"[fail] {script}: no disassembly on stderr (-DARIA_DEBUG_PRINT_CODE=ON build?)")
        result[name] = {"script": script, **data}
        print(f"[run] {name}: {data['total']:,} emitted", file=sys.stderr)
    return result


def fmt_n(n):
    return f"{n:,}"


def report_dynamic(per_workload, top_ops=25, top_pairs=30):
    total_all = sum(w["total"] for w in per_workload.values())
    agg = {}
    for w in per_workload.values():
        for op, count in w["op"].items():
            agg[op] = agg.get(op, 0) + count

    out = []
    out.append("## 一、每负载指令总量\n")
    out.append("| 负载 | 指令数 |")
    out.append("| --- | ---: |")
    for name in sorted(per_workload, key=lambda k: -per_workload[k]["total"]):
        out.append(f"| {name} | {fmt_n(per_workload[name]['total'])} |")
    out.append(f"| **合计** | **{fmt_n(total_all)}** |")

    out.append("\n## 二、高频指令(全负载合计)\n")
    out.append("| # | 指令 | 次数 | 占比 | 累计 |")
    out.append("| --- | --- | ---: | ---: | ---: |")
    ranked = sorted(agg.items(), key=lambda kv: -kv[1])
    cum = 0
    for i, (op, count) in enumerate(ranked[:top_ops], 1):
        cum += count
        out.append(f"| {i} | {op} | {fmt_n(count)} | {count / total_all:.2%} | {cum / total_all:.2%} |")

    out.append("\n## 三、融合候选(静态相邻对,fall-through)\n")
    out.append("| # | 相邻对 | 次数 | 占全部指令 | 占前条指令 | 建议融合名 |")
    out.append("| --- | --- | ---: | ---: | ---: | --- |")
    pairs = {}
    for w in per_workload.values():
        for (a, b), count in w["pair"].items():
            pairs[(a, b)] = pairs.get((a, b), 0) + count
    for i, ((a, b), count) in enumerate(
            sorted(pairs.items(), key=lambda kv: -kv[1])[:top_pairs], 1):
        first = agg.get(a, 0)
        share = f"{count / first:.0%}" if first else "-"
        fused = f"{a.split('_')[0]}_{b}" if b.startswith(("JUMP", "CALL")) else f"{a}_{b}"
        out.append(f"| {i} | {a} ; {b} | {fmt_n(count)} | {count / total_all:.2%} | {share} | {fused} |")

    out.append("\n## 四、操作数直方图(关键指令)\n")
    out.append("非负操作数:桶 0..47 精确,末桶 >=48;LOAD_IMM:夹 [-24,24] 后 +24(24 即 0)。\n")
    operand_agg = {}
    for w in per_workload.values():
        for op, buckets in w["operand"].items():
            if op not in operand_agg:
                operand_agg[op] = [0] * len(buckets)
            for i, v in enumerate(buckets):
                operand_agg[op][i] += v
    interesting = ["LOAD_LOCAL", "STORE_LOCAL", "LOAD_UPVALUE", "STORE_UPVALUE", "POP_N",
                   "CALL", "CALL_METHOD", "LOAD_IMM", "LOAD_CONST", "LOAD_GLOBAL"]
    out.append("| 指令 | " + " | ".join(str(i) for i in range(9)) + " | 9..47 | >=48 |")
    out.append("| --- | " + " | ".join(["---:"] * 9) + " | ---: | ---: |")
    for op in interesting:
        if op not in operand_agg:
            continue
        buckets = operand_agg[op]
        total_op = sum(buckets)
        head = " | ".join(f"{buckets[i] / total_op:.1%}" for i in range(9))
        mid = sum(buckets[9:48]) / total_op
        tail = buckets[48] / total_op
        out.append(f"| {op} | {head} | {mid:.1%} | {tail:.1%} |")

    out.append("\n## 五、条件跳转命中率\n")
    out.append("| 指令 | 执行 | 跳走 | 命中率 |")
    out.append("| --- | ---: | ---: | ---: |")
    branch_agg = {}
    for w in per_workload.values():
        for op, count in w["branch"].items():
            branch_agg[op] = branch_agg.get(op, 0) + count
    for op, taken in sorted(branch_agg.items(), key=lambda kv: -kv[1]):
        executed = agg.get(op, 0)
        out.append(f"| {op} | {fmt_n(executed)} | {fmt_n(taken)} | {taken / executed:.0%} |")

    out.append("\n## 六、方法面调用(PREPARE_METHOD 按接收者类型 + 名)\n")
    mcall = {}
    for w in per_workload.values():
        for key, count in w["mcall"].items():
            mcall[key] = mcall.get(key, 0) + count
    out.append("| 接收者.名 | 次数 | 占全部方法解析 |")
    out.append("| --- | ---: | ---: |")
    mcall_total = sum(mcall.values())
    for key, count in sorted(mcall.items(), key=lambda kv: -kv[1])[:20]:
        out.append(f"| {key} | {fmt_n(count)} | {count / mcall_total:.1%} |")

    out.append("\n## 七、原生函数调用 Top(按名)\n")
    ncall = {}
    for w in per_workload.values():
        for key, count in w["ncall"].items():
            ncall[key] = ncall.get(key, 0) + count
    ncall_total = sum(ncall.values())
    out.append(f"原生调用合计 {fmt_n(ncall_total)}\n")
    out.append("| 名 | 次数 |")
    out.append("| --- | ---: |")
    for key, count in sorted(ncall.items(), key=lambda kv: -kv[1])[:15]:
        out.append(f"| {key} | {fmt_n(count)} |")

    out.append("\n## 八、call 分发面(CALL/CALL_METHOD 取指站点,按 callee 类型;算子钩子等内层派发不计入)\n")
    call = {}
    for w in per_workload.values():
        for key, count in w["call"].items():
            call[key] = call.get(key, 0) + count
    call_total = sum(call.values())
    out.append("| callee | 次数 | 占比 |")
    out.append("| --- | ---: | ---: |")
    for key, count in sorted(call.items(), key=lambda kv: -kv[1]):
        out.append(f"| {key} | {fmt_n(count)} | {count / call_total:.1%} |")

    out.append("\n## 九、每负载 Top-10 指令速览\n")
    for name in sorted(per_workload, key=lambda k: -per_workload[k]["total"]):
        w = per_workload[name]
        tops = sorted(w["op"].items(), key=lambda kv: -kv[1])[:10]
        body = ", ".join(f"{op} {count / w['total']:.1%}" for op, count in tops)
        out.append(f"- **{name}**: {body}")
    return "\n".join(out) + "\n"


def report_static(per_workload, top_ops=25):
    total_all = sum(w["total"] for w in per_workload.values())
    agg = {}
    for w in per_workload.values():
        for op, count in w["op"].items():
            agg[op] = agg.get(op, 0) + count
    out = ["## 静态发射计数(全负载合计)\n", "| # | 指令 | 发射数 | 占比 |", "| --- | --- | ---: | ---: |"]
    ranked = sorted(agg.items(), key=lambda kv: -kv[1])
    cum = 0
    for i, (op, count) in enumerate(ranked[:top_ops], 1):
        cum += count
        out.append(f"| {i} | {op} | {fmt_n(count)} | {count / total_all:.2%} |")
    out.append(f"\n静态合计 {fmt_n(total_all)}\n")
    out.append("## 每负载静态合计\n")
    for name in sorted(per_workload, key=lambda k: -per_workload[k]["total"]):
        out.append(f"- {name}: {fmt_n(per_workload[name]['total'])}")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description="aria opcode frequency profiler driver")
    ap.add_argument("--aria", required=True, help="aria 可执行路径(对应构建配置)")
    ap.add_argument("--static", action="store_true",
                    help="静态发射计数模式(aria 需以 -DARIA_DEBUG_PRINT_CODE=ON 构建)")
    ap.add_argument("--workloads", nargs="*", default=DEFAULT_WORKLOAD_DIRS,
                    help="workload 目录(相对仓库根)")
    ap.add_argument("--json", help="解析结果 JSON 落盘路径")
    ap.add_argument("--top-ops", type=int, default=25)
    ap.add_argument("--top-pairs", type=int, default=30)
    args = ap.parse_args()

    scripts = find_workloads(args.workloads)
    if not scripts:
        sys.exit("no workloads found")
    per_workload = run_static(args.aria, scripts) if args.static else run_dynamic(args.aria, scripts)

    if args.json:
        json_safe = {}
        for name, data in per_workload.items():
            entry = dict(data)
            for kind in ("pair", "jt", "tr"):  # tuple 键序列化不成 JSON,折成 "A|B"
                entry[kind] = {"|".join(k): v for k, v in data[kind].items()}
            # 旧口径 tr = fall-through 配对(pair)+ 跳转目标/帧切换派发边(jt),合成后与
            # 2026-09 快照的转移矩阵口径可比
            merged = dict(entry["pair"])
            for key, count in entry["jt"].items():
                merged[key] = merged.get(key, 0) + count
            entry["tr"] = merged
            json_safe[name] = entry
        with open(args.json, "w") as f:
            json.dump(json_safe, f, ensure_ascii=False)
        print(f"[json] {args.json}", file=sys.stderr)

    if args.static:
        print(report_static(per_workload, args.top_ops))
    else:
        print(report_dynamic(per_workload, args.top_ops, args.top_pairs))


if __name__ == "__main__":
    main()
