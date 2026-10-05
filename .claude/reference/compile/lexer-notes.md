# 词法层实测与被否方案

> 源码位置：`src/compile/Lexer.hpp` / `Lexer.cpp`（主循环、消费辅助）、`src/util/source_file.hpp`（位置派生）
> 模块参考：`.claude/rules/compile.md`（Lexer 条）、`.claude/rules/util.md`（source_file 条）
> 用途：动词法性能之前先读这篇。这里只收**实测数字**与**已实测否决的优化**，机制描述在模块参考里，不重复。

## 1. 怎么量（测量纪律）

- 机器与配置：Apple silicon，`clang++ -std=c++23 -O2 -DNDEBUG`，无 LTO，进程内 best-of-N 取最小。
  **不要用单发进程计时**：曾因紧跟在多文件编译之后而量出 40-49 ms 的假读数（真实值 20 ms 量级），也见过同一二进制 ±0.3 ms 的抖动。**Release 默认开 LTO**，跨 TU 内联让词法再快约 15%（normal 19.6 → 16.3 ms），故各表统一以无 LTO 为基线，复现命令见 bench 文件头。
  同一基准跨构建与机器状态还有 ±5-10% 抖动（normal 行实测 18.7-20.3 ms），差异小于此的量不要当结论。
- 基线取法：`git archive <commit> | tar -x -C /tmp/<dir>` 导出历史树编译对照。手工回退文件当基线出过错（漏改一处的谓词使三条用例红），导出整树既省事又不会写坏工作区。**重配旧 build 目录不覆盖缓存里的 -D 值**：曾把无 LTO 的 build 目录重配成「默认 Release」当 LTO 臂，缓存钉住 ARIA_ENABLE_LTO=OFF 与 -O2 覆盖、产物没变（特征：两配置读数逐毫秒一致），跨配置对照前先看 `CMakeFiles/<target>.dir/flags.make` 实证旗标。
- 两个合成源，分别压不同的成本：
  - `mode 0` = 2.80 MB 常规代码（多行、注释/串/数字混杂，840k token）--贴近真实源码；
  - `mode 1` = 800 KB 单行密集（`1,1,1,…`，800k token，约每字节一个 token）--放大小分派成本，真实代码无此形态。
- **等价性验收用全 token 流 dump 对拍**（类型 | lexeme | 行列，94 个 corpus 文件 = 7505 条记录），外加 19 个词法错误场景的输出 diff。位置/分派类改动只跑 ctest 不足以证明「零行为变化」，这两个 dump 是更强的证据；改造过程中它们是唯一抓出问题的机制之一。
- 基准程序：`bench/lexer_bench.cpp`（词法吞吐四形态 + 端到端编译 + 位置派生 + utf8 解码，复现命令见其文件头，须用 -O2 构建）。本文多数表是入库**之前**用一次性临时程序测的，绝对值与入库版不可直接比；§2 的三张表现按入库版复核过，其余旧数字保留为决策当时的记录（对应代码已不在树上，不可复现）。
- **计时外壳本身会吃掉 5-8%**：同一二进制内对照，写死的循环 17.6-18.0 ms，换成「best-of-N 模板 + 捕获 lambda」外壳后同一段词法变 18.8-19.1 ms。故 bench 每节的计时循环写死在原地、不抽公共外壳；跨二进制比绝对值也要先确认代码布局没变。

## 2. 实测结论

词法吞吐（best-of-30，同一基准）：

