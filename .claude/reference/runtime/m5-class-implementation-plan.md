# M5 类与对象实施计划

> 状态：**已定稿、未实施**（2026-09-09）。M5 开工前重读。验收 = 路线表 M5 样例（类定义/实例化/继承/super）+ 默认与 `build/tagvalue` 双配置 ctest 全绿 + `--eval` 冒烟。
> 行号锚点基于定稿时 HEAD（commit `cc7d927`），后续改动会使行号漂移，定位以符号/描述为准。
>
> **决策记录（2026-09-09，六项定夺）**：
> 1. `ObjClass` **不设 meta 元类指针**（grammar「meta（元类，未来）」裁撤，YAGNI；将来要再加不动 `ObjType`）。
> 2. **静态变量与静态/实例方法同存一张哈希表**（`AriaHashTable<Value,Value>`，键 intern `ObjString`）；静态值/方法的区分在值类型本身（是否 `ObjClosure`），表内无 tag。
> 3. **Object 根类由 VM 构造期 bootstrap，VM 成员单独持有**（不进 `builtins_`/任何模块 globals -- 裸名解析（局部 -> upvalue -> 模块全局 -> builtins 回退）全部够不到，正常代码访问不到；`LOAD_OBJECT` 直推 VM 成员）。
> 4. **方法查找后的 bound-method 缓存写进实例的 fields 哈希表**（与真字段同表同 keyspace），三条铁则见 §2.4。
> 5. **`STORE_FIELD` peek-store 与「MAKE_CLASS 后 class 始终留栈」的衔接取镜像双指令**：`STORE_FIELD [obj,v] -> [v]`（弹 obj 留 v，服务体外赋值）与 `MAKE_STATIC [class,value] -> [class]`（弹 value 留 class，服务体内创建）互为镜像、各自零冗余；DUP/POP 凑法弃（详见 §2.5）。
> 6. **defining class 挂 `ObjClosure` 而非 `ObjFunction`**（grammar 原文「ObjFn 持 defining class」随落地改写，理由见 §2.6）。
>
> **决策补记（2026-09-09，阶段 1 落地 review 改定）**：类成员继承语义由原「共享槽」改定 **Python/JS class attributes 式「读穿透、写遮蔽」**（决策 2 的单表部分不变）：读沿链 fall-through；类上赋值 `Sub.x = v` 落**接收类自身**表--继承来的名字新建遮蔽键、本类已有则原槽更新；沿链全 miss 的新名字仍拒（无 monkey-patch 不变，静态必须 `var` 声明；**2026-09-11 改定：此「仍拒」废止**--类上赋值新名字亦落接收类自身表，动态新增允许，见下方决策补记）。方法槽亦允许经类上赋值改写（不加闭包槽守卫，用户定夺）：bound-method 缓存取**首解析快照**语义（已解析实例沿用旧绑定、新解析取新闭包，仍免失效机制）；`"init"` 赋值特例同步 `set_init`（值非 ObjClosure 则 fail TypeMismatch），保「init_ 与类表 init 槽一致」的实例化不变式。`find_field` 随之收缩为 `Value*` 槽指针（owner 返回值裁撤--它唯一消费者是共享槽写；读与存在性检查吃槽指针足够），`upsert_field` 从「创建路径专用」转正为类成员写路径本体。（2026-09-10 整改四续：find_field/upsert_field 已转 **ObjClass 私有实现细节**，公开写入口收敛为 `set_field(name, value)`，见决策补记四。）
>
> **决策补记（2026-09-10，阶段 2 落地 review 改定）**：方法槽泛化支持 **ObjNativeFn**（uniform「可调用一律绑定」，内建类型方法的承载路径）：类表本就是 Value 型单表（决策 2），可绑定判定由「是 ObjClosure」放宽为**可调用值**（CLOSURE|NATIVE_FN，`is_callable_value` 谓词，全 VM 唯一扩展缝）；`ObjBoundMethod.method_` 由 `ObjClosure*` 泛化为 `Value`，`call_bound_method` 按内部分派 -- 原生不进帧，绑定调用区槽 0（bound 对象）覆写为 receiver 得契约形 `[this, a1..aN]`（**slots[0] = this 兼返回槽**，实参槽位与自由调用一致、零整形，与既有 NativeFn 契约「槽 0 双职」同构），闭包走原整形进帧。副作用语义：静态槽持可调用值经实例读取**同样绑定**（Python 函数语义；对现有不读槽 0 的内建零观察差异。**本句随 2026-09-11 补记「super 静态访问放开 + 方法性注册期定」废止：方法性改 defining class 戳判别，静态槽持函数值/原生读恒原值 --存档留痕**）；`LOAD_SUPER_METHOD` 命中原生同绑（**同随 2026-09-11 补记退役**）；**init 槽仍仅认闭包**（原生 init 拒写 -- 闭包 init 靠编译器尾部 `LOAD_LOCAL 0; RETURN` 实现「`Foo()` 得实例」，原生无此约定，实例化帧形/快路径前提只对闭包成立。**本句已被下方补记（三）的 init Value 化整体推翻，存档留痕**）；快照语义对原生同成立（类上改写方法槽为另一原生，已绑定实例沿旧绑定）。非可调用静态值直读不缓存不变（铁则 1 收紧措辞为「只缓存绑定--需要分配的--方法」）。
>
> **决策补记（2026-09-10，阶段 2 review 改定，二）**：方法帧形由 `[closure, this, a1..aN]` 改定 **`[this, a1..aN]`** -- this 替代 callee 占槽 0（对齐旧版解释器 `call_bound_method` 的「槽 0 原位覆写 receiver_ + 普通 call_function」与 clox 方法语义，`super.m` 经 LOAD_SUPER_METHOD 压栈的 bound 亦同形）；`enter_method_frame`/`init_frame_` slot_extra 方案裁撤，Movement 恢复唯一 `enter_frame(closure, argc)`（slots = top - argc - 1，槽 0 的内容由调用方进帧前写定），`call_class` 慢路径 = callee 槽原位换 instance + init 普通进帧；THIS 系/SUPER 读帧槽 0；this 的 upvalue 捕获槽 0（M4 机制零改动）；init 返回 this = `LOAD_LOCAL 0; RETURN`（编译器发射，测试手写同形）；方法闭包不上栈、经 `frame.closure` 携带（帧 tracer 根化）。原「槽 1」表述为定稿期笔误级偏差，随本补记全面修正。
>
> **决策补记（2026-09-10，阶段 2 整改定案，三 -- 用户 review 驱动）**：四项改定。
> ① **init Value 化**：`ObjClass::init_` 由 `ObjClosure*` 改 **`Value`**（闭包/原生皆可；泛化叙事自洽 --「可调用一律绑定」不再对 init 特例）：Object 根类 init 改**原生 no-op**（`return true` 不写槽，slots[0] 已是 this 即返回实例 -- 消除合成闭包的 module=nullptr 全项目唯一例外，ObjFunction「module 恒非空」不变式恢复）；**init seed 收进 `new_class` 工厂**（super 非空出厂即继承父 init，不变式收进对象构造；Object 根态由 bootstrap 设。**本句 seed 归工厂部分已随下方「init seed 移出工厂」补记改回指令层、再随 2026-09-11「init seed 收进构造函数」补记收进 ObjClass 构造函数 --存档留痕**）；`call_class` 收敛为三步（`new_instance` + 槽 0 原位换实例 + `call_value(cls->init(), argc)`），快路径（指针同一性 + argc==0）与 init 空判删除；「init 仅认闭包」两处检查（MAKE_METHOD 拒原生、STORE_FIELD 拒非闭包）退役死代码删除 -- 类上赋非可调用值放行（store_field 同步 init_），实例化时 call_value 报 CallNonCallable 兜底；「init」赋值同步逻辑内聚进 `ObjClass::set_field`（表槽/init_ 一致由对象自维护；store_member 名随整改四改名）。
> ② **Object 基类虚函数协议**：新增 `load_field/store_field`（命名成员读写协议，LOAD/STORE_FIELD 族统一分派点 --「忘记规划虚函数，VM 长出一组分型辅助方法」的整改；后续增补：`op_call(AriaVM&, Span<Value> slots) -> bool` 可调用协议（2026-09-11，备置 API --签名与 NativeFn 契约同构、slots[0]=callee 双职兼返回槽；call_value 的 switch 对已实装可调用类型精确分派，其余类型落基类默认 fail CallNonCallable；未来可调用新类型 override 即接入，不改 switch）；错误通道约定：协议不碰 VM 寄存器、**不构造 ObjException**（完整烘焙消息含位置前缀，位置只有 VM 知道），load nullopt / store 三态 `StoreResult{Stored,Rejected,Unsupported}` 即未命中/拒绝/不支持，消息 VM 侧按类别统一烘焙、对象描述走非重入 format_value_debug）+ `load_index/store_index`（**下标读写协议，备置 API：现在落地、暂无 override/调用方**，LOAD/STORE_INDEX 接线留容器里程碑，三态同 store_field）+ `op_add/op_sub/op_mul/op_div/op_mod/op_negate`（**算术虚函数族，备置 API：现在落地、暂无 override/调用方**，容器里程碑/运算符重载立项时 VM 只需按协议虚分派）。`ObjInstance::load_field`（fields 优先 + 绑定 + 缓存回填，自 VM helper `load_instance_member` 迁入，该 helper 退役）、`ObjClass::load_field`（沿链读穿透直读）、两 `store_field` override 落地。`is_callable_value` 谓词收口 `value/ObjBridge.hpp`（VM/ObjInstance/ObjBoundMethod ctor 三方共用）。
> ③ **守卫纪律按 GC 核心不变式重写**：HashTable upsert/rehash 走 trivial 分配**永不触发 GC**（GC.hpp 核心不变式），此前「upsert 的 rehash 分配跨 GC 须守卫/须在栈」注释认知错误（IMPORT 注释同错一并修正）--删除三处多余守卫（call_bound_method 的 method 守卫、load_instance_member 的 bound 守卫、MAKE_CLASS 的 name 守卫，name 实为常量池可达非 weak root）。
> ④ **检查分层重整**（内部用码、边界用 Error）：语言不可达的编译器不变式防线一律 ASSERT 化（LOAD/STORE_THIS_FIELD 的 this 非实例、LOAD_SUPER_METHOD 的 defining/super nullptr -- 两运行期错误测试随之退役）；语言可达的活站点保留 raise（MAKE_CLASS 的 super 非类：`var Bar = 5; def Foo : Bar` 合法，superclass 运行期才知值类型；super 命中静态值**--2026-09-11 改定放开为直读压栈，见下方 super 静态访问补记**；全链 miss），类型检查统一 `try_obj/try_as` 一步收口。
>
> **决策补记（2026-09-10，阶段 2 整改定案，四 -- 用户 review 驱动，API 收口）**：五项改定。
> ① **协议改名**：`load_member/store_member` 改名 **`load_field/store_field`**（与 LOAD/STORE_FIELD 指令族对名，"member" 退役）。
> ② **store 协议三态化**：`store_field` 返回由 bool 改 **`StoreResult{Stored, Rejected, Unsupported}`**（enum 定义于 Object.hpp）--携带「错误类别」信息供 VM 分消息（Rejected = 类沿链 miss 拒新增，带 var 声明提示；Unsupported = 类型不支持，与原语同文案；原 bool 单一消息对非类对象会误印「static members must be declared with var」）。**不改 `Result<Value, ObjException*>`**（评审备选）：ObjException 的完整烘焙消息含位置前缀（装箱点经顶帧 last_ip 查行号表烘入），位置只有 VM 知道、object 层给不出；协议保持「结果 + 类别」、位置与烘焙归 VM（错误通道分工见 Object.hpp 协议注释）。（2026-09-10 位置去烘焙后续：消息不再含位置前缀，「位置只有 VM 知道」的论证随之退役；不改 Result 的现行理由 = 码→文案映射单一事实源在 VM + 协议 miss 路径零分配不触 GC。）
> ③ **下标协议备置**：新增 `load_index(GC&, Value key) -> Opt<Value>` / `store_index(GC&, Value key, Value) -> StoreResult`（暂无 override 与调用方，LOAD/STORE_INDEX 接线留容器里程碑；与 op_* 算术协议同理，避免「忘记规划虚函数」覆辙）。
> ④ **find_field/upsert_field 转私有 + set_field 公开**：类成员的公开触面收敛为协议（load_field/store_field）与创建写入口 **`set_field(name, value)`**（void 永不失败，落本类自身表；命中 "init" 同步 init_ --bootstrap/MAKE_METHOD/store_field 三路写合流于此，表槽/init_ 一致由本入口自维护）；`*cls->upsert_field(name) = v` 槽指针风格退役，find_field/upsert_field 为 ObjClass 私有实现细节（VM/测试一律走公开 API）。
> ⑤ **VM case 体收口**：LOAD_FIELD/STORE_FIELD/LOAD_SUPER_METHOD 三指令执行体封装为私有 `run_load_field/run_store_field/run_load_super_method`（与 run_binary_numeric/call_value 族同款 bool 契约：成功含栈形收口返 true、失败 raise 入寄存器返 false，case 体只留「读操作数 + 调执行体 + unwind 善后」）；THIS 对（LOAD/STORE_THIS_FIELD）case 内直调协议**不设执行体**（编译器不变式保证 this 恒实例、无守卫分支，化简后与执行体等长）；LOAD_SUPER_METHOD 的链查找由直调 find_field 改走 **ObjClass::load_field 协议**（起点即 super，语义等价、不再触类内表布局）。
> ⑥ **nil 接收者并入非对象统一文案**（用户化简驱动）：LOAD/STORE_FIELD 对 nil 不再特判 `NilDereference`（"cannot read/set property 'x' of nil"），nil 与原语同走 `UndefinedProperty` "type Nil does not support field access"（类型名已可辨识，分支少一层）；两运行期测试钉子随迁 `ErrorCode::UndefinedProperty`；`NilDereference` 码自此暂无抛出点，留作下标/调用接线预留（ErrorCode.hpp 注释同步）。**2026-09-11 改定：该码删除**（无抛出点的预留码不再保留，nil 场景已并入 UndefinedProperty、下标场景届时并入 TypeMismatch/KeyError）。
>
> **决策补记（2026-09-10，二次整改 -- 用户 review 驱动，协议错误通道模型）**：五项改定（前次整改②③④⑤ 的对应条目随本条修订）。
> ① **错误通道翻 vm.fail 模型**：协议签名一律收 `AriaVM&`（native fn 契约同构 --NativeFn 形参持 `AriaVM&` 先例），失败 `vm.fail(code, fmt, ...)` **就地烘焙**入寄存器，返回值只留信号（load `Opt<Value>`：nullopt ⟺ 已 fail；store `bool`：false ⟺ 已 fail）；错误实体构造即入寄存器即被 VM 根 tracer 标根，消灭「返回值在途」的白色无根窗口；与 call_value 族 / run_binary_numeric / native fn 全部同构，runtime 在途错误自此只有寄存器一种载体。动机：主流解释器对照（CPython PyErr：信号返回 + 线程态异常，消息就地散落于 listobject.c 等各类型模块；Lua luaG_typeerror 就地烘焙 + longjmp、V8 MaybeHandle + isolate、Wren runtimeError + fiber 同构），「以返回值承载错误对象」无一先例、消息烘焙均就地 --前次整改② 的「不改 `Result<Value, ObjException*>`」结论维持，但落地形态取第三条路 vm.fail 而非「类别返回 + VM 统一烘焙」。
> ② **StoreResult 三态消灭**（前次整改② 条目废止）：Rejected/Unsupported 文案随消息就地化移入 override（`ObjClass::store_field` 拒新增带 var 声明提示 / 基类默认报「type X does not support field access」），「错误类别」枚举失去存在必要 --表达力由 override 自选错误码 + 任意细节取代（load_index 未来越界/键错误即走此路）。
> ③ **find_field 转公开**（前次整改④「find_field 转私有」条目部分修订，upsert_field 维持私有）：组合方（`ObjInstance::load_field` 委托类链、VM 的 LOAD_SUPER_METHOD 站点）经纯查询先查后报，把 miss 文案权留在最知道语境的组合外层 --协议 fail 以本类型措辞就地烘焙，组合场景直走协议会吞掉外层措辞（实例路径要保持实例措辞、super 站点要保持 method 措辞）。
> ④ **super 链查找改回 find_field**（前次整改⑤ 末条目修订）：`run_load_super_method` 从 `super->load_field` 改 `super->find_field` 纯查询（起点即 super、不含 defining 自身不变），站点自持「class X has no method 'm'」文案；§5.5 执行清单 LOAD_FIELD 条的「经 ObjClass::load_field 协议」随本条与③ 修订为 find_field 纯查询。
> ⑤ **基类默认体移 Object.cpp + VM 执行体收成信号透传**：基类默认经 vm.fail 模板报错（vm.fail 是 AriaVM.hpp 内模板，而 AriaVM.hpp 经 ObjException.hpp 依赖 Object.hpp、两头互不 include，默认体只能出声明落 .cpp）；VM 侧 run_load_field/run_store_field 只剩「非对象守卫 fail（协议外原语文案留执行体）+ 协议调用 + 信号透传 + 栈形收口」，对象 miss 的错误烘焙职责移入 override；测试断言从返回类别改寄存器取件（码 + 文案子串，take_pending_error 助手）。op_*/load_index/store_index 备置 API 同步翻型（暂无调用方，签名免费改；接线纪律：接收者与 rhs「栈即根」peek 不弹，miss 的 fail 分配与结果路径分配均触 GC --equals 的 GC-pure 契约为反例参照）。
>
> **决策补记（2026-09-10，init seed 移出工厂 --用户驱动，补记（三）① seed 句修订）**：`new_class` 工厂改回**纯分配工厂**（只做一次 new_object、无内部新建对象，与 new_function/new_closure 同纪律）--init 继承是类机制语义，归 VM 指令层、不属对象分配层（初稿设计本就在指令层，补记（三）曾收进工厂，本条改回）；`init_` 出厂恒 nil，「建成的类 init_ 有值」不变式由 VM 侧两 seed 写点维持。`MAKE_CLASS` 执行序列：try_obj 验 super 为类 -> `new_class` -> 写回原槽（发布进根，「栈即根」纪律）-> `set_init(super->init())`（super 已验为类、非类即 raise，无 null 分支；写回与 seed 两步间无 GC 点，seed 直接写安全；沿链语义天然成立 --父的 init_ 已是 seed 后值）；Object 根（super==nullptr）不经 MAKE_CLASS，仍由 bootstrap 设。测试：test_objclass 的 `FactorySeedsInitFromSuper` 改钉新契约 `FactoryDoesNotSeedInit`（工厂只分配不 seed，super 非空出厂亦 nil）；VM 级 seed 行为由既有用例钉住（InstantiateNoInitUsesSeededNativeInit / InheritanceOverrideAndSuperCall / NativeInitInstantiates），行为不变。
>
> **决策补记（2026-09-11，init seed 收进构造函数 --用户驱动再改定，上条补记修订）**：init 继承改由 **`ObjClass` 构造函数自 super 派生**（初始化列表 `init_{super != nullptr ? super->init() : nil}`）。上条「移出工厂」否决的是把语义放进 `new_class`（工厂须纯分配），否决理由不涵盖构造函数；seed 本属对象自身状态初始化，构造函数才是其归宿，`set_init` 公共可变器随之删除（生产调用点唯 MAKE_CLASS 一处，单消费者 setter 退役）。`init_` 写点收敛为对象内两处：**ctor 派生 + set_field 落表同步**；「建成的类 init_ 有值」对 MAKE_CLASS 建的类成为构造期结构保证，不再依赖指令层补写。MAKE_CLASS 收敛为「验 super -> new_class（出厂即继承）-> 写回原槽」三步，原「写回原槽后 set_init、两步间无 GC 点」的序列约束消失（构造原子化；ctor 内读 super->init() 纯读无分配无 GC 点，值经调用方根化的 super 可达）。快照语义不变（ctor 拷贝父 init_ 当前值，此后父 init 经 MAKE_METHOD/类上赋值变更不传导）。Object 根（super==nullptr）出厂 nil，仍由 bootstrap 经 set_field 设。GC 测试注记：init_ 与表槽/super 派生值自此恒同值，`gc.mark_value(init_)` 的「唯一保命路径」合成态不再可经公开 API 构造（原 trace stress 用 set_init 装异于表槽与父链的闭包），init_ 标记经表槽路径覆盖、trace 侧 mark_value(init_) 保留作防御。测试：`FactoryDoesNotSeedInit` 翻转钉新契约 `CtorDerivesInitFromSuper`（ctor 派生 + 快照 + 根态 nil；FactorySeedsInitFromSuper/FactoryDoesNotSeedInit 两轮存档留痕）；trace stress 的 init 改经 set_field 落表（init 断言同步语义不变）；VM 级行为用例不变（InstantiateNoInitUsesSeededNativeInit / InheritanceOverrideAndSuperCall / NativeInitInstantiates）。
>
> **决策补记（2026-09-10，封装收口 --用户 review 驱动，补记（二）③④ 废止）**：**find_field 零类外使用**。① 组合方全部改走协议：`ObjInstance::load_field` 未命中直接**委托 `ObjClass::load_field`**（沿链读穿透直读、命中值原样回传，类协议不绑定不缓存 --绑定与缓存回填仍由实例自持；miss 的类措辞 fail 随协议传播，实例不再自持文案），`run_load_super_method` 从 `super->find_field` 改 `super->load_field`（miss 文案由站点自持「class X has no method 'm'」随协议翻为「<class X> has no member 'm'」，静态值非可调用的 super 站点自持文案保留）；`nullopt`（协议 miss）与 `nullptr`（纯查询 miss）同为未命中信号、无语义差，不再为它保留第二条查询通道。② find_field 随之转**纯私有、零友元**（一次中间态曾以 friend ObjInstance/AriaVM 接缝保组合方，随委托翻型撤销）；整表访问器 `field()`/`fields()` 删除（src 零使用，测试断言翻协议行为级：缓存快照/真字段遮蔽经 load_field/store_field 行为钉）。③ 实例 miss 文案钉子随迁（`<Foo instance> has no member` -> `<class Foo> has no member`，文案随宿主）。
>
> **决策补记（2026-09-11，super 静态访问放开 + 方法性注册期定 --用户驱动）**：三项改定。
> ① **方法性判别翻注册期戳**：读路径（`ObjInstance::load_field` / super 站点）不再按值类型（is_callable_value）判「可绑定」，改判 **defining class 非空的 ObjClosure = 方法闭包**（MAKE_METHOD 注册时戳，戳一职双任：super 来源 + 方法性标记，判别谓词 `is_method(Value)` 收口 ObjBridge；MAKE_STATIC/类上赋值不戳 ⟹ 静态槽持函数值/lambda/原生恒原值直读）。原「静态槽持可调用值经实例读取同样绑定（Python 函数语义）」（补记一）随之废止：函数值静态/静态方法经 obj./super. 读回原值；类上赋值改写方法槽后新解析按静态读原值（方法性随值携带 --类路径拷贝 MAKE_METHOD 出品的戳定闭包如 `Foo.m = Base.m` 保方法性）。
> ② **fun 静态方法改经 MAKE_STATIC 注册**（不戳 defining class；fun 体内 super 本就编译期禁，defining class 对它无用）；MAKE_METHOD 收紧为**仅收闭包**（普通方法 = 戳定闭包；原生落表走 MAKE_STATIC/类上赋值，读恒原值；原生方法绑定随 ① 退役 --内建类型方法留 uniform OOP 在对象协议内实现、不走 ObjClass 表；MAKE_METHOD("init") 配原生的注册形态随之退役，原生 init 经类上赋值路径同步 init_）。
> ③ **`LOAD_SUPER_METHOD` 更名 `LOAD_SUPER_FIELD`**（super 静态访问放开后语义 = 沿父链读成员：方法闭包绑 this、静态槽原值直读，与 LOAD_FIELD 实例路径同构的「super 链读」家族；opcode 数值不变，Disassembler 零改动）。原「static members are not accessible via super」站点文案与补记四「super 命中静态值」raise 列举随本条退役（存档留痕）；阶段 3 随动：`visitFieldAccessNode` Load 模式的 SuperExpr 分支由「fail InvalidSuperUse 兜底」改为发射同指令（指令读 frame.closure 的 defining class，仅直接方法帧可承载，嵌套函数内 super 的处理随阶段 3 定）。（查找+绑定留在 VM 执行体站点 --2026-09-11 二次复盘定形：load_super_field 式对象层封装收 (vm, name, receiver) 三参、泄漏帧语境，不取。）
>
> **范围裁定**：嵌套类**不在本里程碑**（Parser def 体成员分派只收 var/fun/裸标识符，「def 体内只允许 var/fun/方法」，Parser.cpp:308-322；grammar member 列表同 -- 嵌套类 bullet 的语义描述留档为后继方向）。容器三件套（`MAKE_LIST/MAP/RANGE`）、`LOAD/STORE_INDEX`、`INVOKE_METHOD`（维持预留不发射）、match、默认参数/varargs 均不在 M5。

