#include "runtime/AriaVM.hpp"

#include <cmath>
#include <filesystem>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "bytecode/Disassembler.hpp"
#include "bytecode/code.hpp"
#include "compile/Compiler.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "object/Object.hpp"
#include "runtime/Builtins.hpp"
#include "util/fs.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {
    namespace stdfs = std::filesystem;

    namespace {

        // 读 1 字节操作数(假定字节码良构),推进 ip。
        u8 read_u8(CallFrame& frame) noexcept { return *frame.ip++; }

        // 读 2 字节小端 u16 操作数(假定字节码良构),推进 ip。拼装走 util::make_u16。
        u16 read_u16(CallFrame& frame) noexcept {
            const u16 value = util::make_u16(frame.ip[0], frame.ip[1]);
            frame.ip += 2;
            return value;
        }

        // 取常量池 u16 索引处的 ObjString*(全局名/字段名/模块路径与别名等),推进 ip。
        // 良构前提:该常量必为经 intern 驻留的 ObjString*(编译期保证)。故此处不二次校验,
        // 直接 as_obj 取 ObjString*(若编译期出错,后续按 intern 同指针查表会查不到,
        // 属编译器 bug 而非运行期可恢复错)。供 DEF/LOAD/STORE_GLOBAL 与 IMPORT 复用。
        ObjString* read_name(CallFrame& frame) noexcept {
            const auto idx = read_u16(frame);
            return Object::as<ObjString>(frame.unit->constants[idx].as_obj());
        }

        // 把 import 串(specifier)解析为命中文件的绝对规范路径(模块表键)。设计见
        // .claude/reference/runtime/import-path-resolution.md「加载层设计基线」:键 = weakly_canonical(候选)。
        //   - spec:用户写的 import 串(如 "./helper" / "lib/utils")。
        //   - current_module_path:发出 import 的当前模块的绝对文件路径(frame.module->abs_path());
        //     相对分支仅取其 dirname 作基(.aria 后缀在末段,dirname 不受影响)。dir_ 内容可空
        //     (cwd 不可用时 abs_path 返空串)-> 相对解析直接返 nullopt(拒绝锚定)。
        //   - 相对("./" / "../" 开头,或正是 "." / ".."):单基 = 当前模块目录,caller-local,
        //     不碰 source_roots,故相对导入永不逃逸到别的源根。
        //   - 裸名(无 ./ ../ 前缀):基 = source_roots(入口槽 [0] + 配置根),逐个 exists-check,
        //     首个存在 <base>/<spec>.aria 者命中(对齐 Python sys.path 顺序搜索)。
        //   - 末尾 ".aria" 可选:剥后缀查找再统一补回,使 lib/math ≡ lib/math.aria。
        //   - 键 = 绝对规范路径:同模块不同写法归一到各自真实路径,跨根不碰撞、相对不逃逸;
        //     符号链接经 weakly_canonical 规避双加载。
        Opt<String> resolve_module(const StringView spec, const StringView current_module_path,
                                   const List<String>& source_roots) {
            // 1. 剥末段 ".aria" 后缀(段长 > 扩展名长且以 ".aria" 结尾),使 lib/math ≡ lib/math.aria。
            String spec_str{spec};
            {
                const auto last_slash = spec_str.find_last_of('/');
                const auto last_seg   = (last_slash == String::npos) ? StringView{spec_str}
                                                                     : StringView{spec_str}.substr(last_slash + 1);
                if (last_seg.size() > kAriaExtension.size() && last_seg.ends_with(kAriaExtension)) {
                    spec_str.erase(spec_str.size() - kAriaExtension.size());
                }
            }

            // 2. 选基:相对 -> 当前模块目录(单基);裸名 -> source_roots(逐个试)。
            const bool is_relative = spec.starts_with("./") || spec.starts_with("../") || spec == "." || spec == "..";
            List<stdfs::path> bases;
            if (is_relative) {
                if (current_module_path.empty()) {
                    return std::nullopt; // 合成模块无绝对路径,无法相对解析
                }
                bases.push_back(stdfs::path{String{current_module_path}}.parent_path());
            } else {
                for (const auto& root: source_roots) {
                    if (!root.empty()) {
                        bases.emplace_back(root);
                    }
                }
            }

            // 3. 逐基:<base>/<spec>.aria -> weakly_canonical -> exists 则为键。
            auto file_rel = spec_str;
            file_rel.append(kAriaExtension);
            std::error_code ec;
            for (const auto& base: bases) {
                auto canon = stdfs::weakly_canonical(base / file_rel, ec);
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (stdfs::exists(canon, ec) && !ec) {
                    return canon.string();
                }
                ec.clear();
            }
            return std::nullopt;
        }

        // 数值二元运算的操作符记号(报错消息用)。
        constexpr StringView op_symbol(const OpCode op) noexcept {
            switch (op) {
                case OpCode::ADD:
                    return "+";
                case OpCode::SUBTRACT:
                    return "-";
                case OpCode::MULTIPLY:
                    return "*";
                case OpCode::DIVIDE:
                    return "/";
                case OpCode::MOD:
                    return "%";
                case OpCode::GREATER:
                    return ">";
                case OpCode::GREATER_EQUAL:
                    return ">=";
                case OpCode::LESS:
                    return "<";
                case OpCode::LESS_EQUAL:
                    return "<=";
                default:
                    return "?";
            }
        }

        // 模块位置串 "<loc>:<line>":文件模块渲染 abs_path;合成模块(名以 '<' 开头,
        // abs_path 会拼出伪路径)或 abs_path 为空退化为 "<name>"。未捕获堆栈跟踪逐帧渲染
        // (unwind 的 at 行)用 -- 消息本身不烘位置前缀(见 AriaVM::raise),位置串规则单一事实源。
        String module_loc(const ObjModule* module, const u32 line) {
            if (const auto name = module->name()->view(); name.starts_with('<') || module->abs_path().empty()) {
                return std::format("{}:{}", name, line);
            }
            return std::format("{}:{}", module->abs_path(), line);
        }

        // 把寄存器取出的载荷拆为未捕获出口要用的 (码, 完整烘焙消息) 两件:ObjException 直取
        // 自身码与 message_(已是完整烘焙串,与 from_detail 直构文案逐字一致,re-throw 保码,
        // 坑 #7);其它载荷(用户 throw 的非异常值)兜底 UncaughtException,消息渲染值本身(经
        // 烘焙单点 make_message,与 from_detail 同源同串)。仅 unwind 未捕获出口一处消费:
        // 拼好跟踪后经 Error::from_baked 一次物化成边界 Error,不中转 Error 对象(Error 只在边界成型)。
        Pair<ErrorCode, String> uncaught_error_parts(const Value value) {
            if (const auto ex = try_obj<ObjException>(value)) {
                return {ex->code(), String{ex->message()->view()}};
            }
            const auto msg = std::format("uncaught exception: {}", format_value(value));
            return {ErrorCode::UncaughtException, Error::make_message(ErrorCode::UncaughtException, msg)};
        }

        // 构造运行时错误结果(Result<Value, Error> 的 unexpected 态):dispatch_loop 各异常站点
        // (unwind 返 somed Error)的统一收口,只剩 std::unexpected 样板。
        Result<Value, Error> runtime_err(Error err) { return std::unexpected(std::move(err)); }

        // 范围外 opcode 的统一处理:命中即打印提示后经 fatal_error 终止进程。用
        // OpcodeNotImplemented(Internal 类)而非 NotImplemented(Semantic 类):后者经 Error 通道
        // 服务编译期 CodeGen not_impl(可恢复 CompileError);此处是运行期执行到未实现 opcode,
        // 不可恢复,属解释器实现不完整(Internal)。
        [[noreturn]]
        void not_implemented(const StringView op_name) {
            fatal_error(ErrorCode::OpcodeNotImplemented,
                        "opcode '{}' not implemented yet (out of current scope: fields/index/classes come later)",
                        op_name);
        }

        // 执行跟踪:每条指令执行**前**打印字节码/栈/帧/模块信息(stderr,调试用,详尽优先于简洁;
        // 与 GC 调试日志 / DEBUG_PRINT_COMPILED_CODE 同走 stderr,与 PRINT 的 stdout 分流)。常态
        // 编译(与 DEBUG_PRINT_COMPILED_CODE 同形,宏只守 dispatch_loop 内调用点),关闭时无调用点,
        // 热路径零开销。供 dispatch_loop 主循环顶在取 opcode 前调用 -- 此时 frame.ip 指向待执行
        // 指令,据此解码(仅读不推进 VM 的 ip)。
        //   - 栈渲染经 format_value_debug(Value 层非重入渲染),不用 format_value -- 后者 Obj 走
        //     可重载虚 to_string,trace 在 dispatch_loop 内会重入 VM 致无限递归;debug_repr 纯 C++,
        //     绝不触用户重载。
        //   - 三行:字节码行(模块 + 栈顶帧 fn 名 @ip 偏移 + 指令反汇编)/ 栈行(值栈逐槽渲染,
        //     ^ 对齐当前帧栈底所在槽,联动指示本帧局部区)/ 帧栈底标记行。
        [[maybe_unused]] void trace_execution(Movement& ctx) {
            auto&       frames = ctx.frames();
            const auto& frame  = frames.top();
            const auto  ip_off = static_cast<usize>(frame.ip - frame.unit->code.data());
            const auto  instr  = Disassembler::disassembleInstruction(frame.unit, ip_off);

            io::print(stderr, "[trace] {}  {} @{:04X}  {}\n", frame.module->to_string(),
                      frame.closure->function()->to_string(), static_cast<u32>(ip_off), instr);

            // 栈行与 ^ 列号一趟同步算:^ 对齐到当前帧栈底(slots 所指槽)的 [ 下方;slots 越过
            // 栈顶时(异常态)所有槽位都满足 p < slots,累加自然停在全部段之和,无需分支。
            const String prefix = std::format("        stack[{}]: ", ctx.stack_size());
            usize        col    = prefix.size();
            String       stack_str;
            for (Value* p = ctx.stack_base(); p < ctx.stack_top(); ++p) {
                const auto s = std::format("[ {} ]", format_value_debug(*p));
                if (p < frame.slots) {
                    col += s.size();
                }
                stack_str += s;
            }
            if (stack_str.empty()) {
                stack_str = "(empty)";
            }

            io::print(stderr, "{}{}\n", prefix, stack_str);

            // 帧栈底标记行:行首纯空格对齐,只留 ^ 与 frame 索引(fn/ip 已在字节码行,不重复)。
            io::print(stderr, "{}^ frame[{}]\n", String(col, ' '), static_cast<u32>(frames.size() - 1));
        }

    } // namespace

    // 构造:成员初始化,再把 VM 根 tracer 注册进自有 GC,bootstrap Object 根类(先行使
    // object_class_ 进 tracer 根集),最后一次性注册 VM 级 builtins。tracer 为 lambda:[this]
    // 捕获,标记四类根:
    //   1) modules_:解释器级共享模块表(进而 trace 各模块 name_/dir_/entry_/globals_);
    //   2) builtins_:VM 级只读 builtins 表;
    //   3) object_class_:Object 根类(VM 成员单独持有,裸名解析四层全部够不到,须单独标根);
    //   4) current_ 执行链:自 *current_ 沿 previous_ 走到链尾逐个标 --
    //        a) 值栈 [base, top) 全部 Value:run() 期局部/实参/临时值只活在栈上,是最关键的
    //           缺失根;run() 结束 reset() 清空,run() 外 GC 不会标到陈旧栈值;
    //        b) 各活动帧的 closure/module:直标更稳,免依赖「帧函数必在其父常量池」不变式;
    //        c) 挂起错误寄存器载荷:置入寄存器到 take_error 取出之间可能跨安全点分配,须标根;
    //        d) open upvalue 开链:闭包已死而 upvalue 仍在链的悬垂防线(clox 已知坑)。
    //      链尾断言恒 &main_ctx_:锁定「resume/yield 严格成对」的切换纪律(见 Movement.hpp)。
    // VM 持有 gc_(值成员),成员逆序析构下 gc_ 最后析构,tracer 与各成员同生共死,无需析构注销。
    AriaVM::AriaVM() :
        gc_{}, main_ctx_{&gc_}, current_{&main_ctx_}, modules_{&gc_}, builtins_{&gc_}, source_roots_{},
        object_class_{nullptr} {

        gc_.set_vm_roots([this](GC& g) {
            modules_.trace(g);
            builtins_.trace(g);
            // Object 根类:VM 成员单独持有,须由 tracer 单独标根(经其 trace 级联整张类成员图)。
            g.mark_object(object_class_);
            [[maybe_unused]] Movement* tail = nullptr;
            for (Movement* m = current_; m != nullptr; m = m->previous()) {
                for (Value* p = m->stack_base(); p < m->stack_top(); ++p) {
                    g.mark_value(*p); // mark_value 对非对象 Value no-op,栈槽含 int/f64/bool/nil 安全
                }
                if (const auto& pending = m->pending_error(); pending.has_value()) {
                    g.mark_value(*pending);
                }
                for (const auto& f: m->frames().span()) {
                    g.mark_object(f.closure); // 闭包:trace 级联标 function/upvalues(upvalue 再标槽值/闭值);容 nullptr
                    g.mark_object(f.module);
                }
                // open upvalue 开链:节点可能仅被本链引用(闭包已死),必须单独标根
                // (闭包可达时另经闭包 trace 双标,mark 幂等无害)。
                for (ObjUpvalue* uv = m->open_upvalues(); uv != nullptr; uv = uv->next_open()) {
                    g.mark_object(uv);
                }
                tail = m;
            }
            ASSERT(tail == &main_ctx_, "VM roots: context chain must terminate at main_ctx_");
        });

        // 源根默认值初始化(纯路径配置,与 GC 无关):入口槽占位 + stdlib 配置根。
        init_source_roots();

        // Object 根类 bootstrap(tracer 已就绪,object_class_ 先入根集,后续触 GC 安全)。
        bootstrap_object_class();

        // VM 级 builtins 一次性注册:在建对象经 register_builtins 内 make_guard 双守卫根化,
        // 已入表条目经上方 tracer 标根(见 Builtins.cpp)。全 VM 共享一份,不按模块注入。
        builtins::register_builtins(gc_, builtins_);
    }

    void AriaVM::bootstrap_object_class() {
        // M5 决策 3:Object 根类构造期 bootstrap -- ObjClass("Object", super=nullptr) + 原生
        // no-op init(Value 化:不合成 ObjFunction/字节码,保「ObjFunction::module 恒非空」全项目
        // 不变式)。no-op 语义:原生收到 slots[0] = this(call_class 已把 callee 槽原位换实例),
        // 返回 true 不写槽 -- 槽 0 原样即返回实例,「Foo() 得实例」天然成立。
        // 类表 upsert("init", native) + init_ 指同一值(表槽/init_ 一致;其余类 ctor 自 super
        // 派生,set_field 命中 "init" 同步)。名串经工厂 StringView 重载内部 intern 自守;
        // "init" 表键经 intern 命中(与 init_native 的 name 串同指针,零分配)。
        auto guard = gc_.make_guard();

        const auto init_native = new_native_fn(gc_, "init", [](AriaVM&, Span<Value>) { return true; });
        guard.push(init_native);
        auto cls = new_class(gc_, "Object", nullptr); // Object 根:super==nullptr,不经 MAKE_CLASS,init 由本函数设
        guard.push(cls);
        const auto init_key = new_string(gc_, "init"); // intern 命中:= init_native 的 name 串,零分配
        guard.push(init_key);
        // 表槽/init_ 一致:set_field 命中 "init" 即同步 init_。
        cls->set_field(init_key, Value::from_obj(init_native));
        object_class_ = cls; // 发布进 VM 成员:此后经 tracer 第 4 根保命
    }

    void AriaVM::init_source_roots() {
        // source_roots_[0] = 入口槽:构造时占位为 cwd(REPL / 未显式设 dir_ 时裸名搜 cwd,且保证
        //   [0] 槽位恒在,run() 可直接赋值);cwd 不可用时以空串兜底(resolve_module 裸名分支
        //   跳过空根,比拿 "." 锚到坏目录更诚实)。
        source_roots_.push_back(fs::current_dir().value_or(""));
        // source_roots_[1..] = 配置根:编译器相对 stdlib 源根(约定 <exe_dir>/../share/aria/lib),
        //   weakly_canonical 规范化,推导失败跳过。可经 set_source_roots 覆盖。
        if (const auto pd = fs::program_dir()) {
            const auto      stdlib = stdfs::path{*pd} / kStdlibRelPath;
            std::error_code ec;
            if (const auto real = stdfs::weakly_canonical(stdlib, ec); !ec && !real.empty()) {
                source_roots_.push_back(real.string());
            }
        }
    }

    void AriaVM::set_source_roots(List<String> roots) noexcept {
        // 替换配置根 [1..],保留入口槽 [0] -- 入口槽归 run() 管。
        source_roots_.resize(1);
        for (auto& r: roots) {
            source_roots_.push_back(std::move(r));
        }
    }

    Result<Value, Error> AriaVM::run(SourceFile& source, ObjModule* module) {
        // 编译并执行:把 source 编进 module 的入口 ObjFunction,再委托 run(ObjFunction*) 执行。
        // module 由调用方提供(控制 name/root 身份),编译期由 CodeGen::compile 内部 make_guard
        // 根化;source 须存活到本函数返回(编译期 Error 的 SourceLoc 指向它)。编译失败原样
        // 透传首错 Error,不进入执行。
        auto compiled = Compiler{gc_}.compile(source, module);
        if (!compiled.has_value()) {
            return std::unexpected(compiled.error());
        }
        // 内置函数不经此注册 -- VM 级 builtins_ 表由 ctor 一次性填充(见 Builtins.hpp)。
        return run(compiled.value());
    }

    InterpretResult AriaVM::interpret_run(SourceFile& source, ObjModule* module) {
        // 编译并执行，按**失败阶段**分类（不按错误码大类反推时机）：编译期失败 -> CompileError，
        // run 期失败 -> RuntimeError。分类语义：CompileError 意为「主入口编译失败，程序从未开始
        // 执行」；run 期浮现的一切错误归 RuntimeError -- 含运行期才抛的 UndefinedVariable，与经
        // IMPORT 站点异常通道传播的被导入模块编译期错误（可被 try/catch 捕获，「可 catch 的错误」
        // 不构成 CompileError）。失败时渲染 Error 消息到 stderr，只回类别。
        auto compiled = Compiler{gc_}.compile(source, module);
        if (!compiled.has_value()) {
            io::println(stderr, "{}", compiled.error().message());
            return InterpretResult::CompileError;
        }
        auto result = run(compiled.value());
        if (!result.has_value()) {
            io::println(stderr, "{}", result.error().message());
            return InterpretResult::RuntimeError;
        }
        return InterpretResult::Ok;
    }

    InterpretResult AriaVM::interpret_from_src(const StringView src) {
        // 合成入口模块 <script>（dir=cwd，2 参 new_module）。new_module 返回 GC 管理对象,
        // make_guard 根化跨编译+执行(编译期 CodeGen::compile 亦自守)。
        const auto module = new_module(gc_, kScriptModuleName);
        auto       guard  = gc_.make_guard(module);

        // 字符串源 SourceFile(名 = kScriptModuleName,无文件身份);方法内局部,存活至返回,Error 渲染不悬垂。
        SourceFile source{String{kScriptModuleName}, String{kScriptModuleName}, String{src}};
        return interpret_run(source, module);
    }

    InterpretResult AriaVM::interpret_from_path(const StringView path) {
        // 读盘 + BOM 剥除 + CRLF 归一化 + UTF-8 校验。失败渲染路径并返 LoadError（无 SourceFile，无 SourceLoc）。
        auto loaded = SourceFile::from_path(path);
        if (!loaded.has_value()) {
            io::println(stderr, "无法加载源文件 '{}'", path);
            return InterpretResult::LoadError;
        }
        SourceFile source = std::move(loaded.value());

        // 入口模块身份（dirname + basename）：name = basename 去 .aria、dir = dirname(absolute(path))，
        // 拆分收口于 fs::module_name_and_dir。name 为空表路径非合法文件模块（目录 / 空 / 无文件名），
        // 报 LoadError 而非静默兜底--与读盘失败同属「加载不到合法源文件」。
        auto [name_s, dir_s] = fs::module_name_and_dir(path);
        if (name_s.empty()) {
            io::println(stderr, "源文件路径无有效模块名: '{}'", path);
            return InterpretResult::LoadError;
        }
        auto module = new_module(gc_, name_s, dir_s); // 3 参：显式 dir
        auto guard  = gc_.make_guard(module);

        return interpret_run(source, module);
    }

    Result<Value, Error> AriaVM::run(ObjFunction* fn) {
        // 程序入口仪式:入口纪律断言 + 源根入口槽播种 + 前后清场;执行本体委托下方 run_function
        // (仪式与本体分层)。进主循环前 current_ 必已归位 main_ctx_:run() 是唯一驱动入口,
        // resume/yield 只换走 current_ 不产生新循环(vm-design.md §4.9),每次进 run() 必从主
        // 上下文起步。
        ASSERT(current_ == &main_ctx_, "AriaVM::run: current_ is not main_ctx_ (unbalanced context switch)");
        // GC 已启用:值栈/帧经 vm_roots tracer 标根(见 ctor),IMPORT/DEF_GLOBAL 等已按「栈即根」
        // 前置编写(peek-not-pop)。
        // 源根:入口槽 [0] 原地替换为入口模块 dir_(对齐 Python sys.path[0] -- 入口文件所在目录
        // 居首,配置根在 [1..] 不动)。直接赋值 [0],无 flag、reuse 安全。dir_ 内容可空(<script>
        // 在 cwd 不可用时退化),空串由 resolve_module 裸名分支跳空根处理,此处不判空。
        source_roots_[0] = fn->module()->dir()->view();

        // 重复调用先清场(同 Lexer/Parser 式复用):HALT 收场的上一轮不弹帧(dispatch_loop 的 HALT 分支
        // 直接返回),不清场会把新帧叠在陈旧帧上、顶层 RETURN 后驱动陈旧帧的 ip。
        main_ctx_.reset();

        // result 为值拷贝,下方清场不影响返回值;返回值若持对象,由调用方自行根化
        // (Guard / vm 存活),同既有契约。
        auto result = run_function(fn);
        // 结束再清场:确保 run() 外(后续 compile / 测试显式 collect)GC 不会经 tracer 标到陈旧
        // 栈值。清场责任归本入口 -- run_function 无自清场(重入接缝不得 reset,见其注释)。
        main_ctx_.reset();
        return result;
    }

    Result<Value, Error> AriaVM::run_function(ObjFunction* fn) {
        // 执行本体(无入口装饰):入口 fn 现场包空闭包(统一「帧 = 闭包」模型)后压 callee +
        // 进帧 + 驱动 dispatch_loop,作用于 *current_(与 dispatch_loop/call_value 族/raise 的
        // current_ 纪律同源)。run() 的被委托方,未来重入的接缝(vm-design.md §4.7):不播源根、
        // 不 reset(冲掉重入调用者的栈)、不断言主上下文;落地时升公开。
        // 根安全:fn 在 new_closure 顶 maybe_collect 时须有根(run() 路径经 module->entry_ 模块根
        // /测试 guard,重入路径属调用方契约),此处 make_guard 兜底 -- 该 GC 点是 fn 唯一无根窗口
        // (守卫真实承重)。闭包建成立即入栈:push 无 GC 点,入栈即经值栈 tracer 根化,无需跨 push
        // 守卫;入帧后另有帧(tracer)双根。
        auto       fn_guard = gc_.make_guard(fn);
        const auto closure  = new_closure(gc_, fn);
        // 顶层入口无参数:slots 指向槽 0(callee)。
        current_->push(Value::from_obj(closure)); // callee 值躺在主帧槽 0(RETURN 时弹),入栈即根
        current_->enter_frame(closure, 0);
        return dispatch_loop();
    }

    bool AriaVM::call_value(const Value callee, const u8 argc) {
        if (!callee.is_obj()) {
            return fail(ErrorCode::CallNonCallable, "call non-callable {}", type_name(callee));
        }

        switch (Object* obj = callee.as_obj(); obj->type()) {
            case ObjType::CLOSURE:
                return call_closure(Object::as<ObjClosure>(obj), argc);
            case ObjType::NATIVE_FN:
                return call_native(Object::as<ObjNativeFn>(obj), argc);
            case ObjType::CLASS:
                return call_class(Object::as<ObjClass>(obj), argc);
            case ObjType::BOUND_METHOD:
                return call_bound_method(Object::as<ObjBoundMethod>(obj), argc);
            default:
                // 其余 Obj 类型不可调用:经 op_call 协议基类默认 fail(文案在 Object.cpp)。
                // 未来新增可调用对象类型 override op_call 即接入调用协议,无需改本 switch。
                return obj->op_call(*this, Span<Value>{&current_->peek(argc), static_cast<usize>(argc + 1)});
        }
    }

    bool AriaVM::call_class(ObjClass* obj, const u8 argc) {
        // 类实例化(call_value CLASS 分支):new_instance 是唯一 GC 点(obj 经值栈根化),
        // instance 建成即写 callee 槽 -- **槽 0 原位换实例**(即新帧的 this / 原生 init 的
        // slots[0]),余下交 call_value 通用分发,与 call_bound_method 同款「槽 0 调用方改写」
        // 约定:init 为闭包则经 call_closure 进方法帧(编译器尾部 LOAD_LOCAL 0; RETURN 使 init
        // 返回 this)、为原生则同步调用(no-op 不动 slots[0] 即返回实例)、为非可调用值(类上赋
        // Foo.init = 5 经 store_field 放行)则 call_value 报 CallNonCallable 兜底。init_ 恒有值
        // (ctor 自 super 派生/MAKE_METHOD 覆盖/类上赋值同步),无空判与快路径。
        const auto instance  = new_instance(gc_, obj);
        current_->peek(argc) = Value::from_obj(instance); // 建成即写槽:instance 经值栈根化(即新帧 this)
        return call_value(obj->init(), argc);
    }

    bool AriaVM::call_bound_method(const ObjBoundMethod* obj, const u8 argc) {
        // 绑定方法调用(call_value BOUND_METHOD 分支):调用区 [bound, a1..aN] 的槽 0 恰为
        // bound 对象 -- **原位覆写为 receiver**(this 替代 callee,实参槽位不动,零整形),余下
        // 交通用 call_value 分发:闭包走 call_closure 进方法帧(闭包经 frame.closure 携带不上栈)、
        // 原生走 call_native(槽 0 即原生契约的 this,兼返回槽)。
        // 方法值无需守卫:覆写槽 0 后唯一 GC 窗口是方法执行期 -- 闭包经 frame.closure 由帧 tracer
        // 标根;原生经类表槽可达(实例路径 this->fields 缓存回填了 bound、super 路径
        // frame.closure->defining_class 链),回收亦无害。
        current_->peek(argc) = obj->receiver(); // 槽 0:bound -> this(实参槽位不动)
        return call_value(obj->method(), argc);
    }

    bool AriaVM::call_closure(ObjClosure* obj, const u8 argc) {
        if (const auto arity = obj->function()->arity(); arity != argc) {
            // 报错带函数名:实例化经 call_class 委托至此,init 的元数错误同样指名("function 'init' ...")。
            return fail(ErrorCode::WrongArity, "function '{}' expects {} args, got {}", obj->function()->name()->view(),
                        arity, argc);
        }

        if (current_->frames_full()) {
            return fail(ErrorCode::StackOverflow, "call frame stack overflow");
        }
        // 经 current_ 访问(与 dispatch_loop/raise 同源,语义统一):enter_frame 进帧,
        // 与 dispatch_loop 的 exit_frame 对称。
        current_->enter_frame(obj, argc);
        return true;
    }

    bool AriaVM::call_native(const ObjNativeFn* obj, const u8 argc) {
        // 原生函数同步调用,不进帧(bool 成败信号 / 返回值写槽 0 / 错误走侧信道,契约见
        // ObjNativeFn.hpp)。经 current_ 访问调用区与寄存器(与 dispatch_loop/raise 同源);
        // peek(argc) 即槽 0,叶子调用不增长值栈故指针稳定。
        //
        // entered_ctx = 调用发生时的上下文,本函数全部事后簿记(drop/寄存器断言)的统一锚点:
        // M6 前 current_ 恒等于它(下方守卫断言锁定「原生调用不得换走 current_」);M6 切换
        // 合法化后仅删成功路径守卫、其余一字不动(false 路径守卫为永久契约:禁止 false+切换)。
        // 见 vm-design.md §4.9「切换协议」。实参留栈到 drop 亦是 GC 红利:切换型原生函数执行
        // 全程,实参(含协程对象)皆调用者栈根。
        const auto entered_ctx = current_;
        const auto slots       = Span<Value>{&current_->peek(argc), static_cast<usize>(argc + 1)};
        // 进场前寄存器应空(上次错误已被 take_error 取走 / reset 清空)。
        ASSERT(!current_->has_error(), "call_native: pending error not cleared before native call");
        if (obj->fn()(*this, slots)) {
            ASSERT(current_ == entered_ctx,
                   "call_native: current_ not restored across native call"); // M6 删:切换合法化
            // 成功:断言寄存器空(契约 return true ⟺ 未 raise);release 不静默清掉掩盖。
            // drop 实参使返回值升栈顶;一律落在 entered_ctx 上,为 M6 切换形态预铺。
            ASSERT(!entered_ctx->has_error(), "native fn returned true but raised error");
            entered_ctx->drop(argc);
            return true;
        }
        ASSERT(current_ == entered_ctx, "call_native: current_ not restored across native call"); // M6 留:契约守卫
        // 失败:载荷留寄存器交调用方 take_error 取出沿 runtime_err 传播,不在此取出 -- 与
        // call_closure/call_value 的 bool 契约统一(return false ⟺ 已 raise 进调用方上下文)。
        ASSERT(entered_ctx->has_error(), "native fn returned false but raised no error");
        return false;
    }

    ObjModule* AriaVM::load_module(ObjString* canonical_path, const StringView import_specifier) {
        // IMPORT 未命中分支的加载层(契约与两类失败形态总览见 AriaVM.hpp 声明处注释)。
        // canonical_path 已由调用方根化(IMPORT case 的 canonical_path_guard,跨本函数内 upsert)。
        // 步骤:读盘 -> 派生模块身份 -> new_module + 自守 -> 入表占位 -> 编译(set_entry)。
        // 加载事实源 = modules_ 表成员资格(对象无状态字段):入表即「已加载(体待 run-once 或
        // 已跑完)」,循环导入命中表内半初始化对象即复用。

        // 1. 读盘:BOM 剥除 + CRLF->LF + UTF-8 校验。resolve_module 已 exists-check,但读盘/
        //    编码仍可能失败(权限竞争 / 非法 UTF-8)。失败报 ModuleNotFound(带路径)。
        auto loaded_src = SourceFile::from_path(canonical_path->view());
        if (!loaded_src.has_value()) {
            return fail(ErrorCode::ModuleNotFound, "failed to load module '{}': read/decode error", import_specifier);
        }
        SourceFile source = std::move(loaded_src.value());

        // 2. 派生模块身份 {name=stem, dir=dirname}:abs_path() = dir_ + "/" + name_ + ".aria"
        //    还原 canonical key,相对导入基(dirname)正确。
        auto [name_s, dir_s] = fs::module_name_and_dir(canonical_path->view());
        if (name_s.empty()) {
            return fail(ErrorCode::ModuleNotFound, "module path has no valid name: '{}'", import_specifier);
        }

        // 3. 建模块(工厂内部 intern name/dir 并自守)+ 自守跨 upsert/编译。
        auto module = new_module(gc_, name_s, dir_s);
        auto guard  = gc_.make_guard(module);

        // 4. 入表占位:模块体尚未跑,但表内已有 -- 循环导入命中此半初始化对象直接复用。
        //    canonical_path 已由调用方根化;module 由上方 guard 根化,upsert rehash 触 GC 时皆安全。
        const auto mod_entry = modules_.upsert(Value::from_obj(canonical_path));
        mod_entry->value     = Value::from_obj(module);

        // 5. 编译:入口 ObjFunction 名 kModuleEntryName("<module>"),CodeGen::init_module 已
        //    module.set_entry。编译期 Error(位置指向被导入文件内部)就地 new_exception 装箱入
        //    寄存器(from_baked 语义,消息不重烘 -- 不经 AriaVM::raise 会叠上导入方站点前缀)。
        //    module 双重根化(guard + modules_ 表);source 须存活到 compile() 返回(Error 烘
        //    位置串需它)。entry 经 module->entry_ 根可达。
        Compiler compiler{gc_};
        if (auto compiled = compiler.compile(source, module, kModuleEntryName); !compiled) {
            auto err = std::move(compiled).error();
            current_->raise(Value::from_obj(new_exception(gc_, err.code(), err.message())));
            return nullptr;
        }
        // 内置函数不经此注入 -- 由 VM 级 builtins_ 表统一承载,LOAD_GLOBAL 模块 globals 未命中后回退查之。
        return module;
    }

    template<OpCode Op>
    bool AriaVM::run_binary_numeric() {
        // 弹 2 算 1(9 个算术/比较指令共用):双 Int 走整数路径,任一 F64 升浮点 -- int 除/模零
        // 报错、% 为 C++ 语义,f64 按 IEEE(除零得 inf/nan)。失败经 fail 装箱后返 false
        // (bool 契约见 AriaVM.hpp)。
        const Value b = current_->pop();
        const Value a = current_->pop();
        if (!(is_num(a) && is_num(b))) {
            return fail(ErrorCode::TypeMismatch, "operator '{}' requires numbers, got {} and {}", op_symbol(Op),
                        type_name(a), type_name(b));
        }
        if (a.is_int() && b.is_int()) {
            const auto x = a.as_int();
            const auto y = b.as_int();
            if constexpr (Op == OpCode::ADD) {
                current_->push(Value::from_int(x + y));
            } else if constexpr (Op == OpCode::SUBTRACT) {
                current_->push(Value::from_int(x - y));
            } else if constexpr (Op == OpCode::MULTIPLY) {
                current_->push(Value::from_int(x * y));
            } else if constexpr (Op == OpCode::DIVIDE) {
                if (y == 0) {
                    return fail(ErrorCode::DivisionByZero, "integer division by zero");
                }
                current_->push(Value::from_int(x / y));
            } else if constexpr (Op == OpCode::MOD) {
                if (y == 0) {
                    return fail(ErrorCode::ModuloByZero, "integer modulo by zero");
                }
                current_->push(Value::from_int(x % y));
            } else if constexpr (Op == OpCode::GREATER) {
                current_->push(Value::from_bool(x > y));
            } else if constexpr (Op == OpCode::GREATER_EQUAL) {
                current_->push(Value::from_bool(x >= y));
            } else if constexpr (Op == OpCode::LESS) {
                current_->push(Value::from_bool(x < y));
            } else if constexpr (Op == OpCode::LESS_EQUAL) {
                current_->push(Value::from_bool(x <= y));
            } else {
                UNREACHABLE(); // Op 恒为上列 9 个二元指令之一(调用点穷举)
            }
            return true;
        }
        const auto x = a.is_f64() ? a.as_f64() : static_cast<f64>(a.as_int());
        const auto y = b.is_f64() ? b.as_f64() : static_cast<f64>(b.as_int());
        if constexpr (Op == OpCode::ADD) {
            current_->push(Value::from_f64(x + y));
        } else if constexpr (Op == OpCode::SUBTRACT) {
            current_->push(Value::from_f64(x - y));
        } else if constexpr (Op == OpCode::MULTIPLY) {
            current_->push(Value::from_f64(x * y));
        } else if constexpr (Op == OpCode::DIVIDE) {
            current_->push(Value::from_f64(x / y)); // IEEE: 除零得 inf/nan
        } else if constexpr (Op == OpCode::MOD) {
            current_->push(Value::from_f64(std::fmod(x, y)));
        } else if constexpr (Op == OpCode::GREATER) {
            current_->push(Value::from_bool(x > y));
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            current_->push(Value::from_bool(x >= y));
        } else if constexpr (Op == OpCode::LESS) {
            current_->push(Value::from_bool(x < y));
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            current_->push(Value::from_bool(x <= y));
        } else {
            UNREACHABLE();
        }
        return true;
    }

    bool AriaVM::run_load_field(ObjString* name) {
        // LOAD_FIELD 执行体(契约与 peek-not-pop 理由见 AriaVM.hpp「field 族指令执行体」):
        // 非对象(含 nil)是协议外的原语,统一「type X does not support field access」文案
        // (协议分派需要 Object*,原语不进协议);对象 miss 的文案由协议 override 烘焙
        // (nullopt ⟺ 已 fail),本函数只透传信号。
        const Value obj = current_->peek(0);
        if (!obj.is_obj()) {
            return fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (const auto result = obj.as_obj()->load_field(*this, name)) {
            current_->peek(0) = *result; // 写回原槽:[obj] -> [v]
            return true;
        }
        return false; // 载荷已在寄存器(协议 fail 契约:nullopt ⟺ 已 fail)
    }

    bool AriaVM::run_store_field(ObjString* name) {
        // STORE_FIELD 执行体([obj, v] -> [v],完成时单槽下移;契约见 AriaVM.hpp):非对象守卫
        // 同 run_load_field;对象 false ⟺ override 已按自身措辞 fail 入寄存器,本函数只透传信号。
        const Value obj = current_->peek(1);
        if (!obj.is_obj()) {
            return fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (!obj.as_obj()->store_field(*this, name, current_->peek(0))) {
            return false; // 载荷已在寄存器(协议 fail 契约:false ⟺ 已 fail)
        }
        current_->peek(1) = current_->peek(0); // 单槽下移:弹 obj 留 v,[obj, v] -> [v]
        current_->drop(1);
        return true;
    }

    bool AriaVM::run_load_super_field(ObjString* name) {
        // LOAD_SUPER_FIELD 执行体(契约见 AriaVM.hpp):defining class 从 frame.closure 直读
        // (方法闭包恒有戳 -- MAKE_METHOD 注册时设,编译器不变式);从父类起走 load_field 协议
        // 沿链查,miss:类措辞 fail 已入寄存器,本函数只透传信号。
        const auto& frame    = current_->frames().top();
        const auto  defining = frame.closure->defining_class();
        ASSERT(defining != nullptr, "LOAD_SUPER_FIELD: closure has no defining class (compiler invariant)");
        const auto super = defining->superclass();
        ASSERT(super != nullptr, "LOAD_SUPER_FIELD: method class has no superclass (compiler invariant)");
        const auto hit = super->load_field(*this, name); // 从父类起读穿透(类协议)
        if (!hit) {
            return false; // 载荷已在寄存器(类措辞 miss,契约透传)
        }
        if (const auto member = *hit; is_method(member)) {
            // 方法命中:建成立即压栈(push 无 GC 点),this 经帧槽根化;方法对象经 super 链根可达(帧经 tracer 根)
            current_->push(Value::from_obj(new_bound_method(gc_, member, frame.slots[0])));
        } else {
            current_->push(member); // 静态槽原值直读(push 无分配无 GC 点),不绑定不缓存(铁则 2)
        }
        return true;
    }

    Opt<Error> AriaVM::unwind() {
        // 自最内帧向外遍历(pitfalls 坑 #13):每帧以 last_ip(顶帧 = 故障指令起始 / 外层帧 =
        // CALL 站点,均由 dispatch_loop 循环顶写好,坑 #2)反推 offset 查本帧异常记录表。
        // 首命中即在该帧 unwind -- 此前轮次已逐帧 exit_frame 弹掉全部内层帧,本帧即栈顶,
        // 无需 FrameStack::truncate。前提:寄存器已有载荷(契约见 AriaVM.hpp;take 后解引用
        // 空可选是 UB,入口断言把关 -- write 侧 Movement::raise 断言的 read 侧成对)。
        // 未捕获跟踪条目(仅本函数消费):fn/mod 物化路径只有 String 拼接、无 GC 分配点不悬垂;
        // ip_off 收集时就地换算 -- 帧随即被 exit_frame 弹掉,last_ip/unit 不可后取。
        struct TraceEntry {
            ObjFunction* fn;     // 帧函数(名字渲染)
            ObjModule*   mod;    // 帧模块(位置串渲染,module_loc)
            u32          ip_off; // 行号经 fn->unit().line_for_offset 查
        };

        ASSERT(current_->has_error(), "unwind: no pending payload");
        List<TraceEntry> trace; // 收集序:内 -> 外;物化时反转为外 -> 内(Python 式 most recent call last)
        while (!current_->frames().empty()) {
            auto& [closure, unit, module, ip, slots, last_ip] = current_->frames().top();
            const auto ip_off                                 = static_cast<u32>(last_ip - unit->code.data());
            if (const auto rec = unit->find_try_handler(ip_off)) {
                // 命中 handler:先关被丢弃区间的开指(try 体被捕获局部在此迁移自持;槽区尚存活,
                // close 的迁值读安全),再截值栈到本帧 slots + stack_depth(stack_depth 相对
                // frame.slots 非全局基址,坑 #6),ip 跳 handler 入口;寄存器载荷 push 落
                // catch 参数槽(恒 == stack_depth,坑 #10)。take 清寄存器到 push 之间无分配,
                // 载荷不失根。
                const auto record = *rec;
                current_->close_upvalues(slots + record->stack_depth);
                current_->truncate_stack(static_cast<usize>(slots + record->stack_depth - current_->stack_base()));
                ip = unit->code.data() + record->handle;
                current_->push(*current_->take_error());
                return std::nullopt; // 已派发 handler,调用方 break 回循环顶重取帧(坑 #11)
            }
            // 未命中:帧即将被弹,先记跟踪三元组再退帧(帧存活时收集,坑 #16 点 2);exit_frame
            // 内置关本帧区间开指(槽区存活时迁值安全)。
            trace.push_back(TraceEntry{closure->function(), module, ip_off});
            current_->exit_frame();
        }

        // 全帧未命中 -> 未捕获:寄存器载荷反提拆 (码, 烘焙消息) 两件(ObjException 原码原消息,
        // re-throw 保码;用户 throw 原值兜底 UncaughtException,坑 #7),跟踪逐帧烘焙进消息尾部
        // (透传的编译期 Error 不经本路径,无跟踪 -- 坑 #16 点 6),拼完经 from_baked 一次物化。
        // trace 恒非空(dispatch_loop 各调用点帧栈非空不变式)。
        auto [code, msg] = uncaught_error_parts(*current_->take_error());
        for (const auto& [fn, mod, ip_off]: std::views::reverse(trace)) {
            const auto line = fn->unit().line_for_offset(ip_off);
            msg += std::format("\n  at {} ({})", fn->name()->view(), module_loc(mod, line));
        }
        return Error::from_baked(code, msg);
    }

    Result<Value, Error> AriaVM::dispatch_loop() {
        // 语义统一:栈/帧/错误寄存器一律经 current_ 访问当前上下文。M6 单循环切换模型下
        // 「正在执行的字节码所在上下文恒等于 current_」是结构性事实:切换只发生在原生函数体内
        // (resume/yield 换走 current_,call_native 探测发现后循环顶自然采用新上下文),本循环
        // 永不重入;M6 前 call_native 断言锁「原生调用不得换走 current_」。见 vm-design.md §4.9。

        while (true) {
            // 不变式:此处帧栈恒非空(run() 先 enter_frame 才进本循环;唯一弹空帧的 RETURN 顶层
            // 分支立即 return;CALL/IMPORT 切帧后 break 回到循环顶重取)。
            // 取指前记本帧指令起始指针:报错定位的行号锚点 -- 顶帧报错即故障指令、call_*/原生
            // 失败即 CALL 站点(被调帧未进 / 原生不进帧,顶帧仍是 caller);M3 unwind 查表同用
            // 此字段(坑点文档 #1/#2)。init_frame_ 置 code 起始,首次取指前即覆写。
            CallFrame& frame = current_->frames().top();
            frame.last_ip    = frame.ip;
#ifdef DEBUG_TRACE_EXECUTION
            // 取 opcode 前打印执行状态(见 trace_execution)。
            trace_execution(*current_);
#endif
            // 各 case 严格按 bytecode/code.hpp 中 OpCode 枚举的声明顺序排列。
            // 退出约定:一律 break(switch 即整个 while 体,其后无语句,与 continue 等效)。
            // unwind 派发 handler 后帧引用已废,同样靠 break 回循环顶重取 --
            // **switch 之后不得新增引用 frame 的代码**,否则派发路径踩陈旧帧(坑 #11 的防御前提)。
            switch (auto op = static_cast<OpCode>(read_u8(frame))) {
                case OpCode::HALT:
                    return Value::nil_val();

                // ---- 数据加载与存储 ----
                case OpCode::LOAD_CONST: {
                    const auto idx = read_u16(frame);
                    current_->push(frame.unit->constants[idx]);
                    break;
                }
                case OpCode::LOAD_NIL:
                    current_->push(Value::nil_val());
                    break;
                case OpCode::LOAD_TRUE:
                    current_->push(Value::true_val());
                    break;
                case OpCode::LOAD_FALSE:
                    current_->push(Value::false_val());
                    break;
                case OpCode::LOAD_IMM: {
                    const u8 raw = read_u8(frame);
                    // u8 操作数按 i8 位型重解释做符号扩展(发射侧先经 i8 再转 u8,见 CodeGen)。
                    current_->push(Value::from_i32(std::bit_cast<i8>(raw)));
                    break;
                }
                case OpCode::LOAD_LOCAL: {
                    const u8 slot = read_u8(frame);
                    current_->push(frame.slots[slot]);
                    break;
                }
                case OpCode::STORE_LOCAL: {
                    const u8 slot     = read_u8(frame);
                    frame.slots[slot] = current_->peek(0);
                    break;
                }
                case OpCode::LOAD_LOCAL_L: {
                    const auto slot = read_u16(frame);
                    current_->push(frame.slots[slot]);
                    break;
                }
                case OpCode::STORE_LOCAL_L: {
                    const auto slot   = read_u16(frame);
                    frame.slots[slot] = current_->peek(0);
                    break;
                }
                case OpCode::LOAD_UPVALUE: {
                    // idx:u8;[] -> [v]:压本闭包第 idx 个 upvalue 的当前值(开指栈槽或已关闭值,
                    // 统一经 value_slot() 取址,两态不分叉,见 ObjUpvalue)。
                    const u8 idx = read_u8(frame);
                    current_->push(*frame.closure->upvalues()[idx]->value_slot());
                    break;
                }
                case OpCode::STORE_UPVALUE: {
                    // idx:u8;[v] -> [v]:peek-store 到该 upvalue(留栈顶值,与 STORE_LOCAL 同形)。
                    // 写经 value_slot():open 态写穿到栈槽(外层可见),closed 态写自持 closed_。
                    const u8 idx                                  = read_u8(frame);
                    *frame.closure->upvalues()[idx]->value_slot() = current_->peek(0);
                    break;
                }
                case OpCode::CLOSE_UPVALUE: {
                    // [] -> []:关闭所有槽址 >= 当前栈顶的开 upvalue(值迁入各自 ObjUpvalue 自持),
                    // 无弹栈--弹栈由前置 POP_N 承担(语义对齐 Lua OP_CLOSE:编译器在弹区 POP_N 之后
                    // 发射,弹区槽已位于 top 之上,不 push 不覆写即安全;两指令均无分配,之间无
                    // 触发点;外层帧槽址恒低于本帧,新栈顶之上的开 upvalue 只属弹区局部)。
                    current_->close_upvalues(current_->stack_top());
                    break;
                }
                case OpCode::DEF_GLOBAL: {
                    // [v] -> []:以常量池 name(ObjString,intern)为键在当前模块 globals 首次定义
                    // (顶层 var 声明 -- 唯一创建全局的入口)。upsert:命中覆盖(重定义属编译期
                    // RedefinedVariable,运行期按定义处理),未命中插入。
                    //
                    // 根安全(GC 已启用):upsert 可能 rehash 触 GC。name/key 经常量池链根;
                    // v 用 peek 而非 pop -- 留 v 在值栈跨 upsert 的分配(先 pop 则 v 成裸局部,
                    // collect 会回收成悬垂),upsert 返回后再写 e.value、drop。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const Value value = current_->peek(0); // 先不弹:留 v 在栈上跨 upsert 的分配
                    const auto  entry = frame.module->globals().upsert(key);
                    entry->value      = value;
                    current_->drop(1); // 写完才弹,栈效应仍为 [v] -> []
                    break;
                }
                case OpCode::LOAD_GLOBAL: {
                    // [] -> [v]:按名查当前模块 globals,miss 回退 VM 级 builtins_ 表(Python 式
                    // globals -> builtins 查找链);两者皆未命中 -> UndefinedVariable 运行时错误。
                    // STORE_GLOBAL 不回退 builtins(赋值不隐式创建,见 grammar.txt §205-206)。
                    //
                    // 根安全(GC 已启用):find 无分配;push 的唯一分配是值栈 grow,而 push 先写栈
                    // 再增长(Movement::push),e->value 已入栈(根)后方 grow -> collect。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    auto        entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        entry = builtins_.find(key); // 回退 VM 级 builtins(内置 type/len/str/assert)
                        if (entry == nullptr) {
                            raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                            if (auto u = unwind()) {
                                return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                            }
                            break; // 已派发 handler:帧栈可能已截,循环顶重取
                        }
                    }
                    current_->push(entry->value);
                    break;
                }
                case OpCode::STORE_GLOBAL: {
                    // [v] -> [v]:peek-store 到当前模块 globals(不弹,留 v);未定义 ->
                    // UndefinedVariable(赋值不隐式创建,见 grammar.txt §205-206)。本指令无分配,
                    // 无 GC 风险。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    entry->value = current_->peek(0);
                    break;
                }
                case OpCode::LOAD_FIELD:
                    // name:u16;[obj] -> [v]。执行体收口于 run_load_field(协议分派/错误烘焙/
                    // 栈形操作),同 run_binary_numeric 式 bool 契约。
                    if (!run_load_field(read_name(frame))) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    break;
                case OpCode::STORE_FIELD:
                    // name:u16;[obj, v] -> [v]。执行体收口于 run_store_field(协议分派/
                    // 错误烘焙/单槽下移)。
                    if (!run_store_field(read_name(frame))) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    break;
                case OpCode::LOAD_INDEX:
                    not_implemented("LOAD_INDEX");
                case OpCode::STORE_INDEX:
                    not_implemented("STORE_INDEX");
                case OpCode::LOAD_THIS_FIELD: {
                    // name:u16;[] -> [v]:this 取帧槽 0(方法帧形 [this, a1..aN],不经栈),查找与
                    // obj.m 同走 load_field 协议(绑定 + 缓存回填 + 读穿透),结果 push;miss 时
                    // 协议已按类措辞 fail 入寄存器,本 case 只透传信号。帧槽 0 恒实例 -- 指令仅
                    // 编译器于实例方法内发射,运行期以 ASSERT 钉编译器不变式(不走可 catch 的 raise)。
                    auto inst = try_obj<ObjInstance>(frame.slots[0]);
                    ASSERT(inst != nullptr, "LOAD_THIS_FIELD: 'this' slot must be an instance (compiler invariant)");
                    if (const auto result = inst->load_field(*this, read_name(frame))) {
                        current_->push(*result); // [] -> [v]
                        break;
                    }
                    if (auto u = unwind()) {
                        return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                    }
                    break; // 已派发 handler:帧栈可能已截,循环顶重取
                }
                case OpCode::STORE_THIS_FIELD: {
                    // name:u16;[v] -> [v]:peek-store 经 this 的 store_field 协议(实例字段动态,
                    // 即创建;false 分支为契约透传的防御形态,实例路径不可达),值留栈顶、this
                    // 不经栈。帧槽 0 恒实例,校验同 LOAD_THIS_FIELD。
                    auto inst = try_obj<ObjInstance>(frame.slots[0]);
                    ASSERT(inst != nullptr, "STORE_THIS_FIELD: 'this' slot must be an instance (compiler invariant)");
                    if (!inst->store_field(*this, read_name(frame), current_->peek(0))) { // false ⟺ 已 fail(契约)
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    break; // 值留栈(peek-store),this 不经栈
                }

                // ---- 算术与逻辑 ----
                // 相等性(EQUAL 走 == 内容相等;STRICT 走 === 严格相等)
                case OpCode::EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(value_equal(a, b)));
                    break;
                }
                case OpCode::NOT_EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(!value_equal(a, b)));
                    break;
                }
                case OpCode::STRICT_EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(value_identical(a, b)));
                    break;
                }
                case OpCode::STRICT_NOT_EQUAL: {
                    const Value b = current_->pop();
                    const Value a = current_->pop();
                    current_->push(Value::from_bool(!value_identical(a, b)));
                    break;
                }
                // 比较(弹 2 压 1;bool 契约,失败善后与 CALL case 同形,pitfalls 坑 #11)
                case OpCode::GREATER:
                    if (!run_binary_numeric<OpCode::GREATER>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    break;
                case OpCode::GREATER_EQUAL:
                    if (!run_binary_numeric<OpCode::GREATER_EQUAL>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::LESS:
                    if (!run_binary_numeric<OpCode::LESS>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::LESS_EQUAL:
                    if (!run_binary_numeric<OpCode::LESS_EQUAL>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                // 算术(弹 2 压 1;同上)
                case OpCode::ADD:
                    if (!run_binary_numeric<OpCode::ADD>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::SUBTRACT:
                    if (!run_binary_numeric<OpCode::SUBTRACT>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::MULTIPLY:
                    if (!run_binary_numeric<OpCode::MULTIPLY>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::DIVIDE:
                    if (!run_binary_numeric<OpCode::DIVIDE>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::MOD:
                    if (!run_binary_numeric<OpCode::MOD>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                // 一元
                case OpCode::NOT:
                    current_->push(Value::from_bool(!is_truthy(current_->pop())));
                    break;
                case OpCode::NEGATE: {
                    if (const Value v = current_->pop(); v.is_int()) {
                        current_->push(Value::from_int(-v.as_int()));
                    } else if (v.is_f64()) {
                        current_->push(Value::from_f64(-v.as_f64()));
                    } else {
                        raise(ErrorCode::InvalidOperand, "negate requires a number, got {}", type_name(v));
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    break;
                }

                // ---- 栈操作 ----
                case OpCode::POP:
                    current_->drop(1);
                    break;
                case OpCode::POP_N: {
                    const u8 n = read_u8(frame);
                    current_->drop(n);
                    break;
                }
                case OpCode::DUP:
                    current_->push(current_->peek(0));
                    break;
                case OpCode::DUP2: {
                    const Value b = current_->peek(0);
                    const Value a = current_->peek(1);
                    current_->push(a);
                    current_->push(b);
                    break;
                }

                // ---- 输出与调试 ----
                case OpCode::PRINT:
                    io::println("{}", format_value(current_->pop()));
                    break;
                case OpCode::NOP:
                    break;

                // ---- 控制流(u16 无符号;前向 JUMP* ip+=off,后向 JUMP_BACK ip-=off;
                //      偏移以读完操作数后的 ip 为基准,同 Disassembler 解码约定)----
                case OpCode::JUMP: {
                    const auto off = read_u16(frame);
                    frame.ip += off;
                    break;
                }
                case OpCode::JUMP_TRUE: {
                    const auto off = read_u16(frame);
                    if (is_truthy(current_->pop())) {
                        frame.ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_TRUE_OR_POP: {
                    const auto off = read_u16(frame);
                    if (is_truthy(current_->peek(0))) {
                        frame.ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current_->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_FALSE: {
                    const auto off = read_u16(frame);
                    if (!is_truthy(current_->pop())) {
                        frame.ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_FALSE_OR_POP: {
                    const auto off = read_u16(frame);
                    if (!is_truthy(current_->peek(0))) {
                        frame.ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current_->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_BACK: {
                    const auto off = read_u16(frame);
                    frame.ip -= off;
                    // safe point:循环回边触发回收。值栈/帧已接根(tracer),maybe_collect 不移动
                    // 值栈/帧/字节码,frame 引用跨调用有效。stress 下每回边一次 collect(测试用)。
                    gc_.maybe_collect();
                    break;
                }

                // ---- 函数与闭包 ----
                case OpCode::CALL: {
                    const u8 argc = read_u8(frame);
                    // 良构不变式:栈上必有 callee + argc 个实参。
                    ASSERT(current_->stack_size() >= static_cast<usize>(argc) + 1, "CALL on malformed stack");
                    if (const Value callee = current_->peek(argc); !call_value(callee, argc)) {
                        // 失败(契约见 call_value):unwind 查异常记录表派发/物化。
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈已 truncate,循环顶重取
                    }
                    break; // 帧已切换,frame 引用作废,循环顶重新取
                }
                case OpCode::CLOSURE: {
                    // fn:u16;[] -> [closure]:取常量池 ObjFunction,现场包 ObjClosure,按 fn 的
                    // 捕获描述表(upvalue_descs_,存 ObjFunction 元数据、不进字节码流,指令集
                    // §4.13)逐个建/复用 upvalue 后填,压闭包值。
                    //   - is_local=true:捕获直接外围帧(本帧)局部槽,经 capture_upvalue 单点收口
                    //     (「同一局部一份引用」;链上节点经 VM 根 tracer 标根保命)。
                    //   - is_local=false:穿透捕获,复制外围闭包(本帧 frame.closure)的第 index 个
                    //     upvalue(同一 ObjUpvalue 指针,共享同一份引用)。
                    // 根安全(GC 已启用,「栈即根」惯法,同 DEF_GLOBAL 的 peek-not-pop):fn 经
                    //   常量池链根可达;闭包建成立即压栈 -- desc 循环内 new_upvalue 顶 maybe_collect
                    //   不再威胁闭包,免守卫;capture_upvalue 新建 upvalue 到插链间无分配点(入链即
                    //   tracer 根),add_upvalue 的 Array push 走 trivial 分配不触 GC,中途无弹栈
                    //   路径,栈效应仍为 [] -> [closure]。
                    const auto idx     = read_u16(frame);
                    const auto fn      = Object::as<ObjFunction>(frame.unit->constants[idx].as_obj());
                    auto       closure = new_closure(gc_, fn);
                    current_->push(Value::from_obj(closure)); // 立即入栈:值栈即根,跨 desc 循环免守卫
                    for (const auto& [is_local, index]: fn->upvalue_descs()) {
                        if (is_local) {
                            // 捕直接外围帧局部槽(单点收口,见上)
                            closure->add_upvalue(current_->capture_upvalue(gc_, frame.slots + index));
                        } else {
                            // 穿透捕获:复制外围闭包的第 index 个 upvalue(共享同一份引用)
                            closure->add_upvalue(frame.closure->upvalues()[index]);
                        }
                    }
                    break; // 闭包已在栈顶,栈效应仍为 [] -> [closure]
                }

                // ---- 类与对象(M5 阶段 2:VM 机制落地,编译器发射阶段 3 翻转)----
                case OpCode::LOAD_OBJECT:
                    // [] -> [Object]:压 VM bootstrap 的 Object 根类(不经名字查,用户 shadow
                    // 全局名免疫隐式继承,M5 决策 3)。
                    current_->push(Value::from_obj(object_class_));
                    break;
                case OpCode::MAKE_CLASS: {
                    // name:u16;[super] -> [class]:super 经 LOAD_GLOBAL/LOAD_OBJECT 上的类值。
                    // peek super 不先弹 -- new_class 顶 maybe_collect,super 须仍在栈(「栈即根」);
                    // 非类值是**语言可达**错误(`var Bar = 5; def Foo : Bar` 合法 -- superclass
                    // 运行期才知值类型),故 raise 而非 ASSERT。name 经常量池可达,无需守卫。
                    // 建成写回原槽([super] -> [class] 收口,class 即经值栈根);init 继承收进
                    // 对象构造 -- new_class 出厂即自 super 派生(此处 super 已验为类,沿链语义
                    // 天然成立),指令层零 seed 写点。
                    if (const auto super = try_obj<ObjClass>(current_->peek(0))) {
                        const auto cls    = new_class(gc_, read_name(frame), super);
                        current_->peek(0) = Value::from_obj(cls);
                        break;
                    }

                    raise(ErrorCode::TypeMismatch, "superclass must be a class, got {}", type_name(current_->peek(0)));
                    if (auto u = unwind()) {
                        return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                    }
                    break; // 已派发 handler:循环顶重取
                }
                case OpCode::MAKE_METHOD: {
                    // name:u16;[class, closure] -> [class]:**实例方法注册**(静态方法 fun 经
                    // MAKE_STATIC;仅收闭包 -- 方法性标记 = defining class 戳,原生/静态槽读恒
                    // 原值)。编译器路径(值恒来自上一条 CLOSURE),栈形经 ASSERT 钉 -- 语言写不出
                    // 违例,不走可 catch 的 raise。set_field 的 upsert 走 trivial 分配不触 GC,
                    // peek 不弹的真实理由是栈效应(弹 value 留 class 继续接收成员)。注册副作用:
                    // set_field 命中 "init" 同步 init_(值形态不特判)+ 闭包戳 defining class
                    // (M5 决策 6;一职双任 -- super 来源 + 方法性标记,读路径据非空判绑)。
                    const auto cls    = try_obj<ObjClass>(current_->peek(1));
                    const auto method = current_->peek(0);
                    ASSERT(cls != nullptr, "MAKE_METHOD: slot-1 is not a class (malformed stack)");
                    cls->set_field(read_name(frame), method);

                    const auto closure = try_obj<ObjClosure>(method);
                    ASSERT(closure != nullptr,
                           "MAKE_METHOD: slot-0 is not a closure (method registration is closure-only)");
                    closure->set_defining_class(cls);
                    current_->drop(1);
                    break;
                }
                case OpCode::MAKE_STATIC: {
                    // name:u16;[class, value] -> [class]:静态成员注册(var 声明与 fun 静态方法
                    // 同经此;不戳 defining class ⟹ 静态槽读恒原值,方法性判别看戳不看值类型)。
                    // 与 MAKE_METHOD 同形(peek 不弹 + set_field + 弹 value 留 class);编译器
                    // 路径,栈形 ASSERT 钉。
                    const auto cls = try_obj<ObjClass>(current_->peek(1));
                    ASSERT(cls != nullptr, "MAKE_STATIC: slot-1 is not a class (malformed stack)");
                    cls->set_field(read_name(frame), current_->peek(0));
                    current_->drop(1); // 弹 value 留 class:[class, value] -> [class]
                    break;
                }
                case OpCode::LOAD_SUPER_FIELD:
                    // name:u16;[] -> [v]。执行体收口于 run_load_super_field(协议沿链查 +
                    // 方法闭包(defining class 戳)绑 this/其余原值直读,不写 fields 缓存)。
                    if (!run_load_super_field(read_name(frame))) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    break;
                case OpCode::INVOKE_METHOD:
                    not_implemented("INVOKE_METHOD");
                case OpCode::MAKE_LIST:
                    not_implemented("MAKE_LIST");
                case OpCode::MAKE_MAP:
                    not_implemented("MAKE_MAP");
                case OpCode::MAKE_RANGE:
                    not_implemented("MAKE_RANGE");

                // ---- 模块导入 ----
                case OpCode::IMPORT: {
                    // path:u16;解析后把命中的 ObjModule 压栈（[...] -> [..., module]），绑定交
                    // CodeGen 按作用域走（IMPORT 仅负责「取模块对象」）。解析按
                    // .claude/reference/runtime/import-path-resolution.md:相对基 = 当前模块目录、
                    // 裸名基 = source_roots，键 = 绝对规范路径（见 resolve_module）。
                    //   - 命中（表内任意初始化进度）：复用模块对象 -- 命中正在 run-once 的模块即
                    //     循环导入，按文法返回半初始化对象。加载事实源 = 表成员资格。
                    //   - 未命中：load_module（读盘 -> 编译 -> 入表占位）后，以其 entry 作一次
                    //     **普通函数调用**进帧 -- 模块体 run-once 即执行一个函数，其 RETURN 按
                    //     函数名 == <module> 判定后弹弃返回值、压回模块对象，故命中/未命中两分支
                    //     栈效应统一为 [..., module]。无递归 dispatch_loop()。
                    //
                    // 根安全(GC 已启用):path 经常量池根。canonical_path 经 new_string intern 驻留
                    //   (weak root,不保命),跨未命中分支内 load_module 的一串 new_* 分配须守 --
                    //   canonical_path_guard 跨全程根化;modules_.upsert 等表 rehash 走 trivial 分配
                    //   不触 GC,不是守卫承重点。命中/加载产出的 module 与 entry 经 modules_ 根可达,
                    //   入栈后另经值栈根。
                    ObjString* path = read_name(frame);
                    // 当前模块的绝对文件路径,供 resolve_module 相对分支取 dirname 作基;dir_ 内容
                    // 可空(cwd 不可用)时 abs_path 返空串,相对分支判空直接拒绝。
                    const auto canonical_path_str =
                            resolve_module(path->view(), frame.module->abs_path(), source_roots_);
                    if (!canonical_path_str) {
                        raise(ErrorCode::ModuleNotFound, "module not found: '{}'", path->view());
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    auto canonical_path = new_string(gc_, *canonical_path_str);
                    auto guard = gc_.make_guard(canonical_path); // 跨 find / load_module 内 upsert(rehash 触 GC)
                    if (const auto module_entry = modules_.find(Value::from_obj(canonical_path));
                        module_entry != nullptr) {
                        // 命中(体已跑完 / 循环导入半初始化)复用:压模块值。
                        current_->push(module_entry->value);
                        break;
                    }
                    // 未命中:加载链路(契约见 load_module);失败 unwind 查表派发 / 物化 Error 出栈。
                    ObjModule* module = load_module(canonical_path, path->view());
                    if (module == nullptr) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:frame 已废,循环顶重取
                    }
                    // 模块体 run-once = 一次普通 0 参函数调用(见 case 头注释)。根安全:entry 经
                    // module->entry_ 根可达;闭包建成立即压栈即根化。
                    ObjFunction* entry   = module->entry();
                    const auto   closure = new_closure(gc_, entry);
                    current_->push(Value::from_obj(closure)); // callee 压栈
                    if (!call_closure(closure, 0)) {
                        // 进帧失败(栈溢出等):帧未进,callee 仍在栈顶(unwind 截栈时一并丢弃)。
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                        }
                        break; // 已派发 handler:帧栈已 truncate,循环顶重取
                    }
                    break; // 帧已切换,frame 引用作废,循环顶重新取
                }

                // ---- 异常 ----
                case OpCode::THROW: {
                    // 用户 throw:弹抛出值,原值入寄存器(不包 ObjException -- catch 绑原值保类型)
                    // 后 unwind -- 命中 handler 值落 catch 参数槽,全帧未命中物化 UncaughtException
                    // Error(消息渲染值本身,位置由跟踪 at 行给出)。
                    current_->raise(current_->pop());
                    if (auto u = unwind()) {
                        return runtime_err(std::move(*u)); // 未捕获 -> 终止 dispatch_loop
                    }
                    break; // 已派发 handler:帧栈已 truncate,循环顶重取
                }

                // ---- 返回(exit_frame 后 frame 引用作废,故先取返回值与判模块体帧)----
                case OpCode::RETURN: {
                    const Value ret = current_->pop(); // 取返回值(exit_frame 将丢弃其下方栈区)
                    // 模块体 run-once 帧名固定 <module>(kModuleEntryName;主入口 <main> 与用户
                    // 函数名均不含 '<>'),其 RETURN 弹弃模块体返回值、改压该模块对象 -- 模块体
                    // 「返回模块」,使 IMPORT 的栈效应在命中/未命中两分支统一为 [..., module]。
                    // 先取 module 与 fn 名再 exit_frame:exit_frame 后 frame 引用悬垂。
                    auto mod     = frame.module;
                    auto fn_name = frame.closure->function()->name()->view();
                    current_->exit_frame(); // 弹帧 + 关本帧区间开指(值迁入各自 upvalue 自持)+ 值栈顶复位,一体
                    if (current_->frames().empty()) {
                        return ret; // 顶层(主入口 <main>)返回:返回值为程序结果
                    }
                    if (fn_name == kModuleEntryName) { // 模块体帧:名字经 intern 驻留,短串逐 RETURN 比较开销可忽略
                        current_->push(Value::from_obj(mod));
                    } else {
                        current_->push(ret);
                    }
                    break;
                }

                default:
                    UNREACHABLE();
            }
        }
    }

} // namespace aria
