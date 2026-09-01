#include "runtime/AriaVM.hpp"

#include <cmath>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

#include "bytecode/CodeUnit.hpp"
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
        // 转发已构造 Error(如 run_binary_numeric/call_value 失败回传)的场合用下方
        // runtime_err(Error) 重载,同样收口 std::unexpected。耦合 Result<Value, Error>,
        // 故仅限本翻译单元(其它阶段返回类型不同,不复用)。
        template<typename... Args>
        Result<Value, Error> runtime_err(ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
            return std::unexpected(errorf(code, fmt, std::forward<Args>(args)...));
        }

        // 转发已构造 Error 为失败结果:把 std::unexpected(std::move(err)) 样板收口。供 run_
        // 转发 run_binary_numeric/call_value 失败回传的 Error 用,与上面的格式化构造重载成对。
        Result<Value, Error> runtime_err(Error err) { return std::unexpected(std::move(err)); }

        // 数值二元运算(算术 + 比较)。双方皆 Int 走整数路径;任一为 F64 则升 F64
        // (浮点除零走 IEEE 的 inf/nan,不报错)。M1 暂定语义:
        //   - Int / Int 截断整除、% 为 C++ 语义(除/模零报运行时错误);
        //   - Int 溢出 i48 不设防(断言把关,编译期语义阶段再议)。
        // 见 bytecode-instruction-set.md §9 待决 #8。Op 为模板参数,编译期按指令分派。
        template<OpCode Op>
        Result<Value, Error> numeric_op(const Value a, const Value b) {
            if (!(is_num(a) && is_num(b))) {
                return runtime_err(ErrorCode::TypeMismatch, "operator '{}' requires numbers, got {} and {}",
                                   op_symbol(Op), a.type_name(), b.type_name());
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
        // 编译并执行，按结果类别映射。失败时 source 仍存活（调用方 interpret/interpret_from_path 的局部），
        // 故 Error.format() 取 src_->path() 不悬垂；渲染到 stderr 后只回类别，不回 Error。
        auto result = run(source, module);
        if (result.has_value()) {
            return InterpretResult::Ok;
        }
        io::println(stderr, "{}", result.error().format());
        switch (category_of(result.error().code())) {
            case ErrorCategory::Syntax:
            case ErrorCategory::Semantic:
                return InterpretResult::CompileError;
            default: // Runtime / Internal / Resource -> 运行期
                return InterpretResult::RuntimeError;
        }
    }

    InterpretResult AriaVM::interpret(const StringView src) {
        // 合成入口模块 <script>（root=cwd，2 参 new_module）。
        // 与 new_object，故 make_guard 根化；module 一并入根跨编译+执行（编译期 CodeGen::compile 亦自守）。
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

        auto name_str = new_string(gc_, name_s);
        auto guard    = gc_.make_guard(name_str);
        auto root_str = new_string(gc_, root_s);
        guard.push(root_str);
        auto module = new_module(gc_, name_str, root_str); // 3 参：显式 root
        guard.push(module);

        return interpret_run(source, *module);
    }

    Result<Value, Error> AriaVM::run(ObjFunction* fn) {
        // GC 已启用:值栈/帧经 vm_roots tracer 标根(见 ctor),IMPORT/DEF_GLOBAL 等已按「栈即根」
        // 前置编写(peek-not-pop)。
        // 源根:入口槽 [0] 原地替换为入口模块 root_(对齐 Python sys.path[0] -- 入口源根居首,
        // 配置根 stdlib / -L / 环境变量在 [1..] 不动)。直接赋值 [0],无 flag、无重建、reuse 安全
        // (覆盖旧值,不累积)。root_ 指针恒非空(构造期 ASSERT),内容可空(<script> 在 cwd 不可用
        // 时退化为空串,磁盘模块 = 命中源根):空串由 resolve_module 裸名分支跳空根(line 87)处理,
        // 故此处不解空。
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

    Opt<Error> AriaVM::call_value(Movement& ctx, const Value callee, const u8 argc) {
        if (!callee.is_obj()) {
            return errorf(ErrorCode::CallNonCallable, "call non-callable {}", callee.type_name());
        }
        Object* obj = callee.as_obj();

        switch (obj->type()) {
            case ObjType::FUNCTION:
                return call_function(ctx, Object::as<ObjFunction>(obj), argc);
            case ObjType::NATIVE_FN:
                return call_native(ctx, Object::as<ObjNativeFn>(obj), argc);
            default:
                return errorf(ErrorCode::CallNonCallable,
                              "call non-callable {} (M1 supports functions / native functions only)",
                              to_string(obj->type()));
        }
    }

    Opt<Error> AriaVM::call_function(Movement& ctx, ObjFunction* obj, const u8 argc) {
        if (obj->arity() != argc) {
            return errorf(ErrorCode::WrongArity, "function expects {} args, got {}", obj->arity(), argc);
        }
        if (ctx.frames_full()) {
            return Error{ErrorCode::StackOverflow, "call frame stack overflow"};
        }
        // 栈形 [callee, a1..aN]:enter_frame 进帧(slots 指向槽 0,参数即局部槽 1..argc),
        // 与 run_ 的 exit_frame 对称。经传入的 ctx 而非 main_ctx_ -- M6 协程期 ctx 即当前协程上下文。
        ctx.enter_frame(obj, argc);
        return std::nullopt;
    }

    Opt<Error> AriaVM::call_native(Movement& ctx, const ObjNativeFn* obj, const u8 argc) {
        // 原生函数同步调用,不进帧:bool 为成败信号,返回值写槽 0,错误载荷走侧信道寄存器。
        // 调用区 [callee, a1..aN] 的可写视图:slots[0]=槽 0(返回值),slots[1..argc]=实参。
        // peek(argc) 即槽 0,叶子调用不增长值栈故指针稳定;argc==0 时 span 仅含槽 0。
        const auto slots = Span<Value>{&ctx.peek(argc), static_cast<usize>(argc + 1)};
        // 进场前寄存器应空(上次错误已被 take_error 取走 / reset 清空)。
        ASSERT(!ctx.has_error(), "call_native: pending error not cleared before native call");
        if (obj->fn()(*this, slots)) {
            // 成功:断言无载荷,清寄存器防残留泄漏,drop 实参使返回值升栈顶(等价 drop(argc+1)+push)。
            ASSERT(!ctx.has_error(), "native fn returned true but raised error");
            ctx.clear_error();
            ctx.drop(argc);
            return std::nullopt;
        }
        // 失败:断言已 raise 载荷,取出沿 runtime_err 路径传播。
        ASSERT(ctx.has_error(), "native fn returned false but raised no error");
        return *ctx.take_error();
    }

    Result<Value, Error> AriaVM::run_() {
        auto& ctx    = main_ctx_;
        auto& frames = ctx.frames();

        while (true) {
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
                        return runtime_err(ErrorCode::InvalidOperand, "negate requires a number, got {}",
                                           v.type_name());
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
                    if (auto err = call_value(ctx, callee, argc)) {
                        return runtime_err(std::move(*err));
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
                    //   解析按 .claude/reference/runtime/import-path-resolution.md「加载层设计基线」:
                    //   把 import 串经 resolve_module 解析为命中文件的绝对规范路径(weakly_canonical)作
                    //   模块表键,再查 modules_。
                    //   相对(./ ../)基 = 当前模块所在目录(单基,caller-local,不碰 source_roots);
                    //   裸名基 = source_roots(逐个 exists-check,首个 <base>/<spec>.aria 存在者命中)。
                    //   键 = 绝对规范路径,跨根不碰撞、相对不逃逸;末尾 .aria 可选(lib/math ≡ lib/math.aria)。
                    //   命中(任意态):复用模块对象 -- Loaded 即完整、Loading 即循环导入命中,按文法
                    //     「允许循环导入,命中正在初始化的模块返回半初始化对象」直接用(不报错)。
                    //   解析失败(无源根命中)/ 模块表未命中:按设计需嵌入层加载文件 -> 词法/语法/编译 ->
                    //     入表 run-once(见 bytecode-instruction-set.md §4.15)。该链路(磁盘加载 +
                    //     AST->CodeUnit 编译器 + VM 内嵌套执行模块体)尚未就绪,故暂打印未实现提示并报
                    //     ModuleNotFound(表里预注册的模块仍可被命中复用,故解析/命中路径可测)。
                    //
                    // 根安全(M6 解锁 GC 后):path 经常量池根(同 LOAD_CONST)。键经 new_string intern
                    //   驻留(weak root)。module 取自 modules_（VM tracer 标 modules_），经 modules_ 根可达,
                    //   非移动 GC 故 ctx.push（栈溢出增长走 Buffer 重分配）期间指针稳定,无需守卫;入栈后
                    //   另经值栈根可达。
                    ObjString* path = read_name(frame);
                    // 当前模块的绝对文件路径(ObjModule::abs_path = root_ + "/" + name_ + ".aria"),
                    // 供 resolve_module 相对分支取 dirname 作基:dirname(abs_path) = root_ + "/" +
                    // dirname(name_) = 当前模块所在目录(.aria 后缀在末段,dirname 不受影响)。
                    // 裸名分支不依赖此值。root_ 指针恒非空但内容可空(cwd 不可用时空串兜底):
                    // 此时 abs_path 返空串,resolve_module 相对分支判空直接返 nullopt(line 81)。
                    const auto key_str = resolve_module(path->view(), frame.module->abs_path(), source_roots_);
                    if (!key_str) {
                        return runtime_err(ErrorCode::ModuleNotFound,
                                           "module not found: '{}' (no matching source root)", path->view());
                    }
                    auto  key    = new_string(gc_, *std::move(key_str));
                    Value module = Value::nil_val();
                    if (const auto module_entry = modules_.find(Value::from_obj(key)); module_entry != nullptr) {
                        module = module_entry->value;
                    } else {
                        // 文件存在但模块未入表:加载链路(磁盘加载 + AST->CodeUnit 编译器 + VM 内嵌套
                        //   执行模块体)尚未就绪,故暂只打印未实现提示并报 ModuleNotFound(表里预注册的
                        //   模块仍可被命中复用,故解析/命中路径可测)。
                        io::print(stderr, "[aria] module loading not implemented yet: '{}'\n", path->view());
                        return runtime_err(ErrorCode::ModuleNotFound,
                                           "module not loaded: '{}' (loading not implemented yet)", path->view());
                    }
                    ctx.push(module); // 压模块值于栈顶,绑定交 DEF_GLOBAL / 值填槽
                    break;
                }

                // ---- 异常 ----
                case OpCode::THROW:
                    not_implemented("THROW");

                // ---- 返回(exit_frame 后 frame 引用作废,故先取返回值)----
                case OpCode::RETURN: {
                    const Value ret = ctx.pop(); // 取返回值(exit_frame 将丢弃其下方栈区)
                    ctx.exit_frame();            // 弹帧 + 值栈顶复位到 slots,一体
                    if (frames.empty()) {
                        return ret;
                    }
                    ctx.push(ret); // 返回值压入调用者栈顶
                    break;
                }

                default:
                    UNREACHABLE();
            }
        }
    }

} // namespace aria
