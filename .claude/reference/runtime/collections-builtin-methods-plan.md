# P0 语言面补齐实施计划

本文档定稿 P0「已解析未编译」语言面的补齐设计与实施顺序:**批 1-2 为编译器前置批(默认参数 / match,与对象无关先行,2026-09-16 二次拍板;理由:其余集合项归根到底都依赖对象实现,先清掉纯编译器项),批 3 起为对象工作**(list 值表示 → 方法机制地基 → 各类型)。**路线位置:先行于 M6 协程**(2026-09-16 拍板;理由:集合是写程序的基本盘,且 M6 验收样例「协程生成器」要落在定形的迭代协议上)。基准:`docs/grammar.txt`(文法与迭代协议注释)、`bytecode-instruction-set.md` §4.14(MAKE_LIST/MAKE_MAP/MAKE_RANGE/LOAD_INDEX/STORE_INDEX 已预置规格)、`compound-assignment-lowering.md` §4.3(下标四模式矩阵)、`Object.hpp` 成员/下标/运算符协议缝(备置 API)、`ObjType` 预留 LIST/MAP/RANGE/ITERATOR。

## 1. 方法机制:两层分派

架构缝已由 M5 备置,本里程碑补生产侧:

- **引擎缝(C++ 协议虚函数,备置已就绪)**:`Object::load_field/store_field/load_index/store_index` + `op_*` 运算符族 + `op_call`。VM 的唯一分派入口,不按子类型 switch;与 `equals`/`trace`/`debug_repr` 同族的引擎内多态,**语言不可见**。
- **语言方法面(类表)**:语言可见方法(`s.upper`、`it.next`、`xs.iter`)一律住 `ObjClass` 表。内置类型由 VM 构造期 bootstrap 类承载(`string_class_`/`list_class_`/`map_class_`/`range_class_`/`iterator_class_`,对标既有 `object_class_`;GC 经 vm_roots tracer 标根,先例同)。
- **绑定路径(内置侧 = 恒绑定)**:内置类表条目全为原生函数(用户类的方法是闭包——形态差别,机制相同),恒为方法、恒绑定。内置类型的 `load_field` override 收口为「委托自身 bootstrap 类链(`ObjClass::load_field` 读穿透)→ 命中即 `new_bound_method(命中值, receiver)`」,五种类型收口为内置侧专用 helper `load_builtin_member(vm, klass, receiver, name)`(住 ObjClass.hpp),每类型一行委托。
- **与实例路径不同构,不硬合**:实例路径的绑定判别是闭包戳(`is_method`)+ fields 缓存回填(M5 语义);内置类表全是原生函数,`is_method` 恒 false,戳判别不可复用。两路共享的只有 `ObjClass::load_field` 这层(本来就是共享);实例路径保持现状不动,内置侧另立 helper——「共享绑定 helper 自 ObjInstance 提炼」的早期设想作废。
- **`store_field` 不 override**:基类默认「type X does not support field access」即内置类型的正确行为(不可变/无名成员)。

**决策 D1(2026-09-16 拍板):内置类 super 挂 Object 根**。uniform OOP 提前半步;接受 `s.init` 经链解析到 Object 根类的 no-op init(原生不动槽 0,调用返回 receiver 自身)——已知的小语义毛边,uniform OOP 落地时随 Object 方法面一并审视。

**bound 缓存缺口(v1 接受)**:实例路径的 bound 缓存靠写回 fields 表(三铁则),内置类型无 fields 表可写,每次取方法现场物化 ObjBoundMethod。forIn 循环体每迭代 2 次小分配,v1 接受;预留的 `INVOKE_METHOD`(Invoke 操作数 = 名 + argc)为后续融合派发性能批——`recv.name(args)` 不物化 bound 对象直调,bench 驱动再做。备选「forIn 降糖时提升 `var __next = it.next` 到循环外」否决:隐藏局部污染作用域与 dump。

**无方法性戳、`ObjClass` 表零改动**:内置类 v1 无静态、类名不作值暴露,表内条目恒绑定(绑定发生在类型自身的 override 里,不在表项上);`String.fromCharCode` 式静态需求出现时再泛化表项判别。

