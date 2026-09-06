#include "runtime/AriaVM.hpp"

#include <cmath>
#include <filesystem>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/Disassembler.hpp"
#include "bytecode/code.hpp"
#include "compile/Compiler.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/Builtins.hpp"
#include "util/fs.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
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

        // 导入模块的入口函数名(固定;主入口为 "<main>")。用户代码无法产生含 '<'/'>' 的名字,
        // 故函数名 == kModuleName 唯一标识「IMPORT 加载层驱动的模块体 run-once 帧」。RETURN 据它
        // 判定是否弹弃返回值、改压模块对象 -- 取代在 CallFrame 上加 is_module_body
        // 标志位:名字是函数的固有属性,无需进帧时额外置位/复位,亦无帧槽复用残留之虞。
        // 名字经 intern 驻留(指针唯一),view() 为短串(8 字节),逐 RETURN 一次内容比较开销可忽略。
        constexpr StringView kModuleName = "<module>";

        // 把 import 串(specifier)解析为命中文件的绝对规范路径(模块表键)。设计见
        // .claude/reference/runtime/import-path-resolution.md「加载层设计基线」:键 = weakly_canonical(候选)
        // (解析已存在部分的符号链接、折叠 "."/".."、去冗余分隔符)。
        //   - spec:import 串(用户写的 specifier,如 "./helper" / "lib/utils")。
        //   - current_module_path:发出 import 的当前模块的绝对文件路径
        //     (frame.module->abs_path() = dir_ + "/" + name_ + ".aria";dir_ = 模块文件所在目录、
        //     name_ = 文件名去 .aria 后缀)。相对分支仅取其 dirname 作基(.aria 后缀在末段,
        //     dirname 不受影响 -> 即 dir_);裸名分支不依赖此值。
        //   - 相对(以 "./" / "../" 开头,或正是 "." / ".."):基 = 当前模块所在目录(dirname(current_module_path),
        //     单基,caller-local,不碰 source_roots,故相对导入永不逃逸到别的源根。
        //     dir_ 指针恒非空但内容可空:cwd 可用时 current_module_path 为正常绝对路径(合成模块如
        //     <script> 退化为 cwd,相对导入以 cwd 为基);cwd 不可用时 dir_ 为空串 -> abs_path
        //     返空串 -> 下方 current_module_path.empty() 守卫触发,相对解析直接返 nullopt(拒绝锚定)。
        //   - 裸名(无 ./ ../ 前缀):基 = source_roots(入口槽 [0] = 入口模块 dir_、编译器相对
        //     stdlib 目录等),逐个 exists-check,首个存在 <base>/<spec>.aria 者命中(对齐 Python
        //     sys.path 顺序搜索,先入源根者占坑)。
        //   - 末尾 ".aria" 可选:spec 末段以 ".aria" 结尾(段长 > 5)则剥离,查找时统一补回
        //     ".aria",使 lib/math ≡ lib/math.aria。
        //   - 键 = 绝对规范路径:同模块不同写法、不同源根同名模块均归一到各自真实路径,
        //     跨根不碰撞、相对不逃逸。符号链接经 weakly_canonical 规避双加载。
        // 磁盘:每基一次 weakly_canonical + exists(裸名 = N 次 stat,相对 = 1 次);解析缓存待加(TODO)。
        // 越界/沙箱检测(相对导入越出源根)留待加载层:本函数不限制 ".." 折叠后的路径范围。
        Opt<String> resolve_module(const StringView spec, const StringView current_module_path,
                                   const List<String>& source_roots) {
            // 1. 剥末段 ".aria" 后缀(段长 > 5 且以 ".aria" 结尾),使 lib/math ≡ lib/math.aria。
            String spec_str{spec};
            {
                const auto last_slash = spec_str.find_last_of('/');
                const auto last_seg   = (last_slash == String::npos) ? StringView{spec_str}
                                                                     : StringView{spec_str}.substr(last_slash + 1);
                if (last_seg.size() > 5 && last_seg.ends_with(".aria")) {
                    spec_str.erase(spec_str.size() - 5); // 剥末 5 字符(".aria")
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
            const auto      file_rel = spec_str + ".aria";
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

        // 模块位置串 "<loc>:<line>":文件模块渲染 abs_path;合成模块(名以 '<' 开头,如
        // <script>/<test>,abs_path 会拼出伪路径)或 abs_path 为空(cwd 不可用)退化为 "<name>"。
        // 装箱点(runtime_loc)与未捕获堆栈跟踪逐帧渲染(unwind_)共用,位置串规则单一事实源。
        String module_loc(const ObjModule& mod, const u32 line) {
            if (const auto name = mod.name()->view(); name.starts_with('<') || mod.abs_path().empty()) {
                return std::format("{}:{}", name, line);
            }
            return std::format("{}:{}", mod.abs_path(), line);
        }

        // 运行期位置串:行号 = 顶帧 last_ip(主循环取指前写的指令起始指针,与 unit->code.data()
        // 相减反推 offset)查 RLE 行号表;表空(手搓 CodeUnit 无行号)line_for_offset 返 0,不炸。
        // 帧栈空(run 外经 vm.fail 直调等)返空串 -- 无位置。报错是冷路径,一次 std::format 可忽略。
        // 供 raise(装箱点)与未捕获跟踪逐帧渲染(module_loc)共用 -- 装箱点调用时
        // 顶帧恒为故障指令所在帧(直报)或 caller 帧(call_*/原生失败,被调帧未进/原生不进帧),
        // last_ip 恰为故障指令 / CALL 站点(pitfalls 坑 #15 的位置语义)。
        String runtime_loc(Movement& ctx) {
            if (ctx.frames().empty()) {
                return {};
            }
            const auto& frame  = ctx.frames().top();
            const usize offset = frame.last_ip - frame.unit->code.data();
            const auto  line   = frame.unit->line_for_offset(offset);
            return module_loc(*frame.module, line);
        }

        // 把寄存器取出的载荷拆为未捕获出口要用的 (码, 完整烘焙消息) 两件:ObjException 直取
        // 自身码与 message_(已是完整烘焙串,与 from_detail 直构文案逐字一致,re-throw 保码,
        // 坑 #7);其它载荷(未来用户 throw 的任意值 -- THROW 落地前寄存器只可能是 ObjException,
        // 兜底仅防御)兜底 UncaughtException,消息渲染值本身(经烘焙单点 make_message,与
        // from_detail 同源同串)。仅 unwind_ 未捕获出口一处消费:拼好跟踪后经 Error::from_baked
        // 一次物化成边界 Error,不中转 Error 对象(Error 只在边界成型)。
        Pair<ErrorCode, String> uncaught_error_parts(const Value v) {
            if (v.is_obj() && Object::is<ObjException>(v.as_obj())) {
                const auto ex = Object::as<ObjException>(v.as_obj());
                return {ex->code(), String{ex->message()->view()}};
            }
            return {ErrorCode::UncaughtException,
                    Error::make_message(ErrorCode::UncaughtException, {},
                                        std::format("uncaught exception: {}", format_value(v)))};
        }

        // 构造运行时错误结果(Result<Value, Error> 的 unexpected 态),转发 unwind_ 物化的
        // 未捕获 Error 出栈。run_ 各异常站点(unwind_ 返 somed Error)的统一收口,只剩
        // std::unexpected 样板;旧「ctx + 码 + 格式串」直报重载与 Value 转发重载已随 M3 直报
        // 站点统一切入寄存器而退役(pitfalls 坑 #11)。
        Result<Value, Error> runtime_err(Error err) { return std::unexpected(std::move(err)); }

        // 范围外 opcode 的统一处理:后续阶段(闭包/字段/索引/类等)才会实现,
        // 当前不执行。命中即打印提示后直接终止进程(经 fatal_error,不沿 run_ 返回)。
        // 用 OpcodeNotImplemented(Internal 类)而非 NotImplemented(Semantic 类):后者经 Error 通道
        // 服务编译期 CodeGen not_impl(可恢复 CompileError);此处是运行期执行到未实现 opcode,
        // 不可恢复走 fatal_error,属解释器实现不完整(Internal)。
        [[noreturn]]
        void not_implemented(const StringView op_name) {
            fatal_error(ErrorCode::OpcodeNotImplemented,
                        std::format("opcode '{}' not implemented yet (out of current scope: closures/fields/index/"
                                    "classes come later)",
                                    op_name));
        }

#ifdef DEBUG_TRACE_EXECUTION
        // 执行跟踪:在每条指令执行**前**打印字节码/栈/帧/模块信息(经 io::print 到 stderr,与 GC 调试日志
        // / DEBUG_PRINT_COMPILED_CODE 同走 stderr,与 PRINT 的 stdout 输出分流)。DEBUG_TRACE_EXECUTION 关闭时
        // 本函数整体不存在,#ifdef 外零开销。供 run_ 主循环顶在取 opcode 前调用 -- 此时 frame.ip 指向待执行
        // 指令,据此算 offset 并经 Disassembler::disassembleInstruction 解码(仅读不推进 VM 的 ip)。
        //   - 栈渲染经 format_value_debug(Value 层非重入渲染,见 value/Value.hpp),不用 format_value
        //     (后者 Obj 走可重载虚 to_string,未来用户类可重载其运行 aria 字节码,trace 在 run_ 内会重入 VM
        //     致无限递归);format_value_debug 对 Obj 走非虚 obj->type() 分派,绝不触用户重载。
        //   - 字节码行的 frame.function->to_string() / mod->to_string() 是 ObjFunction/ObjModule(内置,纯 C++,
        //     用户无法重载),无重入风险。
        //
        //   - 字节码行:模块信息(to_string,置于 [trace] 与 <fn> 之间)+ 栈顶帧 fn 名 @ip 偏移
        //     + 指令反汇编(opcode + 操作数 + 注释);第一行即含完整位置上下文(模块/函数/ip/字节码),无需下扫模块行;
        //   - 栈+帧:  值栈 [base, top) 全部 Value 经 format_value_debug 渲染,逐槽 [ v ](空栈打印 (empty));
        //             下方一行用 ^ 对齐到当前帧栈底(bottom = slots 基址)所在槽的 [ 下标,联动指示栈中
        //             哪一段是当前帧的局部区,后随 frame 索引(fn/ip 已在字节码行,不重复)。
        // 每条指令三行,调试用,详尽优先于简洁。
        void trace_execution(Movement& ctx) {
            auto&      frames    = ctx.frames();
            CallFrame& frame     = frames.top();
            const auto code_base = frame.unit->code.data();
            const auto ip_off    = static_cast<usize>(frame.ip - code_base);
            const auto instr     = Disassembler::disassembleInstruction(frame.unit, ip_off);

            const auto* mod = frame.module;
            io::print(stderr, "[trace] {}  {} @{:04X}  {}\n", mod->to_string(), frame.function->to_string(),
                      static_cast<u32>(ip_off), instr);

            const String prefix = std::format("        stack[{}]: ", ctx.stack_size());
            List<String> segs;
            for (Value* p = ctx.stack_base(); p < ctx.stack_top(); ++p) {
                segs.emplace_back(std::format("[ {} ]", format_value_debug(*p)));
            }
            String stack_str;
            for (const auto& s: segs) {
                stack_str += s;
            }
            if (stack_str.empty()) {
                stack_str = "(empty)";
            }
            io::print(stderr, "{}{}\n", prefix, stack_str);

            // 帧栈底标记:^ 对齐到当前帧 bottom 槽的 [ 下方,后随 frame 索引;bottom = slots - stack_base。
            // bottom >= 段数时(栈底在栈顶之上,空帧)对齐到栈末尾。行首用纯空格(与栈行等宽 prefix 对齐),
            // 不重复 stack[n]: 前缀,只留 ^ 与标签。
            const auto frame_idx = frames.size() - 1;
            const auto bottom    = static_cast<usize>(frame.slots - ctx.stack_base());
            usize      col       = prefix.size();
            if (bottom < segs.size()) {
                for (usize i = 0; i < bottom; ++i) {
                    col += segs[i].size();
                }
            } else {
                col += stack_str.size();
            }
            String marker;
            marker.append(col, ' ');
            marker += std::format("^ frame[{}]", static_cast<u32>(frame_idx));
            io::print(stderr, "{}\n", marker);
        }
#endif // DEBUG_TRACE_EXECUTION

    } // namespace

    // 构造:成员初始化(gc_ 先,main_ctx_/modules_/builtins_ 借 &gc_,current_ 指 &main_ctx_),再把
    // VM 根 tracer 注册进自有 GC,最后一次性注册 VM 级 builtins(在 tracer 已就绪后,注册内触 GC 时
    // 已入表条目经 builtins_.trace 标根)。tracer 为 lambda:[this] 捕获,标记三类根:
    //   1) modules_:解释器级共享模块表(进而 trace 各模块 name_/dir_/entry_/globals_);
    //   2) builtins_:VM 级只读 builtins 表(内置 ObjNativeFn + name 串,LOAD_GLOBAL 回退查此);
    //   3) current_ 执行链:自 *current_ 沿 previous_ 走到链尾(现为单节点 main_ctx_;M6 协程期为
    //      resume 链,挂起协程的值栈/帧/寄存器皆根),逐个标:
    //        a) 值栈 [base, top) 全部 Value:run() 期局部/实参/临时值只活在栈上,不经常量池链
    //           可达,是最关键的缺失根。run() 结束 reset() 清空,故 run() 外(compile/测试)GC 时
    //           栈遍历为空,不会标到指向已回收对象的陈旧栈值;
    //        b) 各活动帧的 function/module:本可经 module -> entry -> 常量池链可达,直标更稳、
    //           免依赖「帧函数必在其父常量池」不变式。open upvalues 留待 M4;
    //        c) 挂起错误寄存器(M3 起载荷为 Value -- ObjException 或用户 throw 的任意值):
    //           置入寄存器到 take_error 取出之间可能跨安全点分配(如 raise 后 unwind),须标根保命。
    //      链尾断言恒 &main_ctx_:锁定「resume/yield 严格成对」的切换纪律(主上下文是 resume 链
    //      的根锚,previous_ 语义见 Movement.hpp)。M6 定稿单循环切换模型后本遍历保留(main_ctx_
    //      不入堆,运行中协程的 previous_ 指向它,对象图不可达;挂起协程已解链、经对象图可达),
    //      链尾断言届时退役(切换点结构性唯一,无纪律可违,见 vm-design.md §4.9)。
    // 值栈/帧以 tracer 直标代替 Movement 升 Object(M6 协程期再升级 ObjMovement 入对象链表)。
    // VM 持有 gc_(值成员),成员逆序析构下 gc_ 最后析构,tracer 与 modules_/builtins_/main_ctx_ 同生共死,无需析构注销。
    AriaVM::AriaVM() : gc_{}, main_ctx_{&gc_}, current_{&main_ctx_}, modules_{&gc_}, builtins_{&gc_}, source_roots_{} {
        gc_.set_vm_roots([this](GC& g) {
            modules_.trace(g);
            builtins_.trace(g);
            [[maybe_unused]] Movement* tail = nullptr;
            for (Movement* m = current_; m != nullptr; m = m->previous()) {
                for (Value* p = m->stack_base(); p < m->stack_top(); ++p) {
                    g.mark_value(*p); // mark_value 对非对象 Value no-op,栈槽含 int/f64/bool/nil 安全
                }
                if (const auto& pending = m->pending_error(); pending.has_value()) {
                    g.mark_value(*pending);
                }
                for (const auto& f: m->frames().span()) {
                    g.mark_object(f.function); // mark_object 容 nullptr
                    g.mark_object(f.module);
                }
                tail = m;
            }
            ASSERT(tail == &main_ctx_, "VM roots: context chain must terminate at main_ctx_");
        });
        // VM 级 builtins 一次性注册(set_vm_roots 已就绪):type/len/str/assert 经 new_native_fn 包成
        //   ObjNativeFn 后按名 upsert 进 builtins_。注册内 new_string/new_native_fn 各一次 new_object 顶
        //   maybe_collect:已入表条目经上方 tracer 的 builtins_.trace 标根,在建的 name/fn 经
        //   register_builtins 内 make_guard 双守卫根化(见 Builtins.cpp)。全 VM 共享一份,不再每模块注入。
        builtins::register_builtins(gc_, builtins_);
        // source_roots_[0] = 入口槽:构造时占位为当前工作目录(前期源根),run() 时被入口模块 dir_
        //   原地替换。占位用 cwd:既是一个可用的默认源根(REPL / 未显式设 dir_ 时裸名搜 cwd),
        //   又保证 [0] 槽位恒在,run() 可直接赋值无需 null/空判定。cwd 不可用时以空串兜底(不 fatal):
        //   resolve_module 裸名分支跳过空根,仅搜 [1..] 配置根,比拿 "." 锚到坏目录更诚实。
        source_roots_.push_back(fs::current_dir().value_or(""));
        // source_roots_[1..] = 配置根:编译器相对 stdlib 源根(约定 <exe_dir>/../share/aria/lib;
        //   确切路径待定),由 fs::program_dir 推导并 weakly_canonical 规范化,非空则推入。
        //   可经 set_source_roots 覆盖(测试/嵌入配置)。
        if (const auto pd = fs::program_dir()) {
            const auto      stdlib = stdfs::path{*pd} / "../share/aria/lib";
            std::error_code ec;
            if (const auto real = stdfs::weakly_canonical(stdlib, ec); !ec && !real.empty()) {
                source_roots_.push_back(real.string());
            }
        }
    }

    void AriaVM::set_source_roots(List<String> roots) noexcept {
        // 替换配置根 [1..],保留入口槽 [0](cwd 或 run() 设入的入口 dir_)-- 入口槽归 run() 管。
        source_roots_.resize(1);
        for (auto& r: roots) {
            source_roots_.push_back(std::move(r));
        }
    }

    void AriaVM::raise_detail(const ErrorCode code, const StringView detail) {
        // 装箱核心(公开模板 raise(code, fmt, args...) 格式化后经此,契约见 AriaVM.hpp):
        // Error::make_message 把 [位置 + ": "] + "Category: Name" + 细节合成完整消息串 -- 位置经
        // runtime_loc 查 *current_ 顶帧行号表(故障指令 / CALL 站点;合成模块退化为 "<name>:line";
        // 帧栈空即 run 外直调则无位置),detail 为已格式化的原始细节串(不含前缀,防双烘)。装箱入
        // **当前**上下文(*current_,现为 main_ctx_;M6 协程期即当前协程 -- run_/call_value 族与本
        // 函数同源同一 current_,错误随上下文走不串扰)的挂起寄存器(单寄存器模型,载荷统一 Value,
        // 见 exception-implementation-pitfalls.md 坑 #7)。new_exception 工厂内部 new_string 驻留
        // 并自守,跨 new_object 顶 maybe_collect 安全;返回对象到 current_->raise 之间无分配,
        // 入寄存器后由 VM 根 tracer 标 pending_error 保命。
        const auto msg = Error::make_message(code, runtime_loc(*current_), detail);
        current_->raise(Value::from_obj(new_exception(gc_, code, msg)));
    }

    Result<Value, Error> AriaVM::run(SourceFile& source, ObjModule& module) {
        // 编译并执行：经 Compiler（用本 VM 的 gc_，编译期分配与 run 同源）把 source 编进 module 的入口
        // ObjFunction，再委托 run(ObjFunction*) 执行。Compiler 每次就地构造（Lexer/Parser 可复用但本处
        // 一次性编译；REPL 期若需跨次复用可后续提成成员）。module 由调用方提供（控制 name/root 身份），
        // 编译期由 CodeGen::compile 内部 make_guard 根化；source 须存活到本函数返回（编译期 Error 的
        // SourceLoc 指向它）。编译失败原样透传首错 Error，不进入执行。
        Compiler compiler{gc_};
        auto     compiled = compiler.compile(source, module);
        if (!compiled.has_value()) {
            return std::unexpected(compiled.error());
        }
        // 内置函数不经此注册 -- VM 级 builtins_ 表由 ctor 一次性填充,LOAD_GLOBAL 模块 globals
        // 未命中后回退查之,覆盖所有执行入口(见 Builtins.hpp)。
        return run(compiled.value());
    }

    InterpretResult AriaVM::interpret_run(SourceFile& source, ObjModule& module) {
        // 编译并执行，按结果类别映射。失败时 Error 已自有完整消息串（构造期烘焙、不持 SourceFile*），
        // 渲染到 stderr 后只回类别，不回 Error。
        auto result = run(source, module);
        if (result.has_value()) {
            return InterpretResult::Ok;
        }
        io::println(stderr, "{}", result.error().message());
        switch (category_of(result.error().code())) {
            case ErrorCategory::Syntax:
            case ErrorCategory::Semantic:
                return InterpretResult::CompileError;
            default: // Runtime / Internal / Resource -> 运行期
                return InterpretResult::RuntimeError;
        }
    }

    InterpretResult AriaVM::interpret_from_src(const StringView src) {
        // 合成入口模块 <script>（dir=cwd，2 参 new_module）。
        // new_module 返回 GC 管理对象（经 new_object 分配），故 make_guard 根化；module 一并入根跨编译+执行（编译期
        // CodeGen::compile 亦自守）。
        const auto module = new_module(gc_, "<script>");
        auto       guard  = gc_.make_guard(module);

        // 字符串源 SourceFile（名 <script>，无文件身份）；方法内局部，存活至返回，Error 渲染不悬垂。
        SourceFile source{"<script>", "<script>", String{src}};
        return interpret_run(source, *module);
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

        return interpret_run(source, *module);
    }

    Result<Value, Error> AriaVM::run(ObjFunction* fn) {
        // 主上下文入口:进主循环前 current_ 必已归位 main_ctx_。M6 单循环切换模型(vm-design.md §4.9)
        // 下升格为永久不变式:run() 是唯一驱动入口,resume/yield 不重入 run_,切换只发生在 CALL 善后点
        // 且只换走 current_、不产生新循环 -- 每次进 run() 必从主上下文起步。
        ASSERT(current_ == &main_ctx_, "AriaVM::run: current_ is not main_ctx_ (unbalanced context switch)");
        // GC 已启用:值栈/帧经 vm_roots tracer 标根(见 ctor),IMPORT/DEF_GLOBAL 等已按「栈即根」
        // 前置编写(peek-not-pop)。
        // 源根:入口槽 [0] 原地替换为入口模块 dir_(对齐 Python sys.path[0] -- 即入口文件所在目录居首,
        // 配置根 stdlib / -L / 环境变量在 [1..] 不动)。直接赋值 [0],无 flag、无重建、reuse 安全
        // (覆盖旧值,不累积)。dir_ 指针恒非空(构造期 ASSERT),内容可空(<script> 在 cwd 不可用
        // 时退化为空串,磁盘模块 = 命中源根):空串由 resolve_module 裸名分支跳空根处理,
        // 故此处不判空。
        // 注:fn 必属某模块(module_ 非空,见 ObjFunction);[0] 槽位由构造时 cwd 占位恒在。
        source_roots_[0] = fn->module()->dir()->view();

        main_ctx_.reset();
        main_ctx_.push(Value::from_obj(fn)); // callee 值躺在主帧槽 0(RETURN 时弹)

        // 顶层入口无参数:slots 指向槽 0(callee)。enter_frame 收口 acquire + slots 不变量 +
        // VM 专有字段(function/unit/module/ip)填充,定义在 Movement.cpp。
        main_ctx_.enter_frame(fn, 0);

        // run_() 结束后 reset 主上下文:清空值栈/帧,确保 run() 外(后续 compile / 测试显式 collect)
        // GC 不会经 tracer 标到指向已回收对象的陈旧栈值。result 为值拷贝,reset 不影响返回值;
        // 返回值若持对象,由调用方自行根化(Guard / vm 存活),同既有契约。
        auto result = run_();
        main_ctx_.reset();
        return result;
    }

    bool AriaVM::call_value(const Value callee, const u8 argc) {
        if (!callee.is_obj()) {
            return fail(ErrorCode::CallNonCallable, "call non-callable {}", type_name(callee));
        }

        switch (Object* obj = callee.as_obj(); obj->type()) {
            case ObjType::FUNCTION:
                return call_function(Object::as<ObjFunction>(obj), argc);
            case ObjType::NATIVE_FN:
                return call_native(Object::as<ObjNativeFn>(obj), argc);
            default:
                return fail(ErrorCode::CallNonCallable,
                            "call non-callable {} (M1 supports functions / native functions only)", obj->type_name());
        }
    }

    bool AriaVM::call_function(ObjFunction* obj, const u8 argc) {
        if (obj->arity() != argc) {
            return fail(ErrorCode::WrongArity, "function expects {} args, got {}", obj->arity(), argc);
        }
        if (current_->frames_full()) {
            return fail(ErrorCode::StackOverflow, "call frame stack overflow");
        }
        // 栈形 [callee, a1..aN]:enter_frame 进帧(slots 指向槽 0,参数即局部槽 1..argc),
        // 与 run_ 的 exit_frame 对称。经 current_ 访问(与 run_/raise 同源,语义统一;现为
        // main_ctx_,M6 协程期即当前协程上下文 -- 主循环在哪个上下文驱动,进帧就进哪个)。
        current_->enter_frame(obj, argc);
        return true;
    }

    bool AriaVM::call_native(const ObjNativeFn* obj, const u8 argc) {
        // 经 current_ 访问调用区与寄存器(与 run_/raise 同源,语义统一)。原生函数同步调用,
        // 不进帧:bool 为成败信号,返回值写槽 0,错误载荷走侧信道寄存器。
        // 调用区 [callee, a1..aN] 的可写视图:slots[0]=槽 0(返回值),slots[1..argc]=实参。
        // peek(argc) 即槽 0,叶子调用不增长值栈故指针稳定;argc==0 时 span 仅含槽 0。
        //
        // entered_ctx = 调用发生时的上下文,本函数全部事后簿记(drop/寄存器断言)的统一锚点:
        // M6 前 current_ 恒等于它(下方守卫断言锁定「原生调用不得换走 current_」);M6 切换
        // 合法化后,已切换时它恰为刚被挂起的旧上下文 -- drop(argc) 恰好完成调用者侧栈归一,
        // 断言恰好检查调用方自己的寄存器,无需任何分支探测;届时仅删成功路径守卫、其余一字
        // 不动(false 路径守卫保留为永久契约:禁止 false+切换)。见 vm-design.md §4.9「切换协议」。
        // 实参留栈到 drop 亦是 GC 红利:切换型原生函数执行全程,实参(含协程对象)皆调用者栈根。
        const auto entered_ctx = current_;
        const auto slots       = Span<Value>{&current_->peek(argc), static_cast<usize>(argc + 1)};
        // 进场前寄存器应空(上次错误已被 take_error 取走 / reset 清空)。
        ASSERT(!current_->has_error(), "call_native: pending error not cleared before native call");
        if (obj->fn()(*this, slots)) {
            ASSERT(current_ == entered_ctx,
                   "call_native: current_ not restored across native call"); // M6 删:切换合法化
            // 成功:断言寄存器空(契约 return true ⟺ 未 raise)。寄存器本就空(进场已守、
            // 原生未 raise),无需 clear_error -- 若违约,debug 断言即暴露,release 下不静默
            // 清掉掩盖。drop 实参使返回值升栈顶(等价 drop(argc+1)+push);一律落在
            // entered_ctx 上,为 M6 切换形态预铺(见函数头注释)。
            ASSERT(!entered_ctx->has_error(), "native fn returned true but raised error");
            entered_ctx->drop(argc);
            return true;
        }
        ASSERT(current_ == entered_ctx, "call_native: current_ not restored across native call"); // M6 留:契约守卫
        // 失败:断言已 raise 载荷,留在寄存器交调用方 take_error 取出沿 runtime_err 路径传播。
        // 不在此 take_error -- 载荷随调用方上下文(entered_ctx)走,与 call_function/call_value
        // 的 bool 契约统一(return false ⟺ 错误已 raise 进调用方上下文,调用方据 bool 取载荷)。
        ASSERT(entered_ctx->has_error(), "native fn returned false but raised no error");
        return false;
    }

    ObjModule* AriaVM::load_module(ObjString* canonical_path, const StringView import_specifier) {
        // IMPORT 未命中分支的加载层。canonical_path 已由调用方根化(IMPORT case 的 canonical_path_guard,
        // 跨本函数内 upsert)。canonical_path = 命中文件的绝对规范路径(intern ObjString*),一身二任:
        // 既作 modules_ 表键,又作读盘路径。import_specifier = 用户写的原始 import 串(报错用)。
        //
        // 错误契约与 call_value 族同构:return nullptr ⟺ 错误载荷已 raise 入 *current_ 挂起寄存器,
        // 调用方(IMPORT 分支)take_error 取出沿 runtime_err 传播。两类失败:
        //   - 读盘失败/名字无效:fail 烘位置 -- raise 时顶帧即导入方帧,last_ip 指本 IMPORT 指令,
        //     消息带导入方站点位置(与 resolve_module 解析失败的 runtime_err 形态统一);
        //   - 编译期 Error:消息已在 CodeGen 侧烘焙完成(位置指向被导入文件内部),就地
        //     new_exception 直接装配箱(from_baked 语义,不重烘 -- 不经 AriaVM::raise,
        //     其 make_message 会把导入方站点前缀叠上,双重烘焙)。
        // **仅限 run_ 驱动期调用**:寄存器随 *current_ 走,run() 入口 reset 会清 pending_error --
        // run 外直调(未来预加载 API 等)的错误会被静默吞掉;runtime_loc 亦依赖顶帧,帧栈空则无位置。
        //
        // 步骤:读盘 -> 派生模块身份 -> new_module + 自守 -> 入表占位 -> 编译(set_entry)。
        // **仅加载与编译**:模块体 run-once 不在此执行 -- 由调用方(IMPORT 分支)以普通函数调用进帧
        // 驱动。加载事实源 = modules_ 表成员资格(对象无状态字段):入表即「已加载(体待 run-once 或
        // 已跑完)」,循环导入命中表内半初始化对象即复用。
        // 越界检测本轮不做:文件能解析到即读(resolve_module 已做 exists-check)。

        // 1. 读盘:BOM 剥除 + CRLF->LF + UTF-8 校验(见 SourceFile::from_path)。resolve_module 已 exists-check,
        //    理论上必成功,但读盘/编码仍可能失败(权限竞争 / 非法 UTF-8)。失败报 ModuleNotFound(带路径)。
        auto loaded_src = SourceFile::from_path(canonical_path->view());
        if (!loaded_src.has_value()) {
            (void) fail(ErrorCode::ModuleNotFound, "failed to load module '{}': read/decode error", import_specifier);
            return nullptr;
        }
        SourceFile source = std::move(loaded_src.value());

        // 2. 派生模块身份 {name=stem, dir=dirname}(同入口模块 fs::module_name_and_dir 约定):
        //    abs_path() = dir_ + "/" + name_ + ".aria" 还原 canonical key,相对导入基(dirname)正确。
        auto [name_s, dir_s] = fs::module_name_and_dir(canonical_path->view());
        if (name_s.empty()) {
            (void) fail(ErrorCode::ModuleNotFound, "module path has no valid name: '{}'", import_specifier);
            return nullptr;
        }

        // 3. 建模块(工厂内部 intern name/dir 并自守)+ 自守跨 upsert/编译。
        auto module = new_module(gc_, name_s, dir_s);
        auto guard  = gc_.make_guard(module);

        // 4. 入表占位:模块体尚未跑,但表内已有 -- 循环导入命中此半初始化对象直接复用(加载事实源
        //    即表成员资格,见上)。canonical_path 已由调用方根化;module 由上方 guard 根化。
        //    upsert rehash 触 GC 时两者皆安全。upsert 单参返 Entry*,再写其 value(同 DEF_GLOBAL 用法)。
        const auto mod_entry = modules_.upsert(Value::from_obj(canonical_path));
        mod_entry->value     = Value::from_obj(module);

        // 5. 编译:SourceFile -> 入口 ObjFunction(名 <module>,见 kModuleName),CodeGen::init_module
        //    已 module.set_entry。Compiler 用本 VM 的 gc_,编译期分配与 run 同源。编译期 Error(位置
        //    指向被导入文件内部)就地 new_exception 装箱入寄存器(from_baked 语义,消息不重烘)。
        //    module 经 guard + modules_ 根化,CodeGen::compile 内部亦 make_guard(&module),双保险。
        //    source 须存活到 compile() 返回(Error 烘位置串需它)。entry 经 module->entry_ 根可达。
        Compiler compiler{gc_};
        if (auto compiled = compiler.compile(source, *module, kModuleName); !compiled) {
            auto err = std::move(compiled).error();
            current_->raise(Value::from_obj(new_exception(gc_, err.code(), err.message())));
            return nullptr;
        }
        // 模块体尚未 run-once:执行由 IMPORT 分支进帧驱动、RETURN 完成压回模块对象。
        // 内置函数不经此注入 -- 由 VM 级 builtins_ 表统一承载,LOAD_GLOBAL 模块 globals 未命中后回退查之。
        return module;
    }

    template<OpCode Op>
    bool AriaVM::run_binary_numeric() {
        // 弹 2 算 1(9 个算术/比较指令共用,Op 由 run_ 调用点穷举实例化):双 Int 走整数路径,
        // 任一 F64 升浮点 -- int 除/模零报错、% 为 C++ 语义,f64 按 IEEE(除零得 inf/nan)。
        // 成功压结果返 true;失败不置值,经 fail 装箱入 *current_ 寄存器后返 false(call_value
        // 族 bool 契约,调用方 unwind 派发/物化)。
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

    Opt<Error> AriaVM::unwind_() {
        // 自最内帧向外遍历(pitfalls 坑 #13):每帧以 last_ip(顶帧 = 故障指令起始 / 外层帧 =
        // CALL 站点,均由 run_ 循环顶写好,坑 #2)反推 offset 查本帧 CodeUnit 的异常记录表。
        // 首命中即在该帧 unwind -- 此前轮次已逐帧 exit_frame 弹掉全部内层帧,本帧即栈顶,
        // 无需 FrameStack::truncate。全帧未命中 -> 未捕获:寄存器反提 Error + 跟踪烘焙(坑 #16)。
        // 前提:寄存器已有载荷(raise/fail/THROW 刚入;call_value 族 bool 契约 return false ⟺ 已
        // raise 保证) -- take 后解引用空可选是 UB,断言把关(write 侧 Movement::raise 空寄存器
        // 断言的 read 侧成对)。
        // 未捕获跟踪条目(仅本函数消费):fn/mod 物化路径只有 String 拼接、无 GC 分配点不悬垂;
        // ip_off 收集时就地换算 -- 帧随即被 exit_frame 弹掉,last_ip/unit 不可后取。
        struct TraceEntry {
            ObjFunction* fn;     // 帧函数(名字渲染)
            ObjModule*   mod;    // 帧模块(位置串渲染,module_loc)
            u32          ip_off; // 行号经 fn->unit().line_for_offset 查
        };

        ASSERT(current_->has_error(), "unwind_: no pending payload");
        List<TraceEntry> trace; // 收集序:内 -> 外;物化时反转为外 -> 内(Python 式 most recent call last)
        while (!current_->frames().empty()) {
            auto& [fn, unit, module, ip, slots, last_ip] = current_->frames().top();
            const auto ip_off                            = static_cast<u32>(last_ip - unit->code.data());
            if (const auto rec = unit->find_try_handler(ip_off)) {
                // 命中 handler:截值栈到本帧 slots + stack_depth(stack_depth 相对 frame.slots 非
                // 全局基址,坑 #6 -- 丢弃 try 体临时值与本帧残留,保留 callee/参数/已声明局部),
                // ip 跳 handler 入口;寄存器载荷 push 落 catch 参数槽(恒 == stack_depth,值填槽
                // 无 STORE_LOCAL,坑 #10)。take 清寄存器到 push 之间无分配,载荷不失根。
                const auto record = *rec;
                current_->truncate_stack(static_cast<usize>(slots + record->stack_depth - current_->stack_base()));
                ip = unit->code.data() + record->handle;
                current_->push(*current_->take_error());
                return std::nullopt; // 已派发 handler,调用方 break 回循环顶重取帧(坑 #11)
            }
            // 未命中:帧即将被弹,先记跟踪三元组再 exit_frame(坑 #16 点 2 -- 帧存活时收集)。
            trace.push_back(TraceEntry{fn, module, ip_off});
            current_->exit_frame();
        }

        // 全帧未命中 -> 未捕获:寄存器载荷反提拆 (码, 烘焙消息) 两件(ObjException 原码原消息
        // 含位置;用户 throw 原值兜底 UncaughtException,坑 #7),跟踪逐帧烘焙进消息尾部
        // (透传的编译期 Error 不经本路径,无跟踪 -- 坑 #16 点 6),拼完经 from_baked 一次物化
        // 成边界 Error。trace 恒非空(run_ 各调用点帧栈非空不变式),空循环是退化情形。
        auto [code, msg] = uncaught_error_parts(*current_->take_error());
        // 收集序内->外反转(坑 #16:渲染外->内)
        for (const auto& [fn, mod, ip_off]: std::views::reverse(trace)) {
            const auto line = fn->unit().line_for_offset(ip_off);
            msg += std::format("\n  at {} ({})", fn->name()->view(), module_loc(*mod, line));
        }
        return Error::from_baked(code, msg);
    }

    Result<Value, Error> AriaVM::run_() {
        // 语义统一:栈/帧/错误寄存器一律经 current_ 访问当前上下文(现为 main_ctx_;M6 切换时为
        // 被恢复协程的上下文)。M6 单循环切换模型(vm-design.md §4.9)下「正在执行的字节码所在
        // 上下文恒等于 current_」是结构性事实:切换只发生在原生函数体内(resume/yield 换走
        // current_,call_native 探测发现后循环顶自然采用新上下文),本循环永不重入;M6 前
        // call_native 断言锁「原生调用不得换走 current_」。

        while (true) {
            // 不变式:此处帧栈恒非空(run() 先 enter_frame 才进本循环;唯一弹空帧的 RETURN 顶层
            // 分支立即 return;CALL/IMPORT 切帧后 break 回到循环顶重取)。
            // 取指前记本帧指令起始指针:报错定位的行号锚点 -- 顶帧报错
            // 即故障指令、call_*/原生失败即 CALL 站点(被调帧未进 / 原生不进帧,顶帧仍是
            // caller);M3 unwind 查表同用此字段(坑点文档 #1/#2)。init_frame_ 置 code
            // 起始,首次取指前即覆写。offset 由查表冷路径按 last_ip - unit->code.data() 反推。
            CallFrame& frame = current_->frames().top();
            frame.last_ip    = frame.ip;
#ifdef DEBUG_TRACE_EXECUTION
            // 取 opcode 前打印执行状态:此时 frame.ip 指向待执行指令,trace_execution 据此解码(仅读不推进 ip)。
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
                case OpCode::LOAD_UPVALUE:
                    not_implemented("LOAD_UPVALUE");
                case OpCode::STORE_UPVALUE:
                    not_implemented("STORE_UPVALUE");
                case OpCode::CLOSE_UPVALUE:
                    not_implemented("CLOSE_UPVALUE");
                case OpCode::DEF_GLOBAL: {
                    // [v] -> []:以常量池 name(ObjString,intern)为键在当前模块 globals 首次定义
                    // (顶层 var 声明 -- 唯一创建全局的入口)。upsert:命中覆盖(重定义属编译期语义错误
                    // RedefinedVariable,运行期按定义处理),未命中插入新条目,再写入值。
                    //
                    // 根安全(GC 已启用):upsert 可能 rehash -> allocate -> maybe_collect。
                    //   - name/key:常量池内,经 frame.function -> unit -> constants 链根(同 LOAD_CONST)。
                    //   - v:用 peek 而非 pop -- 让 v 在 upsert 期间仍留在值栈(M6 值栈即根),collect
                    //     标得到;upsert 返回后再写 e.value、drop。若先 pop,v 退栈成裸局部,collect
                    //     会回收 v 指向的对象 -> 悬垂。键 Value::from_obj(name),靠 intern 同指针经 === 命中。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const Value value = current_->peek(0); // 先不弹:留 v 在栈上跨 upsert 的分配
                    const auto  entry = frame.module->globals().upsert(key);
                    entry->value      = value;
                    current_->drop(1); // 写完才弹,栈效应仍为 [v] -> []
                    break;
                }
                case OpCode::LOAD_GLOBAL: {
                    // [] -> [v]:按名查当前模块 globals,命中则压入;miss 回退 VM 级 builtins_ 表
                    //   (Python 式 globals -> builtins 查找链,内置经此解析);两者皆未命中 ->
                    //   UndefinedVariable 运行时错误。STORE_GLOBAL 不回退 builtins(赋值不隐式创建,
                    //   必须先 var 声明,见 grammar.txt §205-206),仅 DEF_GLOBAL 写模块 globals 可 shadow 内置。
                    //
                    // 根安全(GC 已启用):find 无分配;push 的唯一分配是值栈 grow,而 push 不变式
                    //   先写栈再增长(Movement::push),e->value 已入栈(根)后方 grow -> collect;且
                    //   e->value 经 module -> globals 或 builtins_ -> VM 根存活。name 同上经常量池根。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    auto        entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        entry = builtins_.find(key); // 回退 VM 级 builtins(内置 type/len/str/assert)
                        if (entry == nullptr) {
                            raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                            if (auto u = unwind_()) {
                                return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                            }
                            break; // 已派发 handler:帧栈可能已截,循环顶重取
                        }
                    }
                    current_->push(entry->value);
                    break;
                }
                case OpCode::STORE_GLOBAL: {
                    // [v] -> [v]:peek-store 到当前模块 globals(不弹,留 v);未定义 -> UndefinedVariable
                    // (赋值不隐式创建,必须先 var 声明,见 grammar.txt §205-206)。
                    //
                    // 根安全(GC 已启用):本指令无分配(find/peek/写均不触 maybe_collect),无风险。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    entry->value = current_->peek(0);
                    break;
                }
                case OpCode::LOAD_FIELD:
                    not_implemented("LOAD_FIELD");
                case OpCode::STORE_FIELD:
                    not_implemented("STORE_FIELD");
                case OpCode::LOAD_INDEX:
                    not_implemented("LOAD_INDEX");
                case OpCode::STORE_INDEX:
                    not_implemented("STORE_INDEX");
                case OpCode::LOAD_THIS_FIELD:
                    not_implemented("LOAD_THIS_FIELD");
                case OpCode::STORE_THIS_FIELD:
                    not_implemented("STORE_THIS_FIELD");

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
                // 比较(弹 2 压 1;run_binary_numeric 与 call_value 族同款 bool 契约,失败善后与
                // CALL case 同形:unwind_ 查表派发 / 未捕获物化 Error 终止 run_,pitfalls 坑 #11)
                case OpCode::GREATER:
                    if (!run_binary_numeric<OpCode::GREATER>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    break;
                case OpCode::GREATER_EQUAL:
                    if (!run_binary_numeric<OpCode::GREATER_EQUAL>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::LESS:
                    if (!run_binary_numeric<OpCode::LESS>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::LESS_EQUAL:
                    if (!run_binary_numeric<OpCode::LESS_EQUAL>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                // 算术(弹 2 压 1;同上)
                case OpCode::ADD:
                    if (!run_binary_numeric<OpCode::ADD>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::SUBTRACT:
                    if (!run_binary_numeric<OpCode::SUBTRACT>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::MULTIPLY:
                    if (!run_binary_numeric<OpCode::MULTIPLY>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::DIVIDE:
                    if (!run_binary_numeric<OpCode::DIVIDE>()) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::MOD:
                    if (!run_binary_numeric<OpCode::MOD>()) {
                        if (auto u = unwind_()) {
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
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
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
                        // 失败载荷已在寄存器(call_value 族 bool 契约),unwind 查异常记录表:
                        // 命中 handler 即截栈派发(值落 catch 参数槽),全未命中物化 Error 出栈。
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                        }
                        break; // 已派发 handler:帧栈已 truncate,循环顶重取
                    }
                    break; // 帧已切换,frame 引用作废,循环顶重新取
                }
                case OpCode::CLOSURE:
                    not_implemented("CLOSURE");

                // ---- 类与对象 ----
                case OpCode::LOAD_OBJECT:
                    not_implemented("LOAD_OBJECT");
                case OpCode::MAKE_CLASS:
                    not_implemented("MAKE_CLASS");
                case OpCode::MAKE_METHOD:
                    not_implemented("MAKE_METHOD");
                case OpCode::MAKE_STATIC:
                    not_implemented("MAKE_STATIC");
                case OpCode::LOAD_SUPER_METHOD:
                    not_implemented("LOAD_SUPER_METHOD");
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
                    // path:u16;解析后把命中的 ObjModule 压栈（[...] -> [..., module]）。绑定交 CodeGen 按
                    //   作用域走：顶层经 DEF_GLOBAL alias 弹值定义全局、嵌套经值填槽（IMPORT 压在 declare
                    //   的 slot 即该局部）+ mark_initialized。IMPORT 仅负责「取模块对象」，不再自绑 globals。
                    //   解析按 .claude/reference/runtime/import-path-resolution.md:把 import 串经 resolve_module
                    //   解析为命中文件的绝对规范路径(weakly_canonical)作模块表键,再查 modules_。
                    //   相对(./ ../)基 = 当前模块所在目录(单基,caller-local,不碰 source_roots);
                    //   裸名基 = source_roots(逐个 exists-check,首个 <base>/<spec>.aria 存在者命中)。
                    //   键 = 绝对规范路径,跨根不碰撞、相对不逃逸;末尾 .aria 可选(lib/math ≡ lib/math.aria)。
                    //   命中(表内任意初始化进度):复用模块对象 -- 体已跑完即完整、命中正在 run-once 的
                    //     模块即循环导入,按文法「允许循环导入,命中正在初始化的模块返回半初始化对象」
                    //     直接用(不报错);压栈即可。加载事实源 = 表成员资格,对象无状态字段。
                    //   未命中(文件解析命中但模块未入表):调 load_module(读盘 -> 编译 -> 入表占位)得
                    //     模块,以其 entry(<module>)作一次**普通函数调用**进帧后 break --
                    //     模块体 run-once 即执行一个函数,由主循环照常驱动;其 RETURN 按函数名 == <module>
                    //     判定模块体帧,弹弃返回值、改压模块对象,等价「模块体返回模块」,
                    //     故命中/未命中两分支栈效应统一为 [..., module],绑定交后续 DEF_GLOBAL / 值填槽。
                    //     无递归 run_()。
                    //
                    // 根安全(GC 已启用):path 经常量池根(同 LOAD_CONST)。canonical_path 经 new_string intern 驻留
                    //   (weak root,不保命),跨 modules_.find(无 GC)与未命中分支内 load_module 的
                    //   modules_.upsert(rehash 触 GC)须守 -- 下方 canonical_path_guard 跨全程根化 canonical_path。
                    //   命中分支取回/加载层产出的 module 与 entry 经 modules_ 根可达,非移动 GC 故 current_->push(栈
                    //   溢出增长走 Buffer 重分配)期间指针稳定,无需守卫;入栈后另经值栈根可达。
                    ObjString* path = read_name(frame);
                    // 当前模块的绝对文件路径(ObjModule::abs_path = dir_ + "/" + name_ + ".aria"),
                    // 供 resolve_module 相对分支取 dirname 作基:dirname(abs_path) = dir_ (.aria 后缀在末段,
                    // dirname 不受影响)= 当前模块所在目录。裸名分支不依赖此值。dir_ 指针恒非空但内容可空
                    // (cwd 不可用时空串兜底):此时 abs_path 返空串,resolve_module 相对分支判空直接返 nullopt。
                    const auto canonical_path_str =
                            resolve_module(path->view(), frame.module->abs_path(), source_roots_);
                    if (!canonical_path_str) {
                        raise(ErrorCode::ModuleNotFound, "module not found: '{}'", path->view());
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                        }
                        break; // 已派发 handler:帧栈可能已截,循环顶重取
                    }
                    auto canonical_path = new_string(gc_, *canonical_path_str);
                    auto guard = gc_.make_guard(canonical_path); // 跨 find / load_module 内 upsert(rehash 触 GC)
                    if (const auto module_entry = modules_.find(Value::from_obj(canonical_path));
                        module_entry != nullptr) {
                        // 命中(体已跑完 / 循环导入半初始化)复用:压模块值,绑定交后续 DEF_GLOBAL / 值填槽。
                        current_->push(module_entry->value);
                        break;
                    }
                    // 未命中:加载链路 -- 读盘 -> 编译 -> 入表占位(见 load_module),不在此执行模块体。
                    //   失败契约同 call_value 族:return nullptr ⟺ 载荷已 raise 入当前上下文寄存器
                    //   (ModuleNotFound 带 IMPORT 站点位置 / 被导入模块编译期 Error 原样装配箱),
                    //   unwind 查表:命中 handler 即截栈派发,全未命中物化 Error 出栈。
                    ObjModule* module = load_module(canonical_path, path->view());
                    if (module == nullptr) {
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                        }
                        break; // 已派发 handler:frame 已废,循环顶重取
                    }
                    // 模块体 run-once = 一次普通 0 参函数调用:压 callee(entry,<module>)-> 进帧 -> break。
                    //   主循环照常执行该帧;其 RETURN 按函数名 == <module> 判定模块体帧,弹弃返回值、
                    //   压回该模块对象,栈效应同命中分支 [..., module]。entry 经 module->entry_ 根可达。
                    ObjFunction* entry = module->entry();
                    current_->push(Value::from_obj(entry)); // callee 压栈
                    if (!call_value(Value::from_obj(entry), 0)) {
                        // 进帧失败(栈溢出等):帧未进,callee 仍在栈顶(unwind 截栈时一并丢弃)。
                        // 载荷已在寄存器,unwind 查表派发 / 物化 Error 出栈。
                        if (auto u = unwind_()) {
                            return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                        }
                        break; // 已派发 handler:帧栈已 truncate,循环顶重取
                    }
                    break; // 帧已切换,frame 引用作废,循环顶重新取
                }

                // ---- 异常 ----
                case OpCode::THROW: {
                    // 用户 throw:弹抛出值,原值入寄存器(不包 ObjException -- catch 绑原值保类型,
                    // 坑 #7/#12)后 unwind -- 命中 handler 截栈跳 handler(值落 catch 参数槽),全帧
                    // 未命中物化 UncaughtException Error(消息渲染值本身,无位置前缀 -- 位置由
                    // 未捕获跟踪的 at 行给出,坑 #16)。
                    current_->raise(current_->pop());
                    if (auto u = unwind_()) {
                        return runtime_err(std::move(*u)); // 未捕获 -> 终止 run_
                    }
                    break; // 已派发 handler:帧栈已 truncate,循环顶重取
                }

                // ---- 返回(exit_frame 后 frame 引用作废,故先取返回值与判模块体帧)----
                case OpCode::RETURN: {
                    const Value ret = current_->pop(); // 取返回值(exit_frame 将丢弃其下方栈区)
                    // 模块体 run-once 帧 = IMPORT 加载层进帧的入口函数,其名固定为 <module>
                    // (见 kModuleName);主入口 <main> 与普通用户函数名均不含 '<>',故按函数名判定。
                    // 其 RETURN 弹弃模块体返回值(无意义),改压该模块对象 -- 模块体「返回模块」,
                    // 使 IMPORT 的栈效应在命中/未命中两分支统一为 [..., module](绑定交 DEF_GLOBAL)。
                    // 先取 module 再 exit_frame:exit_frame 后 frame 引用悬垂。模块体帧必非顶层(IMPORT
                    // 帧在下),故 current_->frames().empty() 分支不会命中模块体帧。
                    auto mod     = frame.module;
                    auto fn_name = frame.function->name()->view();
                    current_->exit_frame(); // 弹帧 + 值栈顶复位到 slots,一体
                    if (current_->frames().empty()) {
                        return ret; // 顶层(主入口 <main>)返回:返回值为程序结果
                    }
                    if (fn_name == kModuleName) {
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