## 1. 语义模型（定调）

def 类 = **类级一张静态表 + 每实例一张字段表 + superclass 单链**（grammar.txt def 节、指令集 §5.5 既定语义）：

- **查找序**：`foo.x` 先查实例 fields 表，未命中沿 super 链查各类静态表（方法 -> 绑定方法并回填缓存；静态值 -> 直读**不缓存**）；`Foo.x` 沿链查类静态表。实例属性可遮蔽同名静态成员/方法（fields 命中优先）。类成员读写取 **Python/JS class attributes 语义（读穿透、写遮蔽，2026-09-09 改定，见决策补记）**：读沿链 fall-through；类上赋值 `Sub.x = v` 落**接收类自身**表--继承来的名字新建遮蔽键、本类已有则原槽更新，父表不动；沿链全 miss 的新名字拒（无 monkey-patch，静态必须 `var` 声明；**2026-09-11 改定废止**：新名亦落接收类自身表，动态新增允许）；方法槽亦允许改写（bound 缓存取首解析快照；"init" 赋值同步 `init_`）。
- **this 是实例方法槽 0 的具名局部**（名 "this"，关键字不可能与用户标识符撞名；2026-09-10 阶段 2 review 改定：方法帧形 `[this, a1..aN]`，this 替代 callee 占槽 0 -- 对齐旧版 VM/clox 方法语义，见决策补记），嵌套函数引用 this 沿外围帧捕获其槽 0 为 upvalue -- M4 捕获机制**零改动复用**。
- **实例字段动态**（`init` 内 `this.x = ...` 落 fields 表，无字段预声明）；**静态 eager**（类定义点求值，类名未绑定前体内裸名不可自引用）；**无 monkey-patch**（类上新名 `Foo.newStatic = v` 运行期报错，静态必须 var 声明；**2026-09-11 改定废止**：动态新增允许，新名落接收类自身表，见决策补记）。
- Object 根类：唯一 super 为空者，链式查找统一终止于它；提供 no-op `init`（`super.init()` 无任何用户 init 时也可调用）。

