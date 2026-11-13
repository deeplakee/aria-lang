# AST bump arena 机制契约

> 源码位置：`src/memory/AstArena.hpp`、`src/compile/Ast.hpp`（节点成员形态）
> 模块参考：`.agents/reference/memory.md`（AstArena / RawAlloc 条）、`.agents/reference/compile.md`（Ast / Parser / Compiler 条）
> 用途：动 Parser / CodeGen 的分配与遍历路径前先读。这里只收**契约**与**设计依据**；测量数字是 bench 输出，不在文档留档。

## 1. 契约

- **块式 bump 分配**：节点与列表缓冲自大块顺序分配（首块 256 KB，放不下按请求字节数取整到 1 MiB 量子扩新块），析构整批释放、**不跑任何析构函数**；块链头兼当前填充块（新块恒头插、只从头分配，单 `head_` 游标即足）。后备经 `mem::alloc/free`（`ARIA_USE_MIMALLOC` 只换后端），失败 `fatal_error(OutOfMemory)`、全程不抛。
- **成立前提：节点成员全为平凡类型**。子节点裸指针互指、名字/字符串借源缓冲、列表为 `Span` 视图--新增节点成员不得引入自持堆资源，否则整批释放即泄漏。
- **块基址只保证 16 B 对齐**（malloc 对齐 + `Block` 头偏移）。`alignas` 超过 16 的节点类型不保证对齐；要加这类成员先改分配器。
- **`make_list(const List<T>&)` 是节点列表字段的唯一生产口**：元素一次性拷入 arena、返回 `Span<T>`（空表零分配，元素须平凡可拷贝，static_assert 钉住）。`std::span` 会自左值容器隐式转换，所以「局部容器直接喂节点字段」会**静默别名悬垂**--这是约定纪律、不是类型保证。
- **生命周期**：`Compiler::compile` 栈上构造 arena，`Parser::parse(AstArena&, ...)` 返回借用的 `ProgramNode*`，CodeGen 消费完随 `compile()` 返回整批释放。异常路径（panic-mode 恢复、投机解析回退）弃掉的节点不单独回收，留作死区到整批释放。
- 节点虚析构只为多态保留（dynamic_cast 与 vtable 既有），从不单独调用。

## 2. 设计依据

- **收益是局部性，不是内存**：子节点从 malloc 散布变为 arena 紧排，逐节点 `accept`/取成员的 cache 缺失减少，parse 与整趟编译都受益。**不作内存收益主张**：mimalloc 对小分配本就紧排，经典 arena 的内存优势在它面前不成立，块闲置与旧缓冲弃置把紧排收益抵回。
- **扩块取按需 + 量子取整**：bump 扩块无搬移成本可摊销，几何倍增只剩尾块闲置；量子不取小值（小量子档逐块常驻开销，1 MiB 起归零）；闲置页从未触碰、不驻留，收益在虚拟足迹与 Windows commit 侧。
- 行为等价由双配置全量 ctest 与语料覆盖；动分配/遍历路径时另做一次临时对拍更稳（见 `compile/lexer.md` 的同款纪律）。

## 3. 两条边界

- **`HiddenNameSource`（隐藏局部名宿主）不得挪进 arena**：它的存活语义独立于 AST 树（名字要活到编译结束、而树是按遍历序消费的），挪进来只会把生命周期搅在一起。
- **arena 不用于运行期**：它不跑析构，只适合「整批同时释放」的编译期数据。运行期对象一律走 GC。
