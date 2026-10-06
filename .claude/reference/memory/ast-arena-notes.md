# AST bump arena（实验批）实测与机制

> 源码位置：`src/memory/AstArena.hpp`（AstArena / AstList，消费方在 compile 层）、`src/compile/ast.hpp`（节点成员换装）
> 模块参考：`.claude/rules/memory.md`（AstArena / RawAlloc 条）、`.claude/rules/compile.md`（ast / Parser / Compiler 条）
> 用途：记录把 AST 所有权从 `UPtr` 递归析构换成 arena 整批释放的机制契约与实测数字；动 Parser /
> CodeGen 的分配与遍历路径前先读这篇。

## 1. 机制契约

- `AstArena`：块式 bump 分配器。节点与列表缓冲自大块（首块 256 KB，放不下按 2 倍翻新块）顺序分配，
  析构整批释放、**不跑任何析构函数**；块链头兼当前填充块（新块恒头插、只从头分配，单 `head_` 游标即足）。
  后备经 `mem::alloc/free`（RawAlloc 同族三口，随 `ARIA_USE_MIMALLOC` 开关），分配失败
  `fatal_error(OutOfMemory)`、全程不抛（先例 `GC::allocate`）。
  成立前提是节点成员全为平凡类型：子节点裸指针互指、名字/字符串借源缓冲、列表为 `AstList`——这些由
  ast.hpp 节点形态保证，新增节点成员不得引入自持堆资源。
- 列表视图：`make_list(List<T>&&)` 把解析期局部 vector 一次性搬入 arena，返回 `Span<T>`（std::span，
  {data, usize size}，16 B）；空表零分配，元素须平凡可析构（static_assert 钉住）。`make_list` 是节点
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

## 3. 复现

```sh
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release -DARIA_ENABLE_LTO=OFF -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
cmake --build build/rel --target lexer_bench -j
./build/rel/bench/lexer_bench   # lex-only / parse-only / ast 计数行为实验分支 bench 临时插桩
```

对照臂 = main（`461eb94..091bd7b` 编译线终点）同配方构建；两臂 bench 代码除按各自 API 写的
parse-only 计时与计数打印外逐行相同。RSS 用 `/usr/bin/time -l`，各 3 轮取 min。

## 4. 遗留观察（不绑定结论）

- arena 增长策略翻倍（2x）在巨型源上闲置比例可观，若将来在意编译峰值可试 1.5x 或尾部块回收，先量后改。
- `AstList` 用 u32 计数，节点/元素规模在 u32 域内（ASSERT 兜底）；单文件 AST 实际远达不到边界。
- CodeGen 的 `HiddenNameSource`（隐藏局部名宿主）与本批无关，仍自备稳定宿主，勿因「反正有 arena」
  而把它挪进 arena——其存活语义独立。
