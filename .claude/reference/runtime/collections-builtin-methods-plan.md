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

**bound 物化缺口(已收口)**:内置类型无 fields 表可写,早期每次取方法现场物化 ObjBoundMethod(forIn 循环体每迭代 2 次小分配)。这条路径实测占 forIn 每迭代时间的七成(基线见 §4.4),已由批 9 收口:内置侧改走 `Object::resolve_invoke` override,`recv.name(args)` 不再铸 bound,分配列由 2.009/1.000 归零(见 §4.4/§4.5)。**实例侧的 bound 缓存随后整体取消**(§4.6):实例 `resolve_invoke` 也走不绑定形态,读路径改为每次访问现场绑定。备选「forIn 降糖时提升 `var __next = it.next` 到循环外」否决:隐藏局部污染作用域与 dump。

**无方法性戳、`ObjClass` 表零改动**:内置类 v1 无静态、类名不作值暴露,表内条目恒绑定(绑定发生在类型自身的 override 里,不在表项上);`String.fromCharCode` 式静态需求出现时再泛化表项判别。

### 1.1 落地要点(批 4 契约)

- **VM 成员与构造序**:`list_class_`/`iterator_class_`(批 4;`string_`/`map_`/`range_class_` 随批 5-7 出生即用)。ctor 里 `bootstrap_builtin_classes()` 排在 `bootstrap_object_class()` 之后(super = Object 先建)、`register_builtin_functions` 之前。
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
- **D5(2026-09-16):string 迭代单位 = 码点**(文法「string->字符」;Go 式分工);**len/s[i]/size = 字节**(拍板);cp 级访问方法已提供 `codepoint_at(i)`(码点值)与 `chars()`(逐码点切分,码点数 = `len(chars())`)。`s[i]` 越界 IndexOutOfBounds、非整数键 TypeMismatch;多字节序列中间字节取到的是该字节自身(字节语义契约的自然结果,非完整字符)。
- 不可迭代值 forIn:`iter` miss 走 `UndefinedProperty` 基类默认,v1 接受;`NotIterable`/`IteratorProtocol` 两码预留,接线时机 = 后续协议强校验需求出现时。

## 3. 下标语义(LOAD_INDEX/STORE_INDEX,栈形见指令集 §4.14)

| 类型 | 读 | 写 |
| :--- | :--- | :--- |
| list | 整数键,越界/负数 IndexOutOfBounds,非整数键 TypeMismatch | 同读;不自动增长(append 走批 4 方法) |
| map | 任意键,miss KeyError | 恒成功(新增键) |
| string | 整数键 → 单字节 1-char 串；Range 键 → 字节切片（端点从尾计数、无上界后缀、倒序段字节倒排） | 不可变,报错(string override 定向文案) |

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
| 8 | 解构:var 声明 pattern / forIn 目标 / 解构赋值(依赖批 3/4) | 文法说明区既定语义:多余忽略、不足越界报错、rest 末尾绑名 **[已落地]** |
| 9(性能) | 不绑定方法派发(bench 驱动,消灭热路径 ObjBoundMethod 物化):先单条融合,后两段化 | bench 前后对照(基线见 §4.4,结果见 §4.5/§4.7)**[已落地]** |

顺序依赖(2026-09-16 二次拍板):批 1-2 与对象无关(批 1 含值寄存器底座),先行清掉;批 3(list 值表示)+ 批 4(方法机制 + 迭代协议)构成对象地基,批 5-7 各踩批 4 的方法表地基;批 8 依赖批 3(下标)+ 批 4(协议);批 9 性能批殿后。每批完成 = 构建 + ctest 双配置(主构建必跑;触及值表示时 TagValue 构建加跑)+ clang-format 幂等。

> **计划表外补缺 · 字符串 `+` 拼接(2026-09-21)**:批 1-9 收官后补的第一处表外缺口(该缺口原不在本表)。语义:两侧皆 `String` 才成立、产新串(经驻留池故与同内容串 `==`/`===` 同真)、其余含 `String` 的组合报运行期 TypeMismatch;不做隐式转字符串,显式转换走内置 `str()`;`+=` 经既有复合赋值 lowering 同域。接线形态:`Object::op_add` 协议缝的**首个接线者**(此前为备置 API)——`ADD` 走新执行体 `run_binary_add`,对象左值派发协议(`ObjString` override,peek 不弹守「栈即根」),非对象左值照旧委托 `run_binary_numeric`(数值快路径与失败文案不变);算术族其余五个(op_sub/mul/div/mod/negate)仍备置,无消费者不加放宽线。**仍缺**:字符串排序比较(`<`/`>`/`<=`/`>=`)。

> **计划表外补缺 · 模块成员访问(2026-09-21)**:批 1-9 收官后补的第二处表外缺口(同样不在本表)。语义:**模块的顶层绑定即模块成员**(不另设 export 声明,`H.x` 读顶层 var/fun/class 原值、`H.f(args)` 直调;嵌套导入的模块本身也是成员,可 `H.Inner.tag`);成员**只读**(`H.x = v` 报 TypeMismatch -- 越模块写会隐式创建他人未声明全局,违「赋值不隐式创建」,暴露可变状态由模块自己的函数承担);miss 报 UndefinedProperty(循环导入的半初始化模块只影响尚未执行到的绑定,读它同报错、可 catch)。接线形态:纯对象层 —— `ObjModule` override `load_field`(成员 = 查 `globals_`,nil 值绑定与 miss 由 find 空态区分)+ `store_field`(恒拒);`resolve_invoke` 不 override(基类默认即委托 load_field,成员是原值直读、无 bound 物化之虞);零新指令、零新错误码、VM 侧零改动。**仍缺**:字符串排序比较(`<`/`>`/`<=`/`>=`,见 `tests/language/README.md` 禁区)。

