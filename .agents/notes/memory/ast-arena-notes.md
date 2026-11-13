# AST bump arena 机制契约

> 源码位置：`src/memory/AstArena.hpp`（AstArena / Span 列表视图，消费方在 compile 层）、`src/compile/Ast.hpp`（节点成员换装）
> 模块参考：`.agents/rules/memory.md`（AstArena / RawAlloc 条）、`.agents/rules/compile.md`（ast / Parser / Compiler 条）
> 用途：记录把 AST 所有权从 `UPtr` 递归析构换成 arena 整批释放的机制契约与设计依据；动 Parser /
> CodeGen 的分配与遍历路径前先读这篇。测量数字是 bench 的输出，不在文档里留档。

## 1. 机制契约

- `AstArena`：块式 bump 分配器。节点与列表缓冲自大块（首块 256 KB，放不下按请求字节数取整到 1 MiB 量子扩新块）顺序分配，
  析构整批释放、**不跑任何析构函数**；块链头兼当前填充块（新块恒头插、只从头分配，单 `head_` 游标即足）。
  后备经 `mem::alloc/free`（RawAlloc 同族三口，随 `ARIA_USE_MIMALLOC` 开关），分配失败
  `fatal_error(OutOfMemory)`、全程不抛（先例 `GC::allocate`）。
  成立前提是节点成员全为平凡类型：子节点裸指针互指、名字/字符串借源缓冲、列表为 Span 视图——这些由
  Ast.hpp 节点形态保证，新增节点成员不得引入自持堆资源。
- 列表视图：`make_list(const List<T>&)` 把解析期局部 vector 的元素一次性拷入 arena，返回 `Span<T>`（std::span，
  {data, usize size}，16 B）；空表零分配，元素须平凡可拷贝（static_assert 钉住）。`make_list` 是节点
  列表字段的唯一生产口——std::span 会自 lvalue 容器隐式转换，局部容器直接喂节点字段会静默别名悬垂，
  这是约定纪律而非类型保证。
- 生命周期：`Compiler::compile` 栈上构造 arena，`Parser::parse(AstArena&, ...)` 返回借用的
  `ProgramNode*`，CodeGen 消费完后随 `compile()` 返回整批释放。异常路径（panic-mode 恢复、投机解析
  回退）弃掉的节点不单独回收，留 arena 死区到整批释放。
- 节点虚析构仅为多态保留（dynamic_cast 与 vtable 既有），从不单独调用。

## 2. 设计依据

- arena 换装是纯性能批：parse 与端到端编译显著变快，收益除建树外还来自 CodeGen 遍历的局部性（子节点从
  malloc 散布变为 arena 紧排，逐节点 accept/取成员的 cache 缺失减少）。**不作内存收益主张**：mimalloc
  对小分配本就紧排，经典 arena 的内存优势在它面前不成立，块闲置与旧缓冲弃置把紧排收益抵回。
- **扩块策略取按需扩块 + 量子取整**（新块 = 请求字节数取整到 1 MiB 量子，首块 256 KB）：bump 扩块无搬移
  成本可摊销，几何倍增只剩尾块闲置；量子不取小值（小量子档逐块常驻开销，1 MiB 起归零）；闲置页从未
  触碰、不驻留，收益在虚拟足迹与 Windows commit 侧。
- 行为等价由全 token dump 对拍与双配置全量 ctest 验收。

## 3. 遗留观察（不绑定结论）

- CodeGen 的 `HiddenNameSource`（隐藏局部名宿主）与本批无关，仍自备稳定宿主，勿因「反正有 arena」
  而把它挪进 arena——其存活语义独立。