## 2. 关键设计决策

### 2.1 ObjClass 无 meta

字段收敛为 `{ name_（intern 非空）, superclass_（唯 Object 为 nullptr）, field_（AriaHashTable，惰性；落地期由 statics_ 改名--表同时存静态成员与方法，单数 field_ 区分 ObjInstance.fields_）, init_（ObjClosure*） }`。元类是未来需求，现在预留只会多一个空指针与 trace 分支。

### 2.2 静态 + 方法一张表

与 `MAKE_STATIC`/`MAKE_METHOD` 写同一表天然吻合；类成员写遮蔽落接收类自身表（统一经 `set_field`，创建路径与类上赋值写路径共用，见决策补记）；区分静态变量/方法在值类型（`is_obj()` + `ObjType::CLOSURE`），无运行期 tag。静态方法经 `Foo.m` 取出的是裸闭包（无 this、不绑不缓存）；实例方法闭包与方法语义差异全在编译期（方法帧槽 0 = this，见 §3 阶段 3），VM 不区分。

### 2.3 Object 构造期 bootstrap、单独存储

与指令集 §4.15 `LOAD_OBJECT`「VM 内部指针，不经名字查，避免 shadow Object 名破坏隐式继承」的现行定义一致。bootstrap 内容：`ObjClass("Object", super=nullptr)` + **合成 no-op init 闭包**（`ObjFunction` arity 0、module nullptr、字节码手发 `LOAD_NIL; RETURN` -- 体内无名字解析故帧 module 空指针无害，tracer/mark 容 nullptr）+ 静态表 upsert("init", closure) + `init_` 指向它。VM 成员 `object_class_` 持有、vm_roots tracer 增标（§3 阶段 2）。