> **计划表外补缺 · 字符串比较排序(2026-09-21)**:批 1-9 收官后补的第三处表外缺口。语义:四个比较算子
> (`<`/`<=`/`>`/`>=`)的域扩到字符串 -- 两侧皆 `String` 时按**无符号字节序**比(`string_view::compare`,
> memcmp 语义),结果 Bool;含 String 的混合组合报 TypeMismatch 定向文案;其余类型(含 list/map)仍仅数值。
> **域选择依据**:与 `len`/`s[i]` 的字节域同域,且 `s[i]` 能切出非法单字节串(`"héllo"[1]` = 孤立
> continuation 字节,实测一等值)故必须对任意字节串全序;UTF-8 保序,故合法文本上结果与按码点比较一致。
> 接线形态:比较四件入算术族的虚函数族(`op_less`/`op_less_equal`/`op_greater`/`op_greater_equal`,命名
> 对齐 OpCode 的 LESS/LESS_EQUAL/GREATER/GREATER_EQUAL),四个比较指令经新执行体
> `run_binary_compare<Op>` -- 与 `run_binary_add` 同形:非对象左值委托 `run_binary_numeric`(数值热路径
> 只多一次 `is_obj()` tag 判定),对象左值派发协议(`ObjString` override 字节序比较,GC-pure)。实现坑:
> 必须走 `string_view::compare`,`char` 在多数平台有符号,手写逐 char 比较会把 0x80 以上字节排到 ASCII
> 之前(`"é" < "z"` 会反过来)。**至此计划表外三处缺口全部补齐**(字符串拼接、模块成员访问、字符串比较);
> 剩余未做:用户类运算符重载(其余五个算子仍备置);容器方法面的 list 部分随后补齐(见下),
> map/range 方法面按需另批。

> **计划内补缺 · list 方法面补全(2026-09-21 落地,其后 review 间三度改定)**:上承上文
> 「剩余未做:容器方法面」的 list 部分,也是字符串字节序比较(0e08c75)的直接消费者。
> 新增十方法 `insert`/`remove`/`remove_at`/`clear`/`sort`/`reverse`/`find`/`contains`/
> `size`/`is_empty`;**pop 契约不动**(恰 0 参 --按位置移除另立 remove_at,不给既有方法
> 加参数)。方法面终态 = push/pop/insert/remove/remove_at/clear/sort/reverse/find/
> contains/size/is_empty/join/iter。语义钉子:①变更方法一律就地改、返 nil(push 先例,
> 变更不鼓励链式);remove 例外:返命中 Bool(miss 走返回值,与 find 返 nil/contains 返
> false 同族 --有信号通道不占错误通道,错误通道留给无通道的结构性失败如空表 pop);
> ②`sort` 就地升序,域 = 比较算子的可比较域(全数值或全字符串:双 Int 整数路径/混合升
> f64/字符串无符号字节序,皆与 `run_binary_*` 同源),**先整体域检再排序**(首元素定类、
> 首个破类元素即报 TypeMismatch「got A and B」,首元素自身不在域内只报单类型),NaN 排
> 在一切数值之前(ES2019 sortCompare 同款)保严格弱序,stable_sort 等值元素(int 1 与
> f64 1.0)保输入相对序;③`insert` 位置语义 = Python insert(`i` 之前插入、`i == size`
> 即追加、负数指「该下标元素之前」),越界 fail-fast 静态文案
> (insert index out of range,与切片同款不带键值;Python 钳制,aria 越界即报);④`remove_at(i)`
> 按位置移除并返回该元素(pop 的任意位置形,负数从尾计数、越界静态文案同款);⑤`remove(x)` 移除**全部** `value_equal` 命中
> 元素(保序,Ruby Array#delete 同款移全;只要一处用 find + remove_at 组合);⑥`find`/
> `contains` 走 `value_equal`(== 内容递归,嵌套容器按内容),find 未命中 **nil**(下标永不为
> nil;aria 有负下标,-1 是合法下标,miss 时 xs[find(x)] 会静默取末元素 --Python str.find 的
> 坑,Ruby Array#index 返 nil 同款;string 的 find 同批一并翻转,其原「-1 非合法下标」注释
> 是负下标落地前的过时前提);⑦`size`/`is_empty` 为元素数与空表谓词(全局 len 的方法形态;方法名定 `size`
> 不用 `len`,空表谓词 is_empty --empty 动词义与 clear 混淆);⑧`clear` 原地清空,区别于
> 重绑 `xs = []` 换新表。回调型方法(map/filter/sort 自定义比较器)不做 --原生回调调
> aria 闭包的 VM 重入是未来机制(run_closure 私有 + dispatch_loop 不可重入),无消费者
> 不预留。GC 走查:各方法体变更全 trivial、无 GC 分配点,receiver 在 slots[0] 跨全程
> (stable_sort 临时缓冲走 std 内存非 GC 堆)。实现形态(review 改定):receiver 解开后
> 直取 elements() 绑为 list(仅 iter 需要 ObjList* 本体),段搬移 insert/remove_at 下沉为
> Array 原语(位置插入/位置移除),insert 位置归一收口 util::resolve_position
> (resolve_index 的姊妹函数,上界放宽到 == size 即追加位),值相等三件 find/contains/
> remove 下沉为 AriaArray 原语(value_equal 在 value 层,Array<T> 保持不知 Value 为何物),
> 排序比较器收口 value_less(value 层自然序,与 value_equal/value_identical 同族自由函数),
> 方法体只余域检查与调用。

> **计划内补缺 · map 方法面(2026-09-22 落地)**:上承上文「map/range 方法面按需另批」的 map
> 部分,是 list 方法面的同族收尾。方法面 = size/is_empty/has/get/keys/values/remove/clear/iter
> 九件(iter 批 5 已有,本批补前八件),review 后再补 **pairs**(返 list、每元素是 [k, v] 二元
> list,与 for-in 每轮产出和 iter().next() 同一形状;与 keys/values 同槽位序,故同一次快照内
> `keys()[i]`/`values()[i]`/`pairs()[i]` 三元对齐)成第十件。语义四拍板:①键判定方法名 **has** 不取 contains
> (map 上 contains 有「判键还是判值」二义,JS Map 同款);②`get(key)` **单参**、未命中返 nil
> --免报错读法与 list find 未命中返 nil 同族,不设默认值参数(map 可合法存 nil,故 get 的 nil
> 与「键存在而值为 nil」不可分,要分清用 has;下标读 m[k] 未命中仍报 KeyError,兜底不靠本方法);
> ③`remove(key)` 返**命中 Bool**、miss 不报错(与 list remove 同口径:miss 走返回值,错误通道
> 留给无信号通道的结构性失败,故不做 Python dict.pop 式返被删值);④增 `keys()`/`values()`
> 两快照方法。**键域 = 表内判等 ===**(find 即 value_identical 匹配),与下标读同域:判键方法
> 一律不做 value_equal 内容相等(int 1 与 f64 1.0 是不同键、可变对象作键按身份),那是 list 的
> 域。`keys()`/`values()` 各铸新 list(与源 map 解耦,此后改源不动已产出的表),序随占用槽、
> unspecified(计划 D4,与 for-in 同);两方法同槽位序推进,故同一次快照内 `keys()[i]` 与
> `values()[i]` 同源同对(序本身不可跨调用依赖)。变更方法一律就地改:`clear` 对数归零、容量
> 保留(重绑 `m = {}` 换新表,别名仍见旧内容),`remove` 置墓碑。GC 走查:查询/变更方法全
> trivial、零 GC 分配;keys/values 的时序同 `ObjMapIterator::next` --map 在 slots[0] 于栈根,
> `new_list` 顶部 maybe_collect 时新 list 未诞生,逐元素 push 走 trivial 分配不触 GC,建成随
> 返回值写回槽发布,窗口内无 GC 点。**pairs 是本表唯一「元素本身也是新对象」的方法**,时序
> 与 keys/values 不同:循环里每铸一个内层 `[k, v]` 都过 `new_list` 顶 maybe_collect,而 receiver
> 之外无根的是外层与已铸内层,故**外层须挂 `make_guard`**(内层铸后仅 push 即入外层、push 走
> trivial 分配不触 GC,窗口内无 GC 点,无需各自挂;守卫存活至函数末,slots[0] 写回时让位的 map
> 已不再需要)。该守卫经对照实验确认承重:临时去掉守卫后,stress GC 语料 `map_pairs` 立即变红,
> 还原即绿。实现形态:receiver 解开后直取 `table()` 绑为 table(仅 iter
> 需要 `ObjMap*` 本体),方法体只余 arity 检查与对 `AriaHashTable` 既有原语的调用
> (find/erase/clear/size/empty/begin 全为现成面),零新增底座 -- 与 list 那批「段搬移下沉 Array
> 原语」不同,本批无需下沉。

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
> bootstrap_string_class + StringBuiltins 方法表)+ ListBuiltins 增 join_fn。GC 模式:单输出方法 receiver 在
> slots[0] 覆写前经栈根;split 读 receiver 全程靠槽 0 栈根(receiver 不被覆写),新 list 挂 make_guard 跨段串
> 铸造的 GC 点保命,循环结束才写回槽 0 发布。测试 33 新
>(test_objstring 8 + ObjStringIterator 5 + Compiler.String* 14 + 语料 5:string_methods/string_subscript_iter/
> string_print_format golden/runtime_string_split_empty/runtime_string_subscript_assign 负 .err);存量翻转一
>(ObjectProtocolDefaults 基类默认钉子 string 换 Module)。string 的 + 拼接(op_add)不在批 6,仍基类默认报错。