| 代码形态 | mode 0（2.80 MB） | mode 1（800 KB 密集） |
| --- | --- | --- |
| 逐 token 急切解析位置（`SourceFile::locate` × 每 token） | 96.4 ms / 29 MB/s | 撞上 O(n²)，未测完 |
| 词法期增量记行列（每码点更新 `line_`/`col_`） | 24.6 ms / 113 MB/s | 11.9 ms |
| 位置只存偏移、行列按需派生 | 22.2 ms / 126 MB/s | 9.7 ms |
| 再加消费辅助收口 | 20.1 ms / 139 MB/s | 10.2 ms |
| 再加 decode 热/冷拆分 + 字符串普通段整段追加 | 18.7-20.3 ms / 138-149 MB/s | 9.0-9.3 ms |
| 再加关键字纯表换装 + 长度早退 | 14.4 ms / 198 MB/s | 8.5 ms |
| 再加 token 表按源预留 | 10.6 ms / 270 MB/s | 6.1 ms |
| 再加 Token 平凡化 + 字符串 arena | 10.1 ms / 283 MB/s | 5.1 ms |
| 再加 lexeme 只存长度、读时按 loc 重建（现态） | 10.1 ms / 282 MB/s | 5.2 ms |

末两行由入库 bench 复核；其余各行是入库前的一次性程序所测，对应代码已不在树上。

关键字查找曾是常规源上的隐性大头：`lookup_keyword` 对每个标识符线性扫全部 68 项 token 表（miss 必满扫），约占 normal 形态词法 25%（进程内探针 17.7 ns/次）。2026-10-05 注册表按类拆子表，查找改为「22 项关键字纯表 + 长度区间早退」--关键字长度 2..8，区间外的 lexeme（单字符名、长生成名、全部 CJK 标识符（UTF-8 每字 3 字节））一次比较出局，bench normal 约 60%、cjk 约 100% 的查找走此早退。ABBA 3 轮交叉 min：normal -27.2%（19.8 → 14.4 ms）、cjk -31.9%（10.8 → 7.4 ms）、端到端编译 -9.8%、numeric 平；行为等价由全 token dump 对拍钉住（tests/ 全部 244 个 .aria，27012 行逐字节一致，含负向语料报错行）。留档中间态：纯表无早退 normal 16.0 ms、cjk 8.5 ms，即早退本身值约 10-17%。

`List<Token>` 的倍增增长是同批发现的另一大头：`Token` 72 字节且含 `variant<String>` 非 trivially copyable，倍增走逐元素 move + 弃置中间缓冲，隔离探针 +4.0-4.5 ms、in-situ 实测同量级（探针未高估）。2026-10-05 Lexer 构造按源预留 `reserve(src/2+1)`：token 数 ≤ 字节数+1（EOF），故至多一次倍增、任何形态不劣于裸倍增；bytes/token ≥ 2 时零搬移（真实语料 239 文件全部 ≥ 3.1 bytes/token）。ABBA 3 轮交叉 min：normal -25.7%（14.3 → 10.6 ms）、numeric -27.1%（8.3 → 6.1 ms）、cjk -22.6%（7.1 → 5.5 ms）、端到端编译 -10.1%，bench 进程 RSS 峰值 -121 MB。不带 `+1` 的形态在纯密集源（tokens = bytes+1 恰越 2C 边界）实测 +9.5%，`+1` 是刀口修复不是笔缀。多付的仅虚拟地址（36× 源字节，未触碰页无物理成本），未设封顶--巨型源的 token 流本身即 GB 级，reserve 的膨胀因子仅 1.6×。已否路径：换容器（Parser 接口涟漪）、自定义分配器复用弃置缓冲（动 MI_OVERRIDE=OFF 边界）；Token 平凡化当时以「收益被 reserve 拿走大半」暂缓，随后经逐 token 构造差价探针翻案落地，见下段。