### 2.4 bound-method 缓存三铁则（写实例 fields 表）

绑定按实例（this=obj）==> 每实例缓存一份。方法集在 def 期一次性建立；类上赋值可改写方法槽（决策补记）但**已绑定实例不失效**--缓存取**首解析快照**语义（新解析见新闭包、旧实例沿用旧绑定），仍**免失效机制**。但必须钉死三条，否则是隐性 bug：

1. **只缓存绑定方法，绝不缓存静态变量值** -- 静态槽可变（`Foo.x = v2` 改类表），值缓存进实例会读到陈旧数据。
2. **`LOAD_SUPER_FIELD` 不写缓存** -- super 查到的是 defining class 链上被覆写**前**的闭包，若写进 fields 表，后续 `foo.m` 动态派发会被 fields 命中劫持到父类实现（缓存污染）。只有 `obj.m` 动态路径命中类表方法才回填。
3. **fields 命中优先 ==> 真字段遮蔽同名方法/缓存项** -- `this.m = v` 落表即覆盖缓存项，与「实例属性遮蔽」既有语义一致，无需专门处理。

GC 侧零额外负担：缓存的 `ObjBoundMethod` 经实例 fields 表 trace 级联标。

### 2.5 镜像双指令（本次对话定夺，完整论证不重复）