### 1.1 落地要点(批 4 契约)

- **VM 成员与构造序**:`list_class_`/`iterator_class_`(批 4;`string_`/`map_`/`range_class_` 随批 5-7 出生即用)。ctor 里 `bootstrap_builtin_classes()` 排在 `bootstrap_object_class()` 之后(super = Object 先建)、`register_builtins` 之前。
- **注册面**:每类型一个 `register_<type>_methods(GC&, ObjClass&)`,住该类型 .cpp(方法体同文件,VM 只编排)。内部 `new_native_fn` + `set_field`,对标 `Builtins.cpp` 的 kBuiltins 循环;注册名必经 intern 池——与 CodeGen `LOAD_FIELD` 常量同指针,`===` 查表成立(Builtins.cpp 先例)。
- **tracer**:vm_roots 加 `mark_object`(各 bootstrap 类),对标 `object_class_` 第 4 根。
- **调用链零改动**:`call_bound_method`(槽 0 覆写 receiver)→ `call_value` → `call_native`(`slots[0]` = this 兼返回槽),M5 已通;`ObjBoundMethod` 的 receiver Value 泛化即为此留的缝。
- **GC 纪律**:bootstrap 期在 ctor 构造临界区(GC 挂起,`make_lock`)内免守卫、建成发布进寄存器组/表(tracer 恒标);`LOAD_FIELD` 绑定路径的 bound 对象白色,接收者在栈(peek 不弹)、方法值经类链可达,返回值写回原槽根化(与实例路径同纪律);list_iter 覆写时序:source 在 `slots[0]` 被覆写前仍为根,建成后先写槽发布再返回,中间无 GC 点。

## 2. 迭代协议

**契约**:`coll.iter()` → 迭代器;`it.has_next()` → bool;`it.next()` → 下一值。三方法**纯约定名(结构性),编译器零魔法**——forIn 降糖已按此发射(`emit_method_call0`:LOAD_FIELD + CALL),用户类今日即可实现协议让 forIn 遍历自己;内置类型补齐后同一降糖走通,协议对两类来源不可区分。

**决策 D2(2026-09-16 拍板):协议三方法是方法表方法,不为迭代协议另开 Object 虚函数**。协议经既有 `load_field` 协议缝解析;理由:①方法必须一等(`var n = it.next; n()` 虚函数做不了);②与用户类统一,单一派发路径;③VM 内部无迭代消费者(解构走下标、GC 不迭代),双通道纯漂移风险;④决定性论据(拍板补记):能实现 iter 的对象必已支持 load_field、必已有方法表——协议放表内是自然归宿。「iter 做成虚函数」方案已否决(消费者不对称:op_add/load_index 的消费者是 VM 自身 opcode,iter 的消费者是编译产物与用户代码,虚函数会把用户类 iterable 排除/迫使 forIn 分叉;引擎侧 C++ 迭代便利留 stdlib 批按需另议,不入语言协议)。M6 红利:生成器 = Movement 类表挂 `next/has_next` 原生,forIn 零改动消费。

**迭代器表示:单一 `ObjIterator`**(对齐预留的单数 `ObjType::ITERATOR`):`{source: Value, cursor: u64}`。source ∈ list/string/range/map;cursor 分别为元素下标/字节偏移/区间当前值/表槽位扫描下标(Swiss table 逐 ctrl 扫描,u64 游标够用)。`has_next`/`next` 原生单点按 source 四分支;不设每源迭代器类(四个机械同构类无语义增益)。`trace` 标 source。迭代器对象不可省:游标状态须随迭代器走——容器不可自带游标(嵌套遍历同一列表互不串扰),「每次 iter() 产出一个带自己游标的小对象」是协议的结构必需,与派发机制选择无关。批 4 bootstrap List + Iterator 两类(String 空类表无验证价值,随批 6 带方法进场)。