> **落地状态(2026-09-19,批 7)**:批 7 已全部落地(工作区待 review,双配置 994/994 绿)。走查五拍板(2026-09-19,
> 全按建议):①空区间(low>high 含上界,或 low==high 不含上界)迭代零轮(Python/Rust 同款,端点是运行期值不设预判义务);
> ②非整数端点 TypeMismatch("range bounds must be integers",静态文案不插端点值;review 改定,原双值插值方案作废);
> ③RangeFlags 位义 0x00 含上界/0x01 不含上界(常量 `kRangeFlagExclusive` 收口 code.hpp X 表后,编译发射与 VM
> 解码同源);④v1 方法面仅 iter(len/下标/contains 不做,基类默认报错;解构 rest 切片若借 range+下标承载再议);
> ⑤TagValue 配置下算术回绕推满上界的理论边不设防(整数域 i48 规格兜底,与 map 迭代中变更同级不承诺)。
> 落地面:ObjRange 纯值壳定长(low/high/is_exclusive,内容哈希构造期烘焙、equals 按内容三字段全等、debug_repr
> `0..10`/`0...10` 与源码拼写一致、trace 空体、store_field/下标/op_* 走基类默认)+ ObjRangeIterator(第五对,
> **唯一无源对象者**:构造期拷三标量自足,不持指针、trace 空体;iter_fn 覆写 slots[0] 后源 range 可回收,标量
> 自足不受影响)+ MAKE_RANGE 执行体(端点 peek 在栈跨 new_range,验整数铸完 drop+push,窗口内无 GC 点;无 u16
> 计数、零上限检查——惰性两端点无物化)+ CodeGen visitRangeExprNode 翻转(端点左→右发射 + flags 字节)+
> Range bootstrap(寄存器 RangeClass 格 + bootstrap_range_class + RangeBuiltins 单 iter 方法表)+ parser 非结合
> 确认(rhs 调 term 不调 range,结构性防住 `a..b..c`)。测试 27 新(test_objrange 15 含哈希确定性/空区间/双迭代器
> 独立/stress + Compiler.Range* 8 + 语料 4:range_forin/range_print_format golden/runtime_range_bounds_type_
> mismatch 负 .err/compile_range_non_associative 负);存量翻转一(Interpret.StringNotImplementedIsCompileError
> 钉子样本 `print 1..2` 退役换解构赋值,机制仍在)。

