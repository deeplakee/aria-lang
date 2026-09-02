#include "runtime/AriaVM.hpp"

#include <cmath>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/Disassembler.hpp"
#include "bytecode/code.hpp"
#include "compile/Compiler.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
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
        // 判定是否弹弃返回值、置该模块 Loaded、改压模块对象 -- 取代在 CallFrame 上加 is_module_body
        // 标志位:名字是函数的固有属性,无需进帧时额外置位/复位,亦无帧槽复用残留之虞。
        // 名字经 intern 驻留(指针唯一),view() 为短串(8 字节),逐 RETURN 一次内容比较开销可忽略。
        constexpr StringView kModuleName = "<module>";

        // 把 import 串解析为命中文件的绝对规范路径(模块表键)。设计见
        // .claude/reference/runtime/import-path-resolution.md「加载层设计基线」:键 = weakly_canonical(候选)
        // (解析已存在部分的符号链接、折叠 "."/".."、去冗余分隔符)。
        //   - 相对(以 "./" / "../" 开头,或正是 "." / ".."):基 = 当前模块所在目录
        //     (dirname(current_abs),current_abs = frame.module->abs_path() = root_ + "/" + name_ + ".aria";
        //     .aria 后缀在末段,dirname 不受影响 -> 等价 dirname(root_ + "/" + name_));
        //     单基,caller-local,不碰 source_roots,故相对导入永不逃逸到别的源根。
        //     root_ 指针恒非空但内容可空:cwd 可用时 current_abs 为正常绝对路径(合成模块如
        //     <script> 退化为 cwd,相对导入以 cwd 为基);cwd 不可用时 root_ 为空串 -> abs_path
        //     返空串 -> 下方 current_abs.empty() 守卫触发,相对解析直接返 nullopt(拒绝锚定)。
        //   - 裸名(无 ./ ../ 前缀):基 = source_roots(入口槽 [0] = 入口模块 root_、编译器相对
        //     stdlib 目录等),逐个 exists-check,首个存在 <base>/<spec>.aria 者命中(对齐 Python
        //     sys.path 顺序搜索,先入源根者占坑)。
        //   - 末尾 ".aria" 可选:spec 末段以 ".aria" 结尾(段长 > 5)则剥离,查找时统一补回
        //     ".aria",使 lib/math ≡ lib/math.aria。
        //   - 键 = 绝对规范路径:同模块不同写法、不同源根同名模块均归一到各自真实路径,
        //     跨根不碰撞、相对不逃逸。符号链接经 weakly_canonical 规避双加载。
        // 磁盘:每基一次 weakly_canonical + exists(裸名 = N 次 stat,相对 = 1 次);解析缓存待加(TODO)。
        // 越界/沙箱检测(相对导入越出源根)留待加载层:本函数不限制 ".." 折叠后的路径范围。
        Opt<String> resolve_module(const StringView spec, const StringView current_abs,
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
                if (current_abs.empty()) {
                    return std::nullopt; // 合成模块无绝对路径,无法相对解析
                }
                bases.push_back(stdfs::path{String{current_abs}}.parent_path());
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

        // 构造运行时错误结果(Result<Value, Error> 的 unexpected 态),以 std::format 风格
        // 直接格式化消息。供 run_ 与 numeric_op 等「返回 Result<Value, Error>」的构造点用,
        // 把 std::unexpected(Error{...}) 样板整体收口;调用方只剩 return runtime_err(...)。
        // 转发已构造 Error(如 run_binary_numeric 失败回传、call_value 失败后从 ctx 取出的载荷)
        // 的场合用下方 runtime_err(Error) 重载,同样收口 std::unexpected。耦合 Result<Value, Error>,
        // 故仅限本翻译单元(其它阶段返回类型不同,不复用)。
        template<typename... Args>
        Result<Value, Error> runtime_err(ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            return std::unexpected(errorf(code, fmt, std::forward<Args>(args)...));
        }

        // 转发已构造 Error 为失败结果:把 std::unexpected(std::move(err)) 样板收口。供 run_
        // 转发 run_binary_numeric 失败回传的 Error、以及 call_value 失败后从 ctx 取出的载荷用,
        // 与上面的格式化构造重载成对。
        Result<Value, Error> runtime_err(Error err) { return std::unexpected(std::move(err)); }

        // 把无位置运行时 Error raise 进传入 ctx 的挂起错误寄存器并返回 false(成败信号)。供
        // call_value / call_function 等以 bool 为成败信号的子例程一行报错 -- 错误载荷走 ctx 侧信道,
        // 调用方(主循环)据返回的 bool 决定是否 take_error 取出沿 runtime_err 传播。与 AriaVM::fail
        // (面向 main_ctx_)分工:本组函数经传入的 ctx 而非 main_ctx_ 报错,贴合 run_ 的重入式风格
        // (M6 协程期 ctx 即当前协程上下文,错误随上下文走,互不串扰)。契约与 NativeFn 一致:
        // return false ⟺ 已 raise;调用方据 bool 取载荷,空则属违约(见 call_native 断言)。
        template<typename... Args>
        bool ctx_fail(Movement& ctx, ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            ctx.raise(errorf(code, fmt, std::forward<Args>(args)...));
            return false;
        }

        // 数值二元运算(算术 + 比较)。双方皆 Int 走整数路径;任一为 F64 则升 F64
        // (浮点除零走 IEEE 的 inf/nan,不报错)。M1 暂定语义:
        //   - Int / Int 截断整除、% 为 C++ 语义(除/模零报运行时错误);
        //   - Int 溢出 i48 不设防(断言把关,编译期语义阶段再议)。
        // 见 bytecode-instruction-set.md §9 待决 #8。Op 为模板参数,编译期按指令分派。
        template<OpCode Op>
        Result<Value, Error> numeric_op(const Value a, const Value b) {
            if (!(is_num(a) && is_num(b))) {
                return runtime_err(ErrorCode::TypeMismatch, "operator '{}' requires numbers, got {} and {}",
                                   op_symbol(Op), type_name(a), type_name(b));
            }
            if (a.is_int() && b.is_int()) {
                const auto x = a.as_int();
                const auto y = b.as_int();
                if constexpr (Op == OpCode::ADD) {
                    return Value::from_int(x + y);
                } else if constexpr (Op == OpCode::SUBTRACT) {
                    return Value::from_int(x - y);
                } else if constexpr (Op == OpCode::MULTIPLY) {
                    return Value::from_int(x * y);
                } else if constexpr (Op == OpCode::DIVIDE) {
                    if (y == 0) {
                        return runtime_err(ErrorCode::DivisionByZero, "integer division by zero");
                    }
                    return Value::from_int(x / y);
                } else if constexpr (Op == OpCode::MOD) {
                    if (y == 0) {
                        return runtime_err(ErrorCode::ModuloByZero, "integer modulo by zero");
                    }
                    return Value::from_int(x % y);
                } else if constexpr (Op == OpCode::GREATER) {
                    return Value::from_bool(x > y);
                } else if constexpr (Op == OpCode::GREATER_EQUAL) {
                    return Value::from_bool(x >= y);
                } else if constexpr (Op == OpCode::LESS) {
                    return Value::from_bool(x < y);
                } else if constexpr (Op == OpCode::LESS_EQUAL) {
                    return Value::from_bool(x <= y);
                }
                UNREACHABLE();
            }
            const auto x = a.is_f64() ? a.as_f64() : static_cast<f64>(a.as_int());
            const auto y = b.is_f64() ? b.as_f64() : static_cast<f64>(b.as_int());
            if constexpr (Op == OpCode::ADD) {
                return Value::from_f64(x + y);
            } else if constexpr (Op == OpCode::SUBTRACT) {
                return Value::from_f64(x - y);
            } else if constexpr (Op == OpCode::MULTIPLY) {
                return Value::from_f64(x * y);
            } else if constexpr (Op == OpCode::DIVIDE) {
                return Value::from_f64(x / y); // IEEE: 除零得 inf/nan
            } else if constexpr (Op == OpCode::MOD) {
                return Value::from_f64(std::fmod(x, y));
            } else if constexpr (Op == OpCode::GREATER) {
                return Value::from_bool(x > y);
            } else if constexpr (Op == OpCode::GREATER_EQUAL) {
                return Value::from_bool(x >= y);
            } else if constexpr (Op == OpCode::LESS) {
                return Value::from_bool(x < y);
            } else if constexpr (Op == OpCode::LESS_EQUAL) {
                return Value::from_bool(x <= y);
            }
            UNREACHABLE();
        }

        // 弹 2 算 1:对栈顶两个值执行二元数值运算(算术/比较),成功压结果、失败返回 Error
        // 供 run_ 终止为 Uncaught。Op 为模板参数,调用方显式指定具体指令;与 numeric_op
        // 分工:后者纯计算(不触栈),此函数只管栈效应。
        template<OpCode Op>
        Opt<Error> run_binary_numeric(Movement& ctx) {
            const Value b = ctx.pop();
            const Value a = ctx.pop();
            const auto  r = numeric_op<Op>(a, b);
            if (!r) {
                return r.error();
            }
            ctx.push(*r);
            return std::nullopt;
        }

        // 范围外 opcode 的统一处理:后续阶段(闭包/字段/索引/类/异常等)才会实现,
        // 当前不执行。命中即打印提示后直接终止进程(经 fatal_error,不沿 run_ 返回)。
        // 用 OpcodeNotImplemented(Internal 类)而非 NotImplemented(Semantic 类):后者经 Error 通道
        // 服务编译期 CodeGen not_impl(可恢复 CompileError);此处是运行期执行到未实现 opcode,
        // 不可恢复走 fatal_error,属解释器实现不完整(Internal)。
        [[noreturn]]
        void not_implemented(const StringView op_name) {
            fatal_error(ErrorCode::OpcodeNotImplemented,
                        std::format("opcode '{}' not implemented yet (out of current scope: closures/fields/index/"
                                    "classes/exceptions come later)",
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
        //   - 字节码行:模块信息(to_string + state,置于 [trace] 与 <fn> 之间)+ 栈顶帧 fn 名 @ip 偏移
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

            // 字节码行:模块信息(to_string + state)+ fn 名 @偏移 + 指令文本。
            // 模块信息置于 [trace] 与 <fn> 之间,使第一行即含完整位置上下文。
            const auto* mod = frame.module;
            io::print(stderr, "[trace] {} {}  {} @{:04X}  {}\n", mod->to_string(),
                      mod->state() == ObjModule::ModuleState::Loading ? "Loading" : "Loaded",
                      frame.function->to_string(), static_cast<u32>(ip_off), instr);

            // 栈:逐槽渲染成段 [ v ](空栈打印 (empty)),记下每段起始偏移供帧标记对齐。
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

    // 构造:成员初始化(gc_ 先,main_ctx_/modules_ 借 &gc_),再把 VM 根 tracer 注册进自有 GC。
    // tracer 为 lambda:[this] 捕获,标记三类根:
    //   1) modules_:解释器级共享模块表(进而 trace 各模块 name_/root_/entry_/globals_);
    //   2) main_ctx_ 值栈 [base, top):run() 期局部/实参/临时值只活在栈上,不经常量池链可达,
    //      是最关键的缺失根。run() 结束 reset() 清空,故 run() 外(compile/测试)GC 时栈遍历为空,
    //      不会标到指向已回收对象的陈旧栈值;
    //   3) 各活动帧的 function/module:本可经 module -> entry -> 常量池链可达,直标更稳、
    //      免依赖「帧函数必在其父常量池」不变式。open upvalues 留待 M4。
    // 值栈/帧以 tracer 直标代替 Movement 升 Object(M6 协程期再升级 ObjMovement 入对象链表)。
    // VM 持有 gc_(值成员),成员逆序析构下 gc_ 最后析构,tracer 与 modules_/main_ctx_ 同生共死,无需析构注销。
    AriaVM::AriaVM() : gc_{}, main_ctx_{&gc_}, modules_{&gc_}, source_roots_{} {
        gc_.set_vm_roots([this](GC& g) {
            modules_.trace(g);
            auto& ctx = main_ctx_;
            for (Value* p = ctx.stack_base(); p < ctx.stack_top(); ++p) {
                g.mark_value(*p); // mark_value 对非对象 Value no-op,栈槽含 int/f64/bool/nil 安全
            }
            for (const auto& f: ctx.frames().span()) {
                g.mark_object(f.function); // mark_object 容 nullptr
                g.mark_object(f.module);
            }
        });
        // source_roots_[0] = 入口槽:构造时占位为当前工作目录(前期源根),run() 时被入口模块 root_
        //   原地替换。占位用 cwd:既是一个可用的默认源根(REPL / 未显式设 root_ 时裸名搜 cwd),
        //   又保证 [0] 槽位恒在,run() 可直接赋值无需 null/空判定。cwd 不可用时以空串兜底(不 fatal):
        //   resolve_module 裸名分支跳过空根(line 87),仅搜 [1..] 配置根,比拿 "." 锚到坏目录更诚实。
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
        // 替换配置根 [1..],保留入口槽 [0](cwd 或 run() 设入的入口 root_)-- 入口槽归 run() 管。
        source_roots_.resize(1);
        for (auto& r: roots) {
            source_roots_.push_back(std::move(r));
        }
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
        // 合成入口模块 <script>（root=cwd，2 参 new_module）。
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

        // 入口模块身份（dirname + basename）：name = basename 去 .aria、root = dirname(absolute(path))，
        // 拆分收口于 fs::module_name_and_root。name 为空表路径非合法文件模块（目录 / 空 / 无文件名），
        // 报 LoadError 而非静默兜底--与读盘失败同属「加载不到合法源文件」。
        auto [name_s, root_s] = fs::module_name_and_root(path);
        if (name_s.empty()) {
            io::println(stderr, "源文件路径无有效模块名: '{}'", path);
            return InterpretResult::LoadError;
        }
        auto module = new_module(gc_, name_s, root_s); // 3 参：显式 root
        auto guard  = gc_.make_guard(module);

        return interpret_run(source, *module);
    }

    Result<Value, Error> AriaVM::run(ObjFunction* fn) {
        // GC 已启用:值栈/帧经 vm_roots tracer 标根(见 ctor),IMPORT/DEF_GLOBAL 等已按「栈即根」
        // 前置编写(peek-not-pop)。
        // 源根:入口槽 [0] 原地替换为入口模块 root_(对齐 Python sys.path[0] -- 入口源根居首,
        // 配置根 stdlib / -L / 环境变量在 [1..] 不动)。直接赋值 [0],无 flag、无重建、reuse 安全
        // (覆盖旧值,不累积)。root_ 指针恒非空(构造期 ASSERT),内容可空(<script> 在 cwd 不可用
        // 时退化为空串,磁盘模块 = 命中源根):空串由 resolve_module 裸名分支跳空根(line 87)处理,
        // 故此处不判空。
        // 注:fn 必属某模块(module_ 非空,见 ObjFunction);[0] 槽位由构造时 cwd 占位恒在。
        source_roots_[0] = fn->module()->root()->view();

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

    bool AriaVM::call_value(Movement& ctx, const Value callee, const u8 argc) {
        if (!callee.is_obj()) {
            return ctx_fail(ctx, ErrorCode::CallNonCallable, "call non-callable {}", type_name(callee));
        }
        Object* obj = callee.as_obj();

        switch (obj->type()) {
            case ObjType::FUNCTION:
                return call_function(ctx, Object::as<ObjFunction>(obj), argc);
            case ObjType::NATIVE_FN:
                return call_native(ctx, Object::as<ObjNativeFn>(obj), argc);
            default:
                return ctx_fail(ctx, ErrorCode::CallNonCallable,
                                "call non-callable {} (M1 supports functions / native functions only)",
                                obj->type_name());
        }
    }

    bool AriaVM::call_function(Movement& ctx, ObjFunction* obj, const u8 argc) {
        if (obj->arity() != argc) {
            return ctx_fail(ctx, ErrorCode::WrongArity, "function expects {} args, got {}", obj->arity(), argc);
        }
        if (ctx.frames_full()) {
            return ctx_fail(ctx, ErrorCode::StackOverflow, "call frame stack overflow");
        }
        // 栈形 [callee, a1..aN]:enter_frame 进帧(slots 指向槽 0,参数即局部槽 1..argc),
        // 与 run_ 的 exit_frame 对称。经传入的 ctx 而非 main_ctx_ -- M6 协程期 ctx 即当前协程上下文。
        ctx.enter_frame(obj, argc);
        return true;
    }

    bool AriaVM::call_native(Movement& ctx, const ObjNativeFn* obj, const u8 argc) {
        // 原生函数同步调用,不进帧:bool 为成败信号,返回值写槽 0,错误载荷走侧信道寄存器。
        // 调用区 [callee, a1..aN] 的可写视图:slots[0]=槽 0(返回值),slots[1..argc]=实参。
        // peek(argc) 即槽 0,叶子调用不增长值栈故指针稳定;argc==0 时 span 仅含槽 0。
        const auto slots = Span<Value>{&ctx.peek(argc), static_cast<usize>(argc + 1)};
        // 进场前寄存器应空(上次错误已被 take_error 取走 / reset 清空)。
        ASSERT(!ctx.has_error(), "call_native: pending error not cleared before native call");
        if (obj->fn()(*this, slots)) {
            // 成功:断言寄存器空(契约 return true ⟺ 未 raise)。寄存器本就空(进场已守、
            // 原生未 raise),无需 clear_error -- 若违约,debug 断言即暴露,release 下不静默
            // 清掉掩盖。drop 实参使返回值升栈顶(等价 drop(argc+1)+push)。
            ASSERT(!ctx.has_error(), "native fn returned true but raised error");
            ctx.drop(argc);
            return true;
        }
        // 失败:断言已 raise 载荷,留在寄存器交调用方 take_error 取出沿 runtime_err 路径传播。
        // 不在此 take_error -- 载荷随 ctx 走,与 call_function/call_value 的 bool 契约统一
        // (return false ⟺ 错误已 raise 进 ctx,调用方据 bool 取载荷)。
        ASSERT(ctx.has_error(), "native fn returned false but raised no error");
        return false;
    }

    Result<ObjModule*, Error> AriaVM::load_module(ObjString* key, const StringView path_spec) {
        // IMPORT 未命中分支的加载层。key 已由调用方根化(IMPORT case 的 key_guard,跨本函数内 upsert)。
        // path_spec = 原始 import 串(报错用);读盘用 key->view()(= 命中文件的绝对规范路径)。
        //
        // 步骤:读盘 -> 派生模块身份 -> new_module(Loading)+ 自守 -> 入表占位 -> 编译(set_entry)。
        // **仅加载与编译**:模块体 run-once 不在此执行 -- 由调用方(IMPORT 分支)以普通函数调用进帧
        // 驱动,其 RETURN 置 Loaded。故返回的模块处于 Loading 态(待 run-once)。
        // 越界检测本轮不做:文件能解析到即读(resolve_module 已做 exists-check)。

        // 1. 读盘:BOM 剥除 + CRLF->LF + UTF-8 校验(见 SourceFile::from_path)。resolve_module 已 exists-check,
        //    理论上必成功,但读盘/编码仍可能失败(权限竞争 / 非法 UTF-8)。失败报 ModuleNotFound(带路径)。
        auto loaded_src = SourceFile::from_path(key->view());
        if (!loaded_src.has_value()) {
            return std::unexpected(
                    errorf(ErrorCode::ModuleNotFound, "failed to load module '{}': read/decode error", path_spec));
        }
        SourceFile source = std::move(loaded_src.value());

        // 2. 派生模块身份 {name=stem, root=dirname}(同入口模块 fs::module_name_and_root 约定):
        //    abs_path() = root_ + "/" + name_ + ".aria" 还原 canonical key,相对导入基(dirname)正确。
        auto [name_s, root_s] = fs::module_name_and_root(key->view());
        if (name_s.empty()) {
            return std::unexpected(errorf(ErrorCode::ModuleNotFound, "module path has no valid name: '{}'", path_spec));
        }

        // 3. 建模块(Loading 态,工厂内部 intern name/root 并自守)+ 自守跨 upsert/编译。
        auto module = new_module(gc_, name_s, root_s);
        auto guard  = gc_.make_guard(module);

        // 4. 入表占位(Loading):供循环导入命中半初始化对象。key 已由调用方根化;module 由上方 guard 根化。
        //    upsert rehash 触 GC 时两者皆安全。upsert 单参返 Entry*,再写其 value(同 DEF_GLOBAL 用法)。
        const auto mod_entry = modules_.upsert(Value::from_obj(key));
        mod_entry->value     = Value::from_obj(module);

        // 5. 编译:SourceFile -> 入口 ObjFunction(名 <module>,见 kModuleName),CodeGen::init_module
        //    已 module.set_entry。Compiler 用本 VM 的 gc_,编译期分配与 run 同源。编译期 Error(含
        //    被导入文件位置)原样透传。module 经 guard + modules_ 根化,CodeGen::compile 内部亦
        //    make_guard(&module),双保险。source 须存活到 compile() 返回(Error 烘位置串需它)。entry
        //    经 module->entry_ 根可达。
        Compiler compiler{gc_};
        auto     compiled = compiler.compile(source, *module, kModuleName);
        if (!compiled.has_value()) {
            return std::unexpected(std::move(compiled).error());
        }
        // 模块保持 Loading 态:run-once + 置 Loaded 由 IMPORT 分支进帧驱动、RETURN 完成。
        return module;
    }

    Result<Value, Error> AriaVM::run_() {
        auto& ctx    = main_ctx_;
        auto& frames = ctx.frames();

        while (true) {
#ifdef DEBUG_TRACE_EXECUTION
            // 取 opcode 前打印执行状态:此时 frame.ip 指向待执行指令,trace_execution 据此解码(仅读不推进 ip)。
            trace_execution(ctx);
#endif
            // 各 case 严格按 bytecode/code.hpp 中 OpCode 枚举的声明顺序排列。
            switch (CallFrame& frame = frames.top(); auto op = static_cast<OpCode>(read_u8(frame))) {
                case OpCode::HALT:
                    return Value::nil_val();

                // ---- 数据加载与存储 ----
                case OpCode::LOAD_CONST: {
                    const auto idx = read_u16(frame);
                    ctx.push(frame.unit->constants[idx]);
                    break;
                }
                case OpCode::LOAD_NIL:
                    ctx.push(Value::nil_val());
                    break;
                case OpCode::LOAD_TRUE:
                    ctx.push(Value::true_val());
                    break;
                case OpCode::LOAD_FALSE:
                    ctx.push(Value::false_val());
                    break;
                case OpCode::LOAD_IMM: {
                    const u8 raw = read_u8(frame);
                    ctx.push(Value::from_i32(std::bit_cast<i8>(raw)));
                    break;
                }
                case OpCode::LOAD_LOCAL: {
                    const u8 slot = read_u8(frame);
                    ctx.push(frame.slots[slot]);
                    break;
                }
                case OpCode::STORE_LOCAL: {
                    const u8 slot     = read_u8(frame);
                    frame.slots[slot] = ctx.peek(0);
                    break;
                }
                case OpCode::LOAD_LOCAL_L: {
                    const auto slot = read_u16(frame);
                    ctx.push(frame.slots[slot]);
                    break;
                }
                case OpCode::STORE_LOCAL_L: {
                    const auto slot   = read_u16(frame);
                    frame.slots[slot] = ctx.peek(0);
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
                    // 根安全(M6 解锁 GC 后):upsert 可能 rehash -> allocate -> maybe_collect。
                    //   - name/key:常量池内,经 frame.function -> unit -> constants 链根(同 LOAD_CONST)。
                    //   - v:用 peek 而非 pop -- 让 v 在 upsert 期间仍留在值栈(M6 值栈即根),collect
                    //     标得到;upsert 返回后再写 e.value、drop。若先 pop,v 退栈成裸局部,collect
                    //     会回收 v 指向的对象 -> 悬垂。键 Value::from_obj(name),靠 intern 同指针经 === 命中。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const Value value = ctx.peek(0); // 先不弹:留 v 在栈上跨 upsert 的分配
                    const auto  entry = frame.module->globals().upsert(key);
                    entry->value      = value;
                    ctx.drop(1); // 写完才弹,栈效应仍为 [v] -> []
                    break;
                }
                case OpCode::LOAD_GLOBAL: {
                    // [] -> [v]:按名查当前模块 globals,压入;未定义 -> UndefinedVariable 运行时错误。
                    //
                    // 根安全(M6 解锁 GC 后):find 无分配;push 的唯一分配是值栈 grow,而 push 不变式
                    //   先写栈再增长(Movement::push),e->value 已入栈(根)后方 grow -> collect;且
                    //   e->value 经 module -> globals 根存活。name 同上经常量池根。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        return runtime_err(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                    }
                    ctx.push(entry->value);
                    break;
                }
                case OpCode::STORE_GLOBAL: {
                    // [v] -> [v]:peek-store 到当前模块 globals(不弹,留 v);未定义 -> UndefinedVariable
                    // (赋值不隐式创建,必须先 var 声明,见 grammar.txt §445)。
                    //
                    // 根安全(M6 解锁 GC 后):本指令无分配(find/peek/写均不触 maybe_collect),无风险。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        return runtime_err(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                    }
                    entry->value = ctx.peek(0);
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
                    const Value b = ctx.pop();
                    const Value a = ctx.pop();
                    ctx.push(Value::from_bool(value_equal(a, b)));
                    break;
                }
                case OpCode::NOT_EQUAL: {
                    const Value b = ctx.pop();
                    const Value a = ctx.pop();
                    ctx.push(Value::from_bool(!value_equal(a, b)));
                    break;
                }
                case OpCode::STRICT_EQUAL: {
                    const Value b = ctx.pop();
                    const Value a = ctx.pop();
                    ctx.push(Value::from_bool(value_identical(a, b)));
                    break;
                }
                case OpCode::STRICT_NOT_EQUAL: {
                    const Value b = ctx.pop();
                    const Value a = ctx.pop();
                    ctx.push(Value::from_bool(!value_identical(a, b)));
                    break;
                }
                // 比较(弹 2 压 1;run_binary_numeric 管栈效应,失败直接终止为 Uncaught)
                case OpCode::GREATER:
                    if (auto err = run_binary_numeric<OpCode::GREATER>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                case OpCode::GREATER_EQUAL:
                    if (auto err = run_binary_numeric<OpCode::GREATER_EQUAL>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                case OpCode::LESS:
                    if (auto err = run_binary_numeric<OpCode::LESS>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                case OpCode::LESS_EQUAL:
                    if (auto err = run_binary_numeric<OpCode::LESS_EQUAL>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                // 算术(弹 2 压 1;同上)
                case OpCode::ADD:
                    if (auto err = run_binary_numeric<OpCode::ADD>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                case OpCode::SUBTRACT:
                    if (auto err = run_binary_numeric<OpCode::SUBTRACT>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                case OpCode::MULTIPLY:
                    if (auto err = run_binary_numeric<OpCode::MULTIPLY>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                case OpCode::DIVIDE:
                    if (auto err = run_binary_numeric<OpCode::DIVIDE>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                case OpCode::MOD:
                    if (auto err = run_binary_numeric<OpCode::MOD>(ctx)) {
                        return runtime_err(std::move(*err));
                    }
                    break;
                // 一元
                case OpCode::NOT:
                    ctx.push(Value::from_bool(!is_truthy(ctx.pop())));
                    break;
                case OpCode::NEGATE: {
                    if (const Value v = ctx.pop(); v.is_int()) {
                        ctx.push(Value::from_int(-v.as_int()));
                    } else if (v.is_f64()) {
                        ctx.push(Value::from_f64(-v.as_f64()));
                    } else {
                        return runtime_err(ErrorCode::InvalidOperand, "negate requires a number, got {}", type_name(v));
                    }
                    break;
                }

                // ---- 栈操作 ----
                case OpCode::POP:
                    ctx.drop(1);
                    break;
                case OpCode::POP_N: {
                    const u8 n = read_u8(frame);
                    ctx.drop(n);
                    break;
                }
                case OpCode::DUP:
                    ctx.push(ctx.peek(0));
                    break;
                case OpCode::DUP2: {
                    const Value b = ctx.peek(0);
                    const Value a = ctx.peek(1);
                    ctx.push(a);
                    ctx.push(b);
                    break;
                }

                // ---- 输出与调试 ----
                case OpCode::PRINT:
                    io::println("{}", format_value(ctx.pop()));
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
                    if (is_truthy(ctx.pop())) {
                        frame.ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_TRUE_OR_POP: {
                    const auto off = read_u16(frame);
                    if (is_truthy(ctx.peek(0))) {
                        frame.ip += off; // 命中:不弹,被测值即结果
                    } else {
                        ctx.drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_FALSE: {
                    const auto off = read_u16(frame);
                    if (!is_truthy(ctx.pop())) {
                        frame.ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_FALSE_OR_POP: {
                    const auto off = read_u16(frame);
                    if (!is_truthy(ctx.peek(0))) {
                        frame.ip += off; // 命中:不弹,被测值即结果
                    } else {
                        ctx.drop(1); // 落空:弹掉
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
                    ASSERT(ctx.stack_size() >= static_cast<usize>(argc) + 1, "CALL on malformed stack");
                    const Value callee = ctx.peek(argc);
                    if (!call_value(ctx, callee, argc)) {
                        return runtime_err(std::move(*ctx.take_error()));
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
                    //   命中(任意态):复用模块对象 -- Loaded 即完整、Loading 即循环导入命中,按文法
                    //     「允许循环导入,命中正在初始化的模块返回半初始化对象」直接用(不报错);压栈即可。
                    //   未命中(文件解析命中但模块未入表):调 load_module(读盘 -> 编译 -> 入表 Loading)得
                    //     模块,以其 entry(<module>)作一次**普通函数调用**进帧后 break --
                    //     模块体 run-once 即执行一个函数,由主循环照常驱动;其 RETURN 按函数名 == <module>
                    //     判定模块体帧,弹弃返回值、置该模块 Loaded、改压模块对象,等价「模块体返回模块」,
                    //     故命中/未命中两分支栈效应统一为 [..., module],绑定交后续 DEF_GLOBAL / 值填槽。
                    //     无递归 run_()。
                    //
                    // 根安全(GC 已启用):path 经常量池根(同 LOAD_CONST)。key 经 new_string intern 驻留
                    //   (weak root,不保命),跨 modules_.find(无 GC)与未命中分支内 load_module 的
                    //   modules_.upsert(rehash 触 GC)须守 -- 下方 key_guard 跨全程根化 key。
                    //   命中分支取回/加载层产出的 module 与 entry 经 modules_ 根可达,非移动 GC 故 ctx.push(栈
                    //   溢出增长走 Buffer 重分配)期间指针稳定,无需守卫;入栈后另经值栈根可达。
                    ObjString* path = read_name(frame);
                    // 当前模块的绝对文件路径(ObjModule::abs_path = root_ + "/" + name_ + ".aria"),
                    // 供 resolve_module 相对分支取 dirname 作基:dirname(abs_path) = root_ + "/" +
                    // dirname(name_) = 当前模块所在目录(.aria 后缀在末段,dirname 不受影响)。
                    // 裸名分支不依赖此值。root_ 指针恒非空但内容可空(cwd 不可用时空串兜底):
                    // 此时 abs_path 返空串,resolve_module 相对分支判空直接返 nullopt(line 81)。
                    const auto key_str = resolve_module(path->view(), frame.module->abs_path(), source_roots_);
                    if (!key_str) {
                        return runtime_err(ErrorCode::ModuleNotFound, "module not found: '{}'", path->view());
                    }
                    auto key       = new_string(gc_, *key_str);
                    auto key_guard = gc_.make_guard(key); // 跨 find / load_module 内 upsert(rehash 触 GC)
                    if (const auto module_entry = modules_.find(Value::from_obj(key)); module_entry != nullptr) {
                        // 命中(Loading 半初始化 / Loaded 完整)复用:压模块值,绑定交后续 DEF_GLOBAL / 值填槽。
                        ctx.push(module_entry->value);
                        break;
                    }
                    // 未命中:加载链路 -- 读盘 -> 编译 -> 入表 Loading(见 load_module),不在此执行模块体。
                    auto loaded = load_module(key, path->view());
                    if (!loaded) {
                        return runtime_err(loaded.error());
                    }
                    // 模块体 run-once = 一次普通 0 参函数调用:压 callee(entry,<module>)-> 进帧 -> break。
                    //   主循环照常执行该帧;其 RETURN 按函数名 == <module> 判定模块体帧,弹弃返回值、置
                    //   Loaded、压回该模块对象,栈效应同命中分支 [..., module]。entry 经 module->entry_ 根可达。
                    ObjFunction* entry = (*loaded)->entry();
                    ctx.push(Value::from_obj(entry)); // callee 压栈
                    if (!call_value(ctx, Value::from_obj(entry), 0)) {
                        // 进帧失败(栈溢出等):帧未进,callee 仍在栈顶,弹掉。
                        ctx.drop(1);
                        return runtime_err(std::move(*ctx.take_error()));
                    }
                    break; // 帧已切换,frame 引用作废,循环顶重新取
                }

                // ---- 异常 ----
                case OpCode::THROW:
                    not_implemented("THROW");

                // ---- 返回(exit_frame 后 frame 引用作废,故先取返回值与判模块体帧)----
                case OpCode::RETURN: {
                    const Value ret = ctx.pop(); // 取返回值(exit_frame 将丢弃其下方栈区)
                    // 模块体 run-once 帧 = IMPORT 加载层进帧的入口函数,其名固定为 <module>
                    // (见 kModuleName);主入口 <main> 与普通用户函数名均不含 '<>',故按函数名判定。
                    // 其 RETURN 弹弃模块体返回值(无意义),改压该模块对象 -- 模块体「返回模块」,
                    // 使 IMPORT 的栈效应在命中/未命中两分支统一为 [..., module](绑定交 DEF_GLOBAL)。
                    // 先取 module 再 exit_frame:exit_frame 后 frame 引用悬垂。模块体帧必非顶层(IMPORT
                    // 帧在下),故 frames.empty() 分支不会命中模块体帧。
                    auto mod     = frame.module;
                    auto fn_name = frame.function->name()->view();
                    ctx.exit_frame(); // 弹帧 + 值栈顶复位到 slots,一体
                    if (frames.empty()) {
                        return ret; // 顶层(主入口 <main>)返回:返回值为程序结果
                    }
                    if (fn_name == kModuleName) {
                        // 模块体 run-once 完成:置 Loaded,压模块对象(弹弃 ret)。
                        mod->set_state(ObjModule::ModuleState::Loaded);
                        ctx.push(Value::from_obj(mod));
                    } else {
                        ctx.push(ret); // 普通函数:返回值压入调用者栈顶
                    }
                    break;
                }

                default:
                    UNREACHABLE();
            }
        }
    }

} // namespace aria
