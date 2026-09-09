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
> **决策补记（2026-09-09，阶段 1 落地 review 改定）**：类成员继承语义由原「共享槽」改定 **Python/JS class attributes 式「读穿透、写遮蔽」**（决策 2 的单表部分不变）：读沿链 fall-through；类上赋值 `Sub.x = v` 落**接收类自身**表--继承来的名字新建遮蔽键、本类已有则原槽更新；沿链全 miss 的新名字仍拒（无 monkey-patch 不变，静态必须 `var` 声明）。方法槽亦允许经类上赋值改写（不加闭包槽守卫，用户定夺）：bound-method 缓存取**首解析快照**语义（已解析实例沿用旧绑定、新解析取新闭包，仍免失效机制）；`"init"` 赋值特例同步 `set_init`（值非 ObjClosure 则 fail TypeMismatch），保「init_ 与类表 init 槽一致」的实例化不变式。`find_field` 随之收缩为 `Value*` 槽指针（owner 返回值裁撤--它唯一消费者是共享槽写；读与存在性检查吃槽指针足够），`upsert_field` 从「创建路径专用」转正为类成员写路径本体。
>
> **范围裁定**：嵌套类**不在本里程碑**（Parser def 体成员分派只收 var/fun/裸标识符，「def 体内只允许 var/fun/方法」，Parser.cpp:308-322；grammar member 列表同 -- 嵌套类 bullet 的语义描述留档为后继方向）。容器三件套（`MAKE_LIST/MAP/RANGE`）、`LOAD/STORE_INDEX`、`INVOKE_METHOD`（维持预留不发射）、match、默认参数/varargs 均不在 M5。

## 1. 语义模型（定调）

def 类 = **类级一张静态表 + 每实例一张字段表 + superclass 单链**（grammar.txt def 节、指令集 §5.5 既定语义）：

- **查找序**：`foo.x` 先查实例 fields 表，未命中沿 super 链查各类静态表（方法 -> 绑定方法并回填缓存；静态值 -> 直读**不缓存**）；`Foo.x` 沿链查类静态表。实例属性可遮蔽同名静态成员/方法（fields 命中优先）。类成员读写取 **Python/JS class attributes 语义（读穿透、写遮蔽，2026-09-09 改定，见决策补记）**：读沿链 fall-through；类上赋值 `Sub.x = v` 落**接收类自身**表--继承来的名字新建遮蔽键、本类已有则原槽更新，父表不动；沿链全 miss 的新名字拒（无 monkey-patch，静态必须 `var` 声明）；方法槽亦允许改写（bound 缓存取首解析快照；"init" 赋值同步 `init_`）。
- **this 是实例方法槽 1 的具名局部**（名 "this"，关键字不可能与用户标识符撞名），嵌套函数引用 this 沿外围帧捕获其槽 1 为 upvalue -- M4 捕获机制**零改动复用**。
- **实例字段动态**（`init` 内 `this.x = ...` 落 fields 表，无字段预声明）；**静态 eager**（类定义点求值，类名未绑定前体内裸名不可自引用）；**无 monkey-patch**（类上新名 `Foo.newStatic = v` 运行期报错，静态必须 var 声明）。
- Object 根类：唯一 super 为空者，链式查找统一终止于它；提供 no-op `init`（`super.init()` 无任何用户 init 时也可调用）。

## 2. 关键设计决策

### 2.1 ObjClass 无 meta

字段收敛为 `{ name_（intern 非空）, superclass_（唯 Object 为 nullptr）, field_（AriaHashTable，惰性；落地期由 statics_ 改名--表同时存静态成员与方法，单数 field_ 区分 ObjInstance.fields_）, init_（ObjClosure*） }`。元类是未来需求，现在预留只会多一个空指针与 trace 分支。

### 2.2 静态 + 方法一张表