> **后继演进(批 7 收官后 range 四批扩展,已拍板;本段记批 1)**:三方向跨语言对照呈报后拍板——①倒序走
> **端点自动推断**(low>high 即倒序,`10..1` 产出 10→1、不含上界 `10...1` 递减到 high+1 产出 10→2;**翻转批 7
> 空区间拍板**:空区间只剩 low==high 且不含上界;ObjRange 本体零改动,方向住迭代器构造期,内容哈希/equals 不变,
> 10..1 != 1..10;主流语言几乎全拒绝端点推断方向,空区间语义为其代价,直觉优先);端点命名改 **from/to**
>(批 1 review:倒序后 low/high 的「大小序」假设名不副实,from/to 零方向假设——start/end 的 end 有 C++
> one-past 条件反射、begin/end 与容器 begin()/end() 撞语境,均否);②切片越界 fail-fast 不采
> Python 钳制;③**单下标负数一并支持**(从尾计数 idx+len,推翻「单下标维持报错」建议——要做就做全套,list/string
> 下标读写对称);④倒序 range 作下标 v1 报错,倒序切片后议。批次:批 1 倒序 range → 批 2 负下标全套
>(list/string) → 批 3 无上界开区间(文法 `term?` + flags unbounded 位 + ObjRange has_high_) → 批 4 切片
>(list 下标 Range 分支,消费批 2+3)。
>
> **批 1(倒序)已落库 f078221(2026-09-19,双配置 997/997 绿)**:ObjRangeIterator 加 forward_(构造期 from<=to
> 定向)+ has_next 方向比较 + next 按方向推进;端点改名 low/high→from/to(字段/访问器/MAKE_RANGE 局部/工厂/
> instruction-set 规格用语全链;start/end 的 end 有 C++ one-past 条件反射、begin/end 与容器迭代器撞语境,均否);
> 测试翻转 2(EmptyRangeZeroRounds→EmptyExclusiveZeroRounds 只留 5...5、语料 range_forin 空区间段)+ 新增 3
>(对象级 ReversedInclusive/ReversedExclusive + 端到端 ReversedSums);grammar/instruction-set §6.3/进度行同步。
>
> **批 2(负下标)已落地(工作区待 review)**:从尾计数 idx+len 一次到位(list 读/写 + string 读,string 写恒
> TypeMismatch 不变;map 任意键不涉)——归一化三处就地字面重复(行数过小不抽),raw=i64min 负支和恒不下溢
> (len<=i64max);越界 fail-fast 报**原始键值**(list index -3 out of range)。测试:对象级翻转 2(越界钉子 -1→
> -len-1)+ 新增 3(LoadIndexNegativeReadsFromTail/StoreIndexNegativeWritesFromTail/ObjString 负下标)+ 端到端
> 4(读写对称/复合赋值同归一化/越界文案/string 负下标);语料翻转 1(runtime_list_negative_index 改 -len-1 形)+
> 正向段(list_subscript_read_write 负下标读写)。
>
> **批 3(无上界开区间)已落库 0e67f21**:文法 `term?` + MAKE_RANGE flags unbounded 位 + ObjRange `to_` 改 Opt<i64>
> (`5..` ≡ `5...` ctor 归一)+ 迭代器无上界 has_next 恒真 + debug_repr `3..`;内容哈希/equals 含 has_high_。
>
> **批 4(切片)已落库 f0c3854(双配置 1021/1021 绿)**:list 的 `load_index` 加 Range 键分支 → `slice()`
> 铸新 list 段拷(MAKE_LIST 同构 GC 纪律:receiver/key 皆在栈为根、段拷 trivial 不触 GC、新 list 白色由
> run_load_index 写回原槽根化);越界 fail-fast(静态文案 `slice index out of range`);`store_index` 对 Range 键
> 不特殊对待,落整数键检查统一文案(切片只读);`xs[a..b] = ...` 不做。
>
> **切片端点表示(2026-09-19 拍板改定)**:`resolve_slice_bounds` 返回 `Opt<Pair<usize, usize>>` = **归一化端点对
> (from, to)**(两端点各自经 `util::resolve_index` 从尾计数 + 越界判定,无上界取末元素),**含否上界不折算进返回
> 值**——折算挪到消费端(`count = is_exclusive() ? to - from : to - from + 1`,两端相等即空切片 count 0),
> 方向由端点对大小关系自带(正序 from<=to、逆序 from>to)。取代早稿的 `Pair<i64,i64>` 有符号闭区间(其
> `end = to - 1` 是推导值、to_index==0 时探到 -1,是 usize 装不下的根因;改 u64 会让 `xs[-1...0]` 这类经
> `Array::ensure_capacity` 回绕成死循环,实测确认且现有测试全绿掩盖)。拍板理由:①两端点都是实元素位置、
> 无推导值 ⇒ 负值从表示层消失、无哨兵无回绕;②v2 逆序切片零表示层包袱(只删 resolve 里的 `*from > *to`
> 拒绝行 + 消费端按方向反向写入),与批 1「range 方向由端点推断」同一判据;③顺带修掉 review 版真缺陷:
> 早稿按**原始端点**判倒序,`xs[1..-1]`/`xs[0..-1]`/`xs[1..-2]`(正起点配负终点)被误报 `slice index out of
> range`,改按归一化端点判后正常(实测补钉);同一改动下 `xs[-1..0]`/`xs[-2...0]`(原始端点递增、归一化后倒序)
> 从静默产空 list 收紧为与 `xs[2..0]` 同类报错(v1 期判定;批 4 续打开倒序后二者同属倒序切片)。测试:对象级
> SliceNegativeEndpoints 增混合正负端点 2、SliceReversedRangeFails 增归一化倒序 1;编译级 ListSliceReads/
> ListSliceFails 各增;语料 list_slice.aria 增混合正负段 + 新增 `runtime_list_slice_reversed_negative_start` 负例对
> (该负例对随批 4 续删除,见下段)。
>
> **批 4 续(倒序切片)已落地**:倒序 range 作下标不再拒绝,产出**倒序段**--`resolve_slice_bounds` 删掉
> `*from > *to` 拒绝行(方向不进返回值,端点对大小关系即方向,与批 1「range 方向由端点推断」同一判据,故
> `xs[3..1]` 与 `for (i in 3..1)` 走同一方向);消费端 `slice` 的 count 改为双向 `util::abs_diff(from, to) +
> (exclusive ? 0 : 1)`(不含上界少走迭代序末元素,两端相等即空切片,两方向同一个式子),拷贝按方向分流:
> 正序走 `copy_from` 整段一次拷,倒序走 `Array::copy_reversed_from` 反转追加(源段以升序 Span 给出,自低端的
> `to` 起、不含上界让开一位即 `to + exclusive`;两个 arm 皆 trivial 分配不触 GC,GC 走查不变;空段两路皆零
> 操作,故消费端不必先行判空)。配套抽出两件工具:`util::abs_diff`(取大减小的两下标距离,命名对齐 C++26
> `std::abs_diff`)与 `Array::copy_reversed_from`(倒序版 `copy_from`,逆序遍历经 `std::views::reverse`,与仓库既
> 有逆序遍历同一形态)。失败面收窄为空表与端点越界两类,`slice index out of range` 文案覆盖完整(不再兼表倒序)。
> 测试翻转:对象级 `SliceReversedRangeFails` → `SliceReversedRangeYieldsReversedOrder`(6 例:闭/半开倒序、全表
> 倒序、负端点倒序、归一化后倒序、空切片);编译级 `ListSliceFails` 的两条倒序断言迁入 `ListSliceReads`(for
> 迭代倒序段序位 + 端点取值 + 长度);`Array::copy_reversed_from` 4 例(反转序/append/跨扩容/空 src);语料两个
> 倒序负例对删除、断言并入 `list_slice.aria` 正向段(语料用例数 1021 → 1019)。

