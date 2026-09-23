# M5 类与对象实现坑点记录

> M5 类（设计基线见 `m5-class-implementation-plan.md`，以六项设计决策与各决策补记为准）在 M4 的闭包/值栈机制上叠加了「类表 + 实例 fields 表（+ 早期的 bound-method 缓存，已于 2026-09-20 取消）+ 方法帧槽 0 = this」维度。阶段 2 整改（虚函数协议 / 错误通道 vm.fail 模型 / 检查分层）已归档进该计划补记，本文不重复；只录实施/复盘期确认的四个坑点高发区。对策均已实施，测试钉在 `tests/runtime/test_ariavm.cpp`（类机制节）与 `tests/compile/test_codegen.cpp`（M5 节）。

## 坑 #1：bound-method 缓存与遮蔽的交互 -- 三铁则 + 首解析快照（**已反转：缓存取消**）

**结局（2026-09-20）**：整个缓存机制连同三铁则一起删除，见 `collections-builtin-methods-plan.md` §4.6。反转理由是语义而非性能：缓存让「类/父类上改写方法」对**既有实例**陈旧、对**新建实例**新鲜，同一表达式的结果取决于该实例是否取过该方法（实测：`C.who = f` 后老实例仍返旧值、新实例返新值），属最难解释的一类语义。取消后成员解析每次都按当前类链进行，monkey patch 完整可用；调用路径经 `load_field_unbound` 走不绑定形态（零分配），代价只在读路径（`obj.m` 每次访问铸一个 bound，与 JS/Python 同款）。

**保留的历史记录**（当时缓存与真字段同表同 keyspace，三类误用都是隐性 bug）：

1. **缓存静态值** -- 静态槽可变（`Foo.x = v2` 改类表），值缓存进实例后 `obj.x` 读到陈旧数据。只缓存绑定方法（需要分配的），静态值每次直读。
2. **`LOAD_SUPER_FIELD` 写缓存** -- super 查到的是 defining class 链上被覆写**前**的实现，若回填 fields 表，后续 `obj.m` 动态派发会被 fields 命中劫持到父类实现。只有 `obj.m` 动态路径命中类表方法才回填。
3. **fields 命中优先** -- 本条是特性不是坑：`this.m = v` 落表即覆盖同名方法/缓存项（实例属性遮蔽），故缓存无需任何失效处理。

**取消后仍成立的两条**：`fields_` 命中优先（现在是纯字段遮蔽类链，语义不变）；`LOAD_SUPER_FIELD` 不写 fields（现在无缓存可写，但「super 站点的解析结果不得驻留成实例成员」这条纪律保留 -- 它靠 `LOAD_SUPER_FIELD` 走独立的 `ObjClass::load_field` 实现，与实例表无关）。

**取消后的等价语义**：类上赋值改写方法槽后，**新解析一律见新值**（每次访问现场解析，实例上不留任何陈旧状态）；仍「沿用旧行为」的只有**已经取出的那个绑定值本身**--它是普通值拷贝，与后续改写无关（`MethodRewriteViaClassAssignmentSnapshot` 钉的就是这一条，不是实例级快照）。

**测试**：`StaticCallableReadsRawOnInstance`（静态槽读原值不绑定）；`SuperCallDoesNotPolluteCache`（super 调用后两次 `obj.m` 均仍走子类实现）；`InstanceFieldShadowsStatic`（实例字段遮蔽类链）；`MethodRewriteViaClassAssignmentSnapshot`；`ClassGraphSurvivesExplicitCollect`（类图跨显式 collect 存活）。GC 侧零额外负担：绑定值本体经实例/类表的 trace 级联保命，无独立缓存表要标。

## 坑 #2：MAKE_* / LOAD_FIELD 的 peek-不弹栈纪律 -- 白色对象发布与「class 留栈」

**风险面**：`new_class`/`new_bound_method`/`new_instance` 顶部 `maybe_collect` 是真 GC 点；工厂返回对象**白色无根**，建成后的下一个动作必须是发布进根（写回原槽 = 值栈根）。def lowering 里 class 是全部成员发射的底座（`MAKE_CLASS [super] -> [class]` 后 class 恒留栈），任何「先弹后用」的写法都会在成员注册前把类丢进无根窗口。

- `MAKE_CLASS`：**peek super 不先弹** -- super（可能刚经 `LOAD_GLOBAL` 解出）跨 `new_class` 分配必须在栈；新建类写回原槽，top 不变。
- `MAKE_METHOD`/`MAKE_STATIC`：`[class, v] -> [class]`，两值均 peek 不弹（`set_field` 落表走 trivial 分配不触 GC，戳 defining class 是裸写），弹值留 class；编译器路径栈形 ASSERT 钉。
- `LOAD_FIELD`：obj = peek(0) **不先弹**（绑定分配跨 GC 须保 obj 在栈）；命中方法闭包时 `new_bound_method` 返回白色 bound，**先写回原槽根化、再回填 fields 表** -- 落表是 trivial 分配不触 GC（GC 核心不变式），发布后 bound 获栈 + 实例内引用双根。
- `call_class`：`new_instance` 是唯一 GC 点（klass 经值栈根化），instance 建成即写 callee 槽（**槽 0 原位换实例** = 新帧 this），余下交 `call_value` 通用分发。