同日追加 Token 平凡化 + 字符串 arena：`TokenValue` 改 `variant<monostate, i64, f64, StringView>`，Token 72B → 64B 且 trivially copyable（`static_assert` 钉住；variant 的 index+对齐税 8B，16B 的 StringView 载荷成 24B），字符串字面量解析内容不再逐 token 持有 String，改追加 Lexer 的字符串 arena（`StringArena`：mark/view_from/append 四口公开面），token 持入其中视图。reserve 治不了逐 token 的构造+写入差价（非平凡 variant move 的分支 + String 拷贝），隔离探针 880k 入预留表 2.2 → 1.2 ms（-1.05 ms）。ABBA 3 轮交叉 min（base = ef39ad6）：normal -4.6%、numeric -16.8%（仍余一次倍增且 memmove 化）、string -5.9%、端到端编译 -5.1%、cjk -1.1%（噪声带内）。稳定性红线（已写进类型注释）：已发段视图恒稳（arena 分块冻结，见下段）、arena 与 token 流同寿命（Parser 消费期读取、拷入 AST 自有 String 后无依赖）。内存（单次 tokenize 足迹，ru_maxrss）：常规源（1.675M token）base 120.7 / arena 107.3 / UPtr·raw 指针形态 95.5 MB，字符串密集源 arena 4.1 MB 对三对照 7.6-7.9 MB（消除逐字符串堆分配与局部 String 增长中间态，近乎减半）。同 base 重实现的两个指针形态对照（UPtr / raw `String*` + 析构释放）实测 normal +2.6% / +0.4%、compile -0.4% / -1.7%——短字符串本可 SSO 零分配，逐 token 堆分配吃掉机械收益，验证 arena 形态。RSS：单次 tokenize 足迹实测 -13.4 MB（1.675M token）；bench 进程峰值 +88 MB 是同一进程连跑 6 种尺寸 tokenize 时分配器大块缓存复用模式差异的假象，非真实占用。arena 形态当晚把裸 `List<char>` 成员换装成 `StringArena` 类封装（调用点 mark/view_from/append 收口），ABBA 复测各形态与换装前同带（会话噪声内），换装性能中性。

同日第三批把 arena 的按整源预留改为分块冻结（分配随实际内容走）：原策略首写入即 reserve 整源大小，巨型混淆源里一个 10 字节串也触发 100MB 分配（Unix 未触碰页不驻留，但 Windows malloc 即 commit；且 token 表的 src/2 预留上界是真紧的，arena 的整源上界在常见形态松几个数量级，两者不对称）。新机制：段协议 open_segment/close_segment 显式化（视图只在段收口发放，扫描中途无视图指向半成品段，故冻结时只有当前开段可安全搬移），首块容量 min(上界, 4MB)，写满则旧块整块冻结收编（地址不动、已发视图恒稳）+ 开段拷入新块续写（容量 max(2x 旧块, 本次需求)，钳剩余上界）。先写 scratch 再整段拷入、每段独立分配两条路沿用 §2 既有实测（逐 token 堆分配 +0.4-2.6%）直接否决。ABBA 3 轮交叉 min（base = 52d3ed1，新 bench 行 string_huge = 6MB 字符串密集、专压冻结路径）：normal 10.09->10.12、numeric 5.12->5.13、cjk 5.41->5.44、string 1.97->1.97、string_huge 4.54->4.55、端到端编译 17.32->17.11，全部 ±1.2% 内（噪声带）。内存画像（100MB 源探针，task resident/virtual）：稀疏形态（字符串内容 ~1KB）arena 分配 104.9->4.2MB、resident 两态相同；100MB 单串两态相同（首块按单次大追加的需求精确落地，零搬移）；100MB 内容拆 1600 段两态相同（钳剩余上界使各块容量之和收敛于上界，几何冗余实测不出现）。Windows commit 收益无实机未测。等价性：全 token dump 对拍 244 语料 27012 行逐字节一致（含字符串值字节）。原样 Release（-O3 -flto=thin）复测 ABBA 同法：normal 8.97->9.16（+2.1%）、numeric 4.79->4.99（+4.2%）、cjk 5.35->5.45、string 1.91->1.92、string_huge 4.41->4.43、端到端 16.16->16.36，全部低于 5% 抖动线；numeric 形态零字符串、arena 一次不碰仍 +4.2%，说明该量级是 Lexer.cpp 布局涟漪而非机制成本（arena 最密集的 string/string_huge 仅 +0.4/+0.5%）。真实解释器整程序对照（bench/lang 全 55 脚本、两臂 aria 均为原样 Release、aria-only、ABBA 两轮取 min）：进程总耗时中位 -0.8%、负载段中位 -1.1%，超 ±4% 的行两侧分布（-5.6%~+5.4%）为双向噪声；startup_floor（纯启动+编译）两臂 ~2.3 ms 重合。另复测 arena 之前形态（ef39ad6，TokenValue 持 String、Token 非平凡）对照现树，两配置 ABBA 各 3 轮 min：无 LTO 下 pre 慢 6.2-14.5%（normal 11.00->10.04、numeric 6.00->5.13、string 2.10->1.96、端到端 18.39->17.17）、LTO 下慢 2.0-15.4%（numeric 5.88->4.97 为主、cjk +1.4% 噪声）；manystrings 探针（100MB 内容 / 1600 串）pre 字符串存储 virtual ~559MB（malloc 缓存逐串几何增长中间体）对现 105MB、峰值 RSS 238.9 对 212.0MB--撤销 arena 的方向被否，numeric 的差距来自 Token 平凡化后 token 表倍增 memmove 化、与 arena 是同批不可拆的两面。