静态成员的「创建」（类体内，栈 `[class, v]`）与「赋值」（体外 `Foo.x = v`，栈 `[class, v]`）**栈进同形、出口要求互反**（创建留 class 弹 v、赋值弹 class 留 v）。单一栈形态服务不了两者：复用 peek-store `STORE_FIELD` 于创建需每成员 `DUP class + 尾 POP` 两条凑指令；改弹净形态 `[obj,v]->[]` 则赋值表达式约定（值留栈顶）迫使**热路径**每次字段赋值多一次 `DUP` 右值。故取专用指令 `MAKE_STATIC/MAKE_METHOD [class,v] -> [class]`（与 `STORE_FIELD` 互为镜像），两场景均零冗余，成本落一次性路径。VM 实现各一行：`MAKE_STATIC` = 写表 + `top_--`；`STORE_FIELD` = 写表 + `stack[top_-2]=stack[top_-1]; top_--`（单槽下移）。`STORE_INDEX [obj,idx,v] -> [v]`（下移两槽）同族，容器里程碑沿用此模式。

「class 留栈」的真实工程点不在指令而在 **CodeGen 栈高记账**：class 是跨整个类体的被持有临时值（非具名局部、无登记），期间所有成员初始化器临时值压其上，须持有计数 RAII 保证每成员发射完栈高回到 held+1，漏记一次则后续槽号与 TryRecord.stack_depth 全错位。

### 2.6 defining class 挂 ObjClosure

`ObjFunction` 是编译期常量、跨闭包实例共享：函数体内 `def` 执行 N 次产生 N 个类（同一 fn 常量的 N 份闭包）。superclass 经 `LOAD_GLOBAL` 运行期解析、两次执行间全局可被重绑到不同类，若 defining class 戳在共享的 fn 上则后写覆盖先写 -- 先建实例的方法会沿错误的 super 链解析。挂 `ObjClosure`（`MAKE_METHOD` 注册时 set）每闭包一份，无共享可变状态；`LOAD_SUPER_FIELD` 从 `frame.closure->defining_class()` 直读（戳一职双任：super 来源 + 方法性标记，2026-09-11 改定）。trace 标之（mark-sweep 处环无碍；方法闭包经此保所属类存活）。

## 3. 实施阶段（四阶段绿提交：object 层 -> VM 机制 -> 编译翻转 -> 文档收尾）

### 阶段 1：object 层