- **D3(2026-09-16):`next()` 越界抛 `IterationExhausted`(新码,fail-fast)**。nil 哨兵否决理由:aria list 可合法存 nil,哨兵与真实 nil 元素不可区分(Lua 的 nil 哨兵依赖「表不能存 nil」前提,aria 无此前提);forIn 靠 has_next 把关,越界仅手写滥用时发生,静默吞 bug 劣于报错。
- **D4(2026-09-16):map 迭代序 unspecified**,产出 `[k, v]` 二元 list(文法「元组」,无 tuple 类型,list 承载,每步一次小分配)。理由(拍板):非定序哈希表是性能上的正确选择,用户不应依赖这一边缘暧昧、各语言/实现标准不一的特性。插序将来另批(需额外内存)。
- **迭代中变更容器:v1 不承诺**(rehash 作废游标),文档明示,不做版本守卫(YAGNI)。
- **D5(2026-09-16):string 迭代单位 = 码点**(文法「string->字符」;Go 式分工);**len/s[i] = 字节**(拍板);cp 级访问方法(codepoint_at/chars 等命名届时定)后继行提供。`s[i]` 越界 IndexOutOfBounds、非整数键 TypeMismatch;多字节序列中间字节取到的是该字节自身(字节语义契约的自然结果,非完整字符)。
- 不可迭代值 forIn:`iter` miss 走 `UndefinedProperty` 基类默认,v1 接受;`NotIterable`/`IteratorProtocol` 两码预留,接线时机 = 后续协议强校验需求出现时。

## 3. 下标语义(LOAD_INDEX/STORE_INDEX,栈形见指令集 §4.14)

| 类型 | 读 | 写 |
| :--- | :--- | :--- |
| list | 整数键,越界/负数 IndexOutOfBounds,非整数键 TypeMismatch | 同读;不自动增长(append 走批 4 方法) |
| map | 任意键,miss KeyError | 恒成功(新增键) |
| string | 字节键 → 单字节 1-char 串 | 不可变,报错(string override 定向文案) |

下标是引擎原语不进方法表:有专门 opcode,复合赋值 lowering 依赖 `[obj, idx, v]` 栈形,方法化反而绕。lowering 按 `compound-assignment-lowering.md` §4.3 矩阵:Prepare 只发 `<obj> <idx>`、Load 发 `LOAD_INDEX`、Store 只发 `STORE_INDEX`、Locate 发 `DUP2 + LOAD_INDEX` 留副本对。

`==` 判等:list/map 按内容递归(`equals` override,GC-pure 纪律);hash 恒地址型(可变对象,作 map 键按身份)。`===` 一律指针。str/print 渲染:`[1, "ab"]` 式,嵌套字符串走 debug 形(带引号),避免 `[1, ab]` 的歧义。

## 4. 实施批次与验收

### 4.1 批 1:默认参数(编译器前置批)

与对象无关的批次(2026-09-16 二次拍板先行);VM 侧 = `call_closure` 垫充 + 值寄存器底座(一条新指令 `LOAD_REG`)。**varargs 拆出至批 4**:rest 参数须把额外实参打包成 list,依赖 list 值表示——原 P0-1 的合称在此修正。