同日第四批把 `Token` 的 lexeme 成员由 `StringView` 改为 `u32` 长度：lexeme 恒为源内子串且区间以 loc 偏移为起点（全部 8 个构造点 slice/loc_at 同起点配对），指针与 `loc_.src` 冗余；`lexeme()` 读口签名不变，读时按 loc 自源缓冲重建（报错/测试冷路径），构造侧少一次 16B 指针存储、token 表 memmove 缩 25%。Token 64B → 48B（u32 落进 type_ 后既有 padding，sizeof 探针实测 48）。无 LTO ABBA 3 轮交叉 min（base = c4889a1）：全形态同向 +0.7-3.1%（normal 9.99→10.14、numeric 5.11→5.24、cjk 5.46→5.50、string 1.91→1.96、string_huge 4.54→4.57、端到端 17.39→17.60）；原样 Release（LTO）二次抽样转双向（numeric -3.7%、cjk -2.0%、string -0.5%、string_huge +0.4%、normal +1.7%、端到端 +2.0%）--两次独立布局抽签符号不相关、量级均在 ±5% 跨构建噪声带内，判为布局涟漪而非机制成本（先例：arena 分块批 numeric 零字符串仍 +4.2%）。内存为本批恒定收益：/usr/bin/time 峰值 RSS（各 2 次）289-293 MB → 231 MB（-58~62 MB，约 -20%），与 1.675M token 段 -27 MB、normal 形态 880k token -14 MB 的推算同量级。等价性：全 token dump 对拍 301 个语料文件、41777 token 行加头行共 42379 行逐字节一致（type/offset/line/col/lexeme/int/float/string/interp 十字段，含 5 个负向 tokenize 场景报错行）；normal + TagValue 双配置 ctest 1331×2 绿。

同日第五批把 `TokenValue` 由 `variant` 换成无 tag union（int_/float_/str_）：活跃成员本就完全由 TokenType 决定（Integer/Float/String+Interp 段），variant 的 index+对齐税 8B 纯冗余；读取全走按 type_ 分闸的取值口，union 全 trivial 成员不破平凡可拷贝。Token 48B → 40B（sizeof 探针实测，TokenValue 24→16）。等价性：全 token dump 对拍 301 个语料文件 42379 行逐字节一致；normal + TagValue 双配置 ctest 1331×2 绿。无 LTO ABBA 3 轮交叉 min（base = cccd9b7）：numeric -6.1%（5.15→4.83 ms，token 最密集的形态与逐 token 写入量直接相关）、string/string_huge 平、normal +1.2%/cjk +1.0%/端到端 +0.6%（带内）；峰值 RSS 231 MB → 202 MB（-29 MB）。

另一档**字符串密集源**（2.66 MB / 22.5k token，长串为主、含多字节与转义）专量字符串扫描路径，三个变体按同一 bench 同形对照：普通段逐字符解码 + 逐字节 `push_back` 7.1 ms → 逐码点 `consume_codepoints` 3.3 ms（每个多字节码点解码一次）→ **按字节扫到分隔符整段 `append` 1.9 ms（对前两者 3.7× / 1.7×）**，故保持按字节扫。真实代码短串多，故常规源上看不出差别（上表末两行在噪声内）。

