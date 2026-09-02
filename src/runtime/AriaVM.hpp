#ifndef ARIA_VM_HPP
#define ARIA_VM_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "runtime/Movement.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

namespace aria {

    class ObjFunction;
    class ObjModule;
    class ObjNativeFn;

    // interpret 结果：编译并执行的结局类别（对齐 clox InterpretResult）。
    // interpret / interpret_from_path 内部已把错误渲染到 stderr（此时 SourceFile 仍存活，SourceLoc 有效），
    // 故只回类别、不回 Error--避免内部构造/读盘的 SourceFile 在方法返回后销毁致 Error 的 SourceLoc 悬垂。
    // 低层 run(ObjFunction*) / run(SourceFile&, ObjModule&) 仍返 Result<Value, Error>，供需要值/错误细节的调用方。
    enum class InterpretResult : u8 {
        Ok,           // 编译并执行成功
        CompileError, // 编译失败（词法 / 语法 / 语义）
        RuntimeError, // 运行期未捕获错误
        LoadError,    // 源文件加载失败（仅 interpret_from_path：I/O 或 UTF-8 编码）
    };

    // 解释器:持解释器级共享状态,驱动 Movement 执行字节码。
    //
    //        M1 范围(.claude/reference/runtime/vm-design.md §6):单一主上下文 main_ctx_,指令子集覆盖
    //        常量/字面量、局部槽、算术/比较/逻辑、栈操作、跳转、CALL(仅 ObjFunction)、
    //        RETURN/HALT/PRINT。闭包/全局/异常/模块/类/协程后续阶段接入。
    //        run() 期间 GC 已启用:值栈/帧经 ctor 注册的 vm_roots tracer 标根(M6 前以 tracer
    //        直标代替 Movement 升 Object;open upvalues 留待 M4)。循环状态全部取自 *current_
    //        (现为 main_ctx_),按可重入风格写 -- 协程期 resume 即换 current_ 重入,循环体无静态/成员临时。
    //
    //        M2 新增:模块表(modules_,解释器级共享)+ 经 std::function 回调纳入 GC 根(组合而非继承:
    //        GC 不识 VM 类型,VM 构造时把标记 lambda 注册进自己的 gc_ -- [this] 捕获,内部 trace
    //        modules_。比「函数指针 + void* ctx + 静态 thunk」干净:无适配器、无 void*、无
    //        static_cast,标记逻辑直写进 lambda。[this] 仅一指针,落在 std::function SBO 内零堆分配)。
    //        模块表键为规范路径 ObjString*(intern),值为 ObjModule*(均装箱为 Value 入 AriaHashTable,
    //        白赚 trace)。collect 时 GC 调 lambda -> modules_.trace 标全部模块(进而 trace 各模块
    //        name_/root_/entry_/globals_)。IMPORT 已部分落地(路径解析 + 模块表命中复用;磁盘加载/编译/run-once
    //        未就绪);DEF/LOAD/STORE_GLOBAL 已落地。
    //
    //        VM 持有自己的 GC(值成员 gc_):每个 VM 一个 GC,无需外部注入。成员声明序
    //        gc_ -> main_ctx_ -> modules_(后者引用 &gc_),故析构逆序下 gc_ 最后析构,
    //        tracer 与 modules_ 同生共死,无需析构时显式注销(GC 不可能比 VM 长寿)。
    class AriaVM {
    public:
        // 构造即把 VM 根 tracer 注册进自有 GC;main_ctx_/modules_ 借 &gc_。
        // 定义在 .cpp(预期随阶段推进:接更多根、内置模块、原生函数表等,体量增长)。
        AriaVM();

        AriaVM(const AriaVM&)            = delete;
        AriaVM& operator=(const AriaVM&) = delete;

        // VM 不可移动:成员间持指向彼此/自身的指针(main_ctx_/modules_ borrow &gc_;
        // GC 根 tracer 捕 [this]),move 后这些指针不自动重绑 -> 悬垂。就地构造或以
        // unique_ptr 持有,勿按值搬迁。显式删 move 把不变式提为显式契约(否则当前仅
        // 由 GC 不可 move 隐式派生,易被误读为可放开)。
        AriaVM(AriaVM&&)            = delete;
        AriaVM& operator=(AriaVM&&) = delete;