- 语义(文法既定):默认值在调用且该参数未传时求值,按缺省参数从左到右逐个补;默认值不得引用同函数参数(DefaultParamSelfRef,码已预注册无消费者,批 1 落地检查--默认值表达式编译期 Identifier 解析命中本函数参数槽即 fail;外层局部经 upvalue 与全局照常)。缺省块连续居后由文法结构性保证(`paramsBody` 定序 plainParams -> defaultParams -> varargs),validate_params 无需新增次序检查--not_impl 改为仅拒 `is_varargs`。
- 底座:值寄存器组(批 1 落地,批 2 复用;2026-09-16 五次拍板:定位放宽为「VM 单例值的统一存放表」)。跨层共享小头注册表 `ARIA_VALUE_REGISTER_LIST` → `enum class ValueRegister` + `kValueRegisterCount`(风格对齐 ARIA_ERROR_LIST 先例,偏移常量 `k<名字>Offset`(即寄存器组内格位)与枚举/名表同源派生,scoped enum 不隐式转整型故下标走常量);**独立小头置 runtime 层**(寄存器是虚拟机的一部分,2026-09-16 拍板,如 `runtime/value_register.hpp`;自含叶子头仅依赖 common.hpp,编译侧 include 无回环--AriaVM → Compiler 的正向依赖不受影响)。`AriaVM` 持 `Value registers_[kValueRegisterCount]` 为**唯一存放处**:每个 VM 单例出生即登记一格(注册表行 + bootstrap 填充行,不再逐个声明成员),`set_vm_roots` 一趟循环标根全表(不再逐成员 tracer 行);现有 object_class_ 同批收编(accessor 改读寄存器,成员退役)。新指令 `LOAD_REG n:u8`(`[] -> [registers_[n]]`,寄存器只读、无 STORE_REG)。两角色分工:**存储面** = VM 单例出生即入格;**发射面** = 有字节码消费者的格才被编译侧 LOAD_REG 引用,其余纯 C++ 存取(`registers_[ValueRegister::ListClass]` 直读,高频位可配薄 accessor)。批次节奏:批 1 底座 + ObjectClass + DefaultMark;批 2 MatchNoArm;批 4 ListClass/IteratorClass;批 5-7 MapClass/StringClass/RangeClass。既有 LOAD_OBJECT 收编(`def` 无 super 的根类加载,发射点仅一处,专用零操作数指令退役);nil/true/false 保留专用指令不收编(全 VM 最热加载,收编徒增操作数字节与寻址一跳,已落地机制不为统一性翻工);可变 VM 状态(模块表/builtins 表/源根)非单例,不进寄存器。
- 机制(印章方案,2026-09-16 拍板:印章 VM 持有入寄存器;LOAD_ARGC 计数方案与印章穿透 Compiler 签名方案出局):印章 = VM bootstrap 铸造的一枚全局私有空操作 ObjNativeFn(身份判等,不注册进 builtins 故用户不可达--**不可伪造是本方案唯一长期不变式**);`ObjFunction` 仅增 `min_arity`(必传参数数);`call_closure` 元数检查改区间检查(argc < min_arity || argc > arity → WrongArity;varargs 落地后上界另行;min_arity == arity 时报错文案保持单数形式,有缺省才用区间文案)。
- 发射:序言每缺省槽 s:`LOAD_LOCAL s; LOAD_REG DefaultMark; EQUAL; JUMP_FALSE 跳过; <默认值表达式>; STORE_LOCAL s; 跳过:`--命中印章现场求值换值(默认值只在未传时求值),实参在位则跳过。`call_closure` 区间检查通过后把槽 [argc+1..n] 按序 push `registers_[DefaultMark]` 再 `enter_frame(closure, n)`--垫充同时把栈顶从实参深度补齐到满参深度(体局部槽号按满参编,参数槽 1..n、体局部自 n+1 起,补齐后体局部才落对槽),方法帧槽 0 = this 同构适用。除 LOAD_REG 外全为既有指令;CallFrame 零新字段。批 4 varargs 接入时垫充只补缺省的固定参数槽,rest 打包按栈形推算,届时细化。
- 验收:直接/间接调用(经变量、bound method)缺省填充一致、min_arity 区间检查报 WrongArity、默认值表达式仅在实际未传时求值(带副作用默认值单测)、实参为函数值(与印章同类型,如显式传 println)不误判为未传。

### 4.2 批 2:match 语句 / 表达式(编译器前置批)

纯降糖,零新指令(LOAD_REG 沿用批 1 底座):

- 形态:subject 求值一次入隐藏临时局部(对标 forIn `<iter>` 隐藏命名);逐臂 `LOAD temp; <pattern 表达式>; EQUAL; JUMP_FALSE 下一臂; <臂体>; JUMP end`;`_` 臂无条件直入(不比较);全臂未命中落尾部 `LOAD_REG MatchNoArm; THROW`——抛寄存器持有的共享 `ObjException(MatchNoArm)`(批 2 注册表加行 + bootstrap 填充;消息静态、无 subject 插值),matchStmt 与 matchExpr 同构(后者臂体为表达式,每臂恰一值)。
- 相等语义 = EQUAL(value_equal,Obj 走 equals 虚函数);pattern 表达式按臂顺序惰性求值(前面臂命中即短路,后面 pattern 不求值)。
- 验收:命中首臂 / `_` 兜底 / 无兜底抛 MatchNoArm / matchExpr 取值 / subject 单次求值(副作用单测)/ pattern 短路不求值。

