# P0 语言面补齐实施计划

本文档定稿 P0「已解析未编译」语言面的补齐设计与实施顺序:**批 1-2 为编译器前置批(默认参数 / match,与对象无关先行,2026-09-16 二次拍板;理由:其余集合项归根到底都依赖对象实现,先清掉纯编译器项),批 3 起为对象工作**(list 值表示 → 方法机制地基 → 各类型)。**路线位置:先行于 M6 协程**(2026-09-16 拍板;理由:集合是写程序的基本盘,且 M6 验收样例「协程生成器」要落在定形的迭代协议上)。基准:`docs/grammar.txt`(文法与迭代协议注释)、`bytecode-instruction-set.md` §4.14(MAKE_LIST/MAKE_MAP/MAKE_RANGE/LOAD_INDEX/STORE_INDEX 已预置规格)、`compound-assignment-lowering.md` §4.3(下标四模式矩阵)、`Object.hpp` 成员/下标/运算符协议缝(备置 API)、`ObjType` 预留 LIST/MAP/RANGE/ITERATOR。

## 1. 方法机制:两层分派

架构缝已由 M5 备置,本里程碑补生产侧:

- **引擎缝(C++ 协议虚函数,备置已就绪)**:`Object::load_field/store_field/load_index/store_index` + `op_*` 运算符族 + `op_call`。VM 的唯一分派入口,不按子类型 switch;与 `equals`/`trace`/`debug_repr` 同族的引擎内多态,**语言不可见**。
- **语言方法面(类表)**:语言可见方法(`s.upper`、`it.next`、`xs.iter`)一律住 `ObjClass` 表。内置类型由 VM 构造期 bootstrap 类承载(`string_class_`/`list_class_`/`map_class_`/`range_class_`/`iterator_class_`,对标既有 `object_class_`;GC 经 vm_roots tracer 标根,先例同)。
- **绑定路径(内置侧 = 恒绑定)**:内置类表条目全为原生函数(用户类的方法是闭包——形态差别,机制相同),恒为方法、恒绑定。内置类型的 `load_field` override 收口为「委托自身 bootstrap 类链(`ObjClass::load_field` 读穿透)→ 命中即 override 自持 `new_bound_method(命中值, receiver)`」,查表即直调类协议(2026-09-18 review 改定:曾收口的内置侧专用 helper `load_builtin_member` 经两轮收窄后整体删除,各类型 override 直调 `ObjClass::load_field`,与实例路径「先委托类协议、后自己绑定」完全同形)。
- **与实例路径不同构,不硬合**:实例路径的绑定判别是闭包戳(`is_method`)+ fields 缓存回填(M5 语义);内置类表全是原生函数,`is_method` 恒 false,戳判别不可复用。两路共享的只有 `ObjClass::load_field` 这层(本来就是共享);实例路径保持现状不动,内置侧另立 helper——「共享绑定 helper 自 ObjInstance 提炼」的早期设想作废。
- **`store_field` 不 override**:基类默认「type X does not support field access」即内置类型的正确行为(不可变/无名成员)。

**决策 D1(2026-09-16 拍板):内置类 super 挂 Object 根**。uniform OOP 提前半步;接受 `s.init` 经链解析到 Object 根类的 no-op init(原生不动槽 0,调用返回 receiver 自身)——已知的小语义毛边,uniform OOP 落地时随 Object 方法面一并审视。

**bound 缓存缺口(v1 接受)**:实例路径的 bound 缓存靠写回 fields 表(三铁则),内置类型无 fields 表可写,每次取方法现场物化 ObjBoundMethod。forIn 循环体每迭代 2 次小分配,v1 接受;预留的 `INVOKE_METHOD`(Invoke 操作数 = 名 + argc)为后续融合派发性能批——`recv.name(args)` 不物化 bound 对象直调,bench 驱动再做。备选「forIn 降糖时提升 `var __next = it.next` 到循环外」否决:隐藏局部污染作用域与 dump。