> **批 8(解构)已落地**:Parser/AST/Visitor 早已就位,本批把 CodeGen 的四处占位翻为真实发射,
> 零新指令、零新错误码。① 组织:P6 定稿——`bind_pattern(node, mode)` 只置模式后 `node.accept`,
> 发射住三个 `visit*PatternNode`(旧 `bind_pattern` 的 dyn_cast 链删除);新增
> `PatternBindMode{Fill,Store}` 成员(对标 `lvalue_mode_`;模式对整个模式子树恒定,递归经 accept
> 无参可传)。② Fill(var 声明 / for-in 目标)走值填槽:identifier 收 `bind_stack_value`(局部登记
> 即初始化、顶层全局 `DEF_GLOBAL` 弹值),listPattern 按**访问数**(非 `_` 元素位 + rest 位)三分
> ——0 次直弹源值、1 次源值即消耗品(取出元素恰落下一局部槽位)、>=2 次先把源值填成隐藏局部再逐
> 位置复取;隐藏局部名 `<destructure_N>` 带槽号(N 为登记时 `locals_.size()`),因同作用域不许重名
> (`is_defined_in_scope`)而同作用域两条解构语句必占不同槽。③ Store(解构赋值)源值恒驻栈顶作临时
> 值,每次访问前 `DUP`、identifier 目标 resolve 后 `STORE_*` 再 `POP`(peek-store),收尾弹本层
> 源值——净消耗栈顶一值,故调用点先 `DUP` 一份右值作表达式的值(**解构赋值求值 = 右值**)。④ `_`
> 位置经 `is_wildcard_pattern` 过滤,**不产生下标访问**(故该位置越界/缺键都不报;反方案「取值后
> 丢弃」对 map 源会误报缺键被否)。⑤ rest:压「元素数」下标 + `MAKE_RANGE`(无上界位)作键取后缀,
> 绑名走 rest 节点自身 visit(AST 里 rest 位是与位置位同为 `IdentifierPatternNode` 的模式节点,非字符串);**空尾语义**——
> 无上界形态并入 `resolve_slice_bounds`(此前不做是因无消费者;解构 rest 即消费者):该函数返
> `Opt<SliceSegment>`(升序源段起点 + 元素数 + 是否反向写入;长度与方向的折算全进解析口),
> 有上界支照旧(两端点各从尾计数、须落在实元素位置),无上界支给后缀——起点从尾计数后**允许 == size**
> 得空段(`[][0..]`、`[7][1..]` 皆空 list),越过长度仍越界;`ObjList::slice` 退化为「解析 -> 段拷」
> 单流,util 侧 resolve_index 与其钉子零改动,翻转对象级 `SliceOnEmptyListFails`
> 的无上界例与语料负例 `runtime_list_slice_empty`(改钉有上界形态)。⑥ 逐位置发射(含 `_` 跳过、递归绑定与 rest 后缀)
> 三处同构,收成成员模板 `emit_list_pattern_accesses`,备源值方式作参数。⑦ 收口:`not_impl` 助手与
> `NotImplemented` 码随唯一消费者(rest 占位)消失而删,43 个 visit 全为真实发射;`Interpret.
> StringNotImplementedIsCompileError` 随之退役(该测试此前已被批 7 换过一次样本,机制消失故整条删)。
> 测试:发射形态 1 条 + 端到端 2 条(原 rest 占位钉子翻为行为测试)+ 对象级 1 条(切片后缀允许空段,`SliceOnEmptyListFails` 的无上界例翻入它)+ 新语料 1 正 1 负(`destructure_rest.aria` / `runtime_destructure_rest_on_string`,后者已随 §4.9 的 string 切片落地删除),`list_slice.aria` 与 `runtime_list_slice_empty.aria` 各一处既有点翻转扩展;删 `Interpret.StringNotImplementedIsCompileError`(机制消失)。双配置 1080/1080 绿。

### 4.4 批 9 基线:方法派发成本实测(2026-09-20)

批 9 的门禁是「bench 驱动」,基准入口 `bench/vm_bench.cpp`(目标 `vm_bench`,非 gtest)。**每次循环体的计数统一 204,800 次**,best-of-15 取最小(同 lexer_bench 纪律;跨构建抖动 ±5-10% 不入结论,见 lexer-notes §1)。

**手法:同一二进制内两形态对照**。每对场景是同一份工作写成两种源形态:

- **协议形态**:源码写 `recv.name(args)`,现编译为 `LOAD_FIELD` + `CALL`(每次调用物化一个 ObjBoundMethod);
- **预绑定形态**:先把方法值读到局部(`var nx = it.next`)再调 `nx()`--循环体内零 `LOAD_FIELD`、零 ObjBoundMethod,即不绑定派发能达到的**下界**。预绑定不是正常写法,只作对照。

两形态算同一个结果(`BENCH_CHECK` 断言相等),**差值 = 每次调用花在 `LOAD_FIELD` + 绑定 + 多一次 dispatch 上的成本,即不绑定派发可回收的上界**。反汇编逐指令核对过两侧的循环体:协议形态 12 条/迭代(含 2 条 `LOAD_FIELD`)、预绑定形态 9 条/迭代(零 `LOAD_FIELD`),结构无其他差异。

| 场景 | 协议形态 ns/次 | 预绑定 ns/次 | 差值 ns/次 | 差值占比 |
| :--- | ---: | ---: | ---: | ---: |
| `forin_list`(list 迭代协议) | 81.0 | 22.4 | 58.6 | 72.4% |
| `forin_range`(range 迭代协议) | 78.3 | 21.1 | 57.2 | 73.0% |
| `starts_with`(单方法调用) | 71.7 | 41.3 | 30.4 | 42.5% |
| `instance_method`(用户类实例方法,无对照) | 54.7 | - | - | - |
| `plain_call`(普通函数调用,无对照) | 42.8 | - | - | - |
| `forin_string`(逐码点产出 1-char string,无对照) | 86.8 | - | - | - |
| `forin_map`(逐 pair 产出 `[k, v]` list,无对照) | 145.1 | - | - | - |

上表为一次完整运行(Release / `-O2 -DNDEBUG` / 无 LTO)。重复运行的抖动:协议/预绑定三对与 `forin_string`/`forin_map` 在 ±1% 内,`instance_method`/`plain_call` 在 ±5% 内(基线行偶有单次偏离;行值即 best-of-15 最小,同二进制内可比)。

**读数**:

- 一次「读方法值」实测约 **30 ns**:`starts_with` 每迭代 1 次读取得 30.4 ns,list/range 迭代协议每迭代 2 次读取得 57-59 ns,两条独立场景互相印证。差值含分配本身与它触发的 GC,不止一次类表查表。
- 内置类型无 fields 缓存,故 for-in 每迭代把**七成时间**花在这条路径上;不绑定派发去掉的是分配与一次 dispatch(类表查表仍在),落地后应落在「预绑定」与「协议」之间--重跑本基准的 `forin_list`/`forin_range` 两行即批 9 的验收读数。
- 实例方法行 54.7 ns/次 vs 普通调用 42.8 ns/次:fields 缓存命中路径只比裸调用贵约 12 ns,印证**实例路径本就已摊薄**(一个实例一个方法名一生只物化一次),批 9 不必改实例路径。

