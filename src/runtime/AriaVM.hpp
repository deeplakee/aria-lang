#ifndef ARIA_VM_HPP
#define ARIA_VM_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
// raise 模板头内内联装箱需 ObjException 完整类型(其依赖已经 GC.hpp 传递拉入)。
#include "object/ObjException.hpp"
#include "runtime/Movement.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    class ObjBoundMethod;
    class ObjClass;
    class ObjClosure;
    class ObjFunction;
    class ObjInstance;
    class ObjModule;
    class ObjNativeFn;

    // 前置声明(bytecode/code.hpp 的 X 表生成物):run_binary_numeric<Op> 模板形参用,
    // 免头文件拖入 bytecode 树(定义处 AriaVM.cpp 已含)。
    enum class OpCode : u8;

    // fail 的返回哨兵:按调用点所在函数的返回类型隐式转换为该类型的失败拼写 --
    // bool -> false(NativeFn/call_value 族)、指针 -> nullptr、Opt<T> -> nullopt。
    // 失败出口惯用法一行 `return vm.fail(...);`;契约不变:nullopt/false/nullptr ⟺ 已 fail,
    // 错误载荷已在挂起错误寄存器。只 raise 不借信号的语句式站点直接用 void 的 raise(...)。
    struct FailSignal {
        operator bool() const noexcept { return false; }
        template<typename T>
        operator T*() const noexcept {
            return nullptr;
        }
        template<typename T>
        operator Opt<T>() const noexcept {
            return std::nullopt;
        }
    };

    // interpret 结果：编译并执行的结局类别（对齐 clox InterpretResult）。
    // interpret / interpret_from_path 内部已把错误渲染到 stderr，故只回类别、不回 Error --
    // turnkey 场景调用方只要成败类别；低层 run(ObjFunction*) / run(SourceFile&, ObjModule&)
    // 仍返 Result<Value, Error>，供需要值/错误细节的调用方。
    enum class InterpretResult : u8 {
        Ok,           // 编译并执行成功
        CompileError, // 编译失败（词法 / 语法 / 语义）
        RuntimeError, // 运行期未捕获错误
        LoadError,    // 源文件加载失败（仅 interpret_from_path：I/O 或 UTF-8 编码）
    };

    // 解释器:持解释器级共享状态,驱动 Movement 执行字节码(阶段路线见
    //        .claude/reference/runtime/vm-design.md §6)。
    //
    //        单一主上下文 main_ctx_ + 当前执行上下文指针 current_(现指 main_ctx_)。循环状态全部
    //        取自 *current_,无循环级 C 局部工作副本 -- M6 单循环切换模型下 resume/yield 只换走
    //        current_,dispatch_loop 永不重入(vm-design.md §4.9)。run() 期间 GC 已启用:值栈/帧
    //        经 ctor 注册的 vm_roots tracer 标根(并标 open upvalue 开链),M6 前以 tracer 直标
    //        代替 Movement 升 Object。
    //
    //        解释器级共享状态:模块表 modules_(IMPORT 按键查重/插入)+ VM 级只读 builtins 表
    //        (LOAD_GLOBAL 未命中模块 globals 后回退查)+ 源根列表,详见各成员注释。GC 根经
    //        std::function 回调注册进自有 gc_([this] 捕获;组合而非继承:GC 不识 VM 类型),
    //        collect 时标 modules_/builtins_/object_class_ + current_ 沿 previous_ 执行链各
    //        上下文的值栈/帧/挂起错误寄存器。
    //
    //        异常通道(try/catch/throw)闭环:dispatch_loop 内运行时错误统一 raise 入挂起寄存器
    //        后经 unwind 查 CodeUnit 异常记录表派发;设计见
    //        .claude/reference/runtime/exception-implementation-pitfalls.md 与下方 raise/fail 注。
    //
    //        VM 持有自己的 GC(值成员 gc_):成员声明序 gc_ -> main_ctx_ -> current_ -> modules_
    //        保证析构逆序下 gc_ 最后析构,tracer 与各成员同生共死,无需析构时显式注销。
    class AriaVM {
    public:
        // 构造即把 VM 根 tracer 注册进自有 GC;main_ctx_/modules_ 借 &gc_。
        // 定义在 .cpp(预期随阶段推进:接更多根、内置模块、原生函数表等,体量增长)。
        AriaVM();

        AriaVM(const AriaVM&)            = delete;
        AriaVM& operator=(const AriaVM&) = delete;

        // VM 不可移动:成员间持指向彼此/自身的指针(main_ctx_/modules_ borrow &gc_;
        // current_ 指 main_ctx_;GC 根 tracer 捕 [this]),move 后这些指针不自动重绑 -> 悬垂。
        // 就地构造或以 unique_ptr 持有,勿按值搬迁。显式删 move 把不变式提为显式契约(否则当前仅
        // 由 GC 不可 move 隐式派生,易被误读为可放开)。
        AriaVM(AriaVM&&)            = delete;
        AriaVM& operator=(AriaVM&&) = delete;

        // 把 fn 当程序入口在主上下文执行:入口纪律断言 + 源根入口槽 [0] 播种 + 前后 reset 清场
        // + 委托私有 run_function(执行本体,见其注释)。重复调用先 reset 主上下文(HALT 收场的
        // 上一轮不弹帧,不清场会把新帧叠在陈旧帧上);入口断言 current_ == &main_ctx_ 是切换
        // 纪律不变式(run() 是唯一驱动入口,vm-design.md §4.9)。
        //
        // 注:fn 的 CodeUnit 假定良构(以 RETURN/HALT 终止),不做逐指令越界设防。
        //
        // 返回 Result<Value, Error>:成功为返回值,失败为未捕获的运行时错误
        // (M6 协程挂起将扩三态,见 vm-design.md §3)。
        Result<Value, Error> run(ObjFunction* fn);

        // 编译 source 到 module 的入口 ObjFunction 并在主上下文执行。
        //   - source 须为调用方拥有/加载的实际源文件，存活到本函数返回（编译期 Error 的
        //     SourceLoc 指向它；成功路径返回值不依赖 source）。
        //   - module 须为 GC 管理的合法 ObjModule（编译期由 CodeGen::compile 内部 make_guard
        //     根化，调用方无需再守）。
        // 成功为返回值；失败为首错 Error（编译期错误原样透传，运行期错误同 run(ObjFunction*)）。
        Result<Value, Error> run(SourceFile& source, ObjModule* module);

        // 编译并执行源码字符串（interpret）：构造 SourceFile（名 <script>）+ 合成入口模块（名 <script>、
        // dir_=cwd）-> 编译 -> 执行。错误渲染到 stderr，返回 InterpretResult（不返 Error，见上枚举注释）。
        InterpretResult interpret_from_src(StringView src);

        // 编译并执行源文件（interpret_from_path）：SourceFile::from_path 读盘（失败渲染并返 LoadError）
        // + 按路径派生入口模块（name=basename 去 .aria、dir_=dirname(absolute(path))）-> 编译 -> 执行。
        // 错误渲染到 stderr，返回 InterpretResult。
        InterpretResult interpret_from_path(StringView path);

        [[nodiscard]]
        GC& gc() noexcept {
            return gc_;
        }

        [[nodiscard]]
        const GC& gc() const noexcept {
            return gc_;
        }

        [[nodiscard]]
        Movement& main_context() noexcept {
            return main_ctx_;
        }

        [[nodiscard]]
        const Movement& main_context() const noexcept {
            return main_ctx_;
        }

        // ---- 挂起错误侧信道(供原生函数等冷路径报错)----
        // 寄存器物理上在 VMContext(Movement::pending_error_);VM 经此转发到 *current_(当前执行
        // 上下文):dispatch_loop 主循环、call_value 族与本转发同源同一 current_,故原生函数体内
        // vm.fail() 报的错误必落进其调用者正在执行的上下文(M6 协程期即该协程的寄存器)。
        // 详见 ObjNativeFn.hpp NativeFn 契约与 .claude/reference/runtime/vm-design.md §4.7(错误通道 2)。
        //
        // 载荷类型(M3 起)为 Value,单寄存器模型(exception-implementation-pitfalls.md 坑 #7):
        // - raise(code, fmt, args...):从零构造消息的装箱入口,一步烘齐 -- detail 经
        //   std::format_string 编译期校验的格式串格式化后,由 Error::make_message(Error 的烘焙
        //   单点)合成完整消息,new_exception 装箱入寄存器。**消息不含位置前缀**:被抛出的错误
        //   只携带码与描述(对齐 clox/Python 惯例),位置由 unwind 未捕获出口的逐帧 at 跟踪行
        //   给出。不经 Error 对象中转 -- Error 只在边界出现(Result 出口 / 未捕获出口物化)。
        //
        //   返回 void -- 纯副作用操作(raise 必 raise),失败信号惯用法由 fail 承载(FailSignal
        //   按调用点上下文转 false/nullptr/nullopt)。Movement::raise(Value)(存原值不包)是
        //   用户 throw 的路由,不经本 VM 层 API。
        template<typename... Args>
        void raise(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            const auto msg = Error::make_message(code, std::format(fmt, std::forward<Args>(args)...));
            current_->raise(Value::from_obj(new_exception(gc_, code, msg)));
        }
        // - fail:便捷工厂 -- raise 后**恒返失败信号**:供各失败出口一行报错 `return vm.fail(...);`
        //   (返回 FailSignal,按所在函数返回类型隐式转换,见 FailSignal 注),与原生函数/
        //   call_value 族/load_module 的失败站点共用。[[nodiscard]] 故意为之:裸 `vm.fail(...);`
        //   (丢弃信号)会触发警告 -- 要么写成 `return vm.fail(...);`(惯用法),要么改用
        //   void 的 raise(...)。VM 以**原生函数返回的 bool 为成败信号**(见 call_value 原生分支),
        //   寄存器仅作错误载荷容器;二者须一致(debug 断言把关),契约 `false ⟺ 已 raise`。
        template<typename... Args>
        [[nodiscard]]
        FailSignal fail(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            raise(code, fmt, std::forward<Args>(args)...);
            return FailSignal{};
        }
        // 寄存器载荷在未捕获出口经 AriaVM.cpp 匿名 uncaught_error_parts 拆为 (码, 烘焙消息) 两件,
        // 边界文案与 Error::from_detail 直构逐字一致。raise 模板内 new_exception(分配)可能在
        // 原生执行中触发 GC:值栈/帧/builtins_ 均已接根,载荷构造后立即入寄存器(pending_error
        // 亦由 VM 根 tracer 标根),根安全由既有接线承保。

        // 模块表(解释器级):键 = 规范路径 ObjString*(intern,装箱为 Value),
        // 值 = ObjModule*(装箱为 Value)。IMPORT 按键查重/插入;trace 由 VM 根 tracer 委托。
        [[nodiscard]]
        AriaHashTable& modules() noexcept {
            return modules_;
        }

        [[nodiscard]]
        const AriaHashTable& modules() const noexcept {
            return modules_;
        }

        // VM 级只读 builtins 表:内置原生函数(type/len/str/assert)按名注册于此,LOAD_GLOBAL
        // 在模块 globals 未命中后回退查此表(Python 式 globals -> builtins 查找链)。构造期由
        // builtins::register_builtins 一次性填充,全 VM 生命周期共享;trace 由 VM 根 tracer 委托。
        [[nodiscard]]
        AriaHashTable& builtins() noexcept {
            return builtins_;
        }

        [[nodiscard]]
        const AriaHashTable& builtins() const noexcept {
            return builtins_;
        }

        // Object 根类(M5 决策 3:bootstrap、VM 成员单独持有,不进任何名字空间)。
        // LOAD_OBJECT 直推该成员;测试白盒检视用。
        [[nodiscard]]
        ObjClass* object_class() noexcept {
            return object_class_;
        }

        [[nodiscard]]
        const ObjClass* object_class() const noexcept {
            return object_class_;
        }

        // 源根列表(解释器级):裸名导入(import "lib/utils")的搜索路径根目录,语义对齐 Python
        // sys.path -- 解析器沿各源根找 <源根>/<spec>.aria,首个存在者命中(详见
        // .claude/reference/runtime/import-path-resolution.md)。
        //
        // 模块表键为命中文件的绝对规范路径(源根不进键)。布局:[0] = 入口槽(构造时 cwd 占位,
        // run() 时被入口模块 dir_ 原地替换);[1..] = 配置根(见 set_source_roots)。存为
        // List<String>(路径元数据,非 ObjString*,不参与 GC 追踪 -- 仅解析器用,不作模块表键)。
        [[nodiscard]]
        const List<String>& source_roots() const noexcept {
            return source_roots_;
        }

        // 覆盖配置源根(stdlib / -L / 环境变量等),替换 [1..]、保留入口槽 [0](入口槽由 run()
        // 按入口模块 dir_ 原地替换,不归此管)。测试 / 嵌入配置用:置空即清掉默认 stdlib(隔离);
        // 置 [dir...] 即指定自定义源根集合。
        void set_source_roots(List<String> roots) noexcept;

    private:
        // 执行本体(无入口装饰):入口 fn 现场包空闭包(顶层也闭包)后压 callee + enter_frame
        // 进帧 -> dispatch_loop 主循环,作用于 *current_。run() 的被委托方,亦是未来重入的接缝
        // (原生回调调 aria 函数 / 嵌入宿主调函数,vm-design.md §4.7):故不播源根、不 reset
        // (重入调用者的栈不可冲掉)、不断言主上下文;落地时升公开,并需 dispatch_loop 按基线
        // 帧深退出(现仅 frames().empty() 返回,中途重入会穿掉调用者帧)。
        Result<Value, Error> run_function(ObjFunction* fn);

        // 主循环:驱动 *current_ 直到顶层返回/错误/显式停止。栈/帧/错误寄存器一律经 current_
        // 访问,与 raise 同源(语义统一)。模块体 run-once 经 IMPORT 未命中分支以普通函数调用
        // 进帧(入口名固定 <module>),由本循环执行,其 RETURN 按函数名判定模块体帧,压回模块
        // 对象 -- 无递归调用。
        Result<Value, Error> dispatch_loop();

        // IMPORT 未命中分支的加载层:读盘 -> 派生身份 -> new_module -> 入表占位 -> 编译
        // (入口名 <module>,即 aria.hpp kModuleEntryName)-> 返回模块对象(已 set_entry)。
        // **仅加载与编译**,不执行模块体 -- run-once 由调用方(IMPORT 分支)以普通函数调用进帧驱动,
        // 其 RETURN 按函数名 == <module> 判定后压回模块对象。
        //
        // 错误契约与 call_value 族同构:return nullptr ⟺ 错误载荷已 raise 入 *current_ 寄存器,
        // 调用方 take_error 取出沿 runtime_err 传播。两类失败:读盘失败/名字无效经 fail 烘位置
        // (raise 时顶帧即导入方帧,last_ip 指本 IMPORT 指令);被导入模块的编译期 Error 就地
        // new_exception 原样装配箱透传(from_baked 语义不重烘,位置指向被导入文件内部)。
        //
        // **仅限 dispatch_loop 驱动期调用**:寄存器随 *current_ 走,run() 入口 reset 会清
        // pending_error -- run 外直调的错误会被静默吞掉。
        //   - canonical_path:命中文件的绝对规范路径(intern ObjString*),一身二任 -- 既作
        //     modules_ 表键,又作读盘路径。**调用方须已根化**(跨本函数内 modules_.upsert 的
        //     rehash 触 GC -- intern weak root 不保命)。
        //   - import_specifier:用户写的原始 import 串(报错消息用,如 "./helper")。
        ObjModule* load_module(ObjString* canonical_path, StringView import_specifier);

        // interpret / interpret_from_path 共用尾段：Compiler{gc_}.compile 编译 + run(ObjFunction*) 执行，
        // 成功返 Ok；失败把 Error.message() 渲染到 stderr 并按**失败阶段**分类 -- 编译期失败 ->
        // CompileError，run 期失败 -> RuntimeError（含运行期 UndefinedVariable 与经异常通道传播的
        // 被导入模块编译期错误，后者可被 try/catch 捕获故不构成 CompileError）。不按错误码大类映射。
        InterpretResult interpret_run(SourceFile& source, ObjModule* module);

        // CALL 分发:栈顶形如 [callee, a1..aN](N=argc,由 CALL 调用方保证)。按 callee 的对象类型
        // 分派到对应 call_* 子例程,其余经 Object::op_call 协议基类默认报 CallNonCallable(未来
        // 可调用新类型 override op_call 即接入,不改本 switch)。M4 起 callable 收敛为闭包,
        // ObjFunction 退为常量池内部物、不以 callable 值上栈。
        //
        // 作用于 *current_(与 dispatch_loop 同源)。返回 bool 为成败信号,契约:
        // return false ⟺ 错误载荷已 raise 进 *current_ 的挂起错误寄存器,调用方据 bool 决定
        // 是否 take_error 取出沿 runtime_err 传播。
        bool call_value(Value callee, u8 argc);

        // 闭包调用:校验 arity + 帧栈未溢出后 enter_frame 进帧(callee 在槽 0,参数即局部槽
        // 1..argc;arity 等元数据经 closure->function() 取)。失败 raise WrongArity /
        // StackOverflow 后返 false(bool 契约见 call_value)。
        bool call_closure(ObjClosure* obj, u8 argc);

        // 原生函数调用:同步调用 obj->fn(),不进帧;原生函数自身以 bool 为成败信号、返回值写槽 0、
        // 错误载荷走侧信道寄存器,本函数透传该 bool 契约(细节见定义处注释;契约详见
        // ObjNativeFn.hpp NativeFn 契约与 .claude/reference/runtime/vm-design.md §4.7)。
        bool call_native(const ObjNativeFn* obj, u8 argc);

        // 类实例化(call_value CLASS 分支):new_instance 为唯一 GC 点,instance 建成即写 callee
        // 槽 -- **槽 0 原位换实例**(即新帧的 this / 原生 init 的 slots[0]),余下交 call_value
        // 通用分发,与 call_bound_method 同款「槽 0 调用方改写」约定(细节见定义处注释)。
        // 成功返 true;失败经分发 raise 后返 false(bool 契约)。
        bool call_class(ObjClass* obj, u8 argc);

        // 绑定方法调用(call_value BOUND_METHOD 分支):调用区 [bound, a1..aN] 的槽 0 恰为
        // bound 对象,原位覆写为 receiver(this 替代 callee,零整形,实参槽位不动),余下交
        // call_value 分发(细节见定义处注释)。方法值无需守卫:覆写槽 0 后经类表槽/缓存可达。
        // 成功返 true;失败经分发 raise 后返 false(bool 契约)。
        bool call_bound_method(const ObjBoundMethod* obj, u8 argc);

        // Object 根类 bootstrap(M5 决策 3,ctor 一次调用):建 ObjClass("Object", super=nullptr)
        // + 原生 no-op init(init Value 化:无 ObjFunction/无模块,保 ObjFunction「module 恒非空」
        // 不变式)并发布:upsert 进类表 init 槽 + init_ 指同一值(细节见定义处注释)。
        // 成员 object_class_ 单独持有、不进 builtins_/任何模块 globals(裸名解析
        // 局部->upvalue->全局->builtins 全部够不到,LOAD_OBJECT 直推成员,用户 shadow 全局名免疫)。
        void bootstrap_object_class();

        // 源根默认值初始化(ctor 一次调用,纯路径配置不触 GC):入口槽 [0] 占位为 cwd + 配置根
        // [1..] = 编译器相对 stdlib 源根(布局与替换权见 source_roots/set_source_roots 注释)。
        void init_source_roots();

        // ---- 异常 unwind(M3,dispatch_loop 驱动期专用;设计见 exception-implementation-pitfalls.md 坑 #11-#16)----

        // 弹 2 算 1:对栈顶两个值执行二元数值运算(9 个算术/比较指令共用,Op 由 dispatch_loop
        // 调用点穷举实例化;数值语义见定义处注释)。与 call_value 族同款 bool 契约
        // (return false ⟺ 已 raise),调用方 unwind 查表派发/未捕获物化。模板成员定义在 .cpp
        // (全部实例化点在同 TU 的 dispatch_loop)。
        template<OpCode Op>
        bool run_binary_numeric();

        // ---- 类与对象(M5):field 族指令执行体 ----
        //
        // 与 run_binary_numeric/call_value 族同款 bool 契约:失败载荷已在 *current_ 挂起寄存器
        // -- 对象协议失败由 override 内 vm.fail 就地烘焙,非对象守卫由执行体 fail,执行体只透传
        // 信号。THIS 对(LOAD/STORE_THIS_FIELD)不设执行体:编译器不变式保证 this 恒实例,
        // 无 nil/原语守卫,case 内直调协议。
        //
        // LOAD_FIELD 执行体(name 操作数已读出):peek obj 不弹,经 Object::load_field 虚函数
        // 协议解析,结果写回原槽([obj] -> [v])。peek 不弹 -- 协议内分配跨 GC 须 this 在栈
        // (「栈即根」)。非对象(含 nil)是协议外的原语,文案留本执行体;对象 miss 的文案由
        // 协议 override 就地烘焙(nullopt ⟺ 已 fail)。
        bool run_load_field(ObjString* name);

        // STORE_FIELD 执行体:[obj, v] -> [v](双值 peek 不弹,完成时单槽下移留 v -- 赋值
        // 表达式约定;peek 不弹兼跨协议内 miss fail 分配的「栈即根」)。经 Object::store_field
        // 协议,false ⟺ override 已按自身措辞 fail 入寄存器;非对象守卫同 run_load_field。
        bool run_store_field(ObjString* name);

        // LOAD_SUPER_FIELD 执行体:defining class 取 *current_ 顶帧 closure 直读(方法闭包恒有
        // 戳(MAKE_METHOD 注册时设),编译器不变式 ASSERT 钉),从其父类起走 ObjClass::load_field
        // 协议沿链读穿透(起点即 super,不含 defining 自身;类协议不绑定不缓存)。
        //
        // 命中方法闭包 -> 绑 this=帧槽 0 压栈供 CALL;其余(静态方法 fun/持函数值的静态变量/
        // 原生/静态值)原值直读压栈;两者均**不写 fields 缓存**(铁则 2:super 查到的是被覆写前
        // 的实现,写缓存会被 fields 命中劫持后续 obj.m 动态派发)。
        //
        // defining/super 非空是编译器保证的不变式;全链 miss 为语言可达错误,经协议 fail 返 false。
        bool run_load_super_field(ObjString* name);

        // 自最内帧向外遍历帧链:每帧以 last_ip 反推 offset 查本帧 CodeUnit 异常记录表
        // (find_try_handler 取最内层覆盖),首命中即在该帧 unwind -- 截值栈、ip 跳 handler、
        // 寄存器载荷 push 落 catch 参数槽,返 nullopt(已派发,调用方 continue);未命中的帧
        // 先记堆栈跟踪三元组(fn/mod/ip_off)再 exit_frame 继续外层。全帧未命中 -> 未捕获:
        // 从寄存器反提载荷为 (码, 烘焙消息) 两件(uncaught_error_parts),逐帧跟踪烘焙
        // "\n  at <fn> (<loc>)" 进消息尾部(收集序内->外,渲染反转为外->内,Python 式
        // most recent call last),经 Error::from_baked 一次物化返回。
        //
        // 前提:寄存器已有载荷(raise/fail/THROW 已入),本函数不构造载荷 -- 入口断言把关
        // (write 侧 Movement::raise 空寄存器断言的 read 侧成对)。帧内 last_ip 由 dispatch_loop
        // 循环顶写(顶帧 = 故障指令,外层帧 = CALL 站点,坑 #2),无参数。
        Opt<Error> unwind();

        GC gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)

        Movement main_ctx_;

        // 当前执行上下文:dispatch_loop 主循环 / call_value 族 / raise 的作用对象,构造即指
        // &main_ctx_,一律直接经 current_ 访问(语义统一,无入口快照)。M6 单循环切换模型见
        // vm-design.md §4.9。
        Movement* current_;

        AriaHashTable modules_;  // 模块表(解释器级共享 + GC 根)
        AriaHashTable builtins_; // VM 级只读 builtins 表(构造期一次填充 + GC 根,LOAD_GLOBAL 回退查)

        // 源根列表:[0]=入口槽(cwd 占位,run() 换成入口 dir_);[1..]=配置根(stdlib/-L/环境变量)
        List<String> source_roots_;

        // Object 根类(M5 决策 3):构造期 bootstrap、单独持有,不进 builtins_/任何模块 globals
        // (LOAD_OBJECT 直推;tracer 第 4 根);bootstrap 前的空态 nullptr 由构造函数初始化列表
        // 显式置,供 tracer 先行注册后 register_builtins 触 GC 时 mark_object 容 nullptr。
        ObjClass* object_class_;
    };

    // VM 不可移动不变式的显式校验(理由见类内 move 删除处注释):若有人删掉 delete 且隐式
    // move 重新生成,断言在此炸出,避免静默变可移动后的悬垂 UB。
    static_assert(!std::is_move_constructible_v<AriaVM>);
    static_assert(!std::is_move_assignable_v<AriaVM>);

} // namespace aria

#endif // ARIA_VM_HPP
