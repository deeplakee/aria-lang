# bench/profile -- 指令频度画像

给解释器插探针(`src/runtime/opcode_profile.hpp`,经 `-DARIA_OPCODE_PROFILE=ON` 构建)回答
「真实负载里指令都花在哪」:高频指令、相邻对融合候选、操作数分布、迭代协议等方法面调用量。
与 `bench/lang` 的分工:那边量墙钟(跑多快),这边量指令构成(时间花在哪类指令上),为
特化(_N 族)/融合(超级指令)/快派发类改动提供数据面。

## 快速开始

```sh
# 探针构建(计数只存在于这份二进制;常态构建零开销)
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=Release -DARIA_OPCODE_PROFILE=ON
cmake --build build/profile -j

# 动态画像:15 个负载逐个起进程,聚合报告打 stdout,原始数据可 --json 留档
python3 bench/profile/opcode_profile.py --aria=build/profile/aria
python3 bench/profile/opcode_profile.py --aria=build/profile/aria --json=/tmp/opprof.json > /tmp/report.md

# 静态发射计数(可选):另配一份 DEBUG_PRINT_COMPILED_CODE 构建,解析每函数反汇编
cmake -S . -B build/disasm -DCMAKE_BUILD_TYPE=Release -DARIA_DEBUG_PRINT_CODE=ON
cmake --build build/disasm --target aria -j
python3 bench/profile/opcode_profile.py --static --aria=build/disasm/aria
```

## 语料

`workloads/` 下 8 个**真实世界形态**的负载(aria-only,不配跨语言端口 -- 不量墙钟,无需对照),
外加 `bench/lang/workloads/` 的 8 个真实负载一起进报告。`bench/lang/workloads` 各语言端口齐全、
有值一致性门禁;本目录每个负载都是一台「完整的小应用」(编解码器 / 解释器 / 模板引擎 / diff /
事件仿真 / 压缩 / 图查询),把语言面(字符串/map/类/闭包/异常/堆/DP)按真实程序里的**自然配比**
顶进画像,避免特性微负载单边放大某一协议的占比,也避免画像被数值循环单边支配。负载间共用
约定:确定性 LCG(minstd)驱动输入,`assert` 烘死校验和,结果跨机器逐位可复现。

| 负载 | 量什么 |
| --- | --- |
| `json_codec` | JSON 编解码:递归下降解析器(字符扫描 + 转义还原)+ canonical 序列化往返 + 残缺片段校验(try/catch) |
| `brainfuck` | BF 解释器:预处理括号配对,字符级取指 + 纸带读写的热派发循环,套件按封闭式期望值自校验 |
| `markov` | 二阶马尔可夫文本模型:map 复合键 + 平行数组扫行训练,按计数加权采样生成 |
| `template_render` | 模板引擎:模板编译成捕获片段的闭包(文本/变量/section),对订单数据反复渲染 + HTML 转义 |
| `word_diff` | 词级 diff:版本变异 + LCS 全表 DP + 回溯统计增/删/同(二维表分配压 GC) |
| `bank_queue` | 离散事件仿真:银行排队,数组二叉堆事件队列 + 有状态柜员类 + match 事件分派 |
| `huffman` | Huffman 编解码:词频统计 + 有序插入建树 + 递归码表 + 逐位编码/解码逐位还原 |
| `route_planner` | 路网最短路:邻接表建图 + 全源点朴素 Dijkstra(线性扫最小 + 松弛)+ range for-in |

新负载按 `bench/lang` 的脚本约定写(头部规模常量、末尾 `value`/`bench-time` 标记),校验和
先跑一遍再烘进 `assert`。

## 探针采集口径

见 `src/runtime/opcode_profile.hpp` 头注释。要点:相邻对只在「上一条指令的结束地址 == 本条
起始地址」(纯 fall-through)时计入 `pair` -- 跳转/调用/返回的跨块边界不算,即**可融合为一条
指令**的候选面;`tr` 是不问 fall-through 的动态转移(看循环头/分支目标用)。操作数直方图:
非负操作数 0..47 精确 + 末桶(>=48),LOAD_IMM 的 i8 夹到 [-24,24] 后 +24(24 即 0)。stderr
输出 `[opprofile]` 行块,格式由 `opcode_profile.py::parse_dynamic` 消费。

## 纪律

- **指令计数是确定量**:同一语料任何机器上逐位相同,可入库可对照;ns/instr 是机器量,只在那台
  机的 Release 原样构建上成立(报告里标注机器)。
- 性能结论一律回到 `build/rel` 原样 Release 做 A/B(对齐 `bench/lang` 纪律);探针构建只出计数,
  不出时间。
- 报告快照存 `opcode-profile-<日期>.md`(分析型文档,数字是当时的语料与本机结论)。