### 4.3 批次总表

| 批 | 内容 | 验收 |
| :--- | :--- | :--- |
| 1 | 值寄存器组底座(`LOAD_REG` + 收编 `LOAD_OBJECT`)+ 默认参数(§4.1;varargs 拆至批 4) | §4.1 |
| 2 | match 语句 / 表达式(§4.2) | §4.2 |
| 3 | `ObjList` + `MAKE_LIST`/`LOAD_INDEX`/`STORE_INDEX` + len/str/print + 下标四模式 lowering(字面量元素数 u16 上限先检后发,新码 TooManyElements) | 字面量/嵌套/下标读写/复合赋值 locator-once(`arr[f()] += 1` 的 f() 单调)/越界与键类型报错 |
| 4 | 对象地基 II:方法机制(bootstrap 类 List + Iterator + `load_builtin_member`)+ `ObjIterator` + list 的 iter/has_next/next/push/pop + forIn 走通 list + varargs(list 载体 + 装配指令)+ 新码 IterationExhausted(有消费者才加码,本批) | forIn 求和/嵌套遍历、用户类 iterable 与内置同降糖路径、varargs 收集正确、GC stress 下无悬垂 |
| 5 | `ObjMap` + `MAKE_MAP` + map 下标 + len + 迭代器 map 分支(`[k,v]`) | map 字面量/键读写/KeyError/forIn 解构 `[k,v]` |
| 6 | string 方法批:string 下标/迭代 + upper/lower/trim/split/join/find/replace/substring/starts_with/ends_with(+cp 方法届时命名) | 逐方法单测 + 字符串 forIn 按码点 |
| 7 | `ObjRange` + `MAKE_RANGE` + 区间迭代(range 分支) | `for (i in 0..10)`、`..`/`...` 含否上界、非结合 |
| 8 | 解构:var 声明 pattern / forIn 目标 / 解构赋值(依赖批 3/4) | 文法说明区既定语义:多余忽略、不足越界报错、rest 末尾绑名 |
| 9(性能) | `INVOKE_METHOD` 融合派发(bench 驱动,消灭热路径 ObjBoundMethod 物化) | bench 前后对照 |

顺序依赖(2026-09-16 二次拍板):批 1-2 与对象无关(批 1 含值寄存器底座),先行清掉;批 3(list 值表示)+ 批 4(方法机制 + 迭代协议)构成对象地基,批 5-7 各踩批 4 的方法表地基;批 8 依赖批 3(下标)+ 批 4(协议);批 9 性能批殿后。每批完成 = 构建 + ctest 双配置(主构建必跑;触及值表示时 TagValue 构建加跑)+ clang-format 幂等。

## 5. 参照

- `Object.hpp` 备置协议缝注释(成员/下标/运算符/可调用四组)——本计划的架构基准。
- `ObjInstance::load_field`(绑定路径)与 `ObjClass::load_field`(链读穿透、类侧不绑定)——共享绑定 helper 的两处消费点。
- `ObjBoundMethod`(method_/receiver_ 均 Value 泛化,原生方法调用约定「slots[0] = receiver」)——运行时侧已就绪,本计划只补生产侧。
- `bytecode-instruction-set.md` §4.14/§6.4(五条预置指令的栈形与操作数)、`compound-assignment-lowering.md` §4.3(下标四模式)。
- Wren 0.4(本地 `/Users/icelake/src/wren`):每值一类 + 类表方法派发(`wren_value.c` value 为 class 成员)、迭代即方法(Wren 无内建迭代器对象,list 迭代走下标,aria 取「协议方法 + 迭代器对象」路,与 Python/JS 同形)。