        // 在主上下文里执行 fn 的顶层帧:压 callee 值 + acquire 主帧 -> run_ 主循环。
        // 重复调用先 reset 主上下文(同 Lexer/Parser 式复用)。
        // 注:fn 的 CodeUnit 假定良构(以 RETURN/HALT 终止),M1 不做逐指令越界设防。
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
        // root=cwd）-> 编译 -> 执行。错误渲染到 stderr，返回 InterpretResult（不返 Error，避免内部
        // SourceFile 返回后悬垂，见上枚举注释）。
        InterpretResult interpret_from_src(StringView src);

        // 编译并执行源文件（interpret_from_path）：SourceFile::from_path 读盘（失败渲染并返 LoadError）
        // + 按路径派生入口模块（name=basename 去 .aria、root=dirname(absolute(path))）-> 编译 -> 执行。
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
        // 寄存器物理上在 VMContext(Movement::pending_error_);VM 经此转发到当前上下文。
        // M1 当前上下文即 main_ctx_;M6 协程期改由 current_ 指向的当前协程上下文承担
        // (届时 run_ 经 current_ 重入,call_value 检查的 ctx 即 current_,与本转发一致)。
        // 详见 ObjNativeFn.hpp NativeFn 契约与 .claude/reference/runtime/vm-design.md 错误通道 2(raise)。
        //
        // raise:写入挂起错误。断言当前无挂起(Movement::raise 内把关)。
        // fail:便捷工厂--errorf 构造无位置 Error 后 raise,供原生函数一行报错。
        //
        // 两者均返回 false -- 供原生函数一行报错 `return vm.fail(...);`(同时置寄存器与返回失败),
        // 成功路径则写 slots[0] 后 `return true;`。[[nodiscard]] 故意为之:裸 `vm.fail(...);`(丢弃
        // 其 false)会触发警告 -- 要么写成 `return vm.fail(...);`(惯用法),要么显式 `(void)vm.fail(...);`
        // 表明「我要 raise 但走别的控制流」。VM 以**返回的 bool 为成败信号**(见 call_value 原生分支),
        // 寄存器仅作错误载荷容器;二者须一致(debug 断言把关),契约 `return false ⟺ 已 raise`。
        [[nodiscard]]
        bool raise(Error err) noexcept {
            main_ctx_.raise(std::move(err));
            return false;
        }