与 `MAKE_STATIC`/`MAKE_METHOD` 写同一表天然吻合；类成员写遮蔽落接收类自身表（`upsert_field`，创建路径与类上赋值写路径共用，见决策补记）；区分静态变量/方法在值类型（`is_obj()` + `ObjType::CLOSURE`），无运行期 tag。静态方法经 `Foo.m` 取出的是裸闭包（无 this、不绑不缓存）；实例方法闭包与方法语义差异全在编译期（this 槽 1，见 §3 阶段 3），VM 不区分。

### 2.3 Object 构造期 bootstrap、单独存储

与指令集 §4.15 `LOAD_OBJECT`「VM 内部指针，不经名字查，避免 shadow Object 名破坏隐式继承」的现行定义一致。bootstrap 内容：`ObjClass("Object", super=nullptr)` + **合成 no-op init 闭包**（`ObjFunction` arity 0、module nullptr、字节码手发 `LOAD_NIL; RETURN` -- 体内无名字解析故帧 module 空指针无害，tracer/mark 容 nullptr）+ 静态表 upsert("init", closure) + `init_` 指向它。VM 成员 `object_class_` 持有、vm_roots tracer 增标（§3 阶段 2）。

### 2.4 bound-method 缓存三铁则（写实例 fields 表）

绑定按实例（this=obj）==> 每实例缓存一份。方法集在 def 期一次性建立；类上赋值可改写方法槽（决策补记）但**已绑定实例不失效**--缓存取**首解析快照**语义（新解析见新闭包、旧实例沿用旧绑定），仍**免失效机制**。但必须钉死三条，否则是隐性 bug：

1. **只缓存绑定方法，绝不缓存静态变量值** -- 静态槽可变（`Foo.x = v2` 改类表），值缓存进实例会读到陈旧数据。
2. **`LOAD_SUPER_METHOD` 不写缓存** -- super 查到的是 defining class 链上被覆写**前**的闭包，若写进 fields 表，后续 `foo.m` 动态派发会被 fields 命中劫持到父类实现（缓存污染）。只有 `obj.m` 动态路径命中类表方法才回填。
3. **fields 命中优先 ==> 真字段遮蔽同名方法/缓存项** -- `this.m = v` 落表即覆盖缓存项，与「实例属性遮蔽」既有语义一致，无需专门处理。

GC 侧零额外负担：缓存的 `ObjBoundMethod` 经实例 fields 表 trace 级联标。

### 2.5 镜像双指令（本次对话定夺，完整论证不重复）

静态成员的「创建」（类体内，栈 `[class, v]`）与「赋值」（体外 `Foo.x = v`，栈 `[class, v]`）**栈进同形、出口要求互反**（创建留 class 弹 v、赋值弹 class 留 v）。单一栈形态服务不了两者：复用 peek-store `STORE_FIELD` 于创建需每成员 `DUP class + 尾 POP` 两条凑指令；改弹净形态 `[obj,v]->[]` 则赋值表达式约定（值留栈顶）迫使**热路径**每次字段赋值多一次 `DUP` 右值。故取专用指令 `MAKE_STATIC/MAKE_METHOD [class,v] -> [class]`（与 `STORE_FIELD` 互为镜像），两场景均零冗余，成本落一次性路径。VM 实现各一行：`MAKE_STATIC` = 写表 + `top_--`；`STORE_FIELD` = 写表 + `stack[top_-2]=stack[top_-1]; top_--`（单槽下移）。`STORE_INDEX [obj,idx,v] -> [v]`（下移两槽）同族，容器里程碑沿用此模式。

「class 留栈」的真实工程点不在指令而在 **CodeGen 栈高记账**：class 是跨整个类体的被持有临时值（非具名局部、无登记），期间所有成员初始化器临时值压其上，须持有计数 RAII 保证每成员发射完栈高回到 held+1，漏记一次则后续槽号与 TryRecord.stack_depth 全错位。

### 2.6 defining class 挂 ObjClosure

