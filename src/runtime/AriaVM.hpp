#ifndef ARIA_VM_HPP
#define ARIA_VM_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
// raise 模板头内内联装箱需 ObjException 完整类型(其依赖已经 GC.hpp 传递拉入)。
#include "object/ObjException.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/opcode_profile.hpp"
#include "runtime/string_constant.hpp"
#include "runtime/value_register.hpp"
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
    class ObjString;

    // 前置声明(bytecode/code.hpp 的 X 表生成物):run_binary_numeric<Op> 模板形参用,
    // 免头文件拖入 bytecode 树(定义处 AriaVM.cpp 已含)。
    enum class OpCode : u8;

    // fail 的返回哨兵:按调用点返回类型转失败拼写(bool -> false、指针 -> nullptr、
    // Opt<T> -> nullopt),失败出口惯用法一行 `return vm.fail(...);`。
    // **Opt<T> 转换只对「不可由 bool 构造」的 T 成立**(Value/指针/类类型,如协议族的 Opt<Value>);
    // T 为标量时 optional<T> 会经 operator bool() 构造出**已初始化的 0/false**,把 fail 吞成成功
    // --标量返回型的站点改用 bool + 出参(先例:StringClass 的算子钩子按 native 契约返 bool、
    // 结果写 slots[0])。
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

    // interpret 结果:错误已渲染到 stderr,只回成败类别(turnkey 场景只要类别),
    // 需要值/错误细节用低层 run()。
    enum class InterpretResult : u8 {
        Ok,           // 编译并执行成功
        CompileError, // 编译失败（词法 / 语法 / 语义）
        RuntimeError, // 运行期未捕获错误
        LoadError,    // 源文件加载失败（仅 interpret_from_path：I/O 或 UTF-8 编码）
    };

    // 解释器:持解释器级共享状态,驱动 ObjMovement 执行字节码(路线见 vm-design.md §6,切换模型
    // §4.9);循环状态全部取自 *current_。GC 根经 std::function tracer 注册进自有 gc_ -- 标根
    // 清单见 runtime.md「共享状态」,其中 open upvalue 开链是「闭包已死而 upvalue 仍在链」的
    // 悬垂防线。成员声明序 gc_ 居首保证析构逆序下 tracer 与各成员同生共死。运行时错误统一
    // raise 入寄存器后经 unwind 查 CodeUnit 异常记录表派发。
    class AriaVM {
    public:
        AriaVM();

        AriaVM(const AriaVM&)            = delete;
        AriaVM& operator=(const AriaVM&) = delete;

        // VM 不可移动:成员持指向彼此/自身的指针(modules_ borrow &gc_、current_ 指 GC 对象、
        // tracer 捕 [this]),move 后不重绑 -> 悬垂。就地构造或以 unique_ptr 持有。
        AriaVM(AriaVM&&)            = delete;
        AriaVM& operator=(AriaVM&&) = delete;

        // 协程模块方法面宿主:四原语静态方法经友元访问 check_arity/prepare_call_args/current_
        // 等私有面(宿主类注释见 runtime/builtins/CoroutineModule.hpp)。
        friend class CoroutineModule;

#ifdef ARIA_OPCODE_PROFILE
        // 指令频度探针(仅探针构建,定义见 runtime/opcode_profile.hpp):以成员 opcode_profiler_
        // 挂载,经友元取 current_ 等私有状态;唯一探测点 = dispatch_loop 取指行的
        // ARIA_FETCH_OPCODE 宏,本类其余代码零探针痕迹。
        friend class OpcodeProfiler;
#endif

        // 程序入口仪式:入口锚定执行上下文(出口断言控制流回到锚点) + 源根入口槽 [0] 播种 +
        // 前后 reset 清场 + 入口 fn 包空闭包(顶层也闭包,统一「帧 = 闭包」模型)后委托
        // run_closure。fn 的 CodeUnit 假定良构(以 RETURN/HALT 终止),不逐指令设防;失败为
        // 未捕获运行时错误。
        Result<Value, Error> run(ObjFunction* fn);

        // 编译 source 到 module 的入口 ObjFunction 并执行。source 须存活到返回(编译期 Error
        // 的 SourceLoc 指向它);module 由编译期内部 make_guard 根化,调用方无需再守。失败为首错
        // Error(编译期原样透传)。
        Result<Value, Error> run(SourceFile& source, ObjModule* module);

        // 编译并执行源码字符串:合成 SourceFile 与入口模块(名 <script>、dir_=cwd)。
        // 错误渲染到 stderr,返 InterpretResult。
        InterpretResult interpret_from_src(StringView src);

        // 编译并执行源文件:读盘失败渲染并返 LoadError;入口模块 name=basename 去 .aria、
        // dir_=dirname(absolute(path))。错误渲染到 stderr,返 InterpretResult。
        InterpretResult interpret_from_path(StringView path);

        [[nodiscard]]
        GC& gc() noexcept {
            return gc_;
        }

        // 当前执行上下文(测试用;切换原语落地前恒为主上下文)。
        [[nodiscard]]
        ObjMovement* current_context() noexcept {
            return current_;
        }

        // 挂起错误侧信道(原生函数等冷路径报错):寄存器在 ObjMovement::pending_error_,经 *current_
        // 转发,错误必落进调用者正在执行的上下文。载荷为 Value(单寄存器模型)。raise 从零构造
        // 消息装箱一步烘齐,**消息不含位置前缀**(位置由 unwind 未捕获出口的逐帧 at 行给出);
        // 用户 throw 经 ObjMovement::raise 原值入寄存器,不走本层。fail = raise + 恒返失败信号
        // (FailSignal),[[nodiscard]] 强制 `return vm.fail(...);`;原生函数 bool 契约 false ⟺
        // 已 raise。raise 内 new_exception 分配可能触 GC,载荷构造后立即入寄存器(tracer 已标根)。
        template<typename... Args>
        void raise(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            const auto msg = Error::make_message(code, std::format(fmt, std::forward<Args>(args)...));
            current_->raise(Value::from_obj(new_exception(gc_, code, msg)));
        }
        template<typename... Args>
        [[nodiscard]]
        FailSignal fail(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            raise(code, fmt, std::forward<Args>(args)...);
            return FailSignal{};
        }

        // 元数报错唯一口:措辞家族「function expects <spec>, got <n>」的唯一构造点,builtins 方法面与
        // call_closure 的用户函数面共用(形状唯一化见 reference/error-message-style.md)。三形态三名
        // (不共用重载:三者是三种约束语义、各对应一种措辞串):精确数 / 闭区间「<lo> or <hi> arguments」
        // / 仅下界「at least <lo> ...」。**比较留在各调用点**(热路径零额外调用),只在失败路径进本口。
        // 被调者一律报泛称 function,不收名字:真名不可靠(匿名函数无区分力;绑定方法/函数值被取出后,
        // 真名不在调用点上出现),而直调场合的名字本就在调用点上。
        [[nodiscard]]
        FailSignal arity_error(usize argc, usize expected);

        [[nodiscard]]
        FailSignal arity_error_range(usize argc, usize low, usize high);

        [[nodiscard]]
        FailSignal arity_error_at_least(usize argc, usize low);

        // 模块表:键 = 规范路径 ObjString*(intern),值 = ObjModule*(均装箱为 Value)。
        [[nodiscard]]
        AriaHashTable& modules() noexcept {
            return modules_;
        }

        // Object 根类:寄存器 ObjectClass 唯一存放(LOAD_REG 直推);不进 builtins_/任何模块
        // globals(用户 shadow 全局名免疫)。定义在 .cpp(Object::as 需 ObjClass 完整类型,
        // 头内只留声明,同 cur_cu 先例)。
        [[nodiscard]]
        ObjClass* object_class() const noexcept;

        // Exception bootstrap 类:寄存器 ExceptionClass 唯一存放,方法面宿主类
        // ExceptionClass(runtime/builtins/);ObjException::load_field 经它取自身类。
        // 同 object_class 先例。与其余 bootstrap 类不同:经 builtins_ 的 "Exception" 键
        // 暴露(裸名可达,可被子类化)。
        [[nodiscard]]
        ObjClass* exception_class() const noexcept;

        // Iterator bootstrap 类:寄存器 IteratorClass 唯一存放,方法面宿主类
        // IteratorClass(runtime/builtins/);ObjIterator::load_field
        // 经它取自身类。同 object_class 先例。
        [[nodiscard]]
        ObjClass* iterator_class() const noexcept;

        // List bootstrap 类:寄存器 ListClass 唯一存放,方法面宿主类 ListClass
        // (runtime/builtins/);ObjList::load_field 经它取自身类。同 object_class 先例。
        [[nodiscard]]
        ObjClass* list_class() const noexcept;

        // Map bootstrap 类:寄存器 MapClass 唯一存放,方法面宿主类 MapClass
        // (runtime/builtins/);ObjMap::load_field 经它取自身类。同 object_class 先例。
        [[nodiscard]]
        ObjClass* map_class() const noexcept;

        // String bootstrap 类:寄存器 StringClass 唯一存放,方法面宿主类 StringClass
        // (runtime/builtins/);ObjString::load_field 经它取自身类。同 object_class 先例。
        [[nodiscard]]
        ObjClass* string_class() const noexcept;

        // Range bootstrap 类:寄存器 RangeClass 唯一存放,方法面宿主类 RangeClass
        // (runtime/builtins/);ObjRange::load_field 经它取自身类。同 object_class 先例。
        [[nodiscard]]
        ObjClass* range_class() const noexcept;

        // 值寄存器组按格位直读(格位常量 k<名字>Offset 见 runtime/value_register.hpp)。内置类型的
        // 算子实现格(String*Fn)经它取用:ObjString 的 5 个 op_*_impl override 直读实现格交算子
        // 派发,免每次过类表查找。
        [[nodiscard]]
        Value register_value(const u8 offset) const noexcept {
            return Value::from_obj(registers_[offset]);
        }

        // 常量串表按枚举直读(下标契约与拼写见 runtime/string_constant.hpp):实例侧算子派发经它取钩子名,
        // 免每次 new_string 从驻留池换串(名字由 bootstrap_string_constants 填好并随本表标根)。
        [[nodiscard]]
        ObjString* string_constant(const StringConstant id) const noexcept {
            ASSERT(std::to_underlying(id) < std::size(kStringConstantSpellings), "StringConstant out of range");
            return string_constants_[std::to_underlying(id)];
        }

        // 常量串表按执行状态直读(status 的返回值,五拼写与 to_string(ExecState) 同源)。
        [[nodiscard]]
        ObjString* string_constant(const ExecState state) const noexcept {
            switch (state) {
                case ExecState::Suspended:
                    return string_constant(StringConstant::CoSuspended);
                case ExecState::Normal:
                    return string_constant(StringConstant::CoNormal);
                case ExecState::Running:
                    return string_constant(StringConstant::CoRunning);
                case ExecState::Done:
                    return string_constant(StringConstant::CoDone);
                case ExecState::Failed:
                    return string_constant(StringConstant::CoFailed);
            }
            UNREACHABLE();
        }

        // 源根列表(语义对齐 Python sys.path):裸名导入的搜索根,解析器沿各源根找
        // <源根>/<spec>.aria 首个存在者命中(详见 import-path-resolution.md)。模块表键为
        // 命中文件绝对规范路径,源根不进键。List<String> 路径元数据,不参与 GC 追踪。
        [[nodiscard]]
        const List<String>& source_roots() const noexcept {
            return source_roots_;
        }

        // 覆盖配置源根(stdlib/-L/环境变量等):替换 [1..]、保留入口槽 [0](由 run() 按入口
        // 模块 dir_ 原地替换)。测试/嵌入配置用:置空清掉默认 stdlib(隔离)。
        void set_source_roots(List<String> roots) noexcept;

    private:
        // 执行本体(无入口装饰):压 callee 经 call_closure 进帧后 dispatch_loop。亦是未来重入
        // 接缝(原生回调调 aria 函数/嵌入宿主,§4.7):不播源根、不 reset(重入调用者的栈不可
        // 冲掉)、不断言主上下文;落地升公开时需 dispatch_loop 按基线帧深退出。
        Result<Value, Error> run_closure(ObjClosure* closure);

        // 主循环:驱动 *current_ 直到顶层返回/错误/显式停止。
        Result<Value, Error> dispatch_loop();

        // IMPORT 执行体(path 已读出):resolve_module 解析 -> new_string intern 后 guard 根化 ->
        // modules_ 查表(命中复用压栈,命中体执行中的对象即循环导入)-> 未命中 load_module 后
        // 以 entry 现场包闭包经 call_closure 进帧 run-once;两分支栈效应统一 [..., module],
        // 无递归 dispatch_loop()。bool 契约同 call_* 族:false ⟺ 载荷已 raise(解析/加载/进帧
        // 三类失败点),unwind 留 dispatch_loop 调用点;仅限 dispatch_loop 驱动期调用。
        bool run_import(const ObjString* path);

        // IMPORT 未命中分支的加载层:读盘 -> 派生身份 -> new_module -> 编译(入口名 <module>)
        // -> **编译成功才入表** -> 返回模块对象(已 set_entry)。只加载与编译,不执行模块体 --
        // nullptr ⟺ 载荷已 raise(读盘失败/名字无效经 fail,被导入模块的编译期 Error 原样装配箱
        // 透传)。失败不留表项,同路径重试重新加载。**仅限 dispatch_loop 驱动期调用**(寄存器
        // 随 *current_ 走,run() 入口 reset 会吞掉)。canonical_path 须调用方已根化
        // (modules_.set 的 rehash 触 GC,intern weak root 不保命)。
        ObjModule* load_module(ObjString* canonical_path, StringView import_specifier);

        // interpret 共用尾段:编译 + 执行;失败渲染 stderr 并按**失败阶段**分类 -- 编译期 ->
        // CompileError,run 期 -> RuntimeError(含经异常通道传播、可被 try/catch 捕获的被导入
        // 模块编译错,故不构成 CompileError)。
        InterpretResult interpret_run(SourceFile& source, ObjModule* module);

        // CALL 分发:栈顶形如 [callee, a1..aN]。按 callee 类型分派到对应 call_* 子例程;其余对象
        // 类型按调用钩子 `__call__` 取实现(Object::op_call_impl)后递归分发(措辞随宿主);非对象
        // callee 直接报 CallNonCallable。**本文件 call_* 族的统一契约:return false/nullptr ⟺
        // 错误载荷已 raise 进 *current_ 挂起错误寄存器**,调用方 take_error 取出沿 runtime_err 传播。
        bool call_value(Value callee, u8 argc);

        // 闭包调用的进帧单点(call_value 分发与 run_closure 共用),只编排:元数检查
        // (check_arity)-> 帧余量检查 -> 实参整形(prepare_call_args)再进帧(callee 在槽 0,
        // 参数即局部槽 1..n)。失败 raise WrongArity/StackOverflow。
        bool call_closure(ObjClosure* obj, u8 argc);

        // call_closure 辅助:元数检查。普通函数区间 [min_arity, arity](差额为带默认值
        // 参数),无缺省报单数文案、有缺省报区间文案;varargs 函数只保下界(多余实参由
        // prepare_call_args 打包进 rest,无上界,上限即 CALL 操作数 u8)。文案经 arity_error
        // 族构造(措辞家族唯一口),被调者位报泛称 function:真名在一等公民下不可靠(匿名函数无
        // 区分力;被取出赋值的函数/绑定方法,其真名不在调用点上出现),归属交给 at 行的调用点
        // (arity 失败在进帧检查处,被调帧未进,at 行只到调用者帧)。
        bool check_arity(const ObjFunction* fn, u8 argc);

        // call_closure 辅助:实参整形 --把调用区栈顶从实参深度整形成帧参数槽深并返回槽深。
        // ①缺省垫充:未传的固定参数槽压入缺省印章,栈顶补齐到固定参数深度(体局部槽号按满参编,
        // 参数槽 1..n、体局部自 n+1 起);②varargs 打包:超出固定参数数的实参整段收集为新 list
        // 压入 rest 槽(帧槽深 = arity + 1;无多余实参铸空表 --rest 恒为 list 非 nil)。
        u8 prepare_call_args(const ObjFunction* fn, u8 argc);

        // 原生函数调用:同步调 obj->fn(),不进帧;bool 契约透传(契约见 ObjNativeFn.hpp)。
        bool call_native(const ObjNativeFn* obj, u8 argc);

        // 协程切换原语对(切换序列的唯一实现处:状态改写 + 链接/解链 + current_ 换指不再由
        // 调用方手拼;恢复者恒取 current_):
        //   enter_coroutine -- 进入协程 coroutine:置链(coroutine -> current_)、双方状态换位
        //   (coroutine Running / current_ Normal)、换指。
        //   leave_coroutine -- current_ 让位回恢复者(previous_):本侧置 departing_state 并解
        //   链、恢复者置 Running、换指。yield(Suspended)/RETURN 完成(Done)/未捕获跳链
        //   (Failed,链式 unwind 共用)三个回切方向收敛于此;让位方的 reset/载荷善后归调用方。
        void enter_coroutine(ObjMovement* coroutine);

        void leave_coroutine(ExecState departing_state);

        // 类实例化:new_instance 为唯一 GC 点,建成即写 callee 槽 -- **槽 0 原位换实例**(即
        // 新帧的 this),余下交 call_value 通用分发(init 恒有值)。
        bool call_class(ObjClass* obj, u8 argc);

        // 绑定方法调用:槽 0 原位覆写为 receiver(this 替代 callee,实参槽位不动),余下交
        // call_value 分发。方法值无需守卫:覆写后经类表槽/缓存可达。
        bool call_bound_method(const ObjBoundMethod* obj, u8 argc);

        // 值寄存器组 bootstrap 编排(ctor 一次调用):逐格初始化全部 VM 单例对象。须在 ctor
        // 构造临界区(GC 挂起)内调用,创建免守卫;各单例收口在 bootstrap_<单例> 系列函数,本
        // 函数只管编排;新单例在此加一行编排。
        void bootstrap_registers();

        // 常量串表 bootstrap(ctor 一次调用,须先于 bootstrap_registers):按注册表
        // (runtime/string_constant.hpp)逐条驻留填入 string_constants_。String 类 bootstrap 的
        // 钩子缓存要按名取串,故编排上必须先于它。
        void bootstrap_string_constants();

        // Object 根类 bootstrap:建 ObjClass("Object", super=nullptr),经方法面宿主类
        // ObjectClass::register_methods 注册,发布进寄存器 ObjectClass 格。
        void bootstrap_object_class();

        // Exception bootstrap 类:建 ObjClass("Exception", super=Object 根),经方法面宿主类
        // ExceptionClass::register_methods 注册,发布进寄存器 ExceptionClass 格;末段经
        // builtins_ 的 "Exception" 键暴露。
        void bootstrap_exception_class();

        // Iterator bootstrap 类:建 ObjClass("Iterator", super=Object 根),经方法面宿主类
        // IteratorClass::register_methods 注册,发布进寄存器 IteratorClass 格。
        void bootstrap_iterator_class();

        // List bootstrap 类:建 ObjClass("List", super=Object 根),经方法面宿主类
        // ListClass::register_methods 注册,发布进寄存器 ListClass 格;末段按公开清单
        // ListClass::kOperatorFns 把类表里的两个算子实现拷进寄存器实现格。
        void bootstrap_list_class();

        // Map bootstrap 类:建 ObjClass("Map", super=Object 根),经方法面宿主类
        // MapClass::register_methods 注册,发布进寄存器 MapClass 格。
        void bootstrap_map_class();

        // String bootstrap 类:建 ObjClass("String", super=Object 根),经方法面宿主类
        // StringClass::register_methods 注册,发布进寄存器 StringClass 格;末段按公开清单
        // StringClass::kOperatorFns 把类表里的六个算子实现拷进寄存器实现格。
        void bootstrap_string_class();

        // Range bootstrap 类:建 ObjClass("Range", super=Object 根),经方法面宿主类
        // RangeClass::register_methods 注册,发布进寄存器 RangeClass 格。
        void bootstrap_range_class();

        // 缺参印章 bootstrap:铸私有 no-op native 入寄存器 DefaultMark 格。身份判等的未传槽
        // 标记,不注册 builtins/任何表 -- 用户不可达,不可伪造是印章方案的长期不变式。
        void bootstrap_default_mark();

        // match 兜底异常 bootstrap:铸共享 ObjException(MatchNoArm,消息静态)入寄存器
        // MatchNoArm 格。全臂未命中由字节码 LOAD_REG + THROW 抛出,同一对象身份恒一。
        void bootstrap_match_no_arm();

        // <coroutine> 合成模块 bootstrap:建模块(不入 modules_ 表)并经 CoroutineModule::
        // register_functions 装载协程方法面,再以 "coroutine" 键注册进 builtins_。须在 ctor
        // 构造临界区内调用(GC 挂起,创建免守卫)。
        void bootstrap_coroutine_module();

        // VM 根 tracer 挂接(ctor 一次调用):gc_.set_vm_roots 挂标根闭包;标根清单见
        // runtime.md「共享状态」。执行上下文只标 current_ 一点,各上下文内部与 previous_ resume
        // 链由 ObjMovement::trace 经对象图级联。
        void hook_vm_roots();

        // 源根默认值初始化(ctor 一次调用):入口槽 [0] 占位 cwd + 配置根 [1..] = 编译器相对
        // stdlib 源根。
        void init_source_roots();

        // 按 Op 取本对象的算子实现(编译期分发到 Object::op_*_impl,Op 由调用点穷举);nullopt ⟺
        // 已 fail(措辞随宿主)。模板成员定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        Opt<Value> get_obj_binary_op_impl(Object& obj);

        // 弹 2 算 1:栈顶两值的二元数值运算入口(9 个算术/比较指令共用,Op 由调用点穷举实例化)--
        // 类型守卫后按**域**分流:双 Int 走 run_binary_int、任一 F64 升浮点走 run_binary_f64(两域
        // 失败语义不同,实现各住自己的辅助方法)。bool 契约同 call_value;模板成员定义在 .cpp
        //(实例化点全在本 TU)。
        template<OpCode Op>
        bool run_binary_numeric();

        // 整数域九算子(结果压栈):除/模零是域特有失败,就地 fail;% 为 C++ 语义。
        template<OpCode Op>
        [[nodiscard]]
        bool run_binary_int(i64 lhs, i64 rhs);

        // 浮点域九算子(结果压栈):按 IEEE,除零得 inf/nan、% 走 fmod,无失败路径。
        template<OpCode Op>
        [[nodiscard]]
        bool run_binary_f64(f64 lhs, f64 rhs) const;

        // 九个二元算子(ADD/SUBTRACT/MULTIPLY/DIVIDE/MOD 与四个比较指令)的执行体,也是算术/比较
        // 两个 block 各自的唯一入口:非对象左值委托 run_binary_numeric;对象左值先经
        // get_obj_binary_op_impl<Op> 取本对象的算子实现,再交 call_value 调(调用区 [lhs, rhs] 即
        // [this, arg1],槽 0 保持 receiver)。peek 不弹 -- receiver 占调用区槽 0(栈即根)跨实现内
        // 分配与 miss fail。
        template<OpCode Op>
        bool run_binary_operator();

        // NEGATE 执行体:[v] -> [r]:整数/浮点就地取负,对象左值取 __neg__ 实现后调用(一元恒零
        // 实参,调用区 [v] 即 [this]);其余类型报 InvalidOperand。契约同上。
        bool run_negate();

        // 类与对象:field 族指令执行体
        // bool 契约同 call_value:失败载荷已在寄存器(对象协议失败由 override 内 vm.fail 就地
        // 烘焙,非对象守卫由执行体 fail)。peek 不弹 -- 协议内分配跨 GC 须 obj 在栈(「栈即根」)。

        // LOAD_FIELD 执行体(name 已读出):peek obj,经 Object::load_field 协议解析,结果写回
        // 原槽([obj] -> [v]);非对象(含 nil)是协议外原语,文案留执行体,对象 miss 文案由协议
        // override 烘焙。
        bool run_load_field(ObjString* name);

        // STORE_FIELD 执行体:[obj, v] -> [v](完成时单槽下移留 v -- 赋值表达式约定)。经
        // Object::store_field 协议。
        bool run_store_field(ObjString* name);

        // THIS 对执行体:this 取顶帧槽 0(方法帧形 [this, a1..aN]),this 不经值栈,与 obj.m 同走
        // field 协议。槽 0 非实例 = 方法原值经自由调用/类 receiver 直调掏空 this(经 call_closure
        // 的语言写法不可达),响亮 TypeMismatch 兜底(帧形不变式只对 call_closure 成立)。
        bool run_load_this_field(ObjString* name);

        // STORE 腿:[v] -> [v](peek-store 经 this 的 store_field,实例字段动态即创建)。非实例
        // 兜底同 LOAD 腿,文案 field assignment 形态。
        bool run_store_this_field(ObjString* name);

        // PREPARE_METHOD 执行体(两段式第一段,name 已读出):[recv] -> [recv, target]。接收者在栈顶
        // (实参尚未求值),经 Object::load_field_unbound 协议解析此刻完成(非对象守卫文案留执行体,
        // 同 run_load_field);待调值压栈跨指令存活(栈即根),实参随后压其上,由 CALL_METHOD 收口。
        // 解析先于实参求值。
        bool run_prepare_method(ObjString* name);

        // CALL_METHOD 执行体(两段式第二段,argc 已读出):[recv, target, a1..aN] -> [r]。待调值在
        // peek(argc)、接收者在 peek(argc + 1);实参整体下移一格补掉待调值占的那格,得调用区
        // [recv, a1..aN](槽 0 = receiver = this),再交 call_value 统一分发。纯调用,不再解析。
        bool run_call_method(u8 argc);

        // LOAD_SUPER_FIELD 执行体:defining class 取顶帧 closure 直读(方法闭包恒有戳,编译器
        // 不变式 ASSERT 钉),从其父类起走 ObjClass::load_field 沿链读穿透(不含 defining 自身,
        // 类协议不绑定不缓存)。命中方法闭包 -> 绑 this=帧槽 0 压栈供 CALL;其余原值直读。全链
        // miss 为语言可达错误,经协议 fail。
        bool run_load_super_field(ObjString* name);

        // 下标族指令执行体(LOAD/STORE_INDEX):契约同 field 族

        // LOAD_INDEX 执行体(操作数全在栈上):peek (obj, idx) 经 Object::load_index 协议,
        // 结果写回 obj 槽再弹 idx([obj, idx] -> [v]);非对象(含 nil)文案留执行体,对象侧
        // 越界/键类型文案由 override 就地烘焙。
        bool run_load_index();

        // STORE_INDEX 执行体:peek (obj, idx, v) 经 Object::store_index 协议,完成时值下移
        // 两格留 v([obj, idx, v] -> [v],peek-store -- 赋值表达式约定)。
        bool run_store_index();

        // range 构造指令执行体(MAKE_RANGE):契约同 field 族

        // MAKE_RANGE 执行体(flags 已读出,位义见 code.hpp kRangeFlag*):有界 [from, to] -> [range]、
        // 无上界 [from] -> [range]。端点须为整数,非整数 fail TypeMismatch(静态文案不插端点值);
        // 端点 peek 在栈跨 new_range 顶部 maybe_collect(「栈即根」),铸完 drop 再 push。
        bool run_make_range(u8 flags);

        // 取走 *current_ 挂起载荷(清空寄存器),拆为 (码, 完整烘焙消息) 两件:ObjException 直取自身
        // 码与 message_(re-throw 保码,坑 #7);其它载荷(用户 throw 的任意值)兜底 UncaughtException。
        // 两个消费点(run_closure 进帧失败 / unwind 未捕获出口)均经 Error::from_baked 一次物化 --
        // Error 只在边界成型。
        Pair<ErrorCode, String> take_uncaught_error() const;

        // 异常派发与未捕获物化,链式多跳:自最内帧向外按 last_ip 纯搜索本上下文各帧 CodeUnit
        // 异常记录表(find_try_handler 取最内层覆盖),不动帧栈/值栈;未命中帧记跟踪三元组
        //(fn/mod/ip_off;每跳重新收集,死在边界的协程帧不并入 -- 跟踪截断在协程边界)。命中:
        // unwind_to_handler 回退到命中帧并转入 catch handler(统一在 ObjMovement),返 nullopt
        //(调用方 break 回循环顶自 current_ 重取帧,坑 #11;命中可能在多跳之后,current_ 已非进入
        // unwind 时的上下文);本上下文全帧未命中且在 resume 链上 -> 让位方先 take 载荷再 reset、
        // leave_coroutine 置 Failed 解链换指、caller raise 载荷续搜(中间层连死),逐跳向链根
        // 推进;链终止 = 主上下文 -> reset 一次清场,从寄存器反提载荷拆 (码, 烘焙消息),逐帧烘焙
        // at 跟踪行进消息尾部(渲染内->外),经 Error::from_baked 一次物化返回。前提:寄存器已有
        // 载荷(入口断言把关);帧内 last_ip 由 dispatch_loop 循环顶写(顶帧 = 故障指令,外层帧 =
        // CALL 站点)。
        Opt<Error> unwind();

        GC gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)

        // 当前执行上下文:dispatch_loop/call_value 族/raise 的作用对象;ctor 首笔 GC 分配、随
        // ~GC 的 free_all_ 释放(GC 对象,内部与 previous_ 链经 trace 级联标根)。主上下文即其
        // 初值(M6 切换落地后为 resume 链链根,由 run() 入口锚 + 出口断言钉住),切换模型见
        // vm-design.md §4.9。
        ObjMovement* current_;

        AriaHashTable modules_;  // 模块表(GC 根)
        AriaHashTable builtins_; // 只读 builtins 表(GC 根)

        // 源根列表:[0]=入口槽(cwd 占位,run() 换成入口 dir_),[1..]=配置根(stdlib/-L/环境变量)。
        List<String> source_roots_;

        // 值寄存器组:VM 单例对象统一存放表(唯一存放处;注册表见 runtime/value_register.hpp)。
        // 构造期预置表长格,bootstrap 按 k<名字>Offset 具名格位逐格填,填完经 assert_slots_filled
        // 收口;tracer 一趟循环标根。格位恒持对象,故元素类型即消费者要的裸指针。
        List<Object*> registers_;

        // 常量串表:VM 自己按名取用的字符串常量(唯一存放处;注册表见 runtime/string_constant.hpp)。
        // 构造期预置表长格,bootstrap 按下标(枚举值)逐格驻留填,同样经 assert_slots_filled 收口;
        // tracer 一趟循环 mark_object 标根 -- 表在则串在(驻留池是 weak root,不标根则下轮 collect 即摘除)。
        List<ObjString*> string_constants_;

#ifdef ARIA_OPCODE_PROFILE
        // 指令频度探针(仅探针构建):计数状态自持,析构时按 ARIA_OPCODE_STATS 环境变量门控 dump。
        OpcodeProfiler opcode_profiler_;
#endif
    };

    // move 删除被移除时在此炸出,防静默变可移动后的悬垂 UB。
    static_assert(!std::is_move_constructible_v<AriaVM>);
    static_assert(!std::is_move_assignable_v<AriaVM>);

} // namespace aria

#endif // ARIA_VM_HPP