- **新增 `src/object/ObjClass.{hpp,cpp}`**（`final : Object`；`ObjType::CLASS` 枚举与 `to_string(ObjType)` case **均已预置，零改动**）：`name_`（intern 非空）/ `superclass_` / `field_`（惰性，落地期由 statics_ 改名）/ `init_`（ctor nullptr）；接口 `find_field(ObjString*) -> Value*` 沿链查（读穿透/存在性检查；落地期由 `(owner, 槽)` 对收缩--共享槽写改定写遮蔽后 owner 无人消费，见决策补记）、`upsert_field`（写遮蔽落接收类自身，与创建路径共用；2026-09-10 整改四：二者转私有，公开写入口 `set_field`，下同）、`init()/set_init`、`superclass()/name()`；trace 标 name + super + field(key+value) + init + 各成员闭包的 defining_class 级联；`to_string` = `<class Foo>`；工厂 `new_class(GC&, ObjString* name, ObjClass* super)`（入参调用方根化，同 `new_module` 3 参重载纪律）。
- **新增 `src/object/ObjInstance.{hpp,cpp}`**（`ObjType::INSTANCE` 已预置）：`class_` + `fields_`（惰性；bound-method 缓存同居此表）；trace 标 class + fields(key+value)；`to_string` = `<Foo instance>`；工厂 `new_instance(GC&, ObjClass*)`（shell 单次分配、fields 惰性 ==> 无内部二级分配，调用方建成即写栈、无中间 GC 点免守卫）。
- **新增 `src/object/ObjBoundMethod.{hpp,cpp}`**（`ObjType::BOUND_METHOD` 已预置）：`method_`（`ObjClosure*`）+ `receiver_`（**Value** -- 为将来内建类型方法留泛化，非 ObjInstance* 专用）；trace 标 method + mark_value(receiver)；`to_string` = `<bound method m>`；equals 默认地址相等；工厂 `new_bound_method(GC&, ObjClosure*, Value)`。
- **`ObjClosure` 增 `defining_class_`**（默认 nullptr）+ setter/getter；trace 补标（容 nullptr）。
- **`Value.cpp`** `format_value_debug` 补 CLASS/INSTANCE/BOUND_METHOD 三分支（渲染同 to_string 文案）。
- **登记**：根 CMakeLists aria_core object 组三对文件（字母序）；tests/CMakeLists 三测试文件。
- **测试**：`tests/object/` 下 test_objclass（建类/查表/沿链命中 owner/trace stress 存活/无根 sweep/to_string）、test_objinstance、test_objboundmethod；`rules/object.md` 子类型清单同步。

### 阶段 2：VM 机制（手写字节码验证，编译器仍 not_impl）

> **注（2026-09-10）**：阶段 2 已实施并经整改 --本节为**初稿设计**，bootstrap（原生 no-op init）/init seed（MAKE_CLASS 指令内，见「init seed 移出工厂」补记；2026-09-11 起收进 ObjClass 构造函数，见「init seed 收进构造函数」补记）/call_class（三步收敛、无快路径）/LOAD/STORE_FIELD 族（Object 虚函数协议分派）/检查分层（编译器不变式 ASSERT 化）等现行形态以**决策补记（三）/（四）/（二次整改）/（init seed 移出工厂）/（init seed 收进构造函数）**为准，下方旧描述与补记冲突处不再修订。

- **bootstrap**（`AriaVM` ctor，AriaVM.cpp:284-313，紧随 `set_vm_roots` 后、顺序与 builtins 无依赖）：`"init"`/`"Object"` 串 intern（make_guard 链，镜像 register_builtins 纪律）-> `new_function(gc, nullptr, init_str, 0)` -> 手发 `LOAD_NIL; RETURN` 进 `fn->unit()`（CodeUnit emit_op；no-op 体内无错误点，无行号表需求）-> `new_class("Object", nullptr)` -> `new_closure` -> upsert("init", closure) + `set_defining_class(cls)` + `cls->set_init(closure)` -> `object_class_` 落成员。**vm_roots tracer 增第 4 类根**：`g.mark_object(object_class_)`（AriaVM.cpp:285-298 lambda 内，注释清单顺延）。
- **Movement 零改动**（2026-09-10 阶段 2 review 改定，原 enter_method_frame 方案裁撤）：方法帧形定 `[this, a1..aN]`（this 替代 callee 占槽 0，对齐旧版 VM `call_bound_method` 的「槽 0 原位覆写 receiver_ + 普通 call_function」与 clox 方法语义）-- 普通 `enter_frame` 服务两态（普通帧槽 0 = 闭包自身、方法帧槽 0 = this，闭包经 frame.closure 携带不上栈），实参槽位不动、无 memmove 无扩槽，进帧/RETURN/exit_frame 纪律与普通调用完全一致。
- **`call_value`（AriaVM.cpp:475-494）+ 两分支**：
  - `CLASS -> call_class`：cls 在 `peek(argc)` 处；`new_instance`（唯一 GC 点，cls 在栈根化，instance 建成即写槽）；init = `cls->init()`；**快路径**：init 为 Object no-op（指针同一性 `init == object_class_->init()`）且 argc==0 -> callee 槽原位换 instance、`drop(argc)` 返 true；**慢路径**：先 call_closure 式 arity/frames_full 预检（报错文案指 init）-> 槽 0 保持 instance（即新帧 this）-> call_closure(init, argc) 进方法帧（init 返回 this，RETURN 后栈 `[.., 实例]`）。无 memmove 无扩槽。
  - `BOUND_METHOD`：读 `method_/receiver_` -> 同款预检 -> 槽 0 原位覆写为 receiver -> 普通 `enter_frame`（原生不进帧，槽 0 即原生契约的 this 兼返回槽，簿记同走 call_native）。