`ObjFunction` 是编译期常量、跨闭包实例共享：函数体内 `def` 执行 N 次产生 N 个类（同一 fn 常量的 N 份闭包）。superclass 经 `LOAD_GLOBAL` 运行期解析、两次执行间全局可被重绑到不同类，若 defining class 戳在共享的 fn 上则后写覆盖先写 -- 先建实例的方法会沿错误的 super 链解析。挂 `ObjClosure`（`MAKE_METHOD` 注册时 set）每闭包一份，无共享可变状态；`LOAD_SUPER_METHOD` 从 `frame.closure->defining_class()` 直读。trace 标之（mark-sweep 处环无碍，免静态方法闭包经 `Foo.m` 上栈时类亡指针悬垂）。

## 3. 实施阶段（四阶段绿提交：object 层 -> VM 机制 -> 编译翻转 -> 文档收尾）

### 阶段 1：object 层

- **新增 `src/object/ObjClass.{hpp,cpp}`**（`final : Object`；`ObjType::CLASS` 枚举与 `to_string(ObjType)` case **均已预置，零改动**）：`name_`（intern 非空）/ `superclass_` / `field_`（惰性，落地期由 statics_ 改名）/ `init_`（ctor nullptr）；接口 `find_field(ObjString*) -> Value*` 沿链查（读穿透/存在性检查；落地期由 `(owner, 槽)` 对收缩--共享槽写改定写遮蔽后 owner 无人消费，见决策补记）、`upsert_field`（写遮蔽落接收类自身，与创建路径共用）、`init()/set_init`、`superclass()/name()`；trace 标 name + super + field(key+value) + init + 各成员闭包的 defining_class 级联；`to_string` = `<class Foo>`；工厂 `new_class(GC&, ObjString* name, ObjClass* super)`（入参调用方根化，同 `new_module` 3 参重载纪律）。
- **新增 `src/object/ObjInstance.{hpp,cpp}`**（`ObjType::INSTANCE` 已预置）：`class_` + `fields_`（惰性；bound-method 缓存同居此表）；trace 标 class + fields(key+value)；`to_string` = `<Foo instance>`；工厂 `new_instance(GC&, ObjClass*)`（shell 单次分配、fields 惰性 ==> 无内部二级分配，调用方建成即写栈、无中间 GC 点免守卫）。
- **新增 `src/object/ObjBoundMethod.{hpp,cpp}`**（`ObjType::BOUND_METHOD` 已预置）：`method_`（`ObjClosure*`）+ `receiver_`（**Value** -- 为将来内建类型方法留泛化，非 ObjInstance* 专用）；trace 标 method + mark_value(receiver)；`to_string` = `<bound method m>`；equals 默认地址相等；工厂 `new_bound_method(GC&, ObjClosure*, Value)`。
- **`ObjClosure` 增 `defining_class_`**（默认 nullptr）+ setter/getter；trace 补标（容 nullptr）。
- **`Value.cpp`** `format_value_debug` 补 CLASS/INSTANCE/BOUND_METHOD 三分支（渲染同 to_string 文案）。
- **登记**：根 CMakeLists aria_core object 组三对文件（字母序）；tests/CMakeLists 三测试文件。
- **测试**：`tests/object/` 下 test_objclass（建类/查表/沿链命中 owner/trace stress 存活/无根 sweep/to_string）、test_objinstance、test_objboundmethod；`rules/object.md` 子类型清单同步。

### 阶段 2：VM 机制（手写字节码验证，编译器仍 not_impl）

