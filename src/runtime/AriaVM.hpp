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
    class ObjString;

    // interpret 结果:错误已渲染到 stderr,只回成败类别(turnkey 场景只要类别),
    // 需要值/错误细节用低层 run()。
    enum class InterpretResult : u8 {
        Ok,           // 编译并执行成功
        CompileError, // 编译失败(词法/语法/语义)
        RuntimeError, // 运行期未捕获错误
        LoadError,    // 源文件加载失败(仅 interpret_from_path:I/O 或 UTF-8 编码)
    };

    // 解释器宿主:持 GC、服务表(模块表/builtins/寄存器组/常量串表)、当前执行上下文与执行入口;
    // 字节码驱动住 eval 层(Eval),经友元缝取宿主内部。GC 根经 tracer 注册进自有 gc_(gc_ 声明
    // 居首,析构逆序下 tracer 与成员同生共死)。运行时错误统一 raise 入寄存器、经 unwind 查
    // CodeUnit 异常记录表派发。
    class AriaVM {
    public:
        AriaVM();

        AriaVM(const AriaVM&)            = delete;
        AriaVM& operator=(const AriaVM&) = delete;

        // VM 不可移动:成员持指向彼此/自身的指针(modules_ borrow &gc_、current_ 指 GC 对象、
        // tracer 捕 [this]),move 后不重绑即悬垂;就地构造或以 unique_ptr 持有。
        AriaVM(AriaVM&&)            = delete;
        AriaVM& operator=(AriaVM&&) = delete;

        // eval 层驱动引擎:主循环/指令执行体/调用机器/异常派发经友元访问宿主内部
        //(current_ 与各服务表)。
        friend class Eval;

        // 协程模块方法面宿主:四原语经友元直调切换原语对与 current_ 等私有面。
        friend class CoroutineModule;

#ifdef ARIA_OPCODE_PROFILE
        // 指令频度探针(仅探针构建):挂载为成员,友元取 current_ 等私有状态;唯一探测点 =
        // dispatch_loop 取指行宏,本类其余代码零探针痕迹。
        friend class OpcodeProfiler;
#endif

        // 唯一的首次执行入口:锚定入口上下文(出口断言 current_ 回锚点,漏切换交接当场炸) +
        // 源根入口槽 [0] 播种 + 前后 reset 清场,入口 fn 包空闭包后内联首次进入本体(出口边界
        // 恒零,弹空帧栈即出口);CodeUnit 假定良构,失败为未捕获运行时错误。
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
        ObjMovement* current_context() const noexcept {
            return current_;
        }

        // 模块表:键 = 规范路径 ObjString*(intern),值 = ObjModule*(均装箱为 Value)。
        [[nodiscard]]
        AriaHashTable& modules() noexcept {
            return modules_;
        }

        // 寄存器格访问器:声明自注册表两段子表生成(X 行 = 枚举名/访问器名/格类型;定义同源住 .cpp,
        // 类格经 as<ObjClass> 收窄、单例格直返)。
#define ARIA_VALUE_REGISTER_ACCESSOR_DECL(name, accessor, type) [[nodiscard]] type* accessor() const noexcept;
        ARIA_VALUE_REGISTER_SINGLETON_LIST(ARIA_VALUE_REGISTER_ACCESSOR_DECL)
        ARIA_VALUE_REGISTER_CLASS_LIST(ARIA_VALUE_REGISTER_ACCESSOR_DECL)
#undef ARIA_VALUE_REGISTER_ACCESSOR_DECL

        // 常量串表按键直读:键经 NTTP 编译期定下标、static_assert 拒表外键,生成物是基址 +
        // 常量下标的一次加载;实例侧算子派发取钩子名,免每次从驻留池换串。
        template<util::FixedString key>
        [[nodiscard]]
        ObjString* str() const noexcept {
            constexpr auto found = str_table::index_of(key.view());
            static_assert(found.has_value(), "constant string key is not in the registry");
            return str_table_[*found];
        }

        // 常量串按下标直读(str<key> 的运行期下标形态;键 -> 下标的解析在调用侧完成)。
        [[nodiscard]]
        ObjString* str(const usize index) const noexcept {
            return str_table_[index];
        }

        // 单字符常量串直读(byte -> 字节值即下标,启动期 bootstrap 满表):1 字节串全值域恒定,
        // s[i]/chars()/iterator 的逐字符产出经它取用,免逐铸一次哈希+池探测+memcmp。
        [[nodiscard]]
        ObjString* char_string(const u8 byte) const noexcept {
            ASSERT(char_cache_[byte] != nullptr, "single char cache slot unfilled");
            return char_cache_[byte];
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
        // 单字符常量表槽数(字节域全值域容量:值域 0-255,容量 = 上限 + 1)。
        static constexpr usize kCharCacheSlots = 256;

        // IMPORT 未命中分支的加载层:读盘 -> 派生身份 -> new_module -> 编译 -> **编译成功才入表**,
        // 返已 set_entry 的模块,不执行模块体。nullptr ⟺ 载荷已 raise;失败不留表项,同路径重试
        // 重加载。仅限 dispatch_loop 驱动期调用,canonical_path 须调用方已根化(根化跨的是本层
        // 编译期分配,intern weak root 不保命)。
        ObjModule* load_module(ObjString* canonical_path, StringView import_specifier);

        // interpret 共用尾段:编译 + 执行,失败渲染 stderr 并按失败阶段分类(编译期 -> CompileError,
        // run 期 -> RuntimeError -- 被导入模块编译错经异常通道传播、可被 try/catch 捕获,故非 CompileError)。
        InterpretResult interpret_run(SourceFile& source, ObjModule* module);

        // 协程切换原语对
        void enter_coroutine(ObjMovement* coroutine);

        void leave_coroutine(ExecState departing_state);

        // 值寄存器组 bootstrap 编排(ctor 一次调用):逐格初始化全部 VM 单例对象;须在 ctor 构造
        // 临界区(GC 挂起)内调用,创建免守卫。
        void bootstrap_registers();

        // 常量串表 bootstrap(ctor 一次调用):按注册表逐条驻留填入 str_table_;须先于
        // bootstrap_registers -- bootstrap 期按名铸造钩子/方法名,靠本表锚保驻留命中。
        void bootstrap_str_table();

        // 单字符常量表 bootstrap(ctor 一次调用):字节域全值域 256 槽一次铸满与 str_table_
        // 同地位(运行期恒驻留常量,tracer 标根);须在 ctor 构造临界区内调用。
        void bootstrap_char_cache();

        // VM 根 tracer 挂接(ctor 一次调用):只标 current_ 一点,各上下文内部与 previous_ resume
        // 链经 ObjMovement::trace 级联。
        void hook_vm_roots();

        // 源根默认值初始化(ctor 一次调用):入口槽 [0] 占位 cwd + 配置根 [1..] = 编译器相对
        // stdlib 源根。
        void init_source_roots();

        // 取走 *current_ 挂起载荷并拆 (码, 烘焙消息) 两件:ObjException 直取自身码与消息
        //(re-throw 保码),其余载荷兜底 UncaughtException;消费点一律 Error::from_baked 物化 --
        // Error 只在边界成型。
        [[nodiscard]]
        Pair<ErrorCode, String> take_uncaught_error() const;

        // 未捕获物化:自顶帧向下收集 (fn, mod, last_ip) 烘跟踪行 -> take_uncaught_error -> reset
        // 清场,烘焙成自足 Error。仅限 dispatch_loop 失败出口后调用(帧链活着,载荷在寄存器)。
        [[nodiscard]]
        Error rebake_uncaught_error() const;

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
        List<ObjString*> str_table_;

        // 单字符常量表:字节域全值域(direct-mapped,byte 值即下标,kCharCacheSlots 槽),bootstrap
        // 一次铸满、运行期恒驻留常量(tracer 标根);逐字符产串(s[i]/chars()/iterator)的直查表。
        List<ObjString*> char_cache_;

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