**无方法性戳、`ObjClass` 表零改动**:内置类 v1 无静态、类名不作值暴露,表内条目恒绑定(绑定发生在类型自身的 override 里,不在表项上);`String.fromCharCode` 式静态需求出现时再泛化表项判别。

### 1.1 落地要点(批 4 契约)

- **VM 成员与构造序**:`list_class_`/`iterator_class_`(批 4;`string_`/`map_`/`range_class_` 随批 5-7 出生即用)。ctor 里 `bootstrap_builtin_classes()` 排在 `bootstrap_object_class()` 之后(super = Object 先建)、`register_builtins` 之前。
- **注册面**:每类型一个 `register_<type>_builtins(GC&, ObjClass*)`,住 runtime/builtins/ 每类型一个 `<Type>Builtins.{hpp,cpp}`(2026-09-18 review 改定:方法面是 VM 侧语言面、object 层保持纯表示,对标 `Builtins.cpp` 先例;方法体同文件,VM 只编排)。内部 `new_native_fn` + `set_field`,对标 `Builtins.cpp` 的 kBuiltins 循环;注册名必经 intern 池——与 CodeGen `LOAD_FIELD` 常量同指针,`===` 查表成立(Builtins.cpp 先例)。
- **tracer**:vm_roots 加 `mark_object`(各 bootstrap 类),对标 `object_class_` 第 4 根。
- **调用链零改动**:`call_bound_method`(槽 0 覆写 receiver)→ `call_value` → `call_native`(`slots[0]` = this 兼返回槽),M5 已通;`ObjBoundMethod` 的 receiver Value 泛化即为此留的缝。
- **GC 纪律**:bootstrap 期在 ctor 构造临界区(GC 挂起,`make_lock`)内免守卫、建成发布进寄存器组/表(tracer 恒标);`LOAD_FIELD` 绑定路径的 bound 对象白色,接收者在栈(peek 不弹)、方法值经类链可达,返回值写回原槽根化(与实例路径同纪律);iter_fn 覆写时序:被遍历 list 在 `slots[0]` 被覆写前仍为根,迭代器建成后先写槽发布再返回,中间无 GC 点。

## 2. 迭代协议

**契约**:`coll.iter()` → 迭代器;`it.has_next()` → bool;`it.next()` → 下一值。三方法**纯约定名(结构性),编译器零魔法**——forIn 降糖已按此发射(`emit_method_call0`:LOAD_FIELD + CALL),用户类今日即可实现协议让 forIn 遍历自己;内置类型补齐后同一降糖走通,协议对两类来源不可区分。

**决策 D2(2026-09-16 拍板):协议三方法是方法表方法,不为迭代协议另开 Object 虚函数**。协议经既有 `load_field` 协议缝解析;理由:①方法必须一等(`var n = it.next; n()` 虚函数做不了);②与用户类统一,单一派发路径;③VM 内部无迭代消费者(解构走下标、GC 不迭代),双通道纯漂移风险;④决定性论据(拍板补记):能实现 iter 的对象必已支持 load_field、必已有方法表——协议放表内是自然归宿。「iter 做成虚函数」方案已否决(消费者不对称:op_add/load_index 的消费者是 VM 自身 opcode,iter 的消费者是编译产物与用户代码,虚函数会把用户类 iterable 排除/迫使 forIn 分叉;引擎侧 C++ 迭代便利留 stdlib 批按需另议,不入语言协议)。M6 红利:生成器 = Movement 类表挂 `next/has_next` 原生,forIn 零改动消费。