- **bootstrap**（`AriaVM` ctor，AriaVM.cpp:284-313，紧随 `set_vm_roots` 后、顺序与 builtins 无依赖）：`"init"`/`"Object"` 串 intern（make_guard 链，镜像 register_builtins 纪律）-> `new_function(gc, nullptr, init_str, 0)` -> 手发 `LOAD_NIL; RETURN` 进 `fn->unit()`（CodeUnit emit_op；no-op 体内无错误点，无行号表需求）-> `new_class("Object", nullptr)` -> `new_closure` -> upsert("init", closure) + `set_defining_class(cls)` + `cls->set_init(closure)` -> `object_class_` 落成员。**vm_roots tracer 增第 4 类根**：`g.mark_object(object_class_)`（AriaVM.cpp:285-298 lambda 内，注释清单顺延）。
- **`Movement` 增 `enter_method_frame(ObjClosure*, u8 argc)`**（Movement.cpp:12-33 `init_frame_` 旁）：slots = `top_ - argc - 2`（帧形 `[closure, this, a1..aN]`），其余同 `init_frame_`；RETURN/exit_frame 纪律不变（返回值写 slots[0]、截到 slots+1，this 与实参一并弹出）。
- **`call_value`（AriaVM.cpp:475-494）+ 两分支**：
  - `CLASS -> instantiate`：cls 在 `peek(argc)` 处；`new_instance`（唯一 GC 点，cls 在栈根化，instance 建成即写槽）；init = `cls->init()`；**快路径**：init 为 Object no-op（指针同一性 `init == object_class_->init()`）且 argc==0 -> callee 槽原位换 instance、`drop(argc)` 返 true；**慢路径**：先 call_closure 式 arity/frames_full 预检（报错文案指 init）-> `push(nil)` 扩容 -> args 上移一槽 memmove -> callee 槽写 init 闭包、callee+1 写 instance -> `enter_method_frame`（RETURN 后栈 `[.., 结果]`）。栈形变全为裸写/memmove，无 GC 点。
  - `BOUND_METHOD`：读 `method_/receiver_` -> 同款预检 -> `push(nil)` + memmove 上移 -> `[closure, receiver, args..]` -> `enter_method_frame`。
- **九 opcode 实装**（替换 AriaVM.cpp:886-897、1144-1153 的 not_implemented）：
  - `LOAD_OBJECT`：`push(object_class_)`。
  - `MAKE_CLASS name`：**peek super 不先弹**（new_class 分配顶 GC，super 须仍在栈）-> 非 ObjClass 值 `fail TypeMismatch`（「superclass 须为类」）-> `new_class` 写回原槽（top 不变原槽换 class）-> **init seed：`set_init(super ? super->init() : nullptr)`**（父已建成其缓存已就绪；Object 的由 bootstrap 设）==> 不变式「建成的类 init_ 非空且与类表 init 槽一致」（类表 init 槽沿链继承于父、`init_` 指同一闭包，决策补记）。
  - `MAKE_METHOD name`：栈 `[class, closure]`，两值均 peek 不弹 -> `upsert_field`（分配点两值在栈）-> `closure->set_defining_class(cls)`（裸写）-> name=="init" 则 `set_init(closure)` 覆盖 seed -> `top_--`（弹 closure 留 class）。
  - `MAKE_STATIC name`：同形，仅 upsert + `top_--`。
  - `LOAD_FIELD name`：**obj = peek(0) 不先弹**（绑定分配跨 GC 须保 obj 在栈，同 M4「闭包建成立即压栈」家族），结果**写回原槽**（top 不变）：ObjInstance -- fields 命中 -> 写回；未命中沿 class_ 链 find_field：命中闭包 -> `new_bound_method`（分配点 instance 在栈）-> **先写回原槽根化 bound** -> `fields_.upsert(name, bound)`（第二分配点，bound 已栈根 + instance 内引用）-> 完成；命中非闭包（静态值）-> 写回**不缓存**（铁则 1）；全链未命中 -> `fail UndefinedProperty`。ObjClass -- 沿自身链 find_field 命中写回（静态方法闭包原样、无绑定无缓存），未命中 fail。nil -> `fail NilDereference`。其余（模块/内建值）-> fail UndefinedProperty（文案区分「无该成员」与「该类型不支持字段访问」）。
  - `STORE_FIELD name`：obj = peek(1)、v = peek(0) 均不弹：ObjInstance -> `fields_.upsert`（动态字段合法）；ObjClass -> 沿链 `find_field` 存在性检查：全 miss -> fail UndefinedProperty（「不能动态新增静态成员」-- 无 monkey-patch）；命中 -> `upsert_field` 落**接收类自身**表（读穿透、写遮蔽：本类已有原槽更新、继承名新建遮蔽键，父表不动）；**name=="init" 特例**：值为 ObjClosure 则同步 `set_init`（非闭包 fail TypeMismatch），保「init_ 与类表 init 槽一致」的实例化不变式；其余 -> 同 LOAD_FIELD 族。完成 -> `stack[top_-2] = stack[top_-1]; top_--`。
  - `LOAD_THIS_FIELD name`：this = `frame.slots[1]`（指令仅编译器于实例方法内发射；VM 断言 + fail 兜底）；查找同 LOAD_FIELD 实例路径（**含绑定 + 缓存回填**，与 `obj.m` 同走一个 helper），结果 push。
  - `STORE_THIS_FIELD name`：v = peek(0)；this 同上 `fields_.upsert`；值留栈（`[v] -> [v]`，this 不经栈）。
  - `LOAD_SUPER_METHOD name`：defining = `frame.closure->defining_class()`（nullptr -> fail InvalidState，编译期已挡仅兜底）；super = `defining->superclass()`（nullptr 即 Object 的方法内用 super -> `fail SuperNoBaseClass`）；沿 super 链 find（**从父类起、不含 defining 自身**）：命中闭包 -> `new_bound_method(gc, closure, frame.slots[1])`（this 在帧槽根化）-> push；命中非闭包 -> fail（super.x 静态不支持）；未命中 -> fail UndefinedProperty。**不写 fields 缓存**（铁则 2）。
