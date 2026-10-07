# AST bump arena（实验批）实测与机制

> 源码位置：`src/memory/AstArena.hpp`（AstArena / AstList，消费方在 compile 层）、`src/compile/Ast.hpp`（节点成员换装）
> 模块参考：`.claude/rules/memory.md`（AstArena / RawAlloc 条）、`.claude/rules/compile.md`（ast / Parser / Compiler 条）
> 用途：记录把 AST 所有权从 `UPtr` 递归析构换成 arena 整批释放的机制契约与实测数字；动 Parser /
> CodeGen 的分配与遍历路径前先读这篇。

## 1. 机制契约

- `AstArena`：块式 bump 分配器。节点与列表缓冲自大块（首块 256 KB，放不下按请求字节数取整到 1 MiB 量子扩新块）顺序分配，
  析构整批释放、**不跑任何析构函数**；块链头兼当前填充块（新块恒头插、只从头分配，单 `head_` 游标即足）。
  后备经 `mem::alloc/free`（RawAlloc 同族三口，随 `ARIA_USE_MIMALLOC` 开关），分配失败
  `fatal_error(OutOfMemory)`、全程不抛（先例 `GC::allocate`）。
  成立前提是节点成员全为平凡类型：子节点裸指针互指、名字/字符串借源缓冲、列表为 `AstList`——这些由
  Ast.hpp 节点形态保证，新增节点成员不得引入自持堆资源。
- 列表视图：`make_list(const List<T>&)` 把解析期局部 vector 的元素一次性拷入 arena，返回 `Span<T>`（std::span，
  {data, usize size}，16 B）；空表零分配，元素须平凡可拷贝（static_assert 钉住）。`make_list` 是节点
  列表字段的唯一生产口——std::span 会自 lvalue 容器隐式转换，局部容器直接喂节点字段会静默别名悬垂，
  这是约定纪律而非类型保证。
- 生命周期：`Compiler::compile` 栈上构造 arena，`Parser::parse(AstArena&, ...)` 返回借用的
  `ProgramNode*`，CodeGen 消费完后随 `compile()` 返回整批释放。异常路径（panic-mode 恢复、投机解析
  回退）弃掉的节点不单独回收，留 arena 死区到整批释放。
- 节点虚析构仅为多态保留（dynamic_cast 与 vtable 既有），从不单独调用。

## 2. 实测（2026-10-06，ABBA 3 轮交叉 min，无 LTO `-O2 -DNDEBUG` 配方，同 lexer-notes §1 口径）

bench 源 = `make_wide_source` 常规形态 0.96 MB / 335k token；AST 规模 230,001 节点 / 16.52 MB arena
（约 0.69 节点/token、49 B arena/节点）。

| 度量 | 换装前 ms | 换装后 ms | 变化 |
| --- | --- | --- | --- |
| parse-only（词法外计时，含建树不含释放） | 4.497 | 3.219 | **-28.4%** |
| 端到端编译（词法+语法+代码生成） | 16.610 | 12.930 | **-22.2%** |
| 词法吞吐 normal / numeric / cjk / string | 9.137 / 4.579 / 5.356 / 1.438 | 9.172 / 4.494 / 5.173 / 1.482 | ±3.5% 布局噪声带 |

- 端到端 -3.68 ms 中 parse-only 只解释 -1.28 ms，其余约 -2.4 ms 来自 CodeGen 遍历的局部性改善
  （子节点从 malloc 散布变为 arena 紧排，逐节点 accept/取成员的 cache 缺失减少）。
- 内存基本持平：bench 进程峰值 RSS 118.4 -> 113.9 MB（-3.8%，峰值由词法 token 表主导，AST 份额小）；
  真实解释器编译 3.17 MB 源（26k 函数定义）探针 79.3 -> 81.6 MB（+3.0%）。持平的原因：mimalloc 对
  小分配本就紧排（无逐节点头部开销，经典 arena 的内存优势在它面前不成立），arena 侧的块翻倍闲置与
  「旧缓冲搬移后弃置不复用」把紧排收益抵回。**本批定性为纯性能批，不作内存收益主张。**
- 等价性：dump 输出逐字节断言（test_parser）、双配置（normal + TagValue）全量 ctest 1342 x 2 绿
  （含 LanguageCorpus 语料端到端）。

### 扩容策略换装：2x 翻倍 → 量子取整（2026-10-07，工作区待 review）

2x 增长换为按需扩块（新块 = 请求字节数取整到 1 MiB 量子，首块保持 256 KB）。依据：bump 扩块无搬移
成本可摊销，几何倍增只剩尾块闲置。探针实测（§3 同款，内容 = 可达节点 + 列表缓冲 + 指针数组）：

| 源 | 内容 | allocated（2x） | allocated（量子） | 变化 |
| --- | --- | --- | --- | --- |
| 宽源 0.94 MB / 335k token | 10.44 MB | 16.52 MB（闲置 36.8%） | 10.75 MB（闲置 ~3%） | **-34.9%** |
| 重源 3.17 MB / 884k token | 29.33 MB（含投机死节点 3.33 MB） | 33.29 MB（闲置 11.9%） | 30.67 MB（闲置 4.4%） | **-7.9%** |

- 重源降幅小的原因：其 2x 块阶梯恰处幸运位（闲置本就只 12%）；残余 4.4% 是量子取整与退役块尾差，
  内容侧 13.4% 的死节点（`[` 投机解析弃置，Parser.cpp assignment 位）与增长策略无关、两态同担。
- **macOS 峰值 RSS 持平**：宽/重源探针 +0.1~0.5%、lexer_bench 带内——闲置页从未触碰本就不驻留，
  收益在虚拟足迹与 Windows commit 侧（后者本机不可测）。
- **量子不取小值的依据**：256 KB 量子档实测探针 RSS +2.4~3.0%（小块走 mimalloc large 类，逐块 ~17 KB
  常驻开销），512 KB 减半，1 MiB 起归零。
- 时间 ABBA 3 轮交叉 min：五形态 + 端到端全部 ±2% 噪声带，中性。
- 等价性：真实仓库全量 ctest 绿（见落地记录）；探针两源解析无异常。

## 3. 复现

```sh
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release -DARIA_ENABLE_LTO=OFF -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
cmake --build build/rel --target lexer_bench -j
./build/rel/bench/lexer_bench   # lex-only / parse-only / ast 计数行为实验分支 bench 临时插桩
```

对照臂 = main（`461eb94..091bd7b` 编译线终点）同配方构建；两臂 bench 代码除按各自 API 写的
parse-only 计时与计数打印外逐行相同。RSS 用 `/usr/bin/time -l`，各 3 轮取 min。

## 4. 遗留观察（不绑定结论）

- CodeGen 的 `HiddenNameSource`（隐藏局部名宿主）与本批无关，仍自备稳定宿主，勿因「反正有 arena」
  而把它挪进 arena——其存活语义独立。
