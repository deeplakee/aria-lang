#ifndef ARIA_VM_HPP
#define ARIA_VM_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "runtime/Movement.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    class ObjClosure;
    class ObjFunction;
    class ObjModule;
    class ObjNativeFn;

    // 前置声明(bytecode/code.hpp 的 X 表生成物):run_binary_numeric<Op> 模板形参用,
    // 免头文件拖入 bytecode 树(定义处 AriaVM.cpp 已含)。
    enum class OpCode : u8;

    // interpret 结果：编译并执行的结局类别（对齐 clox InterpretResult）。
    // interpret / interpret_from_path 内部已把错误渲染到 stderr，故只回类别、不回 Error--
    // turnkey 场景调用方只要成败类别；Error 自有完整消息串、与内部构造/读盘的 SourceFile 解耦，
    // 返回后 SourceFile 销毁亦无碍。低层 run(ObjFunction*) / run(SourceFile&, ObjModule&) 仍返
    // Result<Value, Error>，供需要值/错误细节的调用方。
    enum class InterpretResult : u8 {
        Ok,           // 编译并执行成功
        CompileError, // 编译失败（词法 / 语法 / 语义）
        RuntimeError, // 运行期未捕获错误
        LoadError,    // 源文件加载失败（仅 interpret_from_path：I/O 或 UTF-8 编码）
    };

    // 解释器:持解释器级共享状态,驱动 Movement 执行字节码(阶段路线见
    //        .claude/reference/runtime/vm-design.md §6,主循环/全局/模块/异常已落地,
    //        M4 闭包/M5 类/M6 协程待续)。
    //
    //        单一主上下文 main_ctx_ + 当前执行上下文指针 current_(现指 main_ctx_)。循环状态全部
    //        取自 *current_,无循环级 C 局部工作副本 -- M6 单循环切换模型(Wren 式)下
    //        resume/yield 在 CALL 善后点换 current_、循环自然驱动新上下文,dispatch_loop 永不重入。
    //        run() 期间 GC 已启用:值栈/帧经 ctor 注册的 vm_roots tracer 标根(M6 前以 tracer
    //        直标代替 Movement 升 Object;M4 起并标 open upvalue 开链)。
    //
    //        解释器级共享状态:模块表 modules_(键为规范路径 ObjString* intern、值为 ObjModule*,
    //        均装箱为 Value 入 AriaHashTable,白赚 trace;IMPORT 全链:路径解析 -> 命中复用,
    //        未命中 load_module 读盘 -> 编译 -> 模块体 run-once)+ VM 级只读 builtins 表
    //        (LOAD_GLOBAL 未命中模块 globals 后回退查)+ 源根列表。GC 根经 std::function 回调接入
    //        (组合而非继承:GC 不识 VM 类型,VM 构造时把 tracer lambda 注册进自己的 gc_ -- [this] 捕获。
    //        比「函数指针 + void* ctx + 静态 thunk」干净:无适配器、无 void*、无
    //        static_cast,标记逻辑直写进 lambda。[this] 仅一指针,落在 std::function SBO 内零堆分配)。
    //        collect 时 tracer 标 modules_(进而各模块 name_/dir_/entry_/globals_)
    //        + builtins_ + current_ 沿 previous_ 执行链各上下文的值栈/帧/挂起错误寄存器。
    //
    //        异常通道(try/catch/throw)闭环:dispatch_loop 内运行时错误统一 raise 入挂起寄存器后经
    //        unwind 查 CodeUnit 异常记录表派发(命中 handler 截栈跳 handler / 全未命中物化 Error 带
    //        堆栈跟踪);THROW 弹用户 throw 的原值保类型;try/catch 由 CodeGen 编译期写 try_records
    //        (无 SETUP_EXCEPT 指令)。见下方「异常 unwind」组成员与
    //        .claude/reference/runtime/exception-implementation-pitfalls.md。
    //
    //        VM 持有自己的 GC(值成员 gc_):每个 VM 一个 GC,无需外部注入。成员声明序
    //        gc_ -> main_ctx_ -> current_(指 &main_ctx_) -> modules_(后者引用 &gc_),故析构
    //        逆序下 gc_ 最后析构,tracer 与 modules_ 同生共死,无需析构时显式注销(GC 不可能比 VM 长寿)。
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

        // 把 fn 当程序入口在主上下文执行:程序入口仪式(入口纪律断言 + 源根入口槽 [0] 播种 +
        // 前后 reset 清场)+ 委托私有 run_function 压 callee/进帧/驱动主循环(执行本体,见其注释)。
        // 重复调用先 reset 主上下文(同 Lexer/Parser 式复用;HALT 收场的上一轮不弹帧,不清场会把
        // 新帧叠在陈旧帧上);入口断言 current_ == &main_ctx_ (M6 前恒真;M6 后 resume/yield 严格
        // 成对,协程挂起返回时 VM 层即已回退 -- 不归位即切换纪律被破坏,是错不当静默重置;
        // M6 单循环切换模型下升格为永久不变式:run() 是唯一驱动入口,见 vm-design.md §4.9)。
        // 注:fn 的 CodeUnit 假定良构(以 RETURN/HALT 终止),不做逐指令越界设防。
        // 返回 Result<Value, Error>:成功为返回值,失败为未捕获的运行时错误
        // (M6 协程挂起将扩三态,届时引入 Yielded,见 .claude/reference/runtime/vm-design.md §3)。
        Result<Value, Error> run(ObjFunction* fn);

        // 编译 source 到 module 的入口 ObjFunction 并在主上下文执行（编译并执行）。
        // 经 Compiler（用本 VM 的 gc_，编译期分配与 run 同源）把 source 编进 module，再委托 run(ObjFunction*)。
        //   - source 须为调用方拥有/加载的实际源文件，存活到本函数返回（编译期 Error 的 SourceLoc 指向它；
        //     成功路径返回值不依赖 source）。
        //   - module 须为 GC 管理的合法 ObjModule（编译期由 CodeGen::compile 内部 make_guard 根化，调用方无需再守）。
        // 成功为返回值；失败为首错 Error（编译期错误原样透传，运行期错误同 run(ObjFunction*)）。
        Result<Value, Error> run(SourceFile& source, ObjModule& module);

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
        // 上下文):dispatch_loop 主循环、call_value 族与本转发同源同一 current_,故原生函数体内 vm.fail()
        // 报的错误必落进其调用者正在执行的上下文 -- M6 协程期即该协程的寄存器,不串主上下文。
        // 详见 ObjNativeFn.hpp NativeFn 契约与 .claude/reference/runtime/vm-design.md §4.7(错误通道 2)。
        //
        // 载荷类型(M3 起)为 Value,单寄存器模型(exception-implementation-pitfalls.md 坑 #7):
        // - raise(code, fmt, args...):从零构造消息的装箱入口,一步烘齐 -- detail 按 fmt+args
        //   格式化(格式串经 std::format_string 编译期校验;不含 "Category:" 前缀,防双烘),位置
        //   取自 *current_ 顶帧 last_ip(故障指令 / CALL 站点)查行号表烘 "path:line: " 前缀
        //   (合成模块退化为 "<name>:line";帧栈空即 run 外直调则无位置),经 Error::make_message
        //   (Error 的烘焙单点,公开重载)合成完整消息后 new_exception 装箱入寄存器。不经 Error
        //   对象中转 -- Error 只在边界出现(Result 出口 / 未捕获出口反提物化),不当内部渡船;位置恰
        //   只在装箱点可得,一并烘入正是把烘焙责任归位。返回 void -- 纯副作用操作(raise 必
        //   raise,无成败结局可表),bool 惯用法由 fail 承载。Movement::raise(Value)(存原值不包)
        //   是 M3 用户 throw 的路由,不经本 VM 层 API。
        // - fail:便捷工厂 -- raise 后**恒返 false**:供原生函数一行报错 `return vm.fail(...);`
        //   (同时置寄存器与返回 NativeFn 契约要求的失败信号),与 call_value/call_closure/
        //   call_native/load_module 的失败站点共用。[[nodiscard]] 故意为之:裸 `vm.fail(...);`
        //   (丢弃其 false)会触发警告 -- 要么写成 `return vm.fail(...);`(惯用法),要么显式
        //   `(void)vm.fail(...);` 表明「我要 raise 但走别的控制流」。VM 以**原生函数返回的 bool
        //   为成败信号**(见 call_value 原生分支),寄存器仅作错误载荷容器;二者须一致(debug
        //   断言把关),契约 `return false ⟺ 已 raise`。
        // 寄存器载荷在未捕获出口经 AriaVM.cpp 匿名 uncaught_error_parts 拆为 (码, 烘焙消息) 两件,
        // 边界文案与 Error::from_detail 直构逐字一致。
        //
        // 装箱核心 raise_detail 内 new_exception(分配)可能在原生执行中触发 GC:值栈/帧/builtins_
        // 均已接根,载荷构造后立即入寄存器(pending_error 亦由 VM 根 tracer 标根),根安全由既有
        // 接线承保。核心定义在 .cpp(需 ObjException 完整类型)。
        template<typename... Args>
        void raise(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            raise_detail(code, std::format(fmt, std::forward<Args>(args)...));
        }

        template<typename... Args>
        [[nodiscard]]
        bool fail(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            raise(code, fmt, std::forward<Args>(args)...);
            return false;
        }

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

        // 源根列表(解释器级):裸名导入(import "lib/utils")的搜索路径根目录,语义对齐 Python
        // sys.path -- 解析器沿各源根找 <源根>/<spec>.aria,首个存在者命中(详见 IMPORT 实现 &
        // .claude/reference/runtime/import-path-resolution.md)。模块表键为命中文件的绝对规范路径(weakly_canonical,
        // 源根不进键)。布局固定:source_roots_[0] = 入口槽,source_roots_[1..] = 配置根。
        //   - 构造时 [0] 占位为当前工作目录(前期源根),[1..] 推入编译器相对 stdlib 目录等配置根。
        //   - run() 时 [0] 被入口模块 dir_ 替换(dir_ 恒非空 -- new_module 默认 cwd);[1..] 不动。
        //   - set_source_roots 替换 [1..](配置根),保留 [0](入口槽)。
        // 单一 List<String> 即可,无 flag / 无并列配置列表:[0] 槽位约定 + run() 原地替换。
        // 加载链路(磁盘加载 + AST->CodeUnit 编译 + VM 内嵌套执行模块体)未就绪,源根目前仅服务于
        // 路径解析(命中后查模块表;未命中即 ModuleNotFound)。存为 List<String>(路径元数据,
        // 非 ObjString*,不参与 GC 追踪 -- 仅解析器用,不作模块表键)。
        [[nodiscard]]
        const List<String>& source_roots() const noexcept {
            return source_roots_;
        }

        // 覆盖配置源根(stdlib / -L / 环境变量等;不含入口槽 [0] -- 入口槽由 run() 按入口模块 dir_
        // 原地替换,不归此管)。保留 source_roots_[0],替换 [1..]。测试 / 嵌入配置用:
        // 置空即清掉默认 stdlib(隔离);置 [dir...] 即指定自定义源根集合。
        void set_source_roots(List<String> roots) noexcept;

    private:
        // 执行本体(无入口装饰):入口 fn 现场包空闭包(顶层也闭包,M4)后压 callee 值 + enter_frame
        // 进帧 -> dispatch_loop 主循环,作用于 *current_(程序入口处 run() 已断言 current_ == &main_ctx_,
        // 等价于直访 main_ctx_)。run() 的被委托方,亦是未来重入的接缝:指令执行中临时运行一个
        // ObjFunction(原生回调调 aria 函数 / 嵌入宿主调函数,vm-design.md §4.7「回调 aria 函数属
        // 未来机制(由 vm 提供,自管栈纪律)」)经此进入,故不播源根、不 reset(重入调用者的栈不可
        // 冲掉)、不断言主上下文(current_ 即正在执行的上下文);落地时升公开(原生函数经 AriaVM&
        // 只能触公开面)。落地尚欠 dispatch_loop 按基线帧深退出(现仅 frames().empty() 返回,中途重入会穿掉
        // 调用者帧)与实参布线,届时在此扩。
        Result<Value, Error> run_function(ObjFunction* fn);

        // 主循环:驱动 *current_(现为 main_ctx_;M6 resume 重入时为被恢复协程的上下文)直到顶层
        // 返回/错误/显式停止。栈/帧/错误寄存器一律经 current_ 访问,与 raise 同源(语义统一)。
        // 模块体 run-once 经 IMPORT 未命中分支以普通函数调用进帧(入口名固定 <module>),
        // 由本循环执行,其 RETURN 按函数名判定模块体帧,压回模块对象 -- 无递归调用。
        Result<Value, Error> dispatch_loop();

        // IMPORT 未命中分支的加载层:把已解析命中的磁盘模块读盘 -> 派生身份 -> new_module
        // -> 入表占位 -> 编译(入口名 <module>,即 aria.hpp kModuleEntryName)-> 返回模块对象(已 set_entry)。
        // **仅加载与编译**,不执行模块体 -- run-once 由调用方(IMPORT 分支)以普通函数调用进帧驱动,
        // 其 RETURN 按函数名 == <module> 判定后压回模块对象。
        // 错误契约与 call_value 族同构:return nullptr ⟺ 错误载荷已 raise 入 *current_ 寄存器,
        // 调用方 take_error 取出沿 runtime_err 传播。两类失败:读盘失败/名字无效经 fail 烘位置
        // (raise 时顶帧即导入方帧,last_ip 指本 IMPORT 指令 -- 与 resolve_module 解析失败的
        // runtime_err 形态统一);被导入模块的编译期 Error 就地 new_exception 原样装配箱透传
        // (from_baked 语义不重烘,位置指向被导入文件内部)。**仅限 dispatch_loop 驱动期调用**:寄存器随 *current_ 走,
        // run() 入口 reset 会清 pending_error -- run 外直调的错误会被静默吞掉(runtime_loc 亦
        // 依赖顶帧,帧栈空则无位置)。
        //   - canonical_path:命中文件的绝对规范路径(intern ObjString*),一身二任 -- 既作 modules_
        //     表键,又作读盘路径。**调用方须已根化**(跨本函数内 modules_.upsert 的 rehash 触 GC --
        //     intern weak root 不保命)。
        //   - import_specifier:用户写的原始 import 串(报错消息用,如 "./helper")。
        // 越界检测(相对导入越出源根)本轮不做:文件能解析到即读。
        ObjModule* load_module(ObjString* canonical_path, StringView import_specifier);

        // interpret / interpret_from_path 共用尾段：Compiler{gc_}.compile 编译 + run(ObjFunction*) 执行，
        // 成功返 Ok；失败把 Error.message() 渲染到 stderr（Error 已自有完整消息串、不持 SourceFile*）
        // 并按**失败阶段**分类 -- 编译期失败 -> CompileError（「主入口编译失败，程序从未开始执行」），
        // run 期失败 -> RuntimeError（含运行期 UndefinedVariable 与经异常通道传播的被导入模块编译期
        // 错误，后者可被 try/catch 捕获故不构成 CompileError）。不按错误码大类映射。
        InterpretResult interpret_run(SourceFile& source, ObjModule& module);

        // CALL 分发:栈顶形如 [callee, a1..aN](N=argc,由 CALL 调用方保证)。按 callee 的对象类型
        // 分派到对应 call_* 子例程(ObjClosure -> call_closure、ObjNativeFn -> call_native),其余报
        // CallNonCallable。M4 起 callable 收敛为闭包:ObjFunction 退为常量池内部物,不再以 callable
        // 值上栈(编译器经 CLOSURE 指令现场包闭包;IMPORT 的模块体 entry 由 IMPORT 分支现场包闭包)。
        // 作用于 *current_(与 dispatch_loop 同源;现为 main_ctx_,M6 协程期即当前协程上下文 -- 主循环在哪个
        // 上下文上驱动,调用就发生在哪个上下文,错误随上下文走不串扰)。返回 bool 为成败信号:true
        // 即成功(栈效应由子例程各自负责),false 即失败 -- 错误载荷已 raise 进 *current_ 的挂起
        // 错误寄存器,调用方据 bool 决定是否 take_error 取出沿 runtime_err 传播。
        // 契约:return false ⟺ 已 raise 入 *current_。
        bool call_value(Value callee, u8 argc);

        // 闭包调用:校验 arity + 帧栈未溢出后 enter_frame 进帧(callee 在槽 0,参数即局部槽 1..argc)。
        // 成功返 true;失败 raise WrongArity / StackOverflow 入 *current_ 后返 false。
        // 栈形 [callee, a1..aN] 由 CALL 调用方保证。M4 起 callable 收敛为闭包,arity 等元数据经
        // closure->function() 取。
        bool call_closure(ObjClosure* obj, u8 argc);

        // 原生函数调用:同步调用 obj->fn(),不进帧。原生函数自身以 bool 为成败信号、返回值写槽 0、
        // 错误载荷走侧信道寄存器(Movement::pending_error_);本函数透传该 bool 契约:成功(原生返
        // true)断言无载荷 -> 清寄存器 + drop(argc) 弹实参(返回值升栈顶)后返 true;失败(原生返
        // false)断言已 raise -> 载荷留寄存器交调用方 take_error,返 false(不在本函数取出,与
        // call_closure/call_value 的 bool 契约统一)。调用区 [callee, a1..aN] 经 Span 暴露:
        // slots[0]=槽 0(返回值),slots[1..argc]=实参。详见 ObjNativeFn.hpp NativeFn 契约与
        // .claude/reference/runtime/vm-design.md §4.7。
        bool call_native(const ObjNativeFn* obj, u8 argc);

        // 装箱核心(公开模板 raise(code, fmt, args...) 格式化后经此,契约见上「挂起错误侧信道」):
        // detail 为**已格式化**的原始细节串,runtime_loc 烘位置 + Error::make_message 合成完整消息
        // + new_exception 装箱入 *current_ 挂起寄存器。无调用方需传成品串,故收私有(raise 单一
        // 签名,不与 StringView 版重载分叉)。定义在 .cpp(需 ObjException 完整类型)。
        void raise_detail(ErrorCode code, StringView detail);

        // ---- 异常 unwind(M3,dispatch_loop 驱动期专用;设计见 exception-implementation-pitfalls.md 坑 #11-#16)----

        // 弹 2 算 1:对栈顶两个值执行二元数值运算(9 个算术/比较指令共用,Op 由 dispatch_loop 调用点
        // 穷举实例化;数值语义内联于函数 -- 双 Int 整数路径、任一 F64 升浮点,int 除/模零报错、
        // f64 按 IEEE)。成功压结果返 true;失败不置值,经 fail 装箱入 *current_ 挂起寄存器后
        // 返 false -- 与 call_value 族同款 bool 契约(return false ⟺ 已 raise),调用方 unwind
        // 查表派发/未捕获物化。模板成员定义在 .cpp(全部实例化点在同 TU 的 dispatch_loop)。
        template<OpCode Op>
        bool run_binary_numeric();

        // 自最内帧向外遍历帧链:每帧以 last_ip 反推 offset 查本帧 CodeUnit 异常记录表
        // (find_try_handler 取最内层覆盖),首命中即在该帧 unwind -- 截值栈到 frame.slots +
        // rec->stack_depth、ip 跳 rec->handle、寄存器载荷 push 落 catch 参数槽(恒 == stack_depth,
        // 值填槽无 STORE_LOCAL,坑 #10),返 nullopt(已派发,调用方 continue);未命中的帧先记
        // 堆栈跟踪三元组(fn/mod/ip_off,坑 #16)再 exit_frame 继续外层。全帧未命中 -> 未捕获:
        // 从寄存器反提载荷为 (码, 烘焙消息) 两件(uncaught_error_parts)并把跟踪(收集序内->外,反转为
        // 外->内,Python 式 most recent call last)逐帧烘焙 "\n  at <fn> (<loc>)" 进消息尾部,
        // 经 Error::from_baked 一次物化返回。
        // 前提:寄存器已有载荷(raise/fail/THROW 已入),本函数不构造载荷 -- 入口断言把关
        // (write 侧 Movement::raise 空寄存器断言的 read 侧成对)。帧内 last_ip 由 dispatch_loop
        // 循环顶写(顶帧 = 故障指令,外层帧 = CALL 站点,坑 #2),无参数。
        // raise 与 unwind 不融合成 *_and_* 具名助手:两个直观动作就地两步,全部站点与
        // CALL case 失败善后同形(raise 在 call_* 内则直接 unwind)。
        Opt<Error> unwind();

        GC       gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)
        Movement main_ctx_;
        // 当前执行上下文:dispatch_loop 主循环 / call_value 族 / raise 的作用对象,构造即指 &main_ctx_。
        // 方法纪律:dispatch_loop/call_value 族/raise 一律直接经 current_ 访问(语义统一,无入口快照)。
        // M6 单循环切换模型(vm-design.md §4.9):resume/yield 为原生函数,换 current_ 对
        // call_native 透明(事后簿记一律落 entered_ctx,无需分支探测,CALL case 零改动),
        // dispatch_loop 永不重入,任一时刻正在执行的字节码所在上下文恒等于 current_;previous_ 对齐
        // Wren caller(yield/完成解链、可再 resume),tracer 链遍历保留(main_ctx_ 不入堆),
        // 链尾断言届时退役。
        Movement*     current_;
        AriaHashTable modules_;  // 模块表(M2:解释器级共享 + GC 根)
        AriaHashTable builtins_; // VM 级只读 builtins 表(构造期一次填充 + GC 根,LOAD_GLOBAL 回退查)
        // 源根列表:[0]=入口槽(cwd 占位,run() 换成入口 dir_);[1..]=配置根(stdlib/-L/环境变量)
        List<String> source_roots_;
    };

    // VM 不可移动不变式的显式校验(类完成定义后断言):成员间持指向彼此/自身的指针
    // (main_ctx_/modules_ borrow &gc_;current_ 指 main_ctx_;GC 根 tracer 捕 [this]),move 后不自动重绑 -> 悬垂。
    // 上方已显式 delete move;此断言锁定该不变式 -- 若有人删掉上面的 delete 且 GC 变可
    // move 致隐式 move 重新生成,断言在此炸出,避免静默变可移动后的悬垂 UB。
    static_assert(!std::is_move_constructible_v<AriaVM>);
    static_assert(!std::is_move_assignable_v<AriaVM>);

} // namespace aria

#endif // ARIA_VM_HPP