- **九 opcode 实装**（替换 AriaVM.cpp:886-897、1144-1153 的 not_implemented）：
  - `LOAD_OBJECT`：`push(object_class_)`。
  - `MAKE_CLASS name`：**peek super 不先弹**（new_class 分配顶 GC，super 须仍在栈）-> 非 ObjClass 值 `fail TypeMismatch`（「superclass 须为类」）-> `new_class` 写回原槽（top 不变原槽换 class）-> **init seed：`set_init(super ? super->init() : nullptr)`**（父已建成其缓存已就绪；Object 的由 bootstrap 设）==> 不变式「建成的类 init_ 非空且与类表 init 槽一致」（类表 init 槽沿链继承于父、`init_` 指同一闭包，决策补记）。
  - `MAKE_METHOD name`：栈 `[class, closure]`，两值均 peek 不弹 -> `set_field`（原 upsert_field，已收口为公开写入口；分配点两值在栈）-> `closure->set_defining_class(cls)`（裸写）-> name=="init" 则 `set_init(closure)` 覆盖 seed -> `top_--`（弹 closure 留 class）。
  - `MAKE_STATIC name`：同形，仅 upsert + `top_--`。
  - `LOAD_FIELD name`：**obj = peek(0) 不先弹**（绑定分配跨 GC 须保 obj 在栈，同 M4「闭包建成立即压栈」家族），结果**写回原槽**（top 不变）：ObjInstance -- fields 命中 -> 写回；未命中沿 class_ 链读穿透（经 ObjClass::load_field 协议；原直调 find_field，已随整改四收口）：命中方法闭包（defining class 戳 --2026-09-11 改定方法性看戳不看值类型）-> `new_bound_method`（分配点 instance 在栈）-> **先写回原槽根化 bound** -> `fields_.upsert(name, bound)`（第二分配点，bound 已栈根 + instance 内引用）-> 完成；命中其余（静态方法 fun/函数值静态/原生/静态值）-> 写回**不缓存**（铁则 1）；全链未命中 -> `fail UndefinedProperty`。ObjClass -- 沿自身链命中写回（静态方法闭包原样、无绑定无缓存），未命中 fail。nil -> `fail NilDereference`。其余（模块/内建值）-> fail UndefinedProperty（文案区分「无该成员」与「该类型不支持字段访问」）。
  - `STORE_FIELD name`：obj = peek(1)、v = peek(0) 均不弹：ObjInstance -> `fields_.upsert`（动态字段合法）；ObjClass -> `set_field` 落**接收类自身**表（恒成功；2026-09-11 改定：动态新增允许--本类已有原槽更新、继承名/新名新建键，父表不动；写遮蔽）；**name=="init" 特例**：值为 ObjClosure 则同步 `set_init`（非闭包 fail TypeMismatch），保「init_ 与类表 init 槽一致」的实例化不变式；其余 -> 同 LOAD_FIELD 族。完成 -> `stack[top_-2] = stack[top_-1]; top_--`。
  - `LOAD_THIS_FIELD name`：this = `frame.slots[0]`（指令仅编译器于实例方法内发射；VM 断言 + fail 兜底）；查找同 LOAD_FIELD 实例路径（**含绑定 + 缓存回填**，与 `obj.m` 同走一个 helper），结果 push。
  - `STORE_THIS_FIELD name`：v = peek(0)；this 同上 `fields_.upsert`；值留栈（`[v] -> [v]`，this 不经栈）。
  - `LOAD_SUPER_FIELD name`：defining = `frame.closure->defining_class()`（nullptr -> fail InvalidState，编译期已挡仅兜底）；super = `defining->superclass()`（nullptr 即 Object 的方法内用 super -> `fail SuperNoBaseClass`）；沿 super 链 find（**从父类起、不含 defining 自身**，经 `ObjClass::load_field` 协议，miss 类措辞随协议）：命中**方法闭包（defining class 戳 --2026-09-11 改定方法性看戳不看值类型）** -> `new_bound_method(gc, method_value, frame.slots[0])`（this 在帧槽根化）-> push；命中其余（静态方法 fun/持函数值的静态变量/原生/静态值）-> 原值直读 push 不绑定不缓存（super 静态访问放开，原「super.x 静态不支持」fail 退役，见 super 静态访问补记）；未命中 -> fail UndefinedProperty。**不写 fields 缓存**（铁则 2）。
- **消费点零改动**：trace_execution/Disassembler/kOpCodeCount=63 均不动。
- **测试**（tests/runtime/test_ariavm.cpp 手写 emit，辅助族扩 make_class/make_method）：类建立序列 -> 实例化快/慢路径、init 带参、方法调用改 this 字段、继承覆写 + super 调父实现、类上赋值写遮蔽（`Sub.x = v` 后 `Sub.x` 取新值、`Super.x` 不变；新名字经赋值新增被拒）、方法经类上赋值改写（新解析取新闭包、已解析实例沿用旧绑定--首解析快照）、init 赋值同步 `init_`（非闭包值拒）、bound 缓存同指针（两次 LOAD_FIELD 返回同一 `ObjBoundMethod*`）、super 不污染缓存（super.m 后 obj.m 仍派发子类实现）、STORE_FIELD 单槽下移正确性（深栈多临时值下赋值）、动态新增（原 monkey-patch 拒绝用例翻为新增成功，2026-09-11 改定）、NilDereference/UndefinedProperty/SuperNoBaseClass/TypeMismatch 报错、AriaVMStress 下实例/绑定/类/缓存项跨 GC 存活；既有 NotImplemented 用例若占这九指令改挂仍 fatal 的 `LOAD_INDEX`/`MAKE_LIST` 等。
- `rules/runtime.md`（九指令/两 call 分支/方法帧槽 0 = this/bootstrap/tracer 第 4 根）、`rules/memory.md`（tracer 根清单）同步。

### 阶段 3：compile 翻转

- **`FunctionCtx` 增 `FnKind`**（Function/StaticMethod/InstanceMethod，ctx 链上可查 -- this/super 语境判定沿 enclosing 链找最近 InstanceMethod 需要它）+ `kThisName = "this"`；InstanceMethod 编译时先 `add_local("this")`（槽 0）再形参（随至 1..n，slot>=256 走既有 `_L` 机制），arity 计数不含 this。
- **`compile_function`（CodeGen.cpp:424-493）签名增 kind 形参**（默认 Function）；绑定步骤按三态分派：具名声明（现行为）/ lambda 留栈 / **Method 留栈不绑定**（类成员闭包由 MAKE_METHOD 消费；名字照常进 ObjFunction 供 `<fn m>` 渲染与堆栈跟踪）。
- **`visitDefDeclNode`（CodeGen.cpp:791）**：① 成员名去重（发射前静态检查，`RedefinedMember` -- ErrorCode.hpp:67 已预置，同名 var/fun/方法混报）② superclass：有 -> `resolve_name_or_fail` + `emit_load_var`（运行期解析，跨模块导入类可用；非类值运行期 MAKE_CLASS 报 TypeMismatch；编译期不查全局 -- UndefinedType 枚举保留不启用，未命中沿用运行期 UndefinedVariable）；无 -> `LOAD_OBJECT` ③ `MAKE_CLASS name`（名入常量池）④ **持有计数 RAII**（§2.5：类体发射期间栈高 held+1，每成员发射完断言复位）⑤ 成员按序：StaticVar/StaticMethod -> 求值初始化器（无则 `LOAD_NIL`）/ 发 `CLOSURE`（留栈）+ `MAKE_STATIC`（fun 不戳 defining class ⟹ 静态槽读恒原值，2026-09-11 改定）；InstanceMethod -> `compile_function(..., kind)` 发 `CLOSURE`（留栈）+ `MAKE_METHOD`（戳 defining class = 方法性标记） ⑥ 尾绑定按语境：顶层 -> `DEF_GLOBAL`（弹）；函数/块内 -> `declare_local_or_fail` + `mark_initialized`（**值填槽** -- 类值已在栈顶槽位，同嵌套 fun 机制）。顶层类名重绑定经 `declare_global_or_fail` 既有报错（RedefinedVariable 家族；RedefinedClass 枚举保留不启用，避免同义码分叉）。
  - eager 静态语义随 lowering 自然成立：初始化器在类定义点求值、类名未绑定前体内裸名自引用 -> 运行期 UndefinedVariable（与 grammar「要求 B 先于 A 定义」一致）。
  - **异常语义白赚**（记入测试勿实现）：成员初始化器 throw -> 半成品类随 unwind 截栈丢弃、类名从未绑定（TryRecord.stack_depth 记在 def 语句前）。