端到端编译（0.94 MB 源，词法 + 语法 + 代码生成，best-of-9）：逐 token 急切解析位置 **49.1 ms** → 位置按需派生 **20.5 ms**（位置按需派生形态 bench 复核 19.4-20.9 ms；关键字纯表 + 长度早退换装后 19.8 ms，同轮基线 21.9 ms）。

位置解析原语（4.75 MB / 100k 行 / 162 万 token，访问序列取 token 起点偏移）：

| 度量 | 结果 |
| --- | --- |
| `line_at(offset)` 源序（单条行缓存命中） | 3.3-4.5 ns/次 |
| `line_at(offset)` 随机序（缓存全失效，每次二分） | 37-40 ns/次 |
| `locate(offset)` 全解析（行号 + 行内码点列） | 13 ns/次（平均行长 47.5 B，列部分约占 10 ns） |
| 码点计数速率 | ~0.4 ns/byte（ASCII，即 §5 的 `decode_one` 单次成本） |
| 行号派生在端到端里的可测性 | 测不出：抽掉派生 20.82-21.02 ms vs 带派生 20.63-20.77 ms（噪声内，260k 次调用 92% 命中） |

本表按入库 bench 复核。更早的一次性程序量到的是 187.4 / 16.65 / 1.67 / 31.4 ns/次与 2.6 ns/byte，与上表不可直接比（调用基数不同--`locate` 那次每偏移调了两次却按一次折算；内联上下文与代码布局也不同）。

两条由此确立的判断：

1. **位置不要在词法期烘进每个 token。** token 携带行列 ⇒ 每个 token 都要解析一次位置 ⇒ 逐 token 数码点，单行长文件退化成 O(n²)（65535 元素单行 131 KB 源词法 65 秒）。位置只存偏移、行列在消费点派生之后，这个成因本身消失；列在生产代码里本来也只有错误渲染与测试读。
2. **行解析必须带单条缓存。** 不带缓存是 37-40 ns/次（1.2M 节点量级即数十毫秒），带缓存 3.3-4.5 ns/次，成本被其他工作完全遮盖。

## 3. 已实测否决（勿重提，除非测量条件变了）

| 被否项 | 实测 | 否决理由 |
| --- | --- | --- |
| ASCII 优先分派（主循环抄成「按字节」与「按码点」两条类别链） | 两链内联：mode 0 17.6 ms、mode 1 8.3 ms（对照全 `decode_one` 19.8 / 10.2，看似 -12% / -19%）；但把 ASCII 支路抽成 `dispatch_ascii(byte)` 方法后 mode 0 塌到 19.5 ms（收益仅 1.7%），只剩 mode 1 的 15% | 收益依赖「代码恰好内联在循环体里」这一偶然性；`inline`（19.4 ms）与 `__attribute__((always_inline))`（19.3 ms）都收不回抽取的损失，说明不是单纯的内联失败。撑不起两条重复类别链的维护成本 |
| 单链分派 + 条件取码点（`byte < 0x80 ? cp : decode_one(...)`） | mode 0 20.3 ms、mode 1 9.2 ms | 比完全回退（19.8 / 10.2）还慢：合并成一条链后编译器无法为「纯 ASCII」与「含多字节」两条路径分别优化 |
| `skip_trivia` 的字节级分派 | 17.3 → 18.3 ms | 负收益，且多 8 行代码。空白是逐字节路径，逐次解码反而更快 |
| 标识符续接的 ASCII 字节循环 | 比 `decode_one` + `advance` 更慢 | 多一次四路布尔判定（`A-Z`/`a-z`/`0-9`/`_`）换掉 `decode_one` 的一次比较 |
| 指数段预视（先探再消费，替代消费后退回） | --（未实装，仅评估） | 位置只存偏移之后，回退就是 `pos_ = exp_pos` 一行、语义自明；预视要用 `peek_byte(ahead)` 复制一遍指数扫描逻辑，净增代码且无正确性收益 |
| 在 `consume_codepoints` 内为 ASCII 另开快路径（`byte < 0x80` 时不解码、直接 `advance()`） | 常规源**慢约 5%**（19.4 → 18.5 ms，三轮一致）；数字密集/字符串密集无差别（前者无标识符注释、后者走 consume_byte） | 该模板在 3 个调用点展开，多一条路径的代码体积比省下的那点解码更贵；ASCII 时 `decode_one` 本就返回「该字节、长度 1」，快路径只是把同一件事重写一遍 |
| 位置类型携带区间（`SourceSpan{start, end}`） | -- | 调用点只读起点，`end` 从未被消费；类型已删。将来若需区间诊断（IDE 波浪线）再引入 |
| 数字字面量收尾抽公共 helper（`finish_number(start, digits_begin, base, is_float)`） | 数字密集源 +9%（9.0 → 9.9 ms）；`-S` 显示该 helper 未被内联，每个数字 token 多一次调用 | 三段尾部（radix-int / decimal-float / decimal-int）确实同构，但**就地只有 8 行、错误文案就地完整**；且与既有裁定「相邻报错变体字面重复优于模板抽段」同形。抽出后要把函数体挪进头文件才有望内联，代价不值 |
| `\u{...}` 收集改只收 hex（连带把 from_chars 失败分支换成「缺少十六进制」） | --（仅评估） | 分支数不变、只换错误文案；且 `\u{1_2}` 的提示会从「含非合法 hex 字符」变成「缺少 '}'」，比现状更差 |