- **消费点零改动**：trace_execution/Disassembler/kOpCodeCount=63 均不动。
- **测试**（tests/runtime/test_ariavm.cpp 手写 emit，辅助族扩 make_class/make_method）：类建立序列 -> 实例化快/慢路径、init 带参、方法调用改 this 字段、继承覆写 + super 调父实现、类上赋值写遮蔽（`Sub.x = v` 后 `Sub.x` 取新值、`Super.x` 不变；新名字经赋值新增被拒）、方法经类上赋值改写（新解析取新闭包、已解析实例沿用旧绑定--首解析快照）、init 赋值同步 `init_`（非闭包值拒）、bound 缓存同指针（两次 LOAD_FIELD 返回同一 `ObjBoundMethod*`）、super 不污染缓存（super.m 后 obj.m 仍派发子类实现）、STORE_FIELD 单槽下移正确性（深栈多临时值下赋值）、无 monkey-patch 拒绝、NilDereference/UndefinedProperty/SuperNoBaseClass/TypeMismatch 报错、AriaVMStress 下实例/绑定/类/缓存项跨 GC 存活；既有 NotImplemented 用例若占这九指令改挂仍 fatal 的 `LOAD_INDEX`/`MAKE_LIST` 等。
- `rules/runtime.md`（九指令/两 call 分支/enter_method_frame/bootstrap/tracer 第 4 根）、`rules/memory.md`（tracer 根清单）同步。

### 阶段 3：compile 翻转

