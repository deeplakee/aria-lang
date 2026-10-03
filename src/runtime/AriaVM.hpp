#ifndef ARIA_VM_HPP
#define ARIA_VM_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
// raise 模板头内内联装箱需 ObjException 完整类型(其依赖已经 GC.hpp 传递拉入)。
#include "object/ObjException.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/opcode_profile.hpp"
#include "runtime/str_table.hpp"
#include "runtime/value_register.hpp"
#include "util/util.hpp"
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

    // 前置声明(run_binary_numeric<Op> 模板形参用):免头文件拖入 bytecode 树,定义处 .cpp 已含。
    enum class OpCode : u8;

    // fail 的返回哨兵,失败出口惯用法一行 `return vm.fail(...);`。**Opt<T> 转换只对「不可由 bool
    // 构造」的 T 成立**--标量经 operator bool() 会构造出已初始化的 0/false 把 fail 吞成成功,
    // 标量返回型站点改用 bool + 出参(先例:StringClass 算子钩子契约返 bool、结果写 slots[0])。
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
        CompileError, // 编译失败(词法/语法/语义)
        RuntimeError, // 运行期未捕获错误
        LoadError,    // 源文件加载失败(仅 interpret_from_path:I/O 或 UTF-8 编码)
    };

    // 解释器:驱动 ObjMovement 执行字节码,循环状态全取自 *current_;GC 根经 tracer 注册进自有
    // gc_(gc_ 声明居首,析构逆序下 tracer 与成员同生共死)。运行时错误统一 raise 入寄存器、
    // 经 unwind 查 CodeUnit 异常记录表派发。
    class AriaVM {
    public:
        AriaVM();

        AriaVM(const AriaVM&)            = delete;
        AriaVM& operator=(const AriaVM&) = delete;

        // VM 不可移动:成员持指向彼此/自身的指针(modules_ borrow &gc_、current_ 指 GC 对象、
        // tracer 捕 [this]),move 后不重绑即悬垂;就地构造或以 unique_ptr 持有。
        AriaVM(AriaVM&&)            = delete;
        AriaVM& operator=(AriaVM&&) = delete;

        // 协程模块方法面宿主:四原语经友元访问 check_arity/prepare_call_args/current_ 私有面。
        friend class CoroutineModule;

#ifdef ARIA_OPCODE_PROFILE
        // 指令频度探针(仅探针构建):挂载为成员,友元取 current_ 等私有状态;唯一探测点 =
        // dispatch_loop 取指行宏,本类其余代码零探针痕迹。
        friend class OpcodeProfiler;
#endif

        // 入口执行:锚定入口上下文(出口断言 current_ 回锚点,漏切换交接当场炸) + 源根入口槽
        // [0] 播种 + 前后 reset 清场,入口 fn 包空闭包后 run_closure;CodeUnit 假定良构,失败为
        // 未捕获运行时错误。
        Result<Value, Error> run(ObjFunction* fn);

        // 编译 source 到 module 的入口 ObjFunction 并执行;source 须存活到返回(编译期 Error 的
        // SourceLoc 指着它),module 无需调用方守(编译期已 make_guard 根化)。失败为首错 Error 透传。
        Result<Value, Error> run(SourceFile& source, ObjModule* module);

        // 编译并执行源码字符串:合成 <script> 入口模块(dir_=cwd);错误渲染 stderr,返 InterpretResult。
        InterpretResult interpret_from_src(StringView src);

        // 编译并执行源文件:读盘失败渲染并返 LoadError;入口模块 name=basename 去 .aria、
        // dir_=dirname(absolute(path))。
        InterpretResult interpret_from_path(StringView path);

        [[nodiscard]]
        GC& gc() noexcept {
            return gc_;
        }

        // 当前执行上下文(测试/白盒观察用;协程切换期间随之换指,恒等于正在执行字节码的上下文)。
        [[nodiscard]]
        ObjMovement* current_context() noexcept {
            return current_;
        }

        // 挂起错误侧信道:寄存器在 ObjMovement::pending_error_,经 *current_ 落进调用者上下文;
        // 消息烘焙不含位置前缀(位置由 unwind 未捕获出口的逐帧 at 行给出)。raise 内 new_exception
        // 分配可能触 GC,载荷构造后立即入寄存器(tracer 已标根);fail = raise + 恒返失败信号,
        // [[nodiscard]] 强制 `return vm.fail(...);`。
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

        // 元数报错唯一口(「function expects <spec>, got <n>」措辞家族):三形态三名各对应一种
        // 措辞;比较留在各调用点,仅失败路径进本口。被调者报泛称 function 不收名字 -- 真名在
        // 一等公民下不可靠(匿名无区分力、被取出后真名不出现在调用点),at 行已交待调用点。
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

        // 七个 bootstrap 类访问器:各类格唯一存于寄存器组。
        [[nodiscard]]
        ObjClass* object_class() const noexcept;

        [[nodiscard]]
        ObjClass* exception_class() const noexcept;

        [[nodiscard]]
        ObjClass* iterator_class() const noexcept;

        [[nodiscard]]
        ObjClass* list_class() const noexcept;

        [[nodiscard]]
        ObjClass* map_class() const noexcept;

        [[nodiscard]]
        ObjClass* string_class() const noexcept;

        [[nodiscard]]
        ObjClass* range_class() const noexcept;

        // 寄存器组按格位直读(k<名字>Offset 格位常量):内置类型算子实现格经它取用,免每次类表查找。
        [[nodiscard]]
        Value register_value(const u8 offset) const noexcept {
            return Value::from_obj(registers_[offset]);
        }

        // 常量串表按键直读(本类唯一访问口):键经 NTTP 编译期定下标、static_assert 拒表外键,
        // 生成物是基址 + 常量下标的一次加载;实例侧算子派发取钩子名,免每次从驻留池换串。
        template<util::FixedString key>
        [[nodiscard]]
        ObjString* str() const noexcept {
            constexpr auto found = str_table::index_of(key.view());
            static_assert(found.has_value(), "constant string key is not in the registry");
            return string_constants_[*found];
        }

        // 裸名导入搜索根(语义对齐 Python sys.path):解析器按 <源根>/<spec>.aria 首个存在者命中;
        // 模块表键为命中文件绝对规范路径,源根不进键。List<String> 路径元数据,不参与 GC 追踪。
        [[nodiscard]]
        const List<String>& source_roots() const noexcept {
            return source_roots_;
        }

        // 覆盖配置源根:替换 [1..]、保留入口槽 [0](run() 按入口模块 dir_ 原地替换);当前仅测试
        // 消费,属嵌入预留面。
        void set_source_roots(List<String> roots) noexcept;

    private:
        // 执行本体(无入口装饰):压 callee 进帧后 dispatch_loop;唯一不 reset、不播源根、不断言
        // 主上下文的执行口(重入路径调用者的栈不可冲掉),「切换型原生不得在嵌套 run_closure 内
        // 可达」红线据此立。
        Result<Value, Error> run_closure(ObjClosure* closure);

        // 主循环:驱动 *current_ 直到顶层返回/错误/显式停止。
        Result<Value, Error> dispatch_loop();

        // IMPORT 执行体:resolve_module -> intern 后 guard 根化查 modules_(命中复用即循环导入),
        // 未命中 load_module 后包闭包进帧 run-once;两分支栈效应统一 [..., module]。bool 契约同
        // call_* 族(false ⟺ 载荷已 raise:解析/加载/进帧),仅限 dispatch_loop 驱动期调用。
        bool run_import(const ObjString* path);

        // IMPORT 未命中分支的加载层:读盘 -> 派生身份 -> new_module -> 编译 -> **编译成功才入表**,
        // 返已 set_entry 的模块,不执行模块体。nullptr ⟺ 载荷已 raise;失败不留表项,同路径重试
        // 重加载。仅限 dispatch_loop 驱动期调用,canonical_path 须调用方已根化(根化跨的是本层
        // 编译期分配,intern weak root 不保命)。
        ObjModule* load_module(ObjString* canonical_path, StringView import_specifier);

        // interpret 共用尾段:编译 + 执行,失败渲染 stderr 并按失败阶段分类(编译期 -> CompileError,
        // run 期 -> RuntimeError -- 被导入模块编译错经异常通道传播、可被 try/catch 捕获,故非 CompileError)。
        InterpretResult interpret_run(SourceFile& source, ObjModule* module);

        // CALL 分发:栈顶形如 [callee, a1..aN],按 callee 类型分派 call_* 子例程;其余对象按调用
        // 钩子 __call__ 取实现后递归分发,非对象直接报 CallNonCallable。**call_* 族统一契约:
        // false/nullptr ⟺ 错误载荷已 raise 进 *current_ 挂起错误寄存器**,调用方 take_error 传播。
        bool call_value(Value callee, u8 argc);

        // 闭包调用进帧单点(call_value 与 run_closure 共用):元数检查 -> 帧余量检查 -> 实参整形
        // 后进帧。失败 WrongArity/StackOverflow。
        bool call_closure(ObjClosure* obj, u8 argc);

        // call_closure 辅助:元数检查(纯读无分配)。普通函数区间 [min_arity, arity](差额为带
        // 默认值参数),有缺省报区间文案、无缺省报单数;varargs 只保下界,多余实参由
        // prepare_call_args 打包。at 行只到调用者帧(被调帧未进)。
        bool check_arity(const ObjFunction* fn, u8 argc);

        // call_closure 辅助:实参整形,把调用区栈顶从实参深度整形成帧参数槽深并返回槽深。
        // ①缺省垫充:未传的固定参数槽压缺省印章(体局部槽号按满参编,参数槽 1..n、体局部自
        // n+1 起);②varargs 打包:多余实参整段收集为新 list 压 rest 槽(帧槽深 = arity + 1,
        // 无多余实参铸空表 -- rest 恒为 list 非 nil)。
        u8 prepare_call_args(const ObjFunction* fn, u8 argc);

        // 原生函数调用:同步调 obj->fn(),不进帧;bool 契约透传。
        bool call_native(const ObjNativeFn* obj, u8 argc);

        // 协程切换原语对(切换序列唯一实现处,状态/链/换指不再由调用方手拼):leave_coroutine 收敛
        // yield / RETURN 完成 / 未捕获跳链三个回切方向;让位方的 reset/载荷善后归调用方。
        void enter_coroutine(ObjMovement* coroutine);

        void leave_coroutine(ExecState departing_state);

        // 类实例化:new_instance 为唯一 GC 点,建成即槽 0 原位换实例(即新帧 this),余下交
        // call_value 通用分发。
        bool call_class(ObjClass* obj, u8 argc);

        // 绑定方法调用:槽 0 原位覆写为 receiver(this 替代 callee,实参槽位不动),余下交
        // call_value 分发。方法值无需守卫:覆写后经类表槽/缓存可达。
        bool call_bound_method(const ObjBoundMethod* obj, u8 argc);

        // 值寄存器组 bootstrap 编排(ctor 一次调用):逐格初始化全部 VM 单例对象;须在 ctor 构造
        // 临界区(GC 挂起)内调用,创建免守卫。
        void bootstrap_registers();

        // 常量串表 bootstrap(ctor 一次调用):按注册表逐条驻留填入 string_constants_;须先于
        // bootstrap_registers -- String 类钩子缓存按名取串读的就是本表。
        void bootstrap_string_constants();

        // VM 根 tracer 挂接(ctor 一次调用):只标 current_ 一点,各上下文内部与 previous_ resume
        // 链经 ObjMovement::trace 级联。
        void hook_vm_roots();

        // 源根默认值初始化(ctor 一次调用):入口槽 [0] 占位 cwd + 配置根 [1..] = 编译器相对
        // stdlib 源根。
        void init_source_roots();

        // 按 Op 取本对象的算子实现(编译期分发到 Object::op_*_impl,Op 由调用点穷举);nullopt ⟺
        // 已 fail(措辞随宿主)。模板成员定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        Opt<Value> get_obj_binary_op_impl(Object* obj);

        // 弹 2 算 1 的二元数值运算入口(9 个算术/比较指令共用,Op 由调用点穷举实例化):类型守卫后
        // 按域分流,双 Int 走 run_binary_int、任一 F64 升浮点走 run_binary_f64(两域失败语义不同);
        // 定义在 .cpp(实例化点全在本 TU)。
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

        // 九个二元算子的执行体(算术/比较 block 唯一入口):两侧均 String 走 run_string_binary 就地
        // 完成;其余非对象左值委托 run_binary_numeric,对象左值取算子实现后交 call_value(调用区
        // [lhs, rhs] 即 [this, arg1])。peek 不弹 -- receiver 占调用区槽 0(栈即根)跨实现内分配。
        template<OpCode Op>
        bool run_binary_operator();

        // string 快路径派发门(lhs 非空,调用方已判 is_obj;白名单收在本方法:ADD 与四个字序比较,
        // 其余算子恒 false):两侧均 String 交 run_string_binary 就地完成;返回 true = 已派发,
        // false = 不适用落原路(非失败)。定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        bool try_string_fast_dispatch(Object* lhs);

        // string 算子快路径(白名单收在 try_string_fast_dispatch):[lhs, rhs] -> [r],就地拼接/按
        // 无符号字节序比较,语义与 StringClass 对应钩子逐位一致;定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        bool run_string_binary(ObjString* lhs, ObjString* rhs);

        // NEGATE 执行体:[v] -> [r]:整数/浮点就地取负,对象左值取 __neg__ 实现后调用(一元恒零
        // 实参,调用区 [v] 即 [this]);其余类型报 InvalidOperand。契约同上。
        bool run_negate();

        // field 族指令执行体;bool 契约同 call_value(对象协议失败由 override 内 vm.fail 就地烘焙,
        // 非对象守卫由执行体 fail),peek 不弹 -- 协议内分配跨 GC 须 obj 在栈(栈即根)。

        // LOAD_FIELD 执行体:[obj] -> [v],经 load_field_bound 协议解析写回原槽;非对象守卫文案
        // 留执行体,对象 miss 文案由协议 override 烘焙。
        bool run_load_field(ObjString* name);

        // STORE_FIELD 执行体:[obj, v] -> [v](完成时单槽下移留 v -- 赋值表达式约定)。经
        // Object::store_field 协议。
        bool run_store_field(ObjString* name);

        // THIS 对执行体:this 取顶帧槽 0,this 不经值栈与 obj.m 同走 field 协议。槽 0 非实例 = this
        // 被自由调用/类直调掏空(经 call_closure 的写法不可达),响亮 TypeMismatch 兜底(帧形
        // 不变式只对 call_closure 成立)。
        bool run_load_this_field(ObjString* name);

        // 写入执行体:[v] -> [v](peek-store 经 this 的 store_field,实例字段动态即创建)。非实例
        // 兜底报错同读取执行体,文案用 field assignment 形态。
        bool run_store_this_field(ObjString* name);

        // PREPARE_METHOD 执行体(两段式第一段):[recv] -> [recv, target]。接收者在栈顶、解析
        // 先于实参求值;待调值压栈跨指令存活(栈即根),由 CALL_METHOD 收口。
        bool run_prepare_method(ObjString* name);

        // CALL_METHOD 执行体(两段式第二段):[recv, target, a1..aN] -> [r],实参整体下移一格补掉
        // 待调值占的那格得调用区 [recv, a1..aN](槽 0 = receiver),交 call_value 统一分发;
        // 纯调用,不再解析。
        bool run_call_method(u8 argc);

        // LOAD_SUPER_FIELD 执行体:defining class 取顶帧 closure 直读(编译器不变式,ASSERT 把关),
        // 自父类沿链读穿透、不含 defining 自身;命中方法闭包则绑 this=帧槽 0 压栈,其余原值直读。
        // 全链 miss 为语言可达错误,经协议 fail。
        bool run_load_super_field(ObjString* name);

        // 下标族指令执行体(LOAD/STORE_INDEX):契约同 field 族

        // LOAD_INDEX 执行体:[obj, idx] -> [v],经 load_index 协议写回 obj 槽再弹 idx;非对象守卫
        // 文案留执行体,对象侧越界/键类型文案由 override 就地烘焙。
        bool run_load_index();

        // STORE_INDEX 执行体:[obj, idx, v] -> [v](值下移两格留 v,peek-store --赋值表达式约定),
        // 经 store_index 协议。
        bool run_store_index();

        // range 构造指令执行体(MAKE_RANGE):契约同 field 族

        // MAKE_RANGE 执行体:有界 [from, to] -> [range]、无上界 [from] -> [range]。端点须为整数,
        // 非整数 fail TypeMismatch(静态文案不插端点值);端点 peek 在栈跨 new_range 的
        // maybe_collect(栈即根),铸完 drop 再 push。
        bool run_make_range(u8 flags);

        // MAKE_CLASS 执行体:[super] -> [class] 原地写回。super 须类值、且非内建容器五类
        //(iterator/list/map/string/range,Exception 与 Object 可作 super),否则 fail TypeMismatch;
        // super peek 在栈跨 new_class 的 maybe_collect(栈即根),建成原地写回槽发布。
        bool run_make_class(ObjString* name);

        // 取走 *current_ 挂起载荷并拆 (码, 烘焙消息) 两件:ObjException 直取自身码与消息
        //(re-throw 保码),其余载荷兜底 UncaughtException;消费点一律 Error::from_baked 物化 --
        // Error 只在边界成型。
        Pair<ErrorCode, String> take_uncaught_error() const;

        // 异常派发与未捕获物化:自最内帧向外按 last_ip 遍查本上下文各帧 CodeUnit 异常记录表,
        // 命中回退派发(不动帧栈/值栈),全未命中沿 resume 链逐跳转投(死协程置 Failed 连死),
        // 链根 = 主上下文未捕获物化;命中可能在多跳之后,current_ 已非进入时的上下文。
        Opt<Error> unwind();

        GC gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)

        // 当前执行上下文:dispatch_loop/call_value 族/raise 的作用对象;GC 对象,内部与 previous_
        // 链经 trace 级联标根,主上下文即其初值(链根完整交接由 run() 入口锚 + 出口断言保证)。
        ObjMovement* current_;

        AriaHashTable modules_;  // 模块表(GC 根)
        AriaHashTable builtins_; // 只读 builtins 表(GC 根)

        // 源根列表:[0]=入口槽(cwd 占位,run() 换成入口 dir_),[1..]=配置根(stdlib/-L/环境变量)。
        List<String> source_roots_;

        // 值寄存器组:VM 单例对象(bootstrap 类与运行期实体)的统一存放表。
        List<Object*> registers_;

        // 常量串表:VM 运行期按名取用的字符串常量(tracer 标根保命 -- 驻留池是 weak root)。
        List<ObjString*> string_constants_;

#ifdef ARIA_OPCODE_PROFILE
        // 指令频度探针(仅探针构建):计数状态自持,析构时按 ARIA_OPCODE_STATS 环境变量门控 dump。
        OpcodeProfiler opcode_profiler_;
#endif
    };

    // move 删除被移除时在此炸出,防静默变可移动后悬垂。
    static_assert(!std::is_move_constructible_v<AriaVM>);
    static_assert(!std::is_move_assignable_v<AriaVM>);

} // namespace aria

#endif // ARIA_VM_HPP