复现:

```sh
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release -DARIA_ENABLE_LTO=OFF -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
cmake --build build/rel --target vm_bench -j
./build/rel/bench/vm_bench
```

### 4.5 批 9 落地状态(2026-09-20)

> 本节记录当时的落地形态(单条 `INVOKE_METHOD` 融合派发)。其中「一条指令」这一点已由 §4.7 两段化取代(不绑定解析、零物化不变);本节其余内容(协议缝、清理、测试)仍然有效。

**已落地**。三处改动:

- **编译侧**:`visitCallNode` 对 `recv.name(args)`(含 `this.name(args)`)发 `INVOKE_METHOD name argc`,接收者先压调用区槽 0;for-in 三站点(`iter`/`has_next`/`next`)经 `emit_method_call0` 同改。`super.m(args)`(`SuperExprNode`)与下标调用 `arr[i](args)` 不融合,保持原两步/普通 `CALL`。方法值的**读取**(`obj.m`)仍走 `LOAD_FIELD`(每次访问现场绑定;其后的缓存取消与身份语义变化见 §4.6)。
- **VM**:`run_invoke_method`(替换原 `not_implemented` case):非对象接收者守卫文案同 `run_load_field`;经协议解析出被调值后交 `call_value` 统一分发,**调用区不进**(栈形只剩 `[recv, a1..aN]` 一种)。故 `prepare_call_args`(缺省垫充/varargs 打包)、`TryRecord.stack_depth` 回退、`unwind` 全不受影响。
- **对象侧协议缝**:`Object::resolve_invoke(vm, name) -> Opt<Value>` -- 只回答「该被调的值」,调用区槽 0 由指令保持 receiver 原样(方法经 `call_bound_method` 自覆写、内置原生正需要槽 0 = receiver、闭包不读槽 0,三者皆无需指令干预)。基类默认体 = `load_field`(类/模块等未 override 者);**实例与 List/Map/String/Range/Iterator 各自 override** 成不绑定形态(前者 fields 命中优先 + 类链取原值,后者查自身 bootstrap 类表),零 `ObjBoundMethod` 物化。协议缝形态与 `load_field/store_field` 同族(错误自 fail、`nullopt ⟺ 已 fail`、文案随宿主就地烘焙),未 override 的类型自动落到默认体。

顺带清理:`not_implemented`(运行期「opcode 未实现」fatal 助手)与其独占错误码 `OpcodeNotImplemented` 随唯一消费者消失而删除--指令的 VM case 与 `code.hpp` 表行同批落地,不存在「表里有、VM 没实现」的持久态,兜底由 `dispatch_loop` 的 `UNREACHABLE` 承担。

**前后对照**(`bench/vm_bench.cpp`,Release/`-O2`/无 LTO,每行 1,638,400 次循环体,改动前后各 3 次运行,构建内抖动 ±1%):

| 场景 | 改前 ns/次 | 改后 ns/次 | 分配/次 改前 -> 改后 |
| :--- | ---: | ---: | ---: |
| `forin_list` 协议 | 84.1-84.3 | 31.2-31.3 | 2.009 -> 0.000 |
| `forin_list` 预绑定(控制组) | 21.4-22.4 | 19.6-19.7 | 0.009 -> 0.001 |
| `forin_range` 协议 | 79.8-80.2 | 31.9-32.2 | 2.001 -> 0.000 |
| `starts_with` 协议(单方法调用) | 71.4-71.6 | 45.4-45.7 | 1.000 -> 0.000 |
| `starts_with` 预绑定 | 41.8-42.2 | 39.7-40.1 | 0.000 -> 0.000 |
| `forin_string` | 86.3-87.3 | 38.4-38.7 | 2.006 -> 0.000 |
| `forin_map` | 146.6-147.0 | 92.1-92.7 | 3.006 -> 1.000(只剩每迭代的 `[k,v]` pair) |
| `instance_method`(基线) | 53.7-54.0 | 53.6-54.5 | 0.000 -> 0.000 |
| `plain_call`(基线) | 42.0-42.2 | 41.6-42.0 | 0.000 -> 0.000 |
| 噪声地板(同程序两测) | ±0.1 ns/次 | ±0.2 ns/次 | -- |

读数口径(两条,互补):

- **确定性分配列**是最硬的证据:三次运行同值,不受计时抖动影响。内置侧每迭代 2 次取方法从各铸一个 ObjBoundMethod 变为零分配;`forin_map` 残留的 1.000 是迭代协议设计使然(每迭代产出一个 `[k,v]` 二元 list),不是派发成本。
- **同二进制内「协议 - 预绑定」差值**(每次取方法的开销,免疫跨构建代码布局抖动):list 62.7 -> 11.6 ns/次(每次调用约 31 -> 5.8 ns)、range 58.3 -> 11.8、单方法调用 29.7 -> 5.6--残留的约 6 ns/次 即类表查表 + 一次 dispatch 的净成本,正是融合派发**不能**消掉的部分。
- **跨构建绝对值有 ~10% 抖动**:三条基线行(代码路径未改)在两次对照间摆动 ~10%(`plain_call` 42.1 -> 41.8、`instance_method` 53.8 -> 54.0、`forin_list` 预绑定 21.4-22.4 -> 19.6-19.7),故跨构建只报量级(协议行 2.5-2.7x 提速),判定以分配列与同二进制差值为准。

**语义等价性**:`INVOKE_METHOD` 是 `LOAD_FIELD` + `CALL` 的融合而非「只在类表查方法」--字段优先遮蔽方法、字段里的可调用值原值直调、非可调用成员照旧 `CallNonCallable`、成员 miss 文案随宿主,全部靠复用同一条成员解析路径保证。已知偏差一处:两步形态在求实参前取好方法值,融合派发把解析推到执行期(晚于实参求值),仅当实参表达式反过来改写该接收者/类的同名成员时可观察(病态写法),以「成员解析在调用点发生」为准(见 `bytecode-instruction-set.md` §5.6)。

**测试**:`test_codegen` for-in 反汇编断言翻转为 `INVOKE_METHOD` + 新增融合发射/边界三例(`MethodCallEmitsInvoke`/`InvokeEmissionBoundaries`/`ThisMethodCallEmitsInvoke`);`test_ariavm` 新增三条白盒(实例绑定路径、内置侧两侧分配差 = 1、字段持可调用值时两侧分配差 = 0);`test_gc` 新增 `AllocationCountMonotonic`;语料新增一正(六种宿主调用形态)两负(字段非可调用 `CallNonCallable`、成员 miss `UndefinedProperty`,各配 `.err`)。主构建 ctest 1053/1053 绿。