**迭代器表示:每源迭代器子类 + 引擎缝虚函数**(2026-09-18 拍板改定,推翻早稿「单一 ObjIterator{source: Value, cursor: u64} + 原生按 source 四分支」——类型标签结构体与手写 switch 违背引擎缝「不按子类型分型」的架构;跨语言对照 Python/JS/Java/C#/C++ 均为每源独立迭代器对象 + 统一虚契约;C++ 迭代器因「GC 一等 Value/指针悬垂于元素缓冲扩容/用户类统一协议」三硬点不可直接用,但其「每类型自己的表示 + 统一契约」思想即本方案的运行期对应物):`ObjIterator` 基类(共享单数 `ObjType::ITERATOR`,`type(it)` 恒 "Iterator")钉引擎缝纯虚 `has_next() const noexcept -> bool`(纯查询无分配无 fail,不收 vm)/`next(AriaVM&) -> Opt<Value>`(越界 `return vm.fail(IterationExhausted)` 一行,FailSignal 哨兵)/`trace`(纯虚钉「各子类标各自的源」,忘标 = 编译错);`load_field` override 基类一次(直调 Iterator 类协议查表 + 自持绑定,全子类共享);debug_repr "<iterator>"。各源自持自然游标、源码住 `src/object/iterator/`(每源一对文件):ObjListIterator{list, 元素下标}/ObjStringIterator(批 6){str, 字节偏移,码点步进}/ObjMapIterator(批 5){map, 槽位扫描}/ObjRangeIterator(批 7){区间当前值,无 source 对象}。has_next/next 语言方法面经 Iterator 类表恒绑定(原生是虚缝薄壳,分派编译期封闭,无 switch 无 default);每集合的 iter 与其迭代器子类成对出生(铸造口按类型解开 receiver),批 5-7 只加子类、IteratorBuiltins 零改。迭代器对象不可省(早稿论证保持):游标状态须随迭代器走——容器不可自带游标(嵌套遍历同一列表互不串扰),「每次 iter() 产出一个带自己游标的小对象」是协议的结构必需,与派发机制选择无关。批 4 bootstrap List + Iterator 两类(String 空类表无验证价值,随批 6 带方法进场)。

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

> **落地状态(2026-09-16)**:批 1 已全部落地。底座(LOAD_REG + registers_ + 构造临界区 + LOAD_OBJECT 收编)先行落库;默认参数本批:DefaultMark 印章入寄存器(私有 no-op native `<default>`,不注册 builtins)、`ObjFunction.min_arity`、call_closure 区间检查(min==arity 单数文案保持)+ 印章垫充补满参栈深、序言 `LOAD_LOCAL s; LOAD_REG DefaultMark; EQUAL; JUMP_FALSE 跳过; <默认值>; STORE_LOCAL s` 逐槽换值、参数登记与序言单循环交错(先编缺省表达式后登记本参数名,见语义条 2026-09-16 改定)、validate_params 收窄为仅拒 varargs。附带:`LOAD_REG` 反汇编改专用 `RegU8` 格式附寄存器可读名注释(兑现名表「反汇编注释用」)。

与对象无关的批次(2026-09-16 二次拍板先行);VM 侧 = `call_closure` 垫充 + 值寄存器底座(一条新指令 `LOAD_REG`)。**varargs 拆出至批 4**:rest 参数须把额外实参打包成 list,依赖 list 值表示——原 P0-1 的合称在此修正。

- 语义(文法既定):默认值在调用且该参数未传时求值,按缺省参数从左到右逐个补;默认值表达式可引用先于它的参数(序言按编译序先编缺省表达式、后登记本参数名,轮到本槽时前序槽必已就位;对前序参数赋值亦合法);引用自身/后序参数时名字未登记、按常规解析链落外层/全局--有同名全局静默用之、无则缺省被求值时 UndefinedVariable(2026-09-16 改定:纯自然序,Python/C++ 默认值作用域同款;JS 的 TDZ 式运行期专错与 Kotlin/Swift 式编译期检查不采,印章不可达故无需检查兜底,窗口机制与 DefaultParamSelfRef 码随之删除)。缺省块连续居后由文法结构性保证(`paramsBody` 定序 plainParams -> defaultParams -> varargs),validate_params 无需新增次序检查--not_impl 改为仅拒 `is_varargs`。
- 底座:值寄存器组(批 1 落地,批 2 复用;2026-09-16 五次拍板:定位放宽为「VM 单例值的统一存放表」)。跨层共享小头注册表 `ARIA_VALUE_REGISTER_LIST` → `enum class ValueRegister` + `kValueRegisterCount`(风格对齐 ARIA_ERROR_LIST 先例,偏移常量 `k<名字>Offset`(即寄存器组内格位)与枚举/名表同源派生,scoped enum 不隐式转整型故下标走常量);**独立小头置 runtime 层**(寄存器是虚拟机的一部分,2026-09-16 拍板,如 `runtime/value_register.hpp`;自含叶子头仅依赖 common.hpp,编译侧 include 无回环--AriaVM → Compiler 的正向依赖不受影响)。`AriaVM` 持 `Value registers_[kValueRegisterCount]` 为**唯一存放处**:每个 VM 单例出生即登记一格(注册表行 + bootstrap 填充行,不再逐个声明成员),`set_vm_roots` 一趟循环标根全表(不再逐成员 tracer 行);现有 object_class_ 同批收编(accessor 改读寄存器,成员退役)。新指令 `LOAD_REG n:u8`(`[] -> [registers_[n]]`,寄存器只读、无 STORE_REG)。两角色分工:**存储面** = VM 单例出生即入格;**发射面** = 有字节码消费者的格才被编译侧 LOAD_REG 引用,其余纯 C++ 存取(`registers_[ValueRegister::ListClass]` 直读,高频位可配薄 accessor)。批次节奏:批 1 底座 + ObjectClass + DefaultMark;批 2 MatchNoArm;批 4 ListClass/IteratorClass;批 5-7 MapClass/StringClass/RangeClass。既有 LOAD_OBJECT 收编(`def` 无 super 的根类加载,发射点仅一处,专用零操作数指令退役);nil/true/false 保留专用指令不收编(全 VM 最热加载,收编徒增操作数字节与寻址一跳,已落地机制不为统一性翻工);可变 VM 状态(模块表/builtins 表/源根)非单例,不进寄存器。
- 机制(印章方案,2026-09-16 拍板:印章 VM 持有入寄存器;LOAD_ARGC 计数方案与印章穿透 Compiler 签名方案出局):印章 = VM bootstrap 铸造的一枚全局私有空操作 ObjNativeFn(身份判等,不注册进 builtins 故用户不可达--**不可伪造是本方案唯一长期不变式**);`ObjFunction` 仅增 `min_arity`(必传参数数);`call_closure` 元数检查改区间检查(argc < min_arity || argc > arity → WrongArity;varargs 落地后上界另行;min_arity == arity 时报错文案保持单数形式,有缺省才用区间文案)。
- 发射:序言每缺省槽 s:`LOAD_LOCAL s; LOAD_REG DefaultMark; EQUAL; JUMP_FALSE 跳过; <默认值表达式>; STORE_LOCAL s; 跳过:`--命中印章现场求值换值(默认值只在未传时求值),实参在位则跳过。`call_closure` 区间检查通过后把槽 [argc+1..n] 按序 push `registers_[DefaultMark]` 再 `enter_frame(closure, n)`--垫充同时把栈顶从实参深度补齐到满参深度(体局部槽号按满参编,参数槽 1..n、体局部自 n+1 起,补齐后体局部才落对槽),方法帧槽 0 = this 同构适用。除 LOAD_REG 外全为既有指令;CallFrame 零新字段。批 4 varargs 接入时垫充只补缺省的固定参数槽,rest 打包按栈形推算,届时细化。
- 验收:直接/间接调用(经变量、bound method)缺省填充一致、min_arity 区间检查报 WrongArity、默认值表达式仅在实际未传时求值(带副作用默认值单测)、实参为函数值(与印章同类型,如显式传 println)不误判为未传。

### 4.2 批 2:match 语句 / 表达式(编译器前置批)

> **落地状态(2026-09-17)**:批 2 已全部落地。对草稿形态一处改定:subject 不入隐藏临时局部,改驻留栈上 in-flight(逐臂 `DUP` 副本比较、命中臂入口 `POP` 消费、未命中路径由 `THROW` 的 unwind 清栈)——隐藏临时局部在 matchExpr 的 `L_end` 汇合点下压着臂值,弹区清理会连同臂值一起弹掉(值填槽窗口局限,即 2026-09-14 回退决策点名的窗口);in-flight 零局部登记,窗口无错位。其余同草稿:`JUMP_FALSE` 逐臂链 + `_` 直入 + `LOAD_REG MatchNoArm; THROW` 共享单例(值寄存器注册表加行 + `bootstrap_match_no_arm`,消息按 make_message 同源烘焙 `Runtime: MatchNoArm no arm matched`)。
> 通配臂恒末臂(2026-09-17 拍板,拒绝路线):其后臂任何输入下不可达,静默截断会吞臂序 bug,
> 故编译期拒绝——新码 `UnreachableArm`(Semantic),模板辅助 `validate_match_arms`(validate_params
> 同款,两臂类型同 pattern/body 形)折入模板总口 `emit_match` 开头;多 `_` 由同条检查一并拒绝。
> 两 visit 的重复臂链经用户点名收口同一总口(臂体经 `emit_arm_body` 重载分派,visit 退化一行委派)。

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
| 4 | 对象地基 II:方法机制(bootstrap 类 List + Iterator)+ `ObjIterator` + list 的 iter/has_next/next/push/pop + forIn 走通 list + varargs(list 载体,call_closure 打包段)+ 新码 IterationExhausted(有消费者才加码,本批) | forIn 求和/嵌套遍历、用户类 iterable 与内置同降糖路径、varargs 收集正确、GC stress 下无悬垂 |
| 5 | `ObjMap` + `MAKE_MAP` + map 下标 + len + 迭代器 map 分支(`[k,v]`) | map 字面量/键读写/KeyError/forIn 循环变量拿到整个 `[k,v]` pair(解构目标随批 8,2026-09-19 拍板) |
| 6 | string 方法批:string 下标/迭代 + upper/lower/trim/split/join/find/replace/substring/starts_with/ends_with(+cp 方法届时命名) | 逐方法单测 + 字符串 forIn 按码点 |
| 7 | `ObjRange` + `MAKE_RANGE` + 区间迭代(range 分支) | `for (i in 0..10)`、`..`/`...` 含否上界、非结合 |
| 8 | 解构:var 声明 pattern / forIn 目标 / 解构赋值(依赖批 3/4) | 文法说明区既定语义:多余忽略、不足越界报错、rest 末尾绑名 |
| 9(性能) | `INVOKE_METHOD` 融合派发(bench 驱动,消灭热路径 ObjBoundMethod 物化) | bench 前后对照 |

顺序依赖(2026-09-16 二次拍板):批 1-2 与对象无关(批 1 含值寄存器底座),先行清掉;批 3(list 值表示)+ 批 4(方法机制 + 迭代协议)构成对象地基,批 5-7 各踩批 4 的方法表地基;批 8 依赖批 3(下标)+ 批 4(协议);批 9 性能批殿后。每批完成 = 构建 + ctest 双配置(主构建必跑;触及值表示时 TagValue 构建加跑)+ clang-format 幂等。

> **落地状态(2026-09-18)**:批 3 已全部落地(两步两 commit:前半「列表字面量与 list 值表示」/后半「下标读写」)。
> 前半 = `ObjList`(元素 `AriaArray` 成员直曝 `elements()`,equals 按内容递归,debug_repr 渲染 `[1, "ab"]`)+
> `MAKE_LIST`(VM:元素 peek 在栈跨分配「栈即根」,`copy_from` 整段拷入 trivial 不触 GC)+ 字面量发射先检后发
>(`kMaxListElements`=65535 超限报新码 `TooManyElements`,Resource 类)+ len 增 List 分支(消息改 string or list)、
> str/print 经 debug_repr 零改动。后半 = `LOAD_INDEX`/`STORE_INDEX`(执行体 `run_load_index`/`run_store_index`,
> 统一走 `Object::load_index/store_index` 协议,list override:整数键,越界/负数 IndexOutOfBounds(越界值与长度
> 就地拼文案)、非整数 TypeMismatch、store 不自动增长;非对象守卫文案留执行体,与 field 族同款)+ 下标四模式
> lowering(Prepare 备 obj+idx 对/Locate `DUP2`+`LOAD_INDEX` 留副本对,locator-once)。既有性能坑随边界测试
> 暴露并记档:`SourceFile::locate` 逐 token 行内列号计数,单行长源文件 O(n^2)(测试侧逐元素换行规避,未修)。

> **落地状态(2026-09-18,批 4 子批 ①「方法机制地基」)**:List bootstrap 类(寄存器 ListClass 格,super 挂
> Object 根)+ `ObjList::load_field` 两步 override(直调 `ObjClass::load_field` 查表,命中自持
> `new_bound_method` 恒绑定)+ `register_list_builtins`(push/pop,kListBuiltins 表循环)+ `list_class()`
> 薄 accessor(ObjList 不持 class_ 成员,类型→类映射收敛 VM 侧)。与计划三处出入:①tracer 零改动(§1.1
> 「vm_roots 加 mark_object」写在批 1 寄存器组落地前,registers_ 一趟循环已覆盖);②IteratorClass 未随本批
> bootstrap(类表空、无消费者,随子批 ② ObjIterator 出生);③`register_<type>_methods` 形参按参数传递规范取
> `ObjClass*`(计划原文 `ObjClass&`)。push 返回 nil(Python append 同款)、pop 空表 IndexOutOfBounds
>("pop from empty list")。`xs.foo` 报错主语随 override 从 receiver debug_repr 变为 `<class List>`(类措辞,
> 与实例路径对齐);D1 毛边 `xs.init()` 返回 receiver 自身已钉测试;bound 无缓存 v1 接受(循环取方法 stress
> GC 用例钉根化路径)。Review 三改(2026-09-18):① `load_builtin_member` 收窄为纯查表后整体删除(绑定
> `new_bound_method` 由各类型 override 自持,override 直调 `ObjClass::load_field`,与实例路径完全同形);② 方法面
> 迁出 ObjList.cpp,住 `runtime/builtins/ListBuiltins.{hpp,cpp}`(方法面是 VM 侧语言面,object 层保持纯表示,后续各类型
> 方法批同型)。
> 子批余项:③ varargs。

> **落地状态(2026-09-18,批 4 子批 ②「迭代协议」)**:迭代器表示按当日改定落地(每源子类 + 引擎缝,见 §2)。
> `src/object/iterator/` 新目录:ObjIterator 基类(纯虚 has_next/next/trace + size,load_field 基类一次全子类共享)
> + ObjListIterator{list, 下标};`runtime/builtins/IteratorBuiltins.{hpp,cpp}`(has_next/next 两薄壳原生,kIteratorBuiltins 表
> 循环)+ ListBuiltins 增 iter(铸造口按类型解开 receiver,建成即写回槽发布);寄存器 IteratorClass 格 +
> bootstrap_iterator_class + iterator_class() 访问器;新码 IterationExhausted("iterator exhausted",插在
> NotIterable/IteratorProtocol 预留码旁,两码仍不接线)。forIn 走通 list;基线实测用户类自实现三方法在子批 ②
> 之前 forIn 即可走通(降糖对两类来源不可区分),钉进测试防回归。测试 21 新:Compiler.ForIn*/Manual/
> NextExhausted*/UserClass 等 12 端到端 + ObjIterator/ObjListIterator 9 对象级(bootstrap 契约/恒绑定/游标推进/
> 越界 fail-fast/双迭代器独立/trace stress)。**② 已落库 529705b(2026-09-19 提交)**。

> **落地状态(2026-09-19,批 4 子批 ③「varargs」)**:rest 参数以 list 为载体落地,零新指令、零文法改动。①ObjFunction 增
> `is_varargs_` 标志,`arity_` 语义精确化为**固定参数数**(不含 rest,帧参数槽深 = arity + is_varargs),工厂尾参显式传
>(min_arity 先例,src 两处 + 测试助手八处随改);②CodeGen 三处:validate_params 删 varargs 拒绝循环(ErrNotImplementedVarargs
> 钉随翻转换 VarargsCompiles 正钉)、min_arity 遇 varargs 即止(rest 永非必传)、compile_function 传固定参数数与标志;
> rest 在编译侧就是末位普通局部槽,零发射;③call_closure:varargs 分流元数检查(只保下界,文案 "expects at least N args,
> got K"),垫充不变(仅补固定缺省槽),新增打包段(超出 arity 的实参 `new_list`+`copy_from` 整段收集,白色 list 随即
> drop+push 入栈根,MAKE_LIST case 同构;无多余实参铸空表),`enter_frame` 槽深 = arity + is_varargs。原稿「装配指令」修正为
> 装配段(字节码侧无 argc 源,LOAD_ARGC 已否;§4.3 表行同步)。rest 每次调用新铸、可被闭包捕获、方法帧同构适用。
> 测试 11 增(Compiler.Varargs* 9 端到端 + CodeGen.VarargsCompiles + ObjFunction.VarargsFlag)。Review 改定(2026-09-19):call_closure
> 拆编排形(对标旧版 aria vm.cpp call_function 的 pack_varargs/create_call_frame 分层)——元数检查收 check_arity、
> 缺省垫充+varargs 打包+槽深推导收 prepare_call_args(返回帧参数槽深),call_closure 只剩四行编排。批 4 至此收官
>(本子批工作区待 review,双配置 879/879 绿);CLAUDE.md/README 进度行已同步(批 1-4 落地,待批 5+)。

> **落地状态(2026-09-19,批 5)**:批 5 已全部落地(工作区待 review,双配置 935/935 绿)。`ObjMap`(AriaHashTable 成员,
> Phase 3 备置的缝兑现:trace 委托 ht.trace、键判等表内 ===)+ `MAKE_MAP`(执行体对齐 MAKE_LIST:键值 peek 在栈跨
> new_map 顶部 maybe_collect,逐对 set 走 GC 分配器不触 GC,窗口内无 GC 点;重复键天然后键胜,set 命中原槽覆写,
> Python dict 同款,零特判)+ map 下标 override(读任意键、miss KeyError 键 debug 形入文案;写恒成功 upsert,vm 参数
> 未消费系协议缝签名钉死)+ equals(键 === 表内 find 语义、值 == 递归,EqualGuard 防环;槽位扫描支持首 miss 早退)+
> debug_repr `{"k": v}` 式(键值 debug 形,PrintGuard 防环;多键渲染序随槽位)+ `ObjMapIterator`(槽位扫描游标,
> next 产出 [k,v] 二元 list,每步一小分配 D4 接受;耗尽 IterationExhausted fail-fast)+ bootstrap MapClass(寄存器格
> MapClass + `register_map_builtins` 单 iter 方法,has_next/next 住 Iterator 类表零改)+ len 增 Map 分支(文案改
> "string, list or map")。HashTable 增槽位扫描原语 `next_occupied`/`entry_at`(kNpos 转公开)——本批唯一非对齐面
> 新代码。语义三拍板(2026-09-19):equals 键 === 值 ==(与表内键语义一致)、字面量重复键后键胜、kMaxMapEntries
> 单立(条目对数 u16 上限,与 kMaxListElements 分名,注释各述「元素数/条目对数」)。forIn 解构目标随批 8(本批
> forIn 循环变量拿整个 pair,验收口径收窄见 §4.3 表行)。后继演进(同日,批 5 收官后):HashTable 槽位原语
> next_occupied/entry_at 与回调式 for_each_occupied 正名化为嵌套 const_iterator(begin/end,与 Array 对称,失效
> 语义同 std::unordered_map 惯例),util 增 join(range, delimiter, transform),两处 debug_repr/equals/trace/
> ObjMapIterator 游标全部换装,三原语退役。

> **落地状态(2026-09-19,批 6)**:批 6 已全部落地(工作区待 review,双配置 967/967 绿)。语义十拍板(2026-09-19
> 走查呈报,用户全按建议):①upper/lower v1 ASCII only(Unicode casing 需 case 表后续批);②trim 空白 = ASCII
> 六字符;③空模式串报错 + 新码 `EmptyPattern`(Runtime,插 KeyError 旁,split 空 sep 与 replace 空 old 共用);
> ④split 保留空段("a,,b"->["a","","b"],空串输入->[\"\"],Python/JS 同款);⑤join receiver 挂 list、元素宽松经
> format_value(JS 式,空 sep 合法、空 list 返空串)——批 5 收官铺的 util::join 底座在此兑现;⑥find 未命中返
> -1(单参,字节下标);⑦replace 全部替换;⑧substring 越界(含负数)报 IndexOutOfBounds 不钳制,argc 1/2 双形态;
> ⑨迭代与 s[i] 产出 1-char string;⑩cp 方法 v1 只 codepoint_at(i)->int(码点序号索引,O(i) 扫描无偏移表)。
> 落地面:ObjString 三 override(下标读整数键字节域产出单字节 1-char 串/写恒 TypeMismatch "string does not
> support subscript assignment"/load_field 两步委托 String 类)+ ObjStringIterator{str, 字节偏移}(decode_one
> 码点步进,utf8::encode 铸 1-char 串;src/object/iterator/ 第四对)+ String bootstrap(寄存器 StringClass 格 +
> bootstrap_string_class + StringBuiltins 11 方法表)+ ListBuiltins 增 join_fn。GC 模式:单输出方法 receiver 在
> slots[0] 覆写前经栈根;split 先拷内容进 C++ String(非 GC 内存)再 list 先发布后逐段铸造段串。测试 33 新
>(test_objstring 8 + ObjStringIterator 5 + Compiler.String* 14 + 语料 5:string_methods/string_subscript_iter/
> string_print_format golden/runtime_string_split_empty/runtime_string_subscript_assign 负 .err);存量翻转一
>(ObjectProtocolDefaults 基类默认钉子 string 换 Module)。string 的 + 拼接(op_add)不在批 6,仍基类默认报错。

## 5. 参照

- `Object.hpp` 备置协议缝注释(成员/下标/运算符/可调用四组)——本计划的架构基准。
- `ObjInstance::load_field`(绑定路径)与 `ObjClass::load_field`(链读穿透、类侧不绑定)——共享绑定 helper 的两处消费点。
- `ObjBoundMethod`(method_/receiver_ 均 Value 泛化,原生方法调用约定「slots[0] = receiver」)——运行时侧已就绪,本计划只补生产侧。
- `bytecode-instruction-set.md` §4.14/§6.4(五条预置指令的栈形与操作数)、`compound-assignment-lowering.md` §4.3(下标四模式)。
- Wren 0.4(本地 `/Users/icelake/src/wren`):每值一类 + 类表方法派发(`wren_value.c` value 为 class 成员)、迭代即方法(Wren 无内建迭代器对象,list 迭代走下标,aria 取「协议方法 + 迭代器对象」路,与 Python/JS 同形)。