两个**不是**「被否」而是「按输入形态二选一」的实测（取舍时看目标形态）：

- `scan_operator_or_punct` 的形参：无参版（函数内自读首字节）mode 0 快 6%（20.1 vs 21.3 ms），有参版 mode 1 快 5%（9.5 vs 10.2 ms）。取常规源，故保持无参。
- 行表缓存命中率对 AST 遍历序敏感：顺序访问 3.3-4.5 ns/次，随机序退到 37-40 ns/次。若将来出现非按源序访问行号的消费者，需重新评估缓存形态。

## 4. 基准覆盖与缺口

- **已覆盖**：四形态词法吞吐（`normal` / `numeric` / `cjk` / `string`）、端到端编译、位置派生原语、utf8 解码微基准。CJK 密集源曾列为本节缺口（非 ASCII 占比高的源把成本压到 `utf8::decode_one` 上，常规源几乎量不到这块），现由 `cjk` 行覆盖：2.94 MB / 340k token，7.4 ms / 398 MB/s（常规源 14.4 ms / 198 MB/s，单位字节反而更快--CJK 每字节承载的 token 更少、分派次数更低）。
- **仍缺**：词法时间的分布未逐项拆解--只量过「位置记账」与「逐 token 分派」两块（分别约 17% 与 12% 于特定形态）；关键字查找与 `List<Token>` 增长后经探针 + 隔离实验补测（normal 形态各 ≈ 4-5 ms，均已修，见 §2），`decode_one` 自身、`Token` 构造各自占多少仍没有单独测过。

## 5. utf8 解码的成本解剖（为将来的 utf8 重构留底）

**解码在词法里占多少**（-DNDEBUG，best-of-N；「桩」= 保留边界检查与首字节判定、跳过长度表/续接校验/码点组装/最短编码与范围校验的替换实现。桩列要替换实现，不入 bench，故本表只作历史记录，不可复现）：

| 输入 | 真实解码 | 桩 | 解码占比 |
| --- | --- | --- | --- |
| ASCII 常规源 2.80 MB / 840k token | 19.2-20.0 ms | 15.8 ms | ~19% |
| CJK 密集源 2.94 MB / 340k token | 12.0-12.5 ms | 8.2-8.4 ms | ~31% |
| 单行密集 800 KB / 800k token | 9.8-9.9 ms | 8.2-8.3 ms | ~16% |