- **`visitThisExprNode`（:895）-> 专用 `resolve_this_or_fail`**：沿 fn ctx 链找名为 "this" 的局部 -- 当前帧命中 -> `emit_load_var`（LOAD_LOCAL 0，this 占方法帧槽 0）；enclosing 命中 -> 经既有 `resolve_upvalue` 捕获（LOAD_UPVALUE，**M4 机制零改动复用**）；链上无 -> `fail ThisOutsideClass`（ErrorCode.hpp:55 预置）。**永不落全局**（this 是关键字非标识符，不复用 resolve_name_or_fail 的全局兜底）。
- **`visitSuperExprNode`（:897）-> `fail InvalidSuperUse`**（super 须为 super.method(...) 形态）。
- **`visitFieldAccessNode`（:1039）三模式**（take_lvalue_mode）：
  - Load：object 为 ThisExpr 且 this 解析为**当前帧局部** -> `LOAD_THIS_FIELD`（优化）；this 为 upvalue（嵌套函数）-> `LOAD_UPVALUE this + LOAD_FIELD`（退化 -- THIS_FIELD 系指令 this 取槽 0 只对直接方法体成立）；object 为 SuperExpr -> `LOAD_SUPER_FIELD`（2026-09-11 改定 `super.x` 静态读取放开：方法闭包（defining class 戳）绑 this、静态槽原值直读；指令读 frame.closure 的 defining class，仅直接方法帧可承载，嵌套函数内 super 的处理随阶段 3 定）；一般 -> emit object + `LOAD_FIELD`。
  - Store：object 为 ThisExpr（局部）-> `STORE_THIS_FIELD`；其余同形退化 -> `STORE_FIELD`。
  - **Locate（启用 compound-assignment-lowering.md §4.2 预留方案）**：`obj.f op= e` -> `<obj> DUP LOAD_FIELD f <e> <op> STORE_FIELD f`（locator 单次求值，DUP 副本跨 load/store 复用）；`this.f op= e` -> `<this> DUP LOAD_FIELD ... STORE_FIELD`（locator = this 槽位/捕获索引，重解析廉价 -- 同 Identifier 的 locator-once 语义，不走 THIS_FIELD 系）；前置 `++/--` 同族走复合赋值既有机制。
- **`visitCallNode`（:1025）**：callee 为 `FieldAccessNode(SuperExprNode, m)` -> 先判语境（fn ctx 链无最近 InstanceMethod -> `SuperOutsideMethod`，ErrorCode.hpp:54 预置）-> `LOAD_SUPER_FIELD m` + args + `CALL`；其余 callee 走通用路径（FieldAccess(This,·) 经 visit 正常发射 LOAD_THIS_FIELD/退化，无需特判）。
- **测试**（tests/compile/test_codegen.cpp 端到端 + 反汇编文本）：路线表验收四样例（类定义/实例化/继承/super）；静态共享槽与遮蔽、实例字段遮蔽静态、this 嵌套捕获（lambda 内 this.x 读写）、super 不污染动态派发、动态新增（原 monkey-patch 拒绝翻为新增成功，2026-09-11 改定）、RedefinedMember、ThisOutsideClass/SuperOutsideMethod/InvalidSuperUse 编译错、def 体内 throw 后类名未绑定、函数内 def（值填槽局部类）、反汇编断言（LOAD_OBJECT/MAKE_CLASS/MAKE_METHOD/MAKE_STATIC 出现、成员序正确、方法 CLOSURE+MAKE_METHOD 相邻、无 DEF_GLOBAL 混入方法发射）。
- `rules/compile.md`（FnKind/this 解析/Locate 启用/defDecl lowering）同步；compound-assignment-lowering.md「Locate 当前不使用」翻已启用。

### 阶段 4：文档收尾 + 全量验证

- 指令集：字段组/类组九指令标落地；§5.5 精化写回（defining_class 挂闭包 -- 原文「ObjFn.defining_class」措辞同步；init_ 的 **MAKE_CLASS seed + MAKE_METHOD("init") 覆盖**两时点 + 类上 init 赋值同步；STORE-on-class 写遮蔽落接收类自身/拒绝新增；bound 缓存三铁则 + 首解析快照语义）；§3.1 peek-store 表补 MAKE_STATIC 镜像形态注记。
- grammar.txt def 节：「ObjFn 持 defining class」改写为闭包持（措辞级）；嵌套类 bullet 加 parser 现状注（member 分派无 defDecl，语义描述为后继方向）。
- vm-design §6 M5 行标已落地；§4 相应小节（callable 清单加 CLASS/BOUND_METHOD、this 槽 0、Movement tracer 第 4 根）；gc-implementation-plan 对应行。
- rules/object/compile/runtime/memory 落地状态复核；CLAUDE.md/README 进度行（已落地补 M5，待续收敛 M6 协程）；文档索引本计划条目改「定稿并已落地」。
- 坑点文档补录（预期高发区：bound 缓存与遮蔽交互、MAKE_CLASS/MAKE_METHOD peek-不弹栈纪律、init seed 时序、Locate 模式与既有复合赋值机制的合流）。
- 全量验证：默认与 `build/tagvalue` 双配置 ctest 全绿；`--eval` 冒烟：类定义+实例化、继承+super、静态共享槽、this 嵌套捕获、（顺带）**用户类可迭代 for-in**（iter/has_next/next 经 LOAD_FIELD 返绑定方法 + CALL -- M5 落地后用户定义类即可迭代，内建 list/map/string 迭代仍待容器里程碑）。

## 4. 验收

- 路线表 M5 标准：类定义/实例化/继承/super 样例通过。
- 双值表示配置 ctest 全绿（机制测试覆盖：实例化快/慢路径、方法调用与 this 字段、继承/super、共享槽、缓存三铁则、GC stress 存活）。
- 零新增 opcode（九条早已预留，`kOpCodeCount=63` 不变），Disassembler 零改动。
- **错误码零新增**（InvalidSuperUse/SuperOutsideMethod/ThisOutsideClass/RedefinedMember + NilDereference/UndefinedProperty/SuperNoBaseClass/TypeMismatch/InvalidState 均已预置于 ErrorCode.hpp）。