- **`FunctionCtx` 增 `FnKind`**（Function/StaticMethod/InstanceMethod，ctx 链上可查 -- this/super 语境判定沿 enclosing 链找最近 InstanceMethod 需要它）+ `kThisName = "this"`；InstanceMethod 编译时先 `add_local("this")`（槽 1）再形参（随至 2..n+1，slot>=256 走既有 `_L` 机制），arity 计数不含 this。
- **`compile_function`（CodeGen.cpp:424-493）签名增 kind 形参**（默认 Function）；绑定步骤按三态分派：具名声明（现行为）/ lambda 留栈 / **Method 留栈不绑定**（类成员闭包由 MAKE_METHOD 消费；名字照常进 ObjFunction 供 `<fn m>` 渲染与堆栈跟踪）。
- **`visitDefDeclNode`（CodeGen.cpp:791）**：① 成员名去重（发射前静态检查，`RedefinedMember` -- ErrorCode.hpp:67 已预置，同名 var/fun/方法混报）② superclass：有 -> `resolve_name_or_fail` + `emit_load_var`（运行期解析，跨模块导入类可用；非类值运行期 MAKE_CLASS 报 TypeMismatch；编译期不查全局 -- UndefinedType 枚举保留不启用，未命中沿用运行期 UndefinedVariable）；无 -> `LOAD_OBJECT` ③ `MAKE_CLASS name`（名入常量池）④ **持有计数 RAII**（§2.5：类体发射期间栈高 held+1，每成员发射完断言复位）⑤ 成员按序：StaticVar -> 每 binding 求值初始化器（无则 `LOAD_NIL`）+ `MAKE_STATIC`；StaticMethod/InstanceMethod -> `compile_function(..., kind)` 发 `CLOSURE`（留栈）+ `MAKE_METHOD` ⑥ 尾绑定按语境：顶层 -> `DEF_GLOBAL`（弹）；函数/块内 -> `declare_local_or_fail` + `mark_initialized`（**值填槽** -- 类值已在栈顶槽位，同嵌套 fun 机制）。顶层类名重绑定经 `declare_global_or_fail` 既有报错（RedefinedVariable 家族；RedefinedClass 枚举保留不启用，避免同义码分叉）。
  - eager 静态语义随 lowering 自然成立：初始化器在类定义点求值、类名未绑定前体内裸名自引用 -> 运行期 UndefinedVariable（与 grammar「要求 B 先于 A 定义」一致）。
  - **异常语义白赚**（记入测试勿实现）：成员初始化器 throw -> 半成品类随 unwind 截栈丢弃、类名从未绑定（TryRecord.stack_depth 记在 def 语句前）。
- **`visitThisExprNode`（:895）-> 专用 `resolve_this_or_fail`**：沿 fn ctx 链找名为 "this" 的局部 -- 当前帧命中 -> `emit_load_var`（LOAD_LOCAL 1）；enclosing 命中 -> 经既有 `resolve_upvalue` 捕获（LOAD_UPVALUE，**M4 机制零改动复用**）；链上无 -> `fail ThisOutsideClass`（ErrorCode.hpp:55 预置）。**永不落全局**（this 是关键字非标识符，不复用 resolve_name_or_fail 的全局兜底）。
- **`visitSuperExprNode`（:897）-> `fail InvalidSuperUse`**（super 须为 super.method(...) 形态）。
- **`visitFieldAccessNode`（:1039）三模式**（take_lvalue_mode）：
  - Load：object 为 ThisExpr 且 this 解析为**当前帧局部** -> `LOAD_THIS_FIELD`（优化）；this 为 upvalue（嵌套函数）-> `LOAD_UPVALUE this + LOAD_FIELD`（退化 -- THIS_FIELD 系指令 this 取槽 1 只对直接方法体成立）；object 为 SuperExpr -> fail InvalidSuperUse 兜底（调用位已被 visitCallNode 拦截）；一般 -> emit object + `LOAD_FIELD`。
  - Store：object 为 ThisExpr（局部）-> `STORE_THIS_FIELD`；其余同形退化 -> `STORE_FIELD`。
  - **Locate（启用 compound-assignment-lowering.md §4.2 预留方案）**：`obj.f op= e` -> `<obj> DUP LOAD_FIELD f <e> <op> STORE_FIELD f`（locator 单次求值，DUP 副本跨 load/store 复用）；`this.f op= e` -> `<this> DUP LOAD_FIELD ... STORE_FIELD`（locator = this 槽位/捕获索引，重解析廉价 -- 同 Identifier 的 locator-once 语义，不走 THIS_FIELD 系）；前置 `++/--` 同族走复合赋值既有机制。