### 4.6 bound 缓存取消(2026-09-20,反转 M5 决策 4)

**决定**:实例的 bound-method 缓存整体删除;实例 `resolve_invoke` 改成与内置类型同一条不绑定规则。

**动机**:缓存让「类/父类上改写方法」对**既有实例**陈旧、对**新建实例**新鲜 -- 同一条 `X.who()` 的结果取决于该实例此前有没有取过 `who`。实测(`C.who = f` 后)老实例返旧值、新实例返新值,属最难解释的一类语义,monkey patch 只能算半可用。取消后成员解析每次按当前类链进行,读与调用同一份可见性。

**形态**:`ObjInstance::load_field`(读路径)保留现场绑定但**不写回 fields**;`ObjInstance::resolve_invoke`(调用路径)新增 override -- fields 命中优先,否则沿类链取**原值**(方法戳闭包不绑定),交 VM 以 receiver 占槽 0 直调(方法体从槽 0 读 `this`)。由此全部接收者(实例 + 5 个内置类型)共用同一调用规则,**绑定只活在读路径**。`ObjBoundMethod` 因此补 `equals`(receiver 同一 && method 同一):`obj.m == obj.m` 为真、`obj.m === obj.m` 为假(与语言既有的 `==`/`===` 二分、以及 Python bound method 的 `==`/`is` 分工一致)。

**实测**(`bench/vm_bench.cpp`,同前口径,改前=缓存 + 基类默认体 / 改后=本决定,各 3 次):

| 场景 | 改前 ns/次 | 改后 ns/次 | 分配/次 改前 -> 改后 |
| :--- | ---: | ---: | ---: |
| `instance_call`(实例方法调用) | 53.2-53.9 | 55.6-55.8 | 0.000 -> 0.000 |
| `instance_read`(实例方法值读取 `f = obj.m`) | 41.8-42.0 | 69.9-71.0 | 0.000 -> **1.000** |
| `forin_list`/`starts_with`/`plain_call` 等 | -- | 不变(±1%) | -- |

