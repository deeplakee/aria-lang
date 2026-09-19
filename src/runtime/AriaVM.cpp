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
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "runtime/Builtins.hpp"
#include "runtime/IteratorMethods.hpp"
#include "runtime/ListMethods.hpp"
#include "runtime/MapMethods.hpp"
#include "runtime/StringMethods.hpp"
#include "util/fs.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {
    namespace stdfs = std::filesystem;

    namespace {

        // 寄存器组初值:全表灌 nil。Value{} 零填充并非 nil(NaN-boxing 下是 f64 0.0,见
        // NanBoxing.hpp 注),未填格须是合法 Value 才能被 tracer 与 dispatch 安全触碰,故构造
        // 期经本工厂在初始化列表一步到位;各批 bootstrap 逐格覆写。
        Vector<Value, kValueRegisterCount> make_nil_registers() noexcept {
            Vector<Value, kValueRegisterCount> regs{};
            for (auto& r: regs) {
                r = Value::nil_val();
            }
            return regs;
        }

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

        // 把寄存器取出的载荷拆为 (码, 完整烘焙消息) 两件:ObjException 直取
        // 自身码与 message_(已是完整烘焙串,与 from_detail 直构文案逐字一致,re-throw 保码,
        // 坑 #7);其它载荷(用户 throw 的非异常值)兜底 UncaughtException,消息渲染值本身(经
        // 烘焙单点 make_message,与 from_detail 同源同串)。消费方:unwind 未捕获出口(拼好
        // 跟踪)与 run_closure 入口进帧失败(一帧未进,无跟踪),均经 Error::from_baked 一次
        // 物化成边界 Error,不中转 Error 对象(Error 只在边界成型)。
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
            const auto  ip_off = static_cast<u32>(frame.ip - frame.unit->code.data());
            const auto  instr  = Disassembler::disassembleInstruction(frame.unit, ip_off);

            io::print(stderr, "[trace] {}  {} @{:04X}  {}\n", frame.module->to_string(),
                      frame.closure->function()->to_string(), ip_off, instr);

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

    // 构造:成员初始化(registers_ 经 make_nil_registers 全表灌 nil)-> 注册 VM 根 tracer ->
    // bootstrap 寄存器组(Object 根类入格,先入根集)-> 注册 builtins。gc_ 值成员居声明首,
    // 逆序析构下 tracer 与成员同生共死。
    AriaVM::AriaVM() :
        gc_{}, main_ctx_{&gc_}, current_{&main_ctx_}, modules_{&gc_}, builtins_{&gc_}, source_roots_{},
        registers_{make_nil_registers()} {
        hook_vm_roots();
        init_source_roots();
        {
            // 构造临界区:GC 挂起,窗口内回收不可达(bytes_allocated_ 自零起步、bootstrap 总
            // 分配远小于 kInitialGcThreshold,且锁兜底 -- 将来 bootstrap 变重亦不破),窗口内
            // 创建的白对象免逐个守卫;**解锁前须全部发布进 tracer 可达的家**(registers_ /
            // builtins_,tracer 已挂接)。
            const auto lock = gc_.make_lock();
            bootstrap_registers();
            builtins::register_builtins(gc_, builtins_);
        }
    }

    void AriaVM::bootstrap_registers() {
        // 值寄存器组 bootstrap 编排:逐格初始化全部 VM 单例对象。须在 ctor 构造临界区(GC
        // 挂起)内调用,创建免守卫;各 bootstrap_<单例> 建成即发布进寄存器/tracer 可达之家。
        // 新单例随其批次在此加一行。
        bootstrap_object_class();
        bootstrap_list_class();
        bootstrap_iterator_class();
        bootstrap_map_class();
        bootstrap_string_class();
        bootstrap_default_mark();
        bootstrap_match_no_arm();
    }

    void AriaVM::hook_vm_roots() {
        // VM 根 tracer:collect 时标四类根。①modules_ / ②builtins_(表内容);③registers_
        // (值寄存器组,一趟循环逐格 mark_value,未填格 nil 对非对象 no-op);④current_ 执行
        // 链沿 previous_ 逐个标 -- 值栈(run() 期局部/实参/临时只活在栈上,是最关键的缺失
        // 根)、各帧 closure/module、挂起错误寄存器、open upvalue 开链(闭包已死而 upvalue
        // 仍在链的悬垂防线)。链尾断言恒 &main_ctx_,锁定「resume/yield 严格成对」切换纪律。
        gc_.set_vm_roots([this](GC& g) {
            modules_.trace(g);
            builtins_.trace(g);
            for (const auto& reg: registers_) {
                g.mark_value(reg);
            }
            [[maybe_unused]] Movement* tail = nullptr;
            for (auto m = current_; m != nullptr; m = m->previous()) {
                for (auto p = m->stack_base(); p < m->stack_top(); ++p) {
                    g.mark_value(*p); // mark_value 对非对象 Value no-op,栈槽含 int/f64/bool/nil 安全
                }
                for (const auto& frame: m->frames().span()) {
                    g.mark_object(frame.closure); // trace 级联标 function/upvalues;容 nullptr
                    g.mark_object(frame.module);
                }
                // 开链节点可能仅被本链引用(闭包已死),须单独标根(mark 幂等,双标无害)。
                for (auto upvalue = m->open_upvalues(); upvalue != nullptr; upvalue = upvalue->next_open()) {
                    g.mark_object(upvalue);
                }
                if (const auto& pending = m->pending_error()) {
                    g.mark_value(*pending);
                }
                tail = m;
            }
            ASSERT(tail == &main_ctx_, "VM roots: context chain must terminate at main_ctx_");
        });
    }

    ObjClass* AriaVM::object_class() const noexcept {
        return Object::as<ObjClass>(registers_[kObjectClassOffset].as_obj());
    }

    ObjClass* AriaVM::list_class() const noexcept {
        return Object::as<ObjClass>(registers_[kListClassOffset].as_obj());
    }

    ObjClass* AriaVM::iterator_class() const noexcept {
        return Object::as<ObjClass>(registers_[kIteratorClassOffset].as_obj());
    }

    ObjClass* AriaVM::map_class() const noexcept { return Object::as<ObjClass>(registers_[kMapClassOffset].as_obj()); }

    ObjClass* AriaVM::string_class() const noexcept {
        return Object::as<ObjClass>(registers_[kStringClassOffset].as_obj());
    }

    void AriaVM::bootstrap_object_class() {
        // Object 根类 bootstrap:ObjClass("Object", super=nullptr) + 原生 no-op init(不合成
        // ObjFunction,保「module 恒非空」不变式;收到 slots[0]=this 返回 true 不写槽,槽 0
        // 原样即返回实例)。set_field 命中 "init" 同步 init_;"init" 键经 intern 命中
        // init_native 的 name 串,零分配。须在 ctor 构造临界区内调用,创建免守卫。
        const auto klass       = new_class(gc_, "Object", nullptr);
        const auto init_key    = new_string(gc_, "init");
        const auto init_native = new_native_fn(gc_, "init", [](AriaVM&, Span<Value>) { return true; });
        klass->set_field(init_key, Value::from_obj(init_native));
        registers_[kObjectClassOffset] = Value::from_obj(klass); // 入寄存器组:此后经 tracer 保命
    }

    void AriaVM::bootstrap_list_class() {
        // List bootstrap 类:内置 list 的语言方法面载体,经 ObjList::load_field 查表命中后恒绑定触达;
        // 不注册 builtins/模块 globals(用户不可直接取到类对象)。super 挂 Object 根(计划 D1),
        // 类名与 type() 的类型名一致。须在 ctor 构造临界区内调用,创建免守卫;入格即经 tracer
        // 的 registers_ 一趟循环标根(tracer 零改动)。
        const auto klass = new_class(gc_, "List", object_class());
        register_list_methods(gc_, klass);
        registers_[kListClassOffset] = Value::from_obj(klass); // 入寄存器组:此后经 tracer 保命
    }

    void AriaVM::bootstrap_iterator_class() {
        // Iterator bootstrap 类:迭代器的语言方法面载体(has_next/next,方法体是 ObjIterator
        // 引擎缝虚函数的薄壳,住 runtime/IteratorMethods),经 ObjIterator::load_field 查表
        // 命中后恒绑定触达;不注册 builtins/模块 globals。super 挂 Object 根,类名与 type()
        // 的类型名一致。须在 ctor 构造临界区内调用,创建免守卫;入格即经 tracer 标根。
        const auto klass = new_class(gc_, "Iterator", object_class());
        register_iterator_methods(gc_, klass);
        registers_[kIteratorClassOffset] = Value::from_obj(klass); // 入寄存器组:此后经 tracer 保命
    }

    void AriaVM::bootstrap_map_class() {
        // Map bootstrap 类:内置 map 的语言方法面载体,经 ObjMap::load_field 查表命中后恒绑定
        // 触达;不注册 builtins/模块 globals(用户不可直接取到类对象)。super 挂 Object 根
        //(计划 D1),类名与 type() 的类型名一致。须在 ctor 构造临界区内调用,创建免守卫;
        // 入格即经 tracer 的 registers_ 一趟循环标根(tracer 零改动)。
        const auto klass = new_class(gc_, "Map", object_class());
        register_map_methods(gc_, klass);
        registers_[kMapClassOffset] = Value::from_obj(klass); // 入寄存器组:此后经 tracer 保命
    }

    void AriaVM::bootstrap_string_class() {
        // String bootstrap 类:内置 string 的语言方法面载体,经 ObjString::load_field 查表命中后
        // 恒绑定触达;不注册 builtins/模块 globals(用户不可直接取到类对象)。super 挂 Object 根
        //(计划 D1),类名与 type() 的类型名一致。须在 ctor 构造临界区内调用,创建免守卫;
        // 入格即经 tracer 的 registers_ 一趟循环标根(tracer 零改动)。
        const auto klass = new_class(gc_, "String", object_class());
        register_string_methods(gc_, klass);
        registers_[kStringClassOffset] = Value::from_obj(klass); // 入寄存器组:此后经 tracer 保命
    }

    void AriaVM::bootstrap_default_mark() {
        // 缺参印章:私有 no-op native(语义即空操作:返回 true 不写返回槽;正常路径永不被
        // 调用)。身份判等的未传槽标记,不注册进 builtins/模块表,语言不可达 -- 实参显式传
        // 任意函数值,身份均异于印章,不误判未传。须在 ctor 构造临界区内调用,创建免守卫。
        const auto default_mark        = new_native_fn(gc_, "<default>", [](AriaVM&, Span<Value>) { return true; });
        registers_[kDefaultMarkOffset] = Value::from_obj(default_mark);
    }

    void AriaVM::bootstrap_match_no_arm() {
        // match 兜底异常:全臂未命中时 LOAD_REG + THROW 抛出的共享单例(消息静态、无 subject
        // 插值;不注册 builtins 用户不可达,catch 绑到的即本对象)。消息按 raise 同源形态烘焙。
        // 须在 ctor 构造临界区内调用,创建免守卫。
        const auto msg                = Error::make_message(ErrorCode::MatchNoArm, "no arm matched");
        const auto no_arm             = new_exception(gc_, ErrorCode::MatchNoArm, msg);
        registers_[kMatchNoArmOffset] = Value::from_obj(no_arm);
    }

    void AriaVM::init_source_roots() {
        // 入口槽 [0] 占位 cwd(REPL / 未显式设 dir_ 时裸名搜 cwd);cwd 不可用时空串兜底
        //   (resolve_module 裸名分支跳过空根)。
        source_roots_.push_back(fs::current_dir().value_or(""));
        // 配置根 [1..] = 编译器相对 stdlib 源根(kStdlibRelPath),weakly_canonical 规范化,
        //   推导失败跳过;可经 set_source_roots 覆盖。
        if (const auto pd = fs::program_dir()) {
            const auto      stdlib = stdfs::path{*pd} / kStdlibRelPath;
            std::error_code ec;
            if (const auto real = stdfs::weakly_canonical(stdlib, ec); !ec && !real.empty()) {
                source_roots_.push_back(real.string());
            }
        }
    }

    void AriaVM::set_source_roots(List<String> roots) noexcept {
        source_roots_.resize(1);
        for (auto& root: roots) {
            source_roots_.push_back(std::move(root));
        }
    }

    Result<Value, Error> AriaVM::run(SourceFile& source, ObjModule* module) {
        auto compiled = Compiler::compile(gc_, source, module, kMainEntryName);
        if (!compiled) {
            return std::unexpected(std::move(compiled).error());
        }
        return run(*compiled);
    }

    InterpretResult AriaVM::interpret_run(SourceFile& source, ObjModule* module) {
        auto compiled = Compiler::compile(gc_, source, module, kMainEntryName);
        if (!compiled) {
            io::println(stderr, "{}", compiled.error().message());
            return InterpretResult::CompileError;
        }
        auto result = run(*compiled);
        if (!result) {
            io::println(stderr, "{}", result.error().message());
            return InterpretResult::RuntimeError;
        }
        return InterpretResult::Ok;
    }

    InterpretResult AriaVM::interpret_from_src(const StringView src) {
        // 合成入口模块 <script>(dir=cwd);guard 根化跨编译+执行。
        const auto module = new_module(gc_, kScriptModuleName);
        auto       guard  = gc_.make_guard(module);

        // 方法内局部,存活至返回,Error 渲染不悬垂。
        SourceFile source{String{kScriptModuleName}, String{kScriptModuleName}, String{src}};
        return interpret_run(source, module);
    }

    InterpretResult AriaVM::interpret_from_path(const StringView path) {
        // 读盘 + BOM/CRLF/UTF-8 处理;失败渲染路径返 LoadError(无 SourceLoc)。
        auto loaded = SourceFile::from_path(path);
        if (!loaded) {
            io::println(stderr, "无法加载源文件 '{}'", path);
            return InterpretResult::LoadError;
        }
        SourceFile source = std::move(*loaded);

        // 入口模块身份:name = basename 去 .aria、dir = dirname(absolute(path))(收口于
        // fs::module_name_and_dir);name 为空(目录/空/无文件名)与读盘失败同属加载失败。
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
        ASSERT(current_ == &main_ctx_, "AriaVM::run: current_ is not main_ctx_ (unbalanced context switch)");
        // 入口槽 [0] 原地替换为入口模块 dir_(对齐 Python sys.path[0],配置根 [1..] 不动);
        // dir_ 可空(cwd 不可用的 <script>),由 resolve_module 跳空根处理,此处不判空。
        source_roots_[0] = fn->module()->dir()->view();

        // 重复调用先清场:HALT 收场的上一轮不弹帧,不清场会把新帧叠在陈旧帧上。
        main_ctx_.reset();

        // 包空闭包:fn 跨 new_closure 顶 maybe_collect 须有根,make_guard 兜底;建成传入
        // run_closure 即压栈(push 无 GC 点,入栈即根化)。
        auto       guard   = gc_.make_guard(fn);
        const auto closure = new_closure(gc_, fn);
        auto       result  = run_closure(closure); // 值拷贝,下方清场不影响返回值;持对象由调用方根化
        // 结束再清场:防 run() 外的 GC 经 tracer 标到陈旧栈值(清场归本入口,run_closure 为
        // 重入接缝不自清)。
        main_ctx_.reset();
        return result;
    }

    Result<Value, Error> AriaVM::run_closure(ObjClosure* closure) {
        current_->push(Value::from_obj(closure)); // 入栈即根
        if (!call_closure(closure, 0)) {
            // 进帧失败(重入路径帧满为真实分支):载荷直转 Result -- 不经 unwind(帧栈叠着
            // 调用者的帧,弹不得),一帧未进亦无跟踪可烘。
            const auto [code, msg] = uncaught_error_parts(*current_->take_error());
            return runtime_err(Error::from_baked(code, msg));
        }
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
                return obj->op_call(*this, Span<Value>{&current_->peek(argc), static_cast<usize>(argc + 1)});
        }
    }

    bool AriaVM::call_class(ObjClass* obj, const u8 argc) {
        // 类实例化:new_instance 是唯一 GC 点,建成即写 callee 槽(槽 0 原位换实例 = 新帧
        // this),余下交 call_value 通用分发。init 恒有值:闭包进方法帧(尾部 LOAD_LOCAL 0;
        // RETURN 返回 this)、原生同步调用(no-op 不动 slots[0] 即返回实例)、非可调用值
        // (类上赋值放行)由 call_value 报 CallNonCallable 兜底。
        const auto instance  = new_instance(gc_, obj);
        current_->peek(argc) = Value::from_obj(instance); // 建成即写槽:instance 经值栈根化(即新帧 this)
        return call_value(obj->init(), argc);
    }

    bool AriaVM::call_bound_method(const ObjBoundMethod* obj, const u8 argc) {
        // 绑定方法调用:槽 0 原位覆写为 receiver(this 替代 callee,实参槽位不动),余下交
        // call_value 分发。方法值无需守卫:覆写后闭包经 frame.closure 由帧 tracer 标根、
        // 原生经类表槽/缓存可达。
        current_->peek(argc) = obj->receiver(); // 槽 0:bound -> this(实参槽位不动)
        return call_value(obj->method(), argc);
    }

    bool AriaVM::check_arity(const ObjFunction* fn, const u8 argc) {
        // 契约与文案见 AriaVM.hpp check_arity 注。检查纯读,无分配。
        const auto arity     = fn->arity();
        const auto min_arity = fn->min_arity();
        if (fn->is_varargs()) {
            if (argc < min_arity) {
                return fail(ErrorCode::WrongArity, "expects at least {} args, got {}", min_arity, argc);
            }
            return true;
        }
        if (argc < min_arity || argc > arity) {
            if (min_arity == arity) {
                return fail(ErrorCode::WrongArity, "expects {} args, got {}", arity, argc);
            }
            return fail(ErrorCode::WrongArity, "expects {} to {} args, got {}", min_arity, arity, argc);
        }
        return true;
    }

    u8 AriaVM::prepare_call_args(const ObjFunction* fn, const u8 argc) {
        // 契约见 AriaVM.hpp prepare_call_args 注。GC 走查:new_list 是唯一分配点 --额外
        // 实参 peek 在栈(「栈即根」),copy_from 走 trivial 分配不触 GC,白色 list 随即
        // drop+push 入值栈根,窗口内无 GC 点(MAKE_LIST case 同构);垫充压寄存器单例无分配。
        const auto arity = fn->arity();

        // ① 缺省垫充(仅当实参不足固定参数数;varargs 的 argc 可超 arity,差值不可作 u8 减)。
        const auto missing = argc < arity ? static_cast<u8>(arity - argc) : u8{0};
        for (u8 i = 0; i < missing; ++i) {
            current_->push(registers_[kDefaultMarkOffset]);
        }

        // ② varargs 打包:rest 槽深 = arity + 1;普通函数槽深 = arity。
        if (fn->is_varargs()) {
            const auto extras = argc > arity ? static_cast<u8>(argc - arity) : u8{0};
            const auto rest   = new_list(gc_);
            rest->elements().copy_from({current_->stack_top() - extras, extras});
            current_->drop(extras);
            current_->push(Value::from_obj(rest));
            return static_cast<u8>(arity + 1);
        }
        return arity;
    }

    bool AriaVM::call_closure(ObjClosure* obj, const u8 argc) {
        // 编排:元数检查 -> 帧余量检查 -> 实参整形 -> 进帧;bool 契约同 call_* 族
        //(false ⟺ 载荷已 raise)。
        const auto fn = obj->function();
        if (!check_arity(fn, argc)) {
            return false;
        }
        if (current_->frames_full()) {
            return fail(ErrorCode::StackOverflow, "call frame stack overflow");
        }
        current_->enter_frame(obj, prepare_call_args(fn, argc));
        return true;
    }

    bool AriaVM::call_native(const ObjNativeFn* obj, const u8 argc) {
        // 原生函数同步调用,不进帧(契约见 ObjNativeFn.hpp)。entered_ctx 是全部事后簿记
        // (drop/寄存器断言)的锚点:M6 前 current_ 恒等于它,断言锁「原生调用不得换走
        // current_」;M6 切换合法化后仅删成功路径守卫(false 路径「禁止 false+切换」为永久
        // 契约,§4.9)。实参留栈到 drop 亦是 GC 红利:切换型原生函数执行全程实参皆调用者栈根。
        const auto entered_ctx = current_;
        const auto slots       = Span<Value>{&current_->peek(argc), static_cast<usize>(argc + 1)};
        // 进场前寄存器应空(上次错误已被 take_error 取走 / reset 清空)。
        ASSERT(!current_->has_error(), "call_native: pending error not cleared before native call");
        if (obj->fn()(*this, slots)) {
            ASSERT(current_ == entered_ctx,
                   "call_native: current_ not restored across native call"); // M6 删:切换合法化
            // drop 实参使返回值升栈顶,一律落在 entered_ctx 上(M6 预铺)。
            ASSERT(!entered_ctx->has_error(), "native fn returned true but raised error");
            entered_ctx->drop(argc);
            return true;
        }
        ASSERT(current_ == entered_ctx, "call_native: current_ not restored across native call"); // M6 留:契约守卫
        // 失败:载荷留寄存器交调用方 take_error(bool 契约)。
        ASSERT(entered_ctx->has_error(), "native fn returned false but raised no error");
        return false;
    }

    ObjModule* AriaVM::load_module(ObjString* canonical_path, const StringView import_specifier) {
        // 契约总览见 AriaVM.hpp;canonical_path 已由调用方根化。加载事实源 = modules_ 表成员
        // 资格:编译成功才入表,循环导入命中表内体执行中的对象即复用;失败不留表项。

        // 1. 读盘:resolve_module 已 exists-check,但读盘/编码仍可能失败(权限竞争/非法 UTF-8)。
        auto loaded_src = SourceFile::from_path(canonical_path->view());
        if (!loaded_src) {
            return fail(ErrorCode::ModuleNotFound, "failed to load module '{}': read/decode error", import_specifier);
        }
        SourceFile source = std::move(*loaded_src);

        // 2. 派生模块身份(name/dir):abs_path() 还原 canonical key,相对导入基正确。
        const auto [name_str, dir_str] = fs::module_name_and_dir(canonical_path->view());
        if (name_str.empty()) {
            return fail(ErrorCode::ModuleNotFound, "module path has no valid name: '{}'", import_specifier);
        }

        // 3. 建模块(工厂内部 intern name/dir 并自守)+ 自守跨编译/入表。
        auto module = new_module(gc_, name_str, dir_str);
        auto guard  = gc_.make_guard(module);

        // 4. 编译(入口名 kModuleEntryName,CodeGen::init_module 已 set_entry)。编译期 Error
        //    就地 new_exception 装箱透传(消息不重烘,位置指向被导入文件内部);module 尚未
        //    入表仅由 guard 根化,source 须存活到 compile() 返回。
        if (auto compiled = Compiler::compile(gc_, source, module, kModuleEntryName); !compiled) {
            current_->raise(Value::from_obj(new_exception(gc_, compiled.error())));
            return nullptr;
        }

        // 5. 入表:入表先于模块体 run-once(体由 IMPORT 分支调起),体执行期间的再导入命中
        //    此表项即复用半初始化对象;canonical_path/module 均已根化,set rehash 触 GC 安全。
        modules_.set(Value::from_obj(canonical_path), Value::from_obj(module));
        return module;
    }

    bool AriaVM::run_import(const ObjString* path) {
        // 契约见 AriaVM.hpp;三类失败点(解析/加载/进帧)一律 return false(载荷已 raise),
        // unwind 留 dispatch_loop 调用点。
        const auto& frame = current_->frames().top(); // 导入方帧:abs_path 作相对解析基;进帧后不再引用
        // 相对分支取当前模块 dirname 作基;dir_ 可空时 abs_path 返空串,相对分支拒绝。
        const auto canonical_path_str = resolve_module(path->view(), frame.module->abs_path(), source_roots_);
        if (!canonical_path_str) {
            return fail(ErrorCode::ModuleNotFound, "module not found: '{}'", path->view());
        }
        const auto canonical_path = new_string(gc_, *canonical_path_str);
        auto       guard          = gc_.make_guard(canonical_path); // 跨 find / load_module 内 set(rehash 触 GC)
        if (const auto module_entry = modules_.find(Value::from_obj(canonical_path)); module_entry != nullptr) {
            current_->push(module_entry->value); // 命中:体执行中即循环导入,复用半初始化对象
            return true;
        }
        const auto module = load_module(canonical_path, path->view());
        if (module == nullptr) {
            return false; // 载荷已在寄存器
        }
        // entry 经 module->entry_ 根可达;闭包建成即压栈根化。
        const auto entry = module->entry();
        ASSERT(entry != nullptr, "load_module 返回非空模块须已 set_entry");
        const auto closure = new_closure(gc_, entry);
        current_->push(Value::from_obj(closure));
        // 进帧失败(栈溢出等):帧未进,callee 仍在栈顶(unwind 截栈时一并丢弃)。
        return call_closure(closure, 0);
    }

    template<OpCode Op>
    bool AriaVM::run_binary_numeric() {
        // 双 Int 走整数路径,任一 F64 升浮点(int 除/模零报错,% 为 C++ 语义,f64 按 IEEE)。
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
        // 非对象(含 nil)统一「does not support field access」文案(协议分派需 Object*);
        // 对象 miss 的文案由协议 override 烘焙。
        const Value obj = current_->peek(0);
        if (!obj.is_obj()) {
            return fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (const auto result = obj.as_obj()->load_field(*this, name)) {
            current_->peek(0) = *result; // 写回原槽:[obj] -> [v]
            return true;
        }
        return false; // 载荷已在寄存器
    }

    bool AriaVM::run_store_field(ObjString* name) {
        // 契约见 AriaVM.hpp;非对象守卫同 run_load_field。
        const Value obj = current_->peek(1);
        if (!obj.is_obj()) {
            return fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (!obj.as_obj()->store_field(*this, name, current_->peek(0))) {
            return false; // 载荷已在寄存器
        }
        current_->peek(1) = current_->peek(0); // 单槽下移:弹 obj 留 v,[obj, v] -> [v]
        current_->drop(1);
        return true;
    }

    bool AriaVM::run_load_index() {
        // 契约见 AriaVM.hpp;非对象守卫同 run_load_field(文案与协议基类默认同串)。
        const Value idx = current_->peek(0);
        const Value obj = current_->peek(1);
        if (!obj.is_obj()) {
            return fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        if (const auto result = obj.as_obj()->load_index(*this, idx)) {
            current_->peek(1) = *result; // 写回 obj 槽再弹 idx:[obj, idx] -> [v]
            current_->drop(1);
            return true;
        }
        return false; // 载荷已在寄存器
    }

    bool AriaVM::run_store_index() {
        // 契约见 AriaVM.hpp;非对象守卫同 run_load_index。
        const Value value = current_->peek(0);
        const Value idx   = current_->peek(1);
        const Value obj   = current_->peek(2);
        if (!obj.is_obj()) {
            return fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        if (!obj.as_obj()->store_index(*this, idx, value)) {
            return false; // 载荷已在寄存器
        }
        current_->peek(2) = value; // 值下移两格:弹 obj、idx 留 v,[obj, idx, v] -> [v]
        current_->drop(2);
        return true;
    }

    bool AriaVM::run_load_super_field(ObjString* name) {
        // 契约见 AriaVM.hpp;miss 时类措辞 fail 已入寄存器,本函数只透传信号。
        const auto& frame    = current_->frames().top();
        const auto  defining = frame.closure->defining_class();
        ASSERT(defining != nullptr, "LOAD_SUPER_FIELD: closure has no defining class (compiler invariant)");
        const auto super = defining->superclass();
        ASSERT(super != nullptr, "LOAD_SUPER_FIELD: method class has no superclass (compiler invariant)");
        const auto hit = super->load_field(*this, name); // 从父类起读穿透(类协议)
        if (!hit) {
            return false;
        }
        if (const auto member = *hit; is_method(member)) {
            // 方法命中:建成立即压栈,this 经帧槽根化;方法对象经 super 链根可达
            current_->push(Value::from_obj(new_bound_method(gc_, member, frame.slots[0])));
        } else {
            current_->push(member); // 静态槽原值直读,不绑定不缓存
        }
        return true;
    }

    Opt<Error> AriaVM::unwind() {
        // 前提:寄存器已有载荷(入口断言把关)。搜索阶段不动帧栈/值栈,命中就地回退派发,
        // 全未命中交 reset 清场。
        struct TraceEntry {
            ObjFunction* fn;     // 帧函数(名字渲染)
            ObjModule*   mod;    // 帧模块(位置串渲染,module_loc)
            u32          ip_off; // 行号经 fn->unit().line_for_offset 查
        };

        ASSERT(current_->has_error(), "unwind: no pending payload");
        List<TraceEntry> trace; // 收集序:内 -> 外;物化时反转为外 -> 内(Python 式 most recent call last)

        // 自最内(栈顶)向外搜索 try 记录;命中帧保留 -- handler 偏移与栈基址都属于它。
        auto& frames = current_->frames();
        for (usize i = frames.size() - 1; i < frames.size(); --i) { // 无符号反向:下溢即终止
            auto& [closure, unit, module, ip, slots, last_ip] = frames[i];
            const u32 ip_off                                  = static_cast<u32>(last_ip - unit->code.data());
            if (const auto rec = unit->find_try_handler(ip_off)) {
                // 命中:回退到命中帧并转入 catch handler(弃内层帧+帧内截到 catch 参数槽+ip
                // 跳+载荷落槽,统一在 Movement::unwind_to_handler)。调用方 break 回循环顶重取
                // 帧(坑 #11)。
                current_->unwind_to_handler(i, **rec);
                return std::nullopt;
            }
            trace.push_back(TraceEntry{closure->function(), module, ip_off});
        }

        // 全帧未命中 -> 未捕获:reset 一次清场(全链开指关闭+清帧+栈复位+清寄存器前的载荷
        // 已先行取走),拆 (码, 消息) 逐帧烘焙跟踪行物化(透传的编译期 Error 不经本路径,无
        // 跟踪)。trace 恒非空(调用点帧栈非空不变式);物化路径仅 std::string 拼接,无 GC
        // 分配点,fn/mod 裸指针不悬垂。
        auto [code, msg] = uncaught_error_parts(*current_->take_error());
        current_->reset();
        for (const auto& [fn, mod, ip_off]: std::views::reverse(trace)) {
            const u32 line = fn->unit().line_for_offset(ip_off);
            msg += std::format("\n  at {} ({})", fn->name()->view(), module_loc(mod, line));
        }
        return Error::from_baked(code, msg);
    }

    Result<Value, Error> AriaVM::dispatch_loop() {
        // 栈/帧/寄存器一律经 current_ 访问。M6 切换模型下「正在执行的字节码所在上下文恒等于
        // current_」:切换只发生在原生函数体内,call_native 探测后循环顶自然采用新上下文,
        // 本循环永不重入(§4.9)。

        while (true) {
            // 不变式:此处帧栈恒非空(唯一弹空帧的顶层 RETURN 立即 return;CALL/IMPORT 切帧后
            // break 回循环顶重取)。取指前记本帧指令起始指针(last_ip):报错行号锚点 --
            // 顶帧报错即故障指令、call_*/原生失败即 CALL 站点;unwind 查表同用此字段。
            CallFrame& frame = current_->frames().top();
            frame.last_ip    = frame.ip;
#ifdef DEBUG_TRACE_EXECUTION
            // 取 opcode 前打印执行状态(见 trace_execution)。
            trace_execution(*current_);
#endif
            // 各 case 按 bytecode/code.hpp 枚举序排列。退出约定:一律 break 回循环顶 --
            // unwind 返 Error 即未捕获(return 终止循环),返 nullopt 即已派发 handler、帧引用
            // 已废。**switch 之后不得新增引用 frame 的代码**(坑 #11 的防御前提)。
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
                case OpCode::LOAD_REG: {
                    // [] -> [regs[n]]:压 VM 值寄存器(单例对象,bootstrap 填充;索引即注册表
                    // 枚举值,编译器只发合法下标,同 LOAD_LOCAL 槽访问不设防)。
                    current_->push(registers_[read_u8(frame)]);
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
                    // 压本闭包第 idx 个 upvalue 的当前值(开/闭两态统一经 value_slot() 取址)。
                    const u8 idx = read_u8(frame);
                    current_->push(*frame.closure->upvalues()[idx]->value_slot());
                    break;
                }
                case OpCode::STORE_UPVALUE: {
                    // peek-store 到该 upvalue:open 态写穿到栈槽,closed 态写自持。
                    const u8 idx                                  = read_u8(frame);
                    *frame.closure->upvalues()[idx]->value_slot() = current_->peek(0);
                    break;
                }
                case OpCode::CLOSE_UPVALUE: {
                    // 关闭所有槽址 >= 当前栈顶的开 upvalue,无弹栈 -- 弹栈由前置 POP_N 承担
                    // (对齐 Lua OP_CLOSE:编译器在弹区 POP_N 之后发射,弹区槽已位于 top 之上,
                    // 不 push 不覆写即安全)。
                    current_->close_upvalues(current_->stack_top());
                    break;
                }
                case OpCode::DEF_GLOBAL: {
                    // [v] -> []:以常量池 name 为键在当前模块 globals 首次定义(顶层 var -- 唯一
                    // 创建全局的入口;重定义属编译期 RedefinedVariable,运行期按定义处理)。
                    // 根安全:set 插入可能 rehash 触 GC,v 用 peek 不弹 -- 留 v 在值栈跨分配
                    // (先 pop 则成裸局部被回收),set 返回后才 drop。
                    ObjString* name = read_name(frame);
                    frame.module->globals().set(Value::from_obj(name), current_->peek(0));
                    current_->drop(1); // 写完才弹,栈效应仍为 [v] -> []
                    break;
                }
                case OpCode::LOAD_GLOBAL: {
                    // [] -> [v]:按名查当前模块 globals,miss 回退 VM 级 builtins_(Python 式查找
                    // 链);皆未命中 -> UndefinedVariable。STORE_GLOBAL 不回退 builtins(赋值不
                    // 隐式创建)。push 先写栈再 grow,载荷已入栈后方可能 collect。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    auto        entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        entry = builtins_.find(key); // 回退 builtins_
                        if (entry == nullptr) {
                            raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                            if (auto u = unwind()) {
                                return runtime_err(std::move(*u));
                            }
                            break;
                        }
                    }
                    current_->push(entry->value);
                    break;
                }
                case OpCode::STORE_GLOBAL: {
                    // [v] -> [v]:peek-store 到模块 globals;未定义 -> UndefinedVariable(赋值不
                    // 隐式创建)。无分配。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame.module->globals().find(key);
                    if (entry == nullptr) {
                        raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    entry->value = current_->peek(0);
                    break;
                }
                case OpCode::LOAD_FIELD:
                    // name:u16;[obj] -> [v]。执行体收口于 run_load_field。
                    if (!run_load_field(read_name(frame))) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::STORE_FIELD:
                    // name:u16;[obj, v] -> [v]。执行体收口于 run_store_field。
                    if (!run_store_field(read_name(frame))) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::LOAD_INDEX:
                    // [obj, idx] -> [v]。执行体收口于 run_load_index。
                    if (!run_load_index()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::STORE_INDEX:
                    // [obj, idx, v] -> [v](peek-store)。执行体收口于 run_store_index。
                    if (!run_store_index()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::LOAD_THIS_FIELD: {
                    // name:u16;[] -> [v]:this 取帧槽 0(方法帧形 [this, a1..aN]),与 obj.m 同走
                    // load_field 协议。帧槽 0 恒实例(编译器不变式,ASSERT 钉)。
                    auto inst = try_obj<ObjInstance>(frame.slots[0]);
                    ASSERT(inst != nullptr, "LOAD_THIS_FIELD: 'this' slot must be an instance (compiler invariant)");
                    if (const auto result = inst->load_field(*this, read_name(frame))) {
                        current_->push(*result); // [] -> [v]
                        break;
                    }
                    if (auto u = unwind()) {
                        return runtime_err(std::move(*u));
                    }
                    break;
                }
                case OpCode::STORE_THIS_FIELD: {
                    // name:u16;[v] -> [v]:peek-store 经 this 的 store_field(实例字段动态即
                    // 创建;false 分支为契约透传防御形态,实例路径不可达)。
                    auto inst = try_obj<ObjInstance>(frame.slots[0]);
                    ASSERT(inst != nullptr, "STORE_THIS_FIELD: 'this' slot must be an instance (compiler invariant)");
                    if (!inst->store_field(*this, read_name(frame), current_->peek(0))) { // false ⟺ 已 fail(契约)
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
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
                // 比较(执行体 run_binary_numeric,下同)
                case OpCode::GREATER:
                    if (!run_binary_numeric<OpCode::GREATER>()) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
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
                // 算术
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
                            return runtime_err(std::move(*u));
                        }
                        break;
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
                    const u16 off = read_u16(frame);
                    frame.ip += off;
                    break;
                }
                case OpCode::JUMP_TRUE: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(current_->pop())) {
                        frame.ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_TRUE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(current_->peek(0))) {
                        frame.ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current_->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_FALSE: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(current_->pop())) {
                        frame.ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_FALSE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(current_->peek(0))) {
                        frame.ip += off; // 命中:不弹,被测值即结果
                    } else {
                        current_->drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_BACK: {
                    const u16 off = read_u16(frame);
                    frame.ip -= off;
                    // safe point:循环回边触发回收;maybe_collect 不移动值栈/帧,frame 引用跨调用有效。
                    gc_.maybe_collect();
                    break;
                }

                // ---- 函数与闭包 ----
                case OpCode::CALL: {
                    const u8 argc = read_u8(frame);
                    // 良构不变式:栈上必有 callee + argc 个实参。
                    ASSERT(current_->stack_size() >= static_cast<usize>(argc) + 1, "CALL on malformed stack");
                    if (const Value callee = current_->peek(argc); !call_value(callee, argc)) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                }
                case OpCode::CLOSURE: {
                    // fn:u16;[] -> [closure]:取常量池 ObjFunction 现场包 ObjClosure,按捕获
                    // 描述表(upvalue_descs_,存 fn 元数据不进字节码流,指令集 §4.13)逐个填:
                    // is_local 捕直接外围帧局部槽(经 capture_upvalue 单点收口「同一局部一份
                    // 引用」),否则复制外围闭包的第 index 个 upvalue(共享同一份引用)。
                    // 根安全(「栈即根」):闭包建成立即压栈,desc 循环内 new_upvalue 顶
                    // maybe_collect 不再威胁闭包,免守卫。
                    const auto idx     = read_u16(frame);
                    const auto fn      = Object::as<ObjFunction>(frame.unit->constants[idx].as_obj());
                    auto       closure = new_closure(gc_, fn);
                    current_->push(Value::from_obj(closure)); // 立即入栈:值栈即根,跨 desc 循环免守卫
                    for (const auto& [is_local, index]: fn->upvalue_descs()) {
                        if (is_local) {
                            closure->add_upvalue(current_->capture_upvalue(gc_, frame.slots + index));
                        } else {
                            closure->add_upvalue(frame.closure->upvalues()[index]);
                        }
                    }
                    break;
                }

                // ---- 类与对象(M5 阶段 2:VM 机制落地,编译器发射阶段 3 翻转)----
                case OpCode::MAKE_CLASS: {
                    // name:u16;[super] -> [class]:peek super 不先弹 -- new_class 顶
                    // maybe_collect 须 super 在栈(「栈即根」);非类值是**语言可达**错误
                    // (superclass 运行期才知值类型),故 raise 而非 ASSERT。建成写回原槽;
                    // init 继承收进对象构造(new_class 出厂即自 super 派生),指令层零 seed 写点。
                    if (const auto super = try_obj<ObjClass>(current_->peek(0))) {
                        const auto klass  = new_class(gc_, read_name(frame), super);
                        current_->peek(0) = Value::from_obj(klass);
                        break;
                    }

                    raise(ErrorCode::TypeMismatch, "superclass must be a class, got {}", type_name(current_->peek(0)));
                    if (auto u = unwind()) {
                        return runtime_err(std::move(*u));
                    }
                    break;
                }
                case OpCode::MAKE_METHOD: {
                    // name:u16;[class, closure] -> [class]:实例方法注册(静态经 MAKE_STATIC;
                    // 仅收闭包 -- 方法性 = defining class 戳)。栈形经 ASSERT 钉(值恒来自上一条
                    // CLOSURE,语言写不出违例)。副作用:set_field 命中 "init" 同步 init_ + 闭包戳
                    // defining class(一职双任:super 来源 + 方法性标记,读路径据非空判绑)。
                    const auto klass  = try_obj<ObjClass>(current_->peek(1));
                    const auto method = current_->peek(0);
                    ASSERT(klass != nullptr, "MAKE_METHOD: slot-1 is not a class (malformed stack)");
                    klass->set_field(read_name(frame), method);

                    const auto closure = try_obj<ObjClosure>(method);
                    ASSERT(closure != nullptr,
                           "MAKE_METHOD: slot-0 is not a closure (method registration is closure-only)");
                    closure->set_defining_class(klass);
                    current_->drop(1);
                    break;
                }
                case OpCode::MAKE_STATIC: {
                    // name:u16;[class, value] -> [class]:静态成员注册(var 声明与 fun 静态方法
                    // 同经此;不戳 defining class ⟹ 读恒原值)。与 MAKE_METHOD 同形,栈形 ASSERT 钉。
                    const auto klass = try_obj<ObjClass>(current_->peek(1));
                    ASSERT(klass != nullptr, "MAKE_STATIC: slot-1 is not a class (malformed stack)");
                    klass->set_field(read_name(frame), current_->peek(0));
                    current_->drop(1); // 弹 value 留 class:[class, value] -> [class]
                    break;
                }
                case OpCode::LOAD_SUPER_FIELD:
                    // name:u16;[] -> [v]。执行体收口于 run_load_super_field。
                    if (!run_load_super_field(read_name(frame))) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;
                case OpCode::INVOKE_METHOD:
                    not_implemented("INVOKE_METHOD");
                case OpCode::MAKE_LIST: {
                    // n:u16;[v1..vn] -> [list]:元素 peek 在栈跨 new_list 顶部 maybe_collect
                    // (「栈即根」),整段拷入走 trivial 分配不触 GC,拷完 drop n 再 push(窗口内
                    // 无 GC 点);n 已由编译器上限检查保证 <= 栈深,字节码良构。
                    const u16  count = read_u16(frame);
                    const auto list  = new_list(gc_);
                    list->elements().copy_from({current_->stack_top() - count, count});
                    current_->drop(count);
                    current_->push(Value::from_obj(list));
                    break;
                }
                case OpCode::MAKE_MAP: {
                    // n:u16;[k1,v1..kn,vn] -> [map]:键值 peek 在栈跨 new_map 顶部
                    // maybe_collect(「栈即根」);逐对 set 走 GC 分配器不触 GC(rehash 同,
                    // HashTable 注释),拷完 drop 2n 再 push(窗口内无 GC 点)。重复键天然
                    // 后键胜(set 命中原槽覆写,Python dict 同款);n 已由编译器上限检查,
                    // 字节码良构。
                    const u16  count = read_u16(frame);
                    const auto map   = new_map(gc_);
                    const auto base  = current_->stack_top() - count * 2;
                    for (usize i = 0; i < count; ++i) {
                        map->table().set(base[i * 2], base[i * 2 + 1]);
                    }
                    current_->drop(count * 2);
                    current_->push(Value::from_obj(map));
                    break;
                }
                case OpCode::MAKE_RANGE:
                    not_implemented("MAKE_RANGE");

                // ---- 模块导入 ----
                case OpCode::IMPORT:
                    // path:u16;[..., module]。执行体收口于 run_import。
                    if (!run_import(read_name(frame))) {
                        if (auto u = unwind()) {
                            return runtime_err(std::move(*u));
                        }
                        break;
                    }
                    break;

                // ---- 异常 ----
                case OpCode::THROW: {
                    // 用户 throw:弹抛出值,原值入寄存器(不包 ObjException -- catch 绑原值保类型)
                    // 后 unwind。
                    current_->raise(current_->pop());
                    if (auto u = unwind()) {
                        return runtime_err(std::move(*u));
                    }
                    break;
                }

                // ---- 返回(exit_frame 后 frame 引用作废,故先取返回值与判模块体帧)----
                case OpCode::RETURN: {
                    const Value ret = current_->pop(); // 取返回值(exit_frame 将丢弃其下方栈区)
                    // 模块体 run-once 帧名固定 <module>(主入口 <main> 与用户函数名均不含 '<>'),
                    // 其 RETURN 弹弃返回值、改压该模块对象,使 IMPORT 栈效应统一。先取 module
                    // 与 fn 名再 exit_frame(其后 frame 引用悬垂)。
                    auto mod     = frame.module;
                    auto fn_name = frame.closure->name()->view();
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