        template<typename... Args>
        [[nodiscard]]
        bool fail(ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            return raise(errorf(code, fmt, std::forward<Args>(args)...));
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

        // 源根列表(解释器级):裸名导入(import "lib/utils")的搜索路径根目录,语义对齐 Python
        // sys.path -- 解析器沿各源根找 <源根>/<spec>.aria,首个存在者命中(详见 IMPORT 实现 &
        // .claude/reference/runtime/import-path-resolution.md)。模块表键为命中文件的绝对规范路径(weakly_canonical,
        // 源根不进键)。布局固定:source_roots_[0] = 入口槽,source_roots_[1..] = 配置根。
        //   - 构造时 [0] 占位为当前工作目录(前期源根),[1..] 推入编译器相对 stdlib 目录等配置根。
        //   - run() 时 [0] 被入口模块 root_ 替换(root_ 恒非空 -- new_module 默认 cwd);[1..] 不动。
        //   - set_source_roots 替换 [1..](配置根),保留 [0](入口槽)。
        // 单一 List<String> 即可,无 flag / 无并列配置列表:[0] 槽位约定 + run() 原地替换。
        // 加载链路(磁盘加载 + AST->CodeUnit 编译 + VM 内嵌套执行模块体)未就绪,源根目前仅服务于
        // 路径解析(命中后查模块表;未命中即 ModuleNotFound)。存为 List<String>(路径元数据,
        // 非 ObjString*,不参与 GC 追踪 -- 仅解析器用,不作模块表键)。
        [[nodiscard]]
        const List<String>& source_roots() const noexcept {
            return source_roots_;
        }

        // 覆盖配置源根(stdlib / -L / 环境变量等;不含入口槽 [0] -- 入口槽由 run() 按入口模块 root_
        // 原地替换,不归此管)。保留 source_roots_[0],替换 [1..]。测试 / 嵌入配置用:
        // 置空即清掉默认 stdlib(隔离);置 [dir...] 即指定自定义源根集合。
        void set_source_roots(List<String> roots) noexcept;

    private:
        // 主循环:驱动 main_ctx_ 直到顶层返回/错误/显式停止。状态全部取自上下文。
        // 模块体 run-once 经 IMPORT 未命中分支以普通函数调用进帧(入口名固定 <module>),
        // 由本循环执行,其 RETURN 按函数名判定模块体帧,置该模块 Loaded 并压回模块对象 -- 无递归调用。
        Result<Value, Error> run_();

        // IMPORT 未命中分支的加载层:把已解析命中的磁盘模块读盘 -> 派生身份 -> new_module(Loading)
        // -> 入表占位 -> 编译(入口名 <module>,见 AriaVM.cpp kModuleName)-> 返回模块对象(已 set_entry)。
        // **仅加载与编译**,不执行模块体 -- run-once 由调用方(IMPORT 分支)以普通函数调用进帧驱动,
        // 其 RETURN 按函数名 == <module> 判定后置 Loaded。
        // 成功返回模块对象(已入表 Loading,待 run-once 置 Loaded);失败返回 Error(被导入模块的
        // 编译期错误原样透传,含其文件位置)。
        //   - key:模块表键(绝对规范路径 intern ObjString*)。**调用方须已根化**(跨本函数内
        //     modules_.upsert 的 rehash 触 GC -- key 为 intern weak root 不保命)。
        //   - path_spec:原始 import 串(报错消息用,如 "./helper");读盘用 key->view()。
        // 越界检测(相对导入越出源根)本轮不做:文件能解析到即读。
        Result<ObjModule*, Error> load_module(ObjString* key, StringView path_spec);

        // interpret / interpret_from_path 共用尾段：调 run(SourceFile&, ObjModule&) 编译并执行，成功返 Ok；
        // 失败把 Error.message() 渲染到 stderr（Error 已自有完整消息串、不持 SourceFile*）并按错误大类映射--
        // Syntax / Semantic -> CompileError，余（Runtime / Internal / Resource）-> RuntimeError。
        InterpretResult interpret_run(SourceFile& source, ObjModule& module);

        // CALL 分发:栈顶形如 [callee, a1..aN](N=argc)。按 callee 的对象类型分派到对应
        // call_* 子例程(ObjFunction -> call_function、ObjNativeFn -> call_native),其余报
        // CallNonCallable。失败返回 Error,nullopt 即成功(成功时栈效应由子例程各自负责)。
        // 经当前 ctx 而非 main_ctx_ -- run_ 按重入式风格把所有操作作用于当前 ctx(现为 main_ctx_,
        // M6 协程期换 current_ 重入),call_value 同理,不写死主上下文。
        // M1 仅支持 ObjFunction / ObjNativeFn(闭包/类/方法后续阶段)。
        Opt<Error> call_value(Movement& ctx, Value callee, u8 argc);

        // 用户函数调用:校验 arity + 帧栈未溢出后 enter_frame 进帧(callee 在槽 0,
        // 参数即局部槽 1..argc)。成功返 nullopt;失败返 WrongArity / StackOverflow。
        // 栈形 [callee, a1..aN] 由 CALL 调用方保证。
        Opt<Error> call_function(Movement& ctx, ObjFunction* obj, u8 argc);

        // 原生函数调用:同步调用 obj->fn(),不进帧。bool 为成败信号,返回值写槽 0,
        // 错误载荷走侧信道寄存器(Movement::pending_error_)。成功:断言无载荷 -> 清寄存器 +
        // drop(argc) 弹实参(返回值升栈顶);失败:断言已 raise -> take_error 取出传播。
        // 调用区 [callee, a1..aN] 经 Span 暴露:slots[0]=槽 0(返回值),slots[1..argc]=实参。
        // 详见 ObjNativeFn.hpp NativeFn 契约与 .claude/reference/runtime/vm-design.md §4.7。
        Opt<Error> call_native(Movement& ctx, const ObjNativeFn* obj, u8 argc);

        GC            gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)
        Movement      main_ctx_;
        AriaHashTable modules_; // 模块表(M2:解释器级共享 + GC 根)
        List<String>
                source_roots_; // 源根列表:[0]=入口槽(cwd 占位,run() 换成入口 root_);[1..]=配置根(stdlib/-L/环境变量)
    };

    // VM 不可移动不变式的显式校验(类完成定义后断言):成员间持指向彼此/自身的指针
    // (main_ctx_/modules_ borrow &gc_;GC 根 tracer 捕 [this]),move 后不自动重绑 -> 悬垂。
    // 上方已显式 delete move;此断言锁定该不变式 -- 若有人删掉上面的 delete 且 GC 变可
    // move 致隐式 move 重新生成,断言在此炸出,避免静默变可移动后的悬垂 UB。
    static_assert(!std::is_move_constructible_v<AriaVM>);
    static_assert(!std::is_move_assignable_v<AriaVM>);

} // namespace aria

#endif // ARIA_VM_HPP