**单次成本**（紧循环；入库 bench 的 utf8 一节，码点累加进 checksum 以保证真的解码而非被约简成「按长度跳过」）：`decode_one` ASCII 0.4-0.5 ns/码点（2.0-2.4 GB/s）、CJK 2.4-2.9 ns/码点（0.8-0.9 GB/s）。`is_valid`：ASCII 4.0 GB/s、CJK 1.1 GB/s -- 加载期一遍，不是瓶颈。同一形状的微基准在热/冷拆分之前量到的是 ASCII 3.66 / CJK 4.21 ns/码点（拆分紧随本节所述解剖之后落库，两次之间该路径只发生了这一处改动）。与推论 1（成本在函数体大小而非调用次数）一致：整源词法只快 3-4%，而单次调用快了近一个数量级。（注意：紧循环里每次解码串行依赖游标，这个数仍会**高估**它在真实语流里的边际成本。）

**调用点分布**（ASCII 常规源 3.80 MB / 1.3M token，共 428 万次 = **1.13 次/字节**；插桩构建下的历史快照，不可由 bench 复现--其中 `scan_string` 一行已被后续的「整段追加」取代，见下）：

| 调用点 | 次数 | 占比 |
| --- | --- | --- |
| `run` 主循环分派 | 2,240,000 | 52% |
| `skip_trivia`（每空白字符一次） | 1,920,000 | 45% |
| `scan_string`（每普通字符一次） | 40,000 | 1% |
| `consume_codepoints`（仅非 ASCII） | 80,000 | 2% |
| 算子分支的非 ASCII | 0 | 0% |

三条推论：

1. **95% 的调用是单字节 ASCII**，执行路径只有「边界检查 + 取字节 + 比较 + 返回」。所以成本主要不在执行，而在 **`decode_one` 的函数体在每个调用点被内联展开** -- 多字节那套长度表、续接校验、组装 switch、双重校验在每个站点都占据代码空间。
2. **据此已落地热/冷拆分**：多字节慢路径移出为 `detail::decode_multibyte`（定义在 `src/util/utf8.hpp`；`ARIA_NOINLINE` 宏定义在 `common.hpp`：`_MSC_VER` 走 `__declspec`、否则 `__attribute__`），`decode_one` 只留边界检查 + 首字节判定 + ASCII 返回；`[[nodiscard]]`、`constexpr`、全部调用点均不变。实测：ASCII 常规源 20.1-20.3 → 19.4-19.6 ms（-3~4%）、单行密集 10.1-10.3 → 8.9-9.2 ms（-10~12%）、CJK 密集源 12.6-12.8 → 11.6-12.0 ms（-5~8%）。等价性用穷举对拍验收：17 字节表（ASCII / 续接 / 超长编码 C0-C1 / 2-3-4 字节边界 / 代理区首字节 / F5-FF）× 长度 1-4 的全部组合 × 全部偏移，共 43.8 万次 `decode_one` 调用与全部 `is_valid` 结果逐字节一致。
3. `skip_trivia` 的逐字符解码占调用数 45%，但把它改成字节级分派实测**更慢**（§3）-- 与「调用便宜、函数体贵」一致：要减的是代码体积，不是调用次数。同理，`scan_string` 的普通字符段已改为一次 `consume_byte` 扫到分隔符整段 `append`（§2），把该行的 1% 调用数也一并消掉。

若将来重构 utf8 解码（表驱动 / 状态机）：

- 目标应放在**多字节路径**（长度表、续接校验、组装、双重校验的指令数与分支），它是 CJK 源上那 31% 的主体；ASCII 路径已是最优形态，任何「统一成状态机」的做法都可能把 ASCII 拖慢 -- 本层已三次实测到这类反效果（§3 前三行）。
- 收益上限参考：桩版（解码工作全部消失）才 19%（ASCII）/ 31%（CJK），而状态机仍要逐字节查表与累积，实际收益会明显小于该上限。
- 先补 CJK 密集基准再动手（§4，已随 bench 入库：`cjk` 行）；本节两张表（桩对照、调用点分布）分别要替换实现与插桩，不可由 bench 复现。