读数:调用路径(绝对主路径)零分配不变、耗时 +2 ns 量级(约 +4%,fields_ 未命中 + 一次类链查表取代了「缓存命中 + bound 间接」);读路径是这次的真代价 -- 每次访问铸一个 bound、+28 ns,与 JS/Python 同款(方法值是一等值,这一步省不掉)。换取的是语义一致 + 机制减法:fields_ 回归纯字段,「三铁则」随之取消(见 `class-implementation-pitfalls.md` 坑 #1 的反转记录)。

**波及面**:`ObjInstance` 契约注释、`ObjBoundMethod`(equals)、`class-implementation-pitfalls.md`、`m5-class-implementation-plan.md`/`vm-design.md` 的决策索引、语料(`class_bound_methods`/`invoke_method_forms` 的 `===` 翻成 `!==` + `==`,新增 `class_method_patch.aria` 钉 monkey patch)。

### 4.7 两段式派发(2026-09-21,取代 §4.5 的单条融合)

**决定**:`recv.name(args)` 从单条 `INVOKE_METHOD name argc` 改成两段 `PREPARE_METHOD name` + `CALL_METHOD argc`——解析放回**实参求值之前**。`INVOKE_METHOD` 与其独占的 `OpFormat::Invoke` 一并删除(指令表净 +1 条:`PREPARE_METHOD`/`CALL_METHOD` 两条换 `INVOKE_METHOD` 一条)。

**动机(语义)**:`obj.foo` 是接收者求值后紧接着的一步,应当先于实参求值完成,Python/Lua/JS 皆如此(它们的成员解析可能跑用户代码——描述符/getter/`__index` 元方法,故规范把「取」定死在实参之前)。单条融合把解析推到执行期,可观察两处:① 实参反过来改写接收者同名成员时,本次调用用的是**改写后**的值;② 解析失败时实参**已经跑过**(副作用已发生)。§4.6 收尾记的「已知偏差一处」即 ①。两处虽属病态/边角写法,但次序语义是语言面的确定性,不宜留特例——尤其它同时是「解析可跑用户代码」这一未来特性的**前提**(届时解析要在 `PREPARE_METHOD` 内跑,还需另做同步嵌套调用机制;该边界已写进指令集 §5.6)。

**形态**:`<recv>` + `PREPARE_METHOD name`(解析、结果压栈:[recv] -> [recv, target])+ `<args>` + `CALL_METHOD argc`(纯调用:实参整体下移一格补掉 target 占的那格得 [recv, a1..aN],槽 0 = receiver = this,交 `call_value` 分发)。接收者先于实参求值不变;调用区与两步形态逐位一致,故进帧整形(缺省垫充/varargs 打包)与 unwind 零改动;不绑定解析与零物化照旧。名字索引沿用 `ConstU16`(与 `LOAD_FIELD` 等常量索引同宽)——短形 u8 + 长变体曾被引入,但那只为让两段式与融合编码**逐字节等长**(便于同二进制字节改写对照),不是设计需要,已去。

**实测**:

| 口径 | 数字 |
| :--- | :--- |
| 同二进制字节改写对照(两段式 <-> 融合,同一份字节码) | `forin_list` +1.8~2.5、`forin_range` +1.8~2.1、`starts_with` +0.3~1.4、`instance_call` +1.5~1.8 ns/次迭代;即**每次调用约 1 ns**(argc = 0 时下移为空转) |
| 保留的批 9 收益 | `forin_list` 84.1 -> 33.5~33.7(融合那代 31.1~31.6),即保留 ~96%,分配列不变(0.000/次) |
| 端到端(Release 解释器,四个真实负载,随机序 best-of-12,两轮) | 集合/迭代密集型 +5.8~5.9%、字符串+map 型 +4.9~5.0%、类方法密集型 +0~3%、零派发对照 +0~0.7% |

**备选形态(均已实现并实测,都不如现状)**:

| 形态 | 做法 | 实测 |
| :--- | :--- | :--- |
| target 放调用区**之下**(`[target, recv, a1..aN]`) | 调用侧零搬移;收尾由帧位承担——`CallFrame` 加一位「下方有 target 槽」,`RETURN` 把返回值写进那格并让栈顶落到帧基址;原生调用在 `CALL_METHOD` 里当场收尾 | 比现状慢 0.8~2 ns/次迭代;且改动落在 `RETURN` 上(纯函数调用也走那条路),端到端零派发对照行 +2.7~4.5% |
| 上者 + 调用点补一条 `POP_UNDER` | 去掉帧位,收尾改由调用点后随的栈原语统一做(VM 零新状态) | 比现状慢 1.1~1.6 ns/次迭代——每次调用多一整条指令的 dispatch,是三者里最贵的 |

**测量纪律(这轮踩到的)**:跨构建比较被「代码布局手气」污染到 ±2~4%(同一份字节码在两个构建里可差 1.4~2.1 ns/次迭代;对照组 `plain_call` 自身在 37.9~43.9 间摆动),故**变体之间**的比较只用同二进制字节改写对照,跨构建只用于确认量级与「无意外回归」;端到端计时按每轮随机序 + 零派发对照负载扣除构建级偏移。

**测试**:`test_codegen` 四条发射形态断言改判两段式(`MethodCallEmitsPrepareCall`/`PrepareCallEmissionBoundaries`/`ThisMethodCallEmitsPrepareCall` + for-in 反汇编);`test_ariavm` 三条白盒改按编译器的发射序手写字节码(接收者 -> 解析 -> 实参 -> 调用),分配差断言不变;`test_disassembler` opcode 计数钉 63 -> 64;语料 `invoke_method_forms.aria` 的次序钉子由「解析在调用点」翻转为「解析先于实参」,并补一条「解析失败时实参不跑」的钉子(`side` 计数)。

### 4.8 string 方法面补齐(2026-09-22)

**决定**:补 `chars()`、`size()`、`is_empty()`、`contains(sub)` 四方法,string 方法面 11 -> 15。`chars()` 是 D5 欠账的兑现 -- 码点域此前只有 `codepoint_at(i)`(给码点值),没有「第 i 个字符」与「码点总数」的口子(数码点得手写 `for` 计数),`chars()` 一次补齐:与迭代同为逐码点切分、产出 1-char string,故 `len(chars())` 即码点数、`chars().join("")` 回原文。`size()`/`is_empty()`/`contains(sub)` 纯为与 list/map 方法面对齐(`len(s)`/`len(s)==0`/`find(sub)!=nil` 已可替代);`size()` **是字节域**(与 `len`/`s[i]` 同域),码点数须走 `len(chars())`。`contains` 按字节子串判(非字符集合)、空串参数恒真、非 string 参数 TypeMismatch,与 `find` 同域。

**口径边界**:负数索引与 range 只属下标访问(`s[i]`、list 切片),内建方法一律不收负值/区间 -- 故 `substring`/`find`/`codepoint_at` 的现有口径不变(不加负端点、不加 `from` 起参、`codepoint_at` 仍拒负数)。

**GC 时序**:`chars()` 是「单输出新容器 + 逐个铸造元素」方法(receiver 留 `slots[0]` 由栈标根、新 list 挂 `make_guard` 跨逐串铸造的 GC 点、循环结束才发布),与 `pairs_fn`/`split` 同形。非法字节序列产出替换码点串(U+FFFD,只吞一个坏字节),口径与 `ObjStringIterator::next` 一致。

**测试**:`Compiler.StringSizeIsEmpty`/`StringContainsSubstring`/`StringCharsSplitsCodepoints` 三条(含 `chars()` 循环调用 + stress GC 的 guard 承重用例、非法字节 -> 替换码点钉子)+ 语料 `13_strings/string_methods.aria` 补六条断言。

### 4.9 string 切片与空白切分(2026-09-22)

**决定**:①`ObjString::load_index` 收 Range 键走切片,段解析直接复用 `ObjRange.cpp` 的 `resolve_slice_bounds`(本就 size 泛化),故与 list 切片逐格同口径:含/不含上界、端点从尾计数、无上界 `i..` 取到末尾(`size..` 得空段)、越界与空串 `nullopt` -> `IndexOutOfBounds` 且文案与 list 同串(`slice index out of range`)。**域是字节**(与 `s[i]`/`len`/`size` 同域),故倒序段是字节倒排:多字节输入下产出非合法 UTF-8,与 `s[i]` 能取到续接字节同属字节域契约(按码点反转不在切片口径内)。②`split` 增 0 参形态:按 ASCII 空白**连续段**切开并丢空段(Python `str.split` 同款),全空白与空串返 `[]`;空白集与 `trim` 同源(`is_ascii_space`),故只在 ASCII 域判定。1 参形态语义不变(保留空段)。两形态各由一个匿名命名空间自由函数承载(`split_by_sep`/`split_on_space`),各自建并返回新 list(白色对象跨逐段铸造的 GC 点,由函数内守卫保命),`split_fn` 只余 arity/类型检查与「helper 返回 -> 写回槽 0」两句(其间无 GC 点)。

**连带翻转**:string 的 rest 解构(`var [c, ...r] = "abc"`)此前按「string 无 Range 下标」钉成 TypeMismatch,现经 `MAKE_RANGE i.. + LOAD_INDEX` 同一机制走后缀切片成立;两个负向语料(`runtime_string_range_index`、`runtime_destructure_rest_on_string`)删除,新增正向语料 `13_strings/string_slice.aria`,`Compiler.DestructureRestOnStringFails` 翻为 `DestructureRestOnStringSlicesSuffix`。

**口径边界**(承 §4.8):负数索引与 range 只属下标访问,故本批只动 `load_index`;方法面不收负值/区间,`substring`/`find`/`codepoint_at` 口径照旧。

**测试**:`ObjString.Slice*` 四条(含/不含上界、无上界与空段、负端点、倒序字节倒排含多字节例、越界与空串失败)+ `Compiler.StringRangeSlice`/`StringSplitOnWhitespace` 两条端到端 + 语料 `string_slice.aria`(13 条断言)。

## 5. 参照

- `Object.hpp` 备置协议缝注释(成员/下标/运算符/可调用四组)——本计划的架构基准。
- `ObjInstance::load_field`(绑定路径)与 `ObjClass::load_field`(链读穿透、类侧不绑定)——共享绑定 helper 的两处消费点。
- `ObjBoundMethod`(method_/receiver_ 均 Value 泛化,原生方法调用约定「slots[0] = receiver」)——运行时侧已就绪,本计划只补生产侧。
- `bytecode-instruction-set.md` §4.14/§6.4(五条预置指令的栈形与操作数)、`compound-assignment-lowering.md` §4.3(下标四模式)。
- Wren 0.4(本地 `/Users/icelake/src/wren`):每值一类 + 类表方法派发(`wren_value.c` value 为 class 成员)、迭代即方法(Wren 无内建迭代器对象,list 迭代走下标,aria 取「协议方法 + 迭代器对象」路,与 Python/JS 同形)。