- **`visitCallNode`（:1025）**：callee 为 `FieldAccessNode(SuperExprNode, m)` -> 先判语境（fn ctx 链无最近 InstanceMethod -> `SuperOutsideMethod`，ErrorCode.hpp:54 预置）-> `LOAD_SUPER_METHOD m` + args + `CALL`；其余 callee 走通用路径（FieldAccess(This,·) 经 visit 正常发射 LOAD_THIS_FIELD/退化，无需特判）。
- **测试**（tests/compile/test_codegen.cpp 端到端 + 反汇编文本）：路线表验收四样例（类定义/实例化/继承/super）；静态共享槽与遮蔽、实例字段遮蔽静态、this 嵌套捕获（lambda 内 this.x 读写）、super 不污染动态派发、monkey-patch 拒绝、RedefinedMember、ThisOutsideClass/SuperOutsideMethod/InvalidSuperUse 编译错、def 体内 throw 后类名未绑定、函数内 def（值填槽局部类）、反汇编断言（LOAD_OBJECT/MAKE_CLASS/MAKE_METHOD/MAKE_STATIC 出现、成员序正确、方法 CLOSURE+MAKE_METHOD 相邻、无 DEF_GLOBAL 混入方法发射）。
- `rules/compile.md`（FnKind/this 解析/Locate 启用/defDecl lowering）同步；compound-assignment-lowering.md「Locate 当前不使用」翻已启用。

### 阶段 4：文档收尾 + 全量验证

- 指令集：字段组/类组九指令标落地；§5.5 精化写回（defining_class 挂闭包 -- 原文「ObjFn.defining_class」措辞同步；init_ 的 **MAKE_CLASS seed + MAKE_METHOD("init") 覆盖**两时点 + 类上 init 赋值同步；STORE-on-class 写遮蔽落接收类自身/拒绝新增；bound 缓存三铁则 + 首解析快照语义）；§3.1 peek-store 表补 MAKE_STATIC 镜像形态注记。
- grammar.txt def 节：「ObjFn 持 defining class」改写为闭包持（措辞级）；嵌套类 bullet 加 parser 现状注（member 分派无 defDecl，语义描述为后继方向）。
- vm-design §6 M5 行标已落地；§4 相应小节（callable 清单加 CLASS/BOUND_METHOD、this 槽 1、Movement tracer 第 4 根）；gc-implementation-plan 对应行。
- rules/object/compile/runtime/memory 落地状态复核；CLAUDE.md/README 进度行（已落地补 M5，待续收敛 M6 协程）；文档索引本计划条目改「定稿并已落地」。
- 坑点文档补录（预期高发区：bound 缓存与遮蔽交互、MAKE_CLASS/MAKE_METHOD peek-不弹栈纪律、init seed 时序、Locate 模式与既有复合赋值机制的合流）。
- 全量验证：默认与 `build/tagvalue` 双配置 ctest 全绿；`--eval` 冒烟：类定义+实例化、继承+super、静态共享槽、this 嵌套捕获、（顺带）**用户类可迭代 for-in**（iter/has_next/next 经 LOAD_FIELD 返绑定方法 + CALL -- M5 落地后用户定义类即可迭代，内建 list/map/string 迭代仍待容器里程碑）。

## 4. 验收

- 路线表 M5 标准：类定义/实例化/继承/super 样例通过。
- 双值表示配置 ctest 全绿（机制测试覆盖：实例化快/慢路径、方法调用与 this 字段、继承/super、共享槽、缓存三铁则、GC stress 存活）。
- 零新增 opcode（九条早已预留，`kOpCodeCount=63` 不变），Disassembler 零改动。
- **错误码零新增**（InvalidSuperUse/SuperOutsideMethod/ThisOutsideClass/RedefinedMember + NilDereference/UndefinedProperty/SuperNoBaseClass/TypeMismatch/InvalidState 均已预置于 ErrorCode.hpp）。
