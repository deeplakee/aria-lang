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

    // 解释器:持解释器级共享状态,驱动 Movement 执行字节码。
    //
    //        M1 范围(.claude/reference/runtime/vm-design.md §6):单一主上下文 main_ctx_,指令子集覆盖
    //        常量/字面量、局部槽、算术/比较/逻辑、栈操作、跳转、CALL(仅 ObjFunction)、
    //        RETURN/HALT/PRINT。闭包/全局/异常/模块/类/协程后续阶段接入。
    //        run() 期间 GC 已启用:值栈/帧经 ctor 注册的 vm_roots tracer 标根(M6 前以 tracer
    //        直标代替 Movement 升 Object;open upvalues 留待 M4)。循环状态全部取自 *current_
    //        (现指 main_ctx_),无循环级 C 局部工作副本 -- M6 单循环切换模型(Wren 式)下
    //        resume/yield 在 CALL 善后点换 current_、循环自然驱动新上下文,run_ 永不重入。
    //
    //        M2 新增:模块表(modules_,解释器级共享)+ 经 std::function 回调纳入 GC 根(组合而非继承:
    //        GC 不识 VM 类型,VM 构造时把 tracer lambda 注册进自己的 gc_ -- [this] 捕获。
    //        比「函数指针 + void* ctx + 静态 thunk」干净:无适配器、无 void*、无
    //        static_cast,标记逻辑直写进 lambda。[this] 仅一指针,落在 std::function SBO 内零堆分配)。
    //        模块表键为规范路径 ObjString*(intern),值为 ObjModule*(均装箱为 Value 入 AriaHashTable,
    //        白赚 trace)。collect 时 tracer 标 modules_(进而各模块 name_/dir_/entry_/globals_)
    //        + builtins_ + current_ 沿 previous_ 执行链各上下文的值栈/帧/挂起错误寄存器。
    //        IMPORT 全链已落地(路径解析 -> 命中复用;未命中 load_module:读盘 -> 编译 -> run-once);
    //        DEF/LOAD/STORE_GLOBAL 已落地。
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

        // 在主上下文里执行 fn 的顶层帧:压 callee 值 + acquire 主帧 -> run_ 主循环。
        // 重复调用先 reset 主上下文(同 Lexer/Parser 式复用);入口断言 current_ == &main_ctx_
        // (M6 前恒真;M6 后 resume/yield 严格成对,协程挂起返回时 VM 层即已回退 -- 不归位即
        // 切换纪律被破坏,是错不当静默重置)。
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
        // 上下文):run_ 主循环、call_value 族与本转发同源同一 current_,故原生函数体内 vm.fail()
        // 报的错误必落进其调用者正在执行的上下文 -- M6 协程期即该协程的寄存器,不串主上下文。
        // 详见 ObjNativeFn.hpp NativeFn 契约与 .claude/reference/runtime/vm-design.md §4.7(错误通道 2)。
        //
        // 载荷类型(M3 起)为 Value,单寄存器模型(exception-implementation-pitfalls.md 坑 #7):
        // - raise(code, detail):从零构造消息的装箱入口,一步烘齐--detail 为原始细节串(不含
        //   "Category:" 前缀,防双烘),位置取自 *current_ 顶帧 last_ip(故障指令 / CALL 站点)
        //   查行号表烘 "path:line: " 前缀(合成模块退化为 "<name>:line";帧栈空即 run 外直调
        //   则无位置),经 Error::make_message(Error 的烘焙单点,公开重载)合成完整消息后
        //   new_exception 装箱入寄存器。不经 Error 对象中转 -- Error 只在边界出现(Result 出口
        //   / to_error 反提),不当内部渡船;位置恰只在装箱点可得,一并烘入正是把烘焙责任归位。
        //   Movement::raise(Value)(存原值不包)是 M3 用户 throw 的路由,不经本 VM 层 API。
        // - fail:便捷工厂 -- std::format 格式化 detail 后 raise(原生函数与 call_value/
        //   call_function/call_native/load_module 的失败站点共用)。
        // 寄存器取出的 ObjException 经其 to_error 还原为 Error(见 AriaVM.cpp value_to_error),
        // 边界文案与 Error::from_detail 直构逐字一致。
        //
        // raise / fail 均返回 false -- 供原生函数一行报错 `return vm.fail(...);`(同时置寄存器与
        // 返回失败),成功路径则写 slots[0] 后 `return true;`。[[nodiscard]] 故意为之:裸
        // `vm.fail(...);`(丢弃其 false)会触发警告 -- 要么写成 `return vm.fail(...);`(惯用法),
        // 要么显式 `(void)vm.fail(...);` 表明「我要 raise 但走别的控制流」。VM 以**返回的 bool
        // 为成败信号**(见 call_value 原生分支),寄存器仅作错误载荷容器;二者须一致(debug 断言
        // 把关),契约 `return false ⟺ 已 raise`。raise 内 new_exception(分配)可能在原生
        // 执行中触发 GC:值栈/帧/builtins_ 均已接根,载荷构造后立即入寄存器(pending_error 亦由
        // VM 根 tracer 标根),根安全由既有接线承保。定义在 .cpp(需 ObjException 完整类型)。
        [[nodiscard]]
        bool raise(ErrorCode code, StringView detail);

        template<typename... Args>
        [[nodiscard]]
        bool fail(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            return raise(code, std::format(fmt, std::forward<Args>(args)...));
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
        // 主循环:驱动 *current_(现为 main_ctx_;M6 resume 重入时为被恢复协程的上下文)直到顶层
        // 返回/错误/显式停止。栈/帧/错误寄存器一律经 current_ 访问,与 raise 同源(语义统一)。
        // 模块体 run-once 经 IMPORT 未命中分支以普通函数调用进帧(入口名固定 <module>),
        // 由本循环执行,其 RETURN 按函数名判定模块体帧,压回模块对象 -- 无递归调用。
        Result<Value, Error> run_();

        // IMPORT 未命中分支的加载层:把已解析命中的磁盘模块读盘 -> 派生身份 -> new_module
        // -> 入表占位 -> 编译(入口名 <module>,见 AriaVM.cpp kModuleName)-> 返回模块对象(已 set_entry)。
        // **仅加载与编译**,不执行模块体 -- run-once 由调用方(IMPORT 分支)以普通函数调用进帧驱动,
        // 其 RETURN 按函数名 == <module> 判定后压回模块对象。
        // 错误契约与 call_value 族同构:return nullptr ⟺ 错误载荷已 raise 入 *current_ 寄存器,
        // 调用方 take_error 取出沿 runtime_err 传播。两类失败:读盘失败/名字无效经 fail 烘位置
        // (raise 时顶帧即导入方帧,last_ip 指本 IMPORT 指令 -- 与 resolve_module 解析失败的
        // runtime_err 形态统一);被导入模块的编译期 Error 就地 new_exception 原样装配箱透传
        // (from_baked 语义不重烘,位置指向被导入文件内部)。**仅限 run_ 驱动期调用**:寄存器随 *current_ 走,
        // run() 入口 reset 会清 pending_error -- run 外直调的错误会被静默吞掉(runtime_loc 亦
        // 依赖顶帧,帧栈空则无位置)。
        //   - canonical_path:命中文件的绝对规范路径(intern ObjString*),一身二任 -- 既作 modules_
        //     表键,又作读盘路径。**调用方须已根化**(跨本函数内 modules_.upsert 的 rehash 触 GC --
        //     intern weak root 不保命)。
        //   - import_specifier:用户写的原始 import 串(报错消息用,如 "./helper")。
        // 越界检测(相对导入越出源根)本轮不做:文件能解析到即读。
        ObjModule* load_module(ObjString* canonical_path, StringView import_specifier);

        // interpret / interpret_from_path 共用尾段：调 run(SourceFile&, ObjModule&) 编译并执行，成功返 Ok；
        // 失败把 Error.message() 渲染到 stderr（Error 已自有完整消息串、不持 SourceFile*）并按错误大类映射--
        // Syntax / Semantic -> CompileError，余（Runtime / Internal / Resource）-> RuntimeError。
        InterpretResult interpret_run(SourceFile& source, ObjModule& module);

        // CALL 分发:栈顶形如 [callee, a1..aN](N=argc,由 CALL 调用方保证)。按 callee 的对象类型
        // 分派到对应 call_* 子例程(ObjFunction -> call_function、ObjNativeFn -> call_native),其余报
        // CallNonCallable。作用于 *current_(与 run_ 同源;现为 main_ctx_,M6 协程期即当前协程
        // 上下文 -- 主循环在哪个上下文上驱动,调用就发生在哪个上下文,错误随上下文走不串扰)。
        // 返回 bool 为成败信号:true 即成功(栈效应由子例程各自负责),false 即失败 -- 错误载荷
        // 已 raise 进 *current_ 的挂起错误寄存器,调用方据 bool 决定是否 take_error 取出沿
        // runtime_err 传播。M1 仅支持 ObjFunction / ObjNativeFn(闭包/类/方法后续阶段)。
        // 契约:return false ⟺ 已 raise 入 *current_。
        bool call_value(Value callee, u8 argc);

        // 用户函数调用:校验 arity + 帧栈未溢出后 enter_frame 进帧(callee 在槽 0,
        // 参数即局部槽 1..argc)。成功返 true;失败 raise WrongArity / StackOverflow 入 *current_
        // 后返 false。栈形 [callee, a1..aN] 由 CALL 调用方保证。
        bool call_function(ObjFunction* obj, u8 argc);

        // 原生函数调用:同步调用 obj->fn(),不进帧。原生函数自身以 bool 为成败信号、返回值写槽 0、
        // 错误载荷走侧信道寄存器(Movement::pending_error_);本函数透传该 bool 契约:成功(原生返
        // true)断言无载荷 -> 清寄存器 + drop(argc) 弹实参(返回值升栈顶)后返 true;失败(原生返
        // false)断言已 raise -> 载荷留寄存器交调用方 take_error,返 false(不在本函数取出,与
        // call_function/call_value 的 bool 契约统一)。调用区 [callee, a1..aN] 经 Span 暴露:
        // slots[0]=槽 0(返回值),slots[1..argc]=实参。详见 ObjNativeFn.hpp NativeFn 契约与
        // .claude/reference/runtime/vm-design.md §4.7。
        bool call_native(const ObjNativeFn* obj, u8 argc);

        GC       gc_; // 自有分配器(VM 持有,每个 VM 一个 GC)
        Movement main_ctx_;
        // 当前执行上下文:run_ 主循环 / call_value 族 / raise 的作用对象,构造即指 &main_ctx_。
        // 方法纪律:run_/call_value 族/raise 一律直接经 current_ 访问(语义统一,无入口快照)。
        // M6 单循环切换模型(vm-design.md §4.9):resume/yield 为原生函数,换 current_ 对
        // call_native 透明(事后簿记一律落 entered_ctx,无需分支探测,CALL case 零改动),
        // run_ 永不重入,任一时刻正在执行的字节码所在上下文恒等于 current_;previous_ 对齐
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
