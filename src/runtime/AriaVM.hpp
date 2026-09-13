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

    // fail 的返回哨兵:按调用点返回类型转失败拼写(bool -> false、指针 -> nullptr、
    // Opt<T> -> nullopt),失败出口惯用法一行 `return vm.fail(...);`。
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

    // 解释器:持解释器级共享状态,驱动 Movement 执行字节码(阶段路线见 vm-design.md §6)。
    // 循环状态全部取自 *current_(单一主上下文 main_ctx_;M6 resume/yield 只换走 current_,
    // dispatch_loop 永不重入,§4.9)。GC 根经 std::function tracer 注册进自有 gc_(组合而非
    // 继承:GC 不识 VM 类型),collect 时标 modules_/builtins_/object_class_ + current_ 沿链
    // 各上下文的值栈/帧/开 upvalue 链/挂起错误寄存器;成员声明序 gc_ 居首保证析构逆序下
    // tracer 与各成员同生共死。异常通道:运行时错误统一 raise 入寄存器后经 unwind 查
    // CodeUnit 异常记录表派发。
    class AriaVM {
    public:
        AriaVM();

        AriaVM(const AriaVM&)            = delete;
        AriaVM& operator=(const AriaVM&) = delete;

        // VM 不可移动:成员持指向彼此/自身的指针(main_ctx_/modules_ borrow &gc_、current_ 指
        // main_ctx_、tracer 捕 [this]),move 后不重绑 -> 悬垂。就地构造或以 unique_ptr 持有。
        AriaVM(AriaVM&&)            = delete;
        AriaVM& operator=(AriaVM&&) = delete;

        // 程序入口仪式:断言主上下文 + 源根入口槽 [0] 播种 + 前后 reset 清场 + 入口 fn 包空闭包
        // (顶层也闭包,统一「帧 = 闭包」模型)后委托 run_closure。fn 的 CodeUnit 假定良构
        // (以 RETURN/HALT 终止),不逐指令设防;失败为未捕获运行时错误(M6 挂起将扩三态)。
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

        [[nodiscard]]
        Movement& main_context() noexcept {
            return main_ctx_;
        }

        // ---- 挂起错误侧信道(供原生函数等冷路径报错)----
        // 寄存器物理上在 Movement::pending_error_,经 *current_ 转发 -- 与 dispatch_loop/
        // call_value 族同源同一上下文,错误必落进调用者正在执行的上下文。载荷为 Value(单寄存器
        // 模型)。raise 从零构造消息装箱一步烘齐,**消息不含位置前缀**(位置由 unwind 未捕获
        // 出口的逐帧 at 行给出,对齐 clox/Python);用户 throw 经 Movement::raise 原值入寄存器,
        // 不走本层 API。fail 是 raise + 恒返失败信号(见 FailSignal),[[nodiscard]] 强制
        // `return vm.fail(...);` 惯用法;原生函数以返回 bool 为成败信号,契约 false ⟺ 已 raise。
        // raise 内 new_exception 分配可能触 GC,载荷构造后立即入寄存器(tracer 已标根)。
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

        // 模块表:键 = 规范路径 ObjString*(intern),值 = ObjModule*(均装箱为 Value),
        // IMPORT 按键查重/插入。
        [[nodiscard]]
        AriaHashTable& modules() noexcept {
            return modules_;
        }

        // Object 根类:LOAD_OBJECT 直推;单独持有不进 builtins_/任何模块 globals(用户
        // shadow 全局名免疫)。
        [[nodiscard]]
        ObjClass* object_class() noexcept {
            return object_class_;
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

        // 主循环:驱动 *current_ 直到顶层返回/错误/显式停止。模块体 run-once 经 IMPORT 未命中
        // 分支以普通函数调用进帧(入口名固定 <module>),其 RETURN 按函数名判定模块体帧,压回
        // 模块对象 -- 无递归调用。
        Result<Value, Error> dispatch_loop();

        // IMPORT 未命中分支的加载层:读盘 -> 派生身份 -> new_module -> 编译(入口名 <module>)
        // -> **编译成功才入表** -> 返回模块对象(已 set_entry)。只加载与编译,不执行模块体 --
        // run-once 由调用方进帧驱动。nullptr ⟺ 载荷已 raise:读盘失败/名字无效经 fail 烘位置
        // (raise 时顶帧即导入方帧);被导入模块的编译期 Error 原样装配箱透传(不重烘,位置指向
        // 被导入文件内部)。失败不留表项,同路径重试重新加载。**仅限 dispatch_loop 驱动期调用**
        // (寄存器随 *current_ 走,run() 入口 reset 会吞掉)。canonical_path 须调用方已根化
        // (跨 modules_.set 的 rehash 触 GC,intern weak root 不保命)。
        ObjModule* load_module(ObjString* canonical_path, StringView import_specifier);

        // interpret 共用尾段:编译 + 执行;失败渲染 stderr 并按**失败阶段**分类 -- 编译期 ->
        // CompileError,run 期 -> RuntimeError(含经异常通道传播、可被 try/catch 捕获的被导入
        // 模块编译错,故不构成 CompileError)。
        InterpretResult interpret_run(SourceFile& source, ObjModule* module);

        // CALL 分发:栈顶形如 [callee, a1..aN]。按 callee 的对象类型分派到对应 call_* 子例程,
        // 其余经 Object::op_call 基类默认报 CallNonCallable(未来可调用新类型 override 即接入)。
        // **本文件 call_* 族的统一契约:return false/nullptr ⟺ 错误载荷已 raise 进 *current_
        // 挂起错误寄存器**,调用方 take_error 取出沿 runtime_err 传播。
        bool call_value(Value callee, u8 argc);

        // 闭包调用的进帧单点(call_value 分发与 run_closure 共用):校验 arity 与帧栈未溢出后
        // 进帧(callee 在槽 0,参数即局部槽 1..argc)。失败 raise WrongArity/StackOverflow。
        bool call_closure(ObjClosure* obj, u8 argc);

        // 原生函数调用:同步调 obj->fn(),不进帧;bool 契约透传(契约见 ObjNativeFn.hpp)。
        bool call_native(const ObjNativeFn* obj, u8 argc);

        // 类实例化:new_instance 为唯一 GC 点,建成即写 callee 槽 -- **槽 0 原位换实例**(即
        // 新帧的 this),余下交 call_value 通用分发(init 恒有值)。
        bool call_class(ObjClass* obj, u8 argc);

        // 绑定方法调用:槽 0 原位覆写为 receiver(this 替代 callee,实参槽位不动),余下交
        // call_value 分发。方法值无需守卫:覆写后经类表槽/缓存可达。
        bool call_bound_method(const ObjBoundMethod* obj, u8 argc);

        // Object 根类 bootstrap(ctor 一次调用):建 ObjClass("Object", super=nullptr) + 原生
        // no-op init(无 ObjFunction,保「module 恒非空」不变式)并发布进类表 init 槽与 init_。
        void bootstrap_object_class();

        // 源根默认值初始化(ctor 一次调用):入口槽 [0] 占位 cwd + 配置根 [1..] = 编译器相对
        // stdlib 源根。
        void init_source_roots();

        // ---- 异常 unwind(M3,dispatch_loop 驱动期专用)----

        // 弹 2 算 1:栈顶两值的二元数值运算(9 个算术/比较指令共用,Op 由调用点穷举实例化)。
        // bool 契约同 call_value;模板成员定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        bool run_binary_numeric();

        // ---- 类与对象(M5):field 族指令执行体 ----
        //
        // bool 契约同 call_value:失败载荷已在寄存器(对象协议失败由 override 内 vm.fail 就地
        // 烘焙,非对象守卫由执行体 fail)。peek 不弹 -- 协议内分配跨 GC 须 obj 在栈(「栈即根」)。
        // THIS 对(LOAD/STORE_THIS_FIELD)无执行体:编译器不变式保证 this 恒实例,case 内直调协议。

        // LOAD_FIELD 执行体(name 已读出):peek obj,经 Object::load_field 协议解析,结果写回
        // 原槽([obj] -> [v]);非对象(含 nil)是协议外原语,文案留执行体,对象 miss 文案由协议
        // override 烘焙。
        bool run_load_field(ObjString* name);

        // STORE_FIELD 执行体:[obj, v] -> [v](完成时单槽下移留 v -- 赋值表达式约定)。经
        // Object::store_field 协议。
        bool run_store_field(ObjString* name);

        // LOAD_SUPER_FIELD 执行体:defining class 取顶帧 closure 直读(方法闭包恒有戳,编译器
        // 不变式 ASSERT 钉),从其父类起走 ObjClass::load_field 沿链读穿透(不含 defining 自身,
        // 类协议不绑定不缓存)。命中方法闭包 -> 绑 this=帧槽 0 压栈供 CALL;其余(静态方法/函数值
        // 静态/原生/静态值)原值直读;均**不写 fields 缓存**(super 查到的是被覆写前的实现,写缓存
        // 会被 fields 命中劫持后续 obj.m 动态派发)。全链 miss 为语言可达错误,经协议 fail。
        bool run_load_super_field(ObjString* name);

        // 自最内帧向外按 last_ip 查各帧 CodeUnit 异常记录表(find_try_handler 取最内层覆盖),
        // 首命中即在该帧 unwind -- 截值栈、ip 跳 handler、寄存器载荷 push 落 catch 参数槽,
        // 返 nullopt(已派发,调用方 continue);未命中的帧先记跟踪三元组(fn/mod/ip_off)再
        // exit_frame 继续外层。全帧未命中 -> 未捕获:从寄存器反提载荷拆 (码, 烘焙消息) 两件,
        // 逐帧烘焙 "\n  at <fn> (<loc>)" 进消息尾部(收集序内->外,渲染反转外->内,Python 式),
        // 经 Error::from_baked 一次物化返回。前提:寄存器已有载荷(入口断言把关);帧内 last_ip
        // 由 dispatch_loop 循环顶写(顶帧 = 故障指令,外层帧 = CALL 站点)。
        Opt<Error> unwind();

        GC gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)

        Movement main_ctx_;

        // 当前执行上下文:dispatch_loop/call_value 族/raise 的作用对象,构造即指 &main_ctx_
        // (M6 切换模型见 vm-design.md §4.9)。
        Movement* current_;

        AriaHashTable modules_;  // 模块表(GC 根)
        AriaHashTable builtins_; // 只读 builtins 表(GC 根)

        // 源根列表:[0]=入口槽(cwd 占位,run() 换成入口 dir_),[1..]=配置根(stdlib/-L/环境变量)。
        List<String> source_roots_;

        // Object 根类:构造期 bootstrap、单独持有(tracer 第 4 根);空态 nullptr 供 tracer
        // 先行注册后 register_builtins 触 GC 时容 null。
        ObjClass* object_class_;
    };

    // move 删除被移除时在此炸出,防静默变可移动后的悬垂 UB。
    static_assert(!std::is_move_constructible_v<AriaVM>);
    static_assert(!std::is_move_assignable_v<AriaVM>);

} // namespace aria

#endif // ARIA_VM_HPP