**反向坑（守卫过度）**：阶段 2 曾按「表 upsert 的 rehash 分配跨 GC 须守卫」写三处多余守卫 -- 认知错误，`HashTable` set/upsert 走 trivial 分配**永不触发 GC**（GC 核心不变式），review 后整删（其中 `MAKE_CLASS` 的 name 实为常量池可达，连 weak root 都不是）。守卫纪律按「真 GC 点」划，不按「看起来像分配」划。

**测试**：`ClassGraphSurvivesExplicitCollect`（实例/绑定/类跨 GC 存活）、`StoreFieldDeepStackShift`（`STORE_FIELD` 单槽下移在深栈多临时值下）、`ThisFieldDeepStackInMethod`。

## 坑 #3：init_ 的一致性 -- 两写点 + 快照语义 + Value 形态兜底

**坑本体（机制演化三轮，勿按旧文案理解现码）**：初稿「`MAKE_CLASS` seed + `MAKE_METHOD("init")` 覆盖」两时点 -> 中间稿「seed 收进 `new_class` 工厂」-> 终态「**`ObjClass` 构造函数自 super 派生**（ctor 初始化列表 `init_{super != nullptr ? super->init() : nil}`）+ `set_field` 命中 "init" 同步」，`set_init` 公共可变器退役（单消费者 setter 收进 ctor）。表槽/init_ 一致由对象自维护（同步内聚在唯一公开写入口 `set_field`），「建成的类 init_ 恒有值」成为构造期结构保证。

- **快照语义**：ctor 拷贝父 init_ 当前值，此后父 init 变更（`MAKE_METHOD`/类上赋值）**不传导** -- 有意快照，非漏同步。
- **Value 形态**：init_ 是 `Value`（闭包/原生皆可）；类上赋非可调用值放行（`Foo.init = 5`），实例化时 `call_value` 报 `CallNonCallable` 兜底 -- 检查在**调用点**不在写点（「init 仅认闭包」的写点检查是死代码，已删）。
- **实例化帧形**：init 闭包靠编译器尾部 `LOAD_LOCAL 0; RETURN` 返 this；Object 根的 no-op 原生 init 不写槽即返回实例（`slots[0]` 已是 this）。

**测试**：`InstantiateNoInitUsesSeededNativeInit` / `InheritanceOverrideAndSuperCall` / `NativeInitInstantiates` / `ClassInitAssignNonCallableErrorsOnInstantiate` / `CtorDerivesInitFromSuper`（tests/object，钉 ctor 派生 + 快照 + 根态 nil）。

## 坑 #4：Locate 模式与既有复合赋值机制的合流 -- THIS_FIELD 系不经栈取 this，DUP 副本无人消费（阶段 3b 复盘实证）

**现象**：直接方法帧内 `this.f += e` / `++this.f` 每条语句泄漏一个栈槽；泄漏值把后续局部槽位顶偏一位，报错形态离奇（`var b = 2` 的槽读到泄漏实例 -> `TypeMismatch "operator '+' requires numbers, got Int and Instance"`）。

**根因**：一般定位腿 `<obj> DUP LOAD_FIELD` 的 DUP 副本留给收尾 `STORE_FIELD` 消费（`[obj,v] -> [v]` 消费接收者）；但 this 目标的复合流 Store 腿命中 THIS_FIELD 分支发 `STORE_THIS_FIELD`（peek-store `[v] -> [v]`，this 取帧槽 0 只消费值）-> DUP 副本滞留栈上。嵌套捕获 this 走一般 Store（`STORE_FIELD` 消费副本）本就平衡，泄漏仅限直接方法帧。

**全绿掩盖**：直线测试里泄漏值随帧丢弃、不撞 `POP_N`，768 绿下带病落地 -- **全绿不证栈平衡**；帧内泄漏类改动须配「泄漏撞 `POP_N`/改变量」的钉子。

**对策**：编译期常量 locator（帧内 this / Identifier）**节点侧折叠** -- Locate 与 Load 同臂发同指令（`visitIdentifierNode` `case Load: case Locate:` 折叠、帧内 this 三臂折叠），不发 DUP；一般路径 `obj.f op= e` 保持 DUP 定位腿（`STORE_FIELD` 消费副本，几何平衡）。全组合发射矩阵与逐行栈形见 `compound-assignment-lowering.md` §6。

**测试**：`ThisFieldCompoundAsCallArg`（复合赋值作调用实参 `h(1, this.n += 3)` -- 暴露形态：修复前副本把 callee 顶偏报 `CallNonCallable`，修复后 110）、`CompoundAssignOnThisField`。