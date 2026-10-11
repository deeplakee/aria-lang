#include "runtime/Eval.hpp"

#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <system_error>

#include "aria.hpp"
#include "bytecode/CodeUnit.hpp"
#include "bytecode/Disassembler.hpp"
#include "bytecode/code.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjRange.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/opcode_profile.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {
    namespace stdfs = std::filesystem;

    namespace {

        // 读 1 字节操作数(假定字节码良构),推进 ip。
        u8 read_u8(CallFrame* frame) noexcept { return *frame->ip++; }

        // 读 2 字节小端 u16 操作数(假定字节码良构),推进 ip。拼装走 util::make_u16。
        u16 read_u16(CallFrame* frame) noexcept {
            const u16 value = util::make_u16(frame->ip[0], frame->ip[1]);
            frame->ip += 2;
            return value;
        }

        // 取常量池 u16 索引处的 ObjString*(全局名/字段名/模块路径等),推进 ip。
        // 良构前提:该常量必为经 intern 驻留的 ObjString*(编译期保证),故不二次校验 -- 编译期
        // 出错属 bug 非运行期可恢复错。
        ObjString* read_name(CallFrame* frame) noexcept {
            const auto idx = read_u16(frame);
            return frame->unit->constants[idx].as_obj()->as<ObjString>();
        }

        // 执行跟踪:每条指令执行前打印字节码/栈/帧(stderr,调试用)。常态编译,宏只守 dispatch_loop
        // 内调用点,关闭时零开销;frame.ip 此时指向待执行指令,据此解码(仅读不推进 VM 的 ip)。
        // 栈渲染用 repr 不用 stringify:后者 Obj 走可重载虚 to_string,在
        // dispatch_loop 内会重入 VM 致无限递归;debug_repr 纯 C++,绝不触用户重载。
        [[maybe_unused]] void trace_execution(AriaExeContext& ctx) {
            const auto& frames = ctx.frames();
            const auto& frame  = frames.top();
            const auto  ip_off = static_cast<u32>(frame.ip - frame.unit->code.data());
            const auto  instr  = Disassembler::disassembleInstruction(frame.unit, ip_off);

            io::print(stderr, "[trace] ctx {:p}  {}  {} @{:04X}  {}\n", static_cast<const void*>(&ctx),
                      frame.module->to_string(), frame.closure->function()->to_string(), ip_off, instr);

            // 栈行与 ^ 列号一趟同步算:^ 对齐到当前帧栈底(slots 所指槽)的 [ 下方;slots 越过
            // 栈顶时(异常态)所有槽位都满足 p < slots,累加自然停在全部段之和,无需分支。
            const String prefix = std::format("        stack[{}]: ", ctx.stack_size());
            usize        col    = prefix.size();
            String       stack_str;
            for (auto p = ctx.stack_base(); p < ctx.stack_top(); ++p) {
                const auto s = std::format("[ {} ]", repr(*p));
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

        // specifier 的 stem:剥末段 ".aria" 后缀(import "x.aria" 与 "x" 等价);末段恰为 ".aria" 者不剥。
        [[nodiscard]]
        StringView stem_of(const StringView spec) {
            const auto last_slash = spec.find_last_of('/');
            const auto last_seg   = (last_slash == StringView::npos) ? spec : spec.substr(last_slash + 1);
            if (last_seg.size() > kAriaExtension.size() && last_seg.ends_with(kAriaExtension)) {
                return spec.substr(0, spec.size() - kAriaExtension.size());
            }
            return spec;
        }

        // 逐基探测:<base>/<file_name> 规范化(weakly_canonical),存在即返绝对路径;不存在/失败试下一基。
        [[nodiscard]]
        Opt<String> probe_bases(const List<stdfs::path>& bases, const StringView file_name) {
            std::error_code ec;
            for (const auto& base: bases) {
                const auto canon = stdfs::weakly_canonical(base / file_name, ec);
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

        // 把 import 串(specifier)解析为命中文件的绝对规范路径(= 模块表键)。
        Opt<String> resolve_module(const StringView spec, const StringView current_module_path,
                                   const List<String>& source_roots) {
            const auto file_name = util::concat_string(stem_of(spec), kAriaExtension);

            // 相对 specifier:基恒为当前模块自己的目录(合成模块无锚点即失败),永不逃逸到别的源根。
            if (spec.starts_with("./") || spec.starts_with("../") || spec == "." || spec == "..") {
                if (current_module_path.empty()) {
                    return std::nullopt;
                }
                List<stdfs::path> bases;
                bases.push_back(stdfs::path{String{current_module_path}}.parent_path());
                return probe_bases(bases, file_name);
            }

            // 裸名:沿 source_roots 逐根试(对齐 Python sys.path);空根跳过。
            List<stdfs::path> bases;
            for (const auto& root: source_roots) {
                if (!root.empty()) {
                    bases.emplace_back(root);
                }
            }
            return probe_bases(bases, file_name);
        }

    } // namespace

    bool Eval::call_value(AriaExeContext& ctx, const Value callee, const u8 argc) {
        if (!callee.is_obj()) {
            return ctx.fail(ErrorCode::CallNonCallable, "type {} does not support '__call__'", type_name(callee));
        }

        switch (Object* obj = callee.as_obj(); obj->type()) {
            case ObjType::CLOSURE:
                return call_closure(ctx, obj->as<ObjClosure>(), argc);
            case ObjType::NATIVE_FN:
                return call_native(ctx, obj->as<ObjNativeFn>(), argc);
            case ObjType::CLASS:
                return call_class(ctx, obj->as<ObjClass>(), argc);
            case ObjType::BOUND_METHOD:
                return call_bound_method(ctx, obj->as<ObjBoundMethod>(), argc);
            default: {
                // 其余对象类型:按 `__call__` 钩子名经成员协议取实现后调;调用区就地复用
                // [callee, a1..aN] 恰是 [this, args](槽 0 兼返回槽),递归交 call_value 分发。
                if (const auto target = obj->load_field(ctx, ctx.str<"__call__">())) {
                    return call_value(ctx, *target, argc);
                }
                return false; // 载荷已在挂起错误寄存器
            }
        }
    }

    bool Eval::call_class(AriaExeContext& ctx, ObjClass* obj, const u8 argc) {
        // GC 点仅 new_instance,建成即原生写槽;init 按契约恒有值(非可调值由 call_value 报错)。
        const auto instance = new_instance(ctx.gc(), obj);
        ctx.peek(argc)      = Value::from_obj(instance); // 建成即写槽:instance 经值栈根化(即新帧 this)
        return call_value(ctx, obj->init(), argc);
    }

    bool Eval::call_bound_method(AriaExeContext& ctx, const ObjBoundMethod* obj, const u8 argc) {
        ctx.peek(argc) = obj->receiver(); // 槽 0:bound -> this(实参槽位不动)
        return call_value(ctx, obj->method(), argc);
    }

    bool Eval::call_closure(AriaExeContext& ctx, ObjClosure* obj, const u8 argc) {
        const auto fn = obj->function();
        if (!ctx.check_arity(fn, argc)) {
            return false;
        }
        if (ctx.frames_full()) {
            return ctx.fail(ErrorCode::StackOverflow, "call frame stack overflow");
        }
        ctx.enter_frame(obj, ctx.prepare_call_args(fn, argc));
        return true;
    }

    Opt<Value> Eval::execute_call(AriaExeContext& ctx, const Value slot0, ObjClosure* closure,
                                  const Span<const Value> args) {
        // 重入执行体:压 slot0 与 args 后同步驱动一次闭包调用至结算。成功 = 栈顶收取闭包返回值;
        // 失败 nullopt 且载荷留寄存器、残留帧已截净,调用方按 bool 契约 return false 交外层续搜。
        // args 契约见声明处(不指进值栈,或留足压档余量)。重入返回后写返回槽恒经 peek
        // 重寻址(重入期间值栈可能搬迁)。
        auto& vm = ctx.host();
        ASSERT(&ctx == vm.current_, "reentry must target the executing context");
        if (ctx.recursion_depth() >= ObjMovement::kMaxRecursionDepth) {
            return ctx.fail(ErrorCode::StackOverflow, "nested execution too deep"); // 载荷入寄存器
        }
        if (args.size() > std::numeric_limits<u8>::max()) {
            return ctx.fail(ErrorCode::WrongArity, "too many arguments: {}", args.size()); // 载荷入寄存器
        }
        ASSERT(args.data() >= ctx.stack_top() || args.data() + args.size() <= ctx.stack_base() ||
                       ctx.stack_size() + 1 + args.size() <= ctx.stack_capacity(),
               "args alias the value stack without room to push");
        ctx.push(slot0); // 入栈即根(槽 0 = 本调用返回槽)
        for (const auto& arg: args) {
            ctx.push(arg);
        }
        const auto saved_ret = ctx.enter_recursion(); // 换防本循环出口边界,取走外层值
        if (call_closure(ctx, closure, args.size()) && dispatch_loop(vm)) {
            ctx.leave_recursion(saved_ret);
            return ctx.pop(); // 闭包返回值
        }
        ctx.leave_recursion(saved_ret);
        return std::nullopt; // 错误值在寄存器
    }

    bool Eval::call_native(AriaExeContext& ctx, const ObjNativeFn* obj, const u8 argc) {
        // 同步调用不进帧。entered_ctx 是全部事后簿记(drop/寄存器断言)的锚点:切换型原生成功后
        // vm.current_ 已被换至对侧,drop 恒落 entered_ctx 使栈顶停在该调用的槽 0(切换模型下的
        // 预留结果槽)。「false + 切换」为永久契约 -- 失败载荷在调用方上下文,切走即无人消费。
        // 实参留栈到 drop:切换型原生执行全程实参皆调用者栈根。
        const auto entered_ctx = &ctx;
        const auto slots       = Span<Value>{&ctx.peek(argc), static_cast<usize>(argc + 1)};
        // 进场前寄存器应空(上次错误已被 take_error 取走 / reset 清空)。
        ASSERT(!ctx.has_error(), "pending error not cleared before native call");
        if (obj->fn()(*entered_ctx, slots)) {
            ASSERT(!entered_ctx->has_error(), "native fn returned true but raised error");
            entered_ctx->drop(argc);
            return true;
        }
        ASSERT(ctx.host().current_ == entered_ctx, "native fn returned false after switching vm.current_");
        // 失败:载荷留寄存器交调用方 take_error(bool 契约)。
        ASSERT(entered_ctx->has_error(), "native fn returned false but raised no error");
        return false;
    }

    template<OpCode Op>
    bool Eval::run_binary_operator(AriaExeContext& ctx) {
        // 对象左值先试 string 快路径门,再取本对象算子实现后调(调用区 [lhs, rhs] 即 [this, arg1],
        // 槽 0 保持 receiver);非对象左值落 run_binary_numeric。GC 走查:receiver 占槽 0(栈即根),
        // 取到的实现必可达(类表值 / 实例字段值),故无白色在途窗口、不挂守卫。
        if (const auto lhs = ctx.peek(1); lhs.is_obj()) {
            const auto obj = lhs.as_obj();
            if (try_string_fast_dispatch<Op>(ctx, obj)) {
                return true;
            }
            if (const auto target = get_obj_binary_op_impl<Op>(ctx, obj)) {
                return call_value(ctx, *target, 1);
            }
            return false;
        }
        return run_binary_numeric<Op>(ctx);
    }

    template<OpCode Op>
    bool Eval::try_string_fast_dispatch(AriaExeContext& ctx, Object* lhs) {
        // string 快路径(lhs 非空,调用方已判 is_obj):两侧 tag 均 STRING 时就地拼接/字序比较,
        // 免整条原生调用链;白名单仅 ADD 与四个字序比较实例化,其余算子恒 false。返回 true = 已
        // 派发完成,false = 不适用落原路(非失败,两侧皆 string 的臂全无 fail 路径)。门体用 is/as
        // 两段式守卫:热路径 flags-only(与平铺比较链同指令选择),免 try_as_obj/try_as 返 T* 的
        // 指针物化分支(实测见 local-string-perf 报告 §7/§12)。
        if constexpr (Op == OpCode::ADD || Op == OpCode::LESS || Op == OpCode::LESS_EQUAL || Op == OpCode::GREATER ||
                      Op == OpCode::GREATER_EQUAL) {
            if (lhs->is<ObjString>()) {
                if (const Value rhs = ctx.peek(0); rhs.is_obj()) {
                    if (const auto right = rhs.as_obj(); right->is<ObjString>()) {
                        return run_string_binary<Op>(ctx, lhs->as<ObjString>(), right->as<ObjString>());
                    }
                }
            }
        }
        return false;
    }

    template<OpCode Op>
    bool Eval::run_string_binary(AriaExeContext& ctx, ObjString* lhs, ObjString* rhs) {
        // [lhs, rhs] -> [r]:拼接先查驻留(哈希自 lhs 终态续算 rhs,命中零拷贝),比较按无符号字节序;
        // 语义与 StringClass 对应钩子逐位一致。GC 走查:唯一 GC 点是 new_string 内部 new_object 顶部
        // maybe_collect,两操作数 peek 未弹在栈(栈即根);命中/铸后 drop+push 无 GC 点。
        if constexpr (Op == OpCode::ADD) {
            const auto hash   = util::hash_str(lhs->hash(), rhs->view());
            const auto joined = new_string(ctx.gc(), lhs->view(), rhs->view(), hash);
            ctx.drop(2);
            ctx.push(Value::from_obj(joined));
        } else if constexpr (Op == OpCode::LESS) {
            ctx.drop(2);
            ctx.push(Value::from_bool(lhs->view().compare(rhs->view()) < 0));
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            ctx.drop(2);
            ctx.push(Value::from_bool(lhs->view().compare(rhs->view()) <= 0));
        } else if constexpr (Op == OpCode::GREATER) {
            ctx.drop(2);
            ctx.push(Value::from_bool(lhs->view().compare(rhs->view()) > 0));
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            ctx.drop(2);
            ctx.push(Value::from_bool(lhs->view().compare(rhs->view()) >= 0));
        } else {
            UNREACHABLE(); // 白名单收在 try_string_fast_dispatch,可达 Op 恒为上列五个之一
        }
        return true;
    }

    bool Eval::run_negate(AriaExeContext& ctx) {
        // [v] -> [r]:整数/浮点就地取负;对象左值取 __neg__ 实现后调用(一元恒零实参,调用区
        // [v] 即 [this]);其余类型报 InvalidOperand。GC 走查同 run_binary_operator。
        const Value operand = ctx.peek(0);
        if (operand.is_int()) {
            ctx.peek(0) = Value::from_int(-operand.as_int());
            return true;
        }
        if (operand.is_f64()) {
            ctx.peek(0) = Value::from_f64(-operand.as_f64());
            return true;
        }
        if (operand.is_obj()) {
            if (const auto target = operand.as_obj()->load_field(ctx, ctx.str<"__neg__">())) {
                return call_value(ctx, *target, 0);
            }
            return false;
        }
        return ctx.fail(ErrorCode::InvalidOperand, "negate requires a number, got {}", type_name(operand));
    }

    template<OpCode Op>
    Opt<Value> Eval::get_obj_binary_op_impl(AriaExeContext& ctx, Object* obj) {
        // 指令 -> 算子钩子名的编译期映射(Op 由调用点穷举);经成员协议裸查找取实现,与成员访问
        // 同一协议缝(fields 遮蔽/类表/miss 措辞都由各类型 load_field 承担)。
        if constexpr (Op == OpCode::ADD) {
            return obj->load_field(ctx, ctx.str<"__add__">());
        } else if constexpr (Op == OpCode::SUBTRACT) {
            return obj->load_field(ctx, ctx.str<"__sub__">());
        } else if constexpr (Op == OpCode::MULTIPLY) {
            return obj->load_field(ctx, ctx.str<"__mul__">());
        } else if constexpr (Op == OpCode::DIVIDE) {
            return obj->load_field(ctx, ctx.str<"__div__">());
        } else if constexpr (Op == OpCode::MOD) {
            return obj->load_field(ctx, ctx.str<"__mod__">());
        } else if constexpr (Op == OpCode::GREATER) {
            return obj->load_field(ctx, ctx.str<"__gt__">());
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            return obj->load_field(ctx, ctx.str<"__ge__">());
        } else if constexpr (Op == OpCode::LESS) {
            return obj->load_field(ctx, ctx.str<"__lt__">());
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            return obj->load_field(ctx, ctx.str<"__le__">());
        } else {
            UNREACHABLE(); // Op 恒为上列 9 个二元指令之一(调用点穷举)
        }
    }

    template<OpCode Op>
    bool Eval::run_binary_numeric(AriaExeContext& ctx) {
        // 数值二元的入口:弹 2、类型守卫、按域分流(两个域语义不同,实现各住自己的辅助方法)。
        const Value b = ctx.pop();
        const Value a = ctx.pop();
        if (!(is_num(a) && is_num(b))) {
            return ctx.fail(ErrorCode::TypeMismatch, "operator '{}' requires numbers, got {} and {}", op_symbol(Op),
                            type_name(a), type_name(b));
        }
        if (a.is_int() && b.is_int()) {
            return run_binary_int<Op>(ctx, a.as_int(), b.as_int());
        }
        // 任一 F64 升浮点:另一侧按字面值升。
        return run_binary_f64<Op>(ctx, as_num(a), as_num(b));
    }

    template<OpCode Op>
    bool Eval::run_binary_int(AriaExeContext& ctx, const i64 lhs, const i64 rhs) {
        // 整数域九算子:结果压栈。除/模零是域特有失败(整数无 inf/nan),就地 fail;% 为 C++ 语义。
        if constexpr (Op == OpCode::ADD) {
            ctx.push(Value::from_int(lhs + rhs));
        } else if constexpr (Op == OpCode::SUBTRACT) {
            ctx.push(Value::from_int(lhs - rhs));
        } else if constexpr (Op == OpCode::MULTIPLY) {
            ctx.push(Value::from_int(lhs * rhs));
        } else if constexpr (Op == OpCode::DIVIDE) {
            if (rhs == 0) {
                return ctx.fail(ErrorCode::DivisionByZero, "integer division by zero");
            }
            ctx.push(Value::from_int(lhs / rhs));
        } else if constexpr (Op == OpCode::MOD) {
            if (rhs == 0) {
                return ctx.fail(ErrorCode::ModuloByZero, "integer modulo by zero");
            }
            ctx.push(Value::from_int(lhs % rhs));
        } else if constexpr (Op == OpCode::GREATER) {
            ctx.push(Value::from_bool(lhs > rhs));
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            ctx.push(Value::from_bool(lhs >= rhs));
        } else if constexpr (Op == OpCode::LESS) {
            ctx.push(Value::from_bool(lhs < rhs));
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            ctx.push(Value::from_bool(lhs <= rhs));
        } else {
            UNREACHABLE(); // Op 恒为上列 9 个二元指令之一(调用点穷举)
        }
        return true;
    }

    template<OpCode Op>
    bool Eval::run_binary_f64(AriaExeContext& ctx, const f64 lhs, const f64 rhs) {
        // 浮点域九算子:结果压栈。按 IEEE(除零得 inf/nan、% 走 fmod,无失败路径),故无 fail 分支。
        if constexpr (Op == OpCode::ADD) {
            ctx.push(Value::from_f64(lhs + rhs));
        } else if constexpr (Op == OpCode::SUBTRACT) {
            ctx.push(Value::from_f64(lhs - rhs));
        } else if constexpr (Op == OpCode::MULTIPLY) {
            ctx.push(Value::from_f64(lhs * rhs));
        } else if constexpr (Op == OpCode::DIVIDE) {
            ctx.push(Value::from_f64(lhs / rhs));
        } else if constexpr (Op == OpCode::MOD) {
            ctx.push(Value::from_f64(std::fmod(lhs, rhs)));
        } else if constexpr (Op == OpCode::GREATER) {
            ctx.push(Value::from_bool(lhs > rhs));
        } else if constexpr (Op == OpCode::GREATER_EQUAL) {
            ctx.push(Value::from_bool(lhs >= rhs));
        } else if constexpr (Op == OpCode::LESS) {
            ctx.push(Value::from_bool(lhs < rhs));
        } else if constexpr (Op == OpCode::LESS_EQUAL) {
            ctx.push(Value::from_bool(lhs <= rhs));
        } else {
            UNREACHABLE(); // Op 恒为上列 9 个二元指令之一(调用点穷举)
        }
        return true;
    }

    bool Eval::run_load_field(AriaExeContext& ctx, ObjString* name) {
        // 非对象(含 nil)统一「does not support field access」文案(协议分派需 Object*);
        // 对象 miss 的文案由协议 override 烘焙。
        const Value obj = ctx.peek(0);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (const auto result = obj.as_obj()->load_field_bound(ctx, name)) {
            ctx.peek(0) = *result; // 写回原槽:[obj] -> [v]
            return true;
        }
        return false; // 载荷已在寄存器
    }

    bool Eval::run_store_field(AriaExeContext& ctx, ObjString* name) {
        // 契约见 Eval.hpp;非对象守卫同 run_load_field。
        const Value obj = ctx.peek(1);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(obj));
        }
        if (!obj.as_obj()->store_field(ctx, name, ctx.peek(0))) {
            return false; // 载荷已在寄存器
        }
        ctx.peek(1) = ctx.peek(0); // 单槽下移:弹 obj 留 v,[obj, v] -> [v]
        ctx.drop(1);
        return true;
    }

    bool Eval::run_load_this_field(AriaExeContext& ctx, ObjString* name) {
        // 契约见 Eval.hpp;this 取顶帧槽 0(帧槽「栈即根」同 run_load_super_field),经
        // load_field_bound 协议出值压栈([] -> [v]),miss 文案由协议 override 就地烘焙。
        const Value this_value = ctx.frames().top().slots[0];
        if (const auto inst = try_as_obj<ObjInstance>(this_value)) {
            if (const auto result = inst->load_field_bound(ctx, name)) {
                ctx.push(*result); // [] -> [v]
                return true;
            }
            return false;
        }
        return ctx.fail(ErrorCode::TypeMismatch, "field access requires an instance, got {}", type_name(this_value));
    }

    bool Eval::run_store_this_field(AriaExeContext& ctx, ObjString* name) {
        // 契约见 Eval.hpp;写入:[v] -> [v](peek-store,值留栈,this 不经值栈)。store_field
        // 恒成功(实例字段动态),false 分支为契约透传防御形态。非实例兜底同读取执行体。
        const Value this_value = ctx.frames().top().slots[0];
        if (const auto inst = try_as_obj<ObjInstance>(this_value)) {
            if (inst->store_field(ctx, name, ctx.peek(0))) { // false ⟺ 已 fail(契约)
                return true;                                 // 值留栈(peek-store)
            }
            return false;
        }
        return ctx.fail(ErrorCode::TypeMismatch, "field assignment requires an instance, got {}",
                        type_name(this_value));
    }

    bool Eval::run_prepare_method(AriaExeContext& ctx, ObjString* name) {
        // 契约见 Eval.hpp。两段式第一段:接收者在栈顶(实参尚未求值)。协议解析期间它须在栈
        // (「栈即根」)-- 裸查找命中零分配(基类默认 fail;宿主 override 皆纯透传/纯查询),唯一
        // 分配点是 miss 的 fail 装箱。解析先于实参求值。
        const Value recv = ctx.peek(0);
        if (!recv.is_obj()) {
            return ctx.fail(ErrorCode::UndefinedProperty, "type {} does not support field access", type_name(recv));
        }
        if (const auto target = recv.as_obj()->load_field(ctx, name)) {
            ctx.push(*target); // 待调值压栈:跨指令存活,GC 根由值栈承担(「栈即根」)
            return true;
        }
        return false;
    }

    bool Eval::run_call_method(AriaExeContext& ctx, const u8 argc) {
        // 契约见 Eval.hpp。纯调用,不再解析:[recv, target, a1..aN] 里实参整体下移一格补掉待调值
        // 占的那格 -> [recv, a1..aN](槽 0 = receiver = this);待调值交 call_value 统一分发
        // (argc == 0 时下移为空转)。
        const Value target = ctx.peek(argc);
        for (u8 i = argc; i >= 1; --i) {
            ctx.peek(i) = ctx.peek(i - 1);
        }
        ctx.drop(1);
        return call_value(ctx, target, argc);
    }

    bool Eval::try_list_load_fast_dispatch(AriaExeContext& ctx, Object* target, const Value idx) {
        // 契约见 Eval.hpp;命中臂写回 obj 槽再弹 idx,与虚路径同栈效应。
        if (target->is<ObjList>() && idx.is_int()) {
            const auto list = target->as<ObjList>();
            if (const auto slot = util::resolve_index(idx.as_int(), list->elements().size())) {
                ctx.peek(1) = list->elements()[*slot];
                ctx.drop(1);
                return true;
            }
        }
        return false;
    }

    bool Eval::try_list_store_fast_dispatch(AriaExeContext& ctx, Object* target, const Value idx, const Value value) {
        // 契约见 Eval.hpp。
        if (target->is<ObjList>() && idx.is_int()) {
            const auto list = target->as<ObjList>();
            if (const auto slot = util::resolve_index(idx.as_int(), list->elements().size())) {
                list->elements()[*slot] = value;
                ctx.peek(2)             = value;
                ctx.drop(2);
                return true;
            }
        }
        return false;
    }

    bool Eval::run_load_index(AriaExeContext& ctx) {
        // 契约见 Eval.hpp;非对象守卫同 run_load_field(文案与协议基类默认同串)。
        const Value idx = ctx.peek(0);
        const Value obj = ctx.peek(1);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        const auto target = obj.as_obj();
        if (try_list_load_fast_dispatch(ctx, target, idx)) {
            return true;
        }
        if (const auto result = target->load_index(ctx, idx)) {
            ctx.peek(1) = *result; // 写回 obj 槽再弹 idx:[obj, idx] -> [v]
            ctx.drop(1);
            return true;
        }
        return false; // 载荷已在寄存器
    }

    bool Eval::run_store_index(AriaExeContext& ctx) {
        // 契约见 Eval.hpp;非对象守卫同 run_load_index。
        const Value value = ctx.peek(0);
        const Value idx   = ctx.peek(1);
        const Value obj   = ctx.peek(2);
        if (!obj.is_obj()) {
            return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support subscript access", type_name(obj));
        }
        const auto target = obj.as_obj();
        if (try_list_store_fast_dispatch(ctx, target, idx, value)) {
            return true;
        }
        if (!target->store_index(ctx, idx, value)) {
            return false; // 载荷已在寄存器
        }
        ctx.peek(2) = value; // 值下移两格:弹 obj、idx 留 v,[obj, idx, v] -> [v]
        ctx.drop(2);
        return true;
    }

    bool Eval::run_make_range(AriaExeContext& ctx, const u8 flags) {
        // 契约见 Eval.hpp。端点 peek 在栈跨 new_range 顶部 maybe_collect(「栈即根」,端点为标量
        // 整数非对象);铸完 drop 再 push(窗口内无 GC 点)。非整数端点 TypeMismatch,文案报端点类型。
        if ((flags & kRangeFlagUnbounded) != 0) {
            const Value from = ctx.peek(0);
            if (!from.is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "range bound must be an integer, got {}", type_name(from));
            }
            const auto range = new_range(ctx.gc(), from.as_int());
            ctx.drop(1);
            ctx.push(Value::from_obj(range));
            return true;
        }
        const bool  exclusive = (flags & kRangeFlagExclusive) != 0;
        const Value to        = ctx.peek(0);
        const Value from      = ctx.peek(1);
        if (!from.is_int() || !to.is_int()) {
            return ctx.fail(ErrorCode::TypeMismatch, "range bounds must be integers, got {} and {}", type_name(from),
                            type_name(to));
        }
        const auto range = new_range(ctx.gc(), from.as_int(), to.as_int(), exclusive);
        ctx.drop(2);
        ctx.push(Value::from_obj(range));
        return true;
    }

    bool Eval::run_make_class(AriaExeContext& ctx, ObjString* name) {
        // 契约见 Eval.hpp;[super] -> [class] 原地写回,铸类不弹不压。
        if (const auto super = try_as_obj<ObjClass>(ctx.peek(0))) {
            // 内建容器五类不可继承(方法面 receiver 恒具体内建对象,实例无从满足);Object 放行:
            // 无 super 的 def 以 LOAD_REG 直发它,与显式 : Object 同栈形不可分。
            if (super == ctx.host().list_class() || super == ctx.host().map_class() ||
                super == ctx.host().string_class() || super == ctx.host().range_class() ||
                super == ctx.host().iterator_class()) {
                return ctx.fail(ErrorCode::TypeMismatch, "cannot inherit from built-in class '{}'",
                                super->name()->view());
            }
            const auto klass = new_class(ctx.gc(), name, super);
            ctx.peek(0)      = Value::from_obj(klass);
            return true;
        }
        // 非类值是语言可达错误,故 fail 而非 ASSERT。
        return ctx.fail(ErrorCode::TypeMismatch, "superclass must be a class, got {}", type_name(ctx.peek(0)));
    }

    bool Eval::run_load_super_field(AriaExeContext& ctx, ObjString* name) {
        // 契约见 Eval.hpp;miss 时类措辞 fail 已入寄存器,本函数只透传信号。
        const auto& frame    = ctx.frames().top();
        const auto  defining = frame.closure->defining_class();
        ASSERT(defining != nullptr, "closure has no defining class (compiler invariant)");
        const auto super = defining->superclass();
        ASSERT(super != nullptr, "method class has no superclass (compiler invariant)");
        const auto hit = super->load_field(ctx, name); // 从父类起读穿透(类协议)
        if (!hit) {
            return false;
        }
        if (const auto member = *hit; is_method(member)) {
            // 方法命中:建成立即压栈,this 经帧槽根化;方法对象经 super 链根可达
            ctx.push(Value::from_obj(new_bound_method(ctx.gc(), member, frame.slots[0])));
        } else {
            ctx.push(member); // 静态槽原值直读,不绑定不缓存
        }
        return true;
    }

    bool Eval::run_import(AriaExeContext& ctx, const ObjString* path) {
        // 三类失败点(解析/加载/进帧)一律 return false(载荷已 raise),unwind 留 dispatch_loop 调用点。
        const auto& frame = ctx.frames().top(); // 导入方帧:abs_path 作相对解析基;进帧后不再引用
        // 相对分支取当前模块 dirname 作基;dir_ 可空时 abs_path 返空串,相对分支拒绝。
        const auto canonical_path_str =
                resolve_module(path->view(), frame.module->abs_path(), ctx.host().source_roots());
        if (!canonical_path_str) {
            return ctx.fail(ErrorCode::ModuleNotFound, "module not found: '{}'", path->view());
        }
        const auto canonical_path = new_string(ctx.gc(), *canonical_path_str);
        // guard 跨 load_module 的编译期分配(intern weak root 不保命;表 set 无 GC 点)。
        auto guard = ctx.gc().make_guard(canonical_path);
        if (const auto module_entry = ctx.host().modules().find(Value::from_obj(canonical_path));
            module_entry != nullptr) {
            ctx.push(module_entry->value); // 命中:体执行中即循环导入,复用半初始化对象
            return true;
        }
        const auto module = ctx.host().load_module(canonical_path, path->view());
        if (module == nullptr) {
            return false; // 载荷已在寄存器
        }
        // entry 经 module->entry_ 根可达;闭包建成即压栈根化。
        const auto entry = module->entry();
        ASSERT(entry != nullptr, "load_module returned a module whose entry is not set");
        const auto closure = new_closure(ctx.gc(), entry);
        ctx.push(Value::from_obj(closure));
        // 进帧失败(栈溢出等):帧未进,callee 仍在栈顶(unwind 截栈时一并丢弃)。
        return call_closure(ctx, closure, 0);
    }

    bool Eval::run_return(AriaExeContext& ctx) {
        // 取返回值 -> 弹帧 -> 按弹后帧深分流(出口边界 = ctx.ret_frame()):递归边界(非零)
        // 弹尽 = 本循环出口,值落调用区槽 0;根边界(零)弹尽再分主顶层/协程完成。
        const Value ret = ctx.pop(); // 保存返回值
        ctx.exit_frame();            // 清除调用栈帧
        if (ctx.frames().size() != ctx.ret_frame()) {
            ctx.push(ret); // 帧间返回:写回 callee 槽
            return false;
        }
        if (ctx.ret_frame() != 0) {
            ctx.push(ret); // VM 重入出口:返回值留在栈顶
            return true;
        }
        if (const auto resumer = ctx.previous()) {
            // 协程完成:值写 resumer 预留槽 -> reset 清场 -> 换指置 Done。
            resumer->peek(0) = ret;
            ctx.reset();
            ctx.host().leave_coroutine(ExecState::Done);
            return false;
        }
        ctx.push(ret); // 主顶层返回:返回值留在栈顶(主模块对象)
        return true;
    }

    bool Eval::unwind(AriaVM& vm) {
        // 前提:寄存器已有载荷(入口断言把关)。搜索阶段不动帧栈/值栈,命中就地回退派发。搜索
        // 边界 = vm.current_->ret_frame()(随上下文走,逐跳重读)。返回 true = 已派发(落回循环顶重取帧);
        // false = 边界之上全帧未命中,载荷留寄存器:边界非零(嵌套执行)即本次调用无法处理,截到
        // 边界清残帧后交外层循环续搜;边界为零则链上有恢复者已转投(协程置 Failed 死去,载荷 raise
        // 进恢复者,由其循环续搜),链根仍无 handler -> 调用方善后(收跟踪物化)。命中可能在多跳
        // 之后,vm.current_ 已非进入时的上下文。
        ASSERT(vm.current_->has_error(), "no pending payload");

        while (true) {
            // 自最内(栈顶)向基线搜索 try 记录;命中帧保留 -- handler 偏移与栈基址都属于它。
            auto& frames = vm.current_->frames();
            // 无符号下溢即终止
            for (usize i = frames.size() - 1; i >= vm.current_->ret_frame() && i < frames.size(); --i) {
                auto& [closure, unit, module, ip, slots, last_ip] = frames[i];
                const u32 ip_off                                  = static_cast<u32>(last_ip - unit->code.data());
                if (const auto record = unit->find_try_handler(ip_off)) {
                    // 命中:回退到命中帧并转入 record 的 catch handler(统一在 ObjMovement::unwind_to_handler)。
                    // 命中可能在多跳之后 -- vm.current_ 已非进入 unwind 时的上下文,调用方一律经循环顶
                    // 自 vm.current_ 重取帧,无需特判。
                    vm.current_->unwind_to_handler(i, **record);
                    return true;
                }
            }

            // 嵌套执行(ret_frame_ 非零):边界之上全帧未命中,本次调用无法处理。截到本循环边
            // 界清嵌套残帧(只弃边界之上,不越界:外层帧链与后续搜索不受扰),载荷留寄存器交
            // 外层循环续搜。
            if (vm.current_->ret_frame() != 0) {
                vm.current_->truncate_frames_to(vm.current_->ret_frame());
                return false;
            }

            // 边界为零仍全帧未命中:在链上转投一跳,链终止(主上下文)交调用方善后。
            if (const auto caller = vm.current_->previous()) {
                // 四步定序承重:take 先于 reset(reset 一并清寄存器,且先关开指、死协程不留栈,
                // 向外泄漏的捕获闭包取值安全);caller 寄存器此刻必空(call_native 进场断言锁,
                // 挂起期间无人可写非执行上下文的寄存器);take 到 raise 之间无 GC 点,载荷局部持有
                // 不丢根。
                const auto payload = vm.current_->take_error();
                vm.current_->reset();
                vm.leave_coroutine(ExecState::Failed);
                caller->raise(*payload);
            } else {
                return false; // 全链未命中:载荷在寄存器、帧栈活着,善后归调用方
            }
        }
    }

    bool Eval::dispatch_loop(AriaVM& vm) {
        // 栈/帧/寄存器一律经循环顶重绑的本轮 ctx 访问(切换必经 call_native 尾位置或 unwind 跳链,
        // 两者出循环体即回循环顶,引用随轮更新,体内无过期引用);「正在执行的字节码所在上下文恒等于
        // vm.current_」且切换点唯一显式。出口协议(返 bool):成功 = 返回值已写回调用区槽 0(栈顶);失败 = 载荷留寄存器,
        // 失败出口均就地善后(未捕获搜索截到边界或物化清场;进帧前失败帧未进;错误不走返回值通
        // 道)。出口边界 = vm.current_->ret_frame()(住 ObjMovement 随上下文走,重入场写入/退场归
        // 还;最外执行恒零,弹空帧栈即本循环驱动的最外帧弹尽;协程
        // 各链独立计帧,链内弹尽即跨到完成/顶层出口)。

        while (true) {
            // 本轮执行的上下文(= vm.current_ 的快照引用):仅本轮有效,见函数头注释。
            AriaExeContext& ctx = *vm.current_;
            // 不变式:此处帧栈恒非空(唯一弹空帧的顶层 RETURN 立即 return)。last_ip 在取指前写入:
            // 行号/unwind 查表的共同锚点(顶帧 = 故障指令,外层帧 = CALL 站点)。
            const auto frame = &ctx.frames().top();
            frame->last_ip   = frame->ip;
#ifdef DEBUG_TRACE_EXECUTION
            // 取 opcode 前打印执行状态(见 trace_execution)。
            trace_execution(ctx);
#endif
            // 各 case 按 code.hpp 枚举序排列;错误站点(raise / run_* 返 false)一律 goto
            // unwind_check,成功路径一律 break 回循环顶。**switch 之后不得新增引用 frame 的代码**
            //(unwind 派发后帧引用已废,unwind_check 标签体同守)。
            switch (ARIA_FETCH_OPCODE(frame)) {
                case OpCode::HALT:
                    // 真实程序不产 HALT(仅手写字节码用):运行态空 previous 即主上下文,协程内
                    // 落 HALT 会打破 run() 出口断言,提前在此拦截。
                    ASSERT(ctx.previous() == nullptr, "HALT reached inside a coroutine");
                    ctx.push(Value::nil_val()); // 停机值留栈顶(形态与 RETURN 顶层出口一致)
                    return true;

                // 数据加载与存储
                case OpCode::LOAD_CONST: {
                    const auto idx = read_u16(frame);
                    ctx.push(frame->unit->constants[idx]);
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
                    // u8 操作数按 i8 位型重解释做符号扩展(发射侧同型转 u8)。
                    ctx.push(Value::from_i32(std::bit_cast<i8>(raw)));
                    break;
                }
                case OpCode::LOAD_REG: {
                    // [] -> [regs[n]]:压 VM 单例寄存器;下标即注册表枚举值,编译器只发合法下标,
                    // 同 LOAD_LOCAL 槽访问不设防。
                    ctx.push(Value::from_obj(vm.registers_[read_u8(frame)]));
                    break;
                }
                case OpCode::LOAD_LOCAL: {
                    const auto slot = read_u16(frame);
                    ctx.push(frame->slots[slot]);
                    break;
                }
                // N 短变体:槽号即枚举名尾号,零操作数;逐条独立 case 写死槽号常量,免运行期换算。
                case OpCode::LOAD_LOCAL_1:
                    ctx.push(frame->slots[1]);
                    break;
                case OpCode::LOAD_LOCAL_2:
                    ctx.push(frame->slots[2]);
                    break;
                case OpCode::LOAD_LOCAL_3:
                    ctx.push(frame->slots[3]);
                    break;
                case OpCode::LOAD_LOCAL_4:
                    ctx.push(frame->slots[4]);
                    break;
                case OpCode::LOAD_LOCAL_5:
                    ctx.push(frame->slots[5]);
                    break;
                case OpCode::LOAD_LOCAL_6:
                    ctx.push(frame->slots[6]);
                    break;
                case OpCode::LOAD_LOCAL_7:
                    ctx.push(frame->slots[7]);
                    break;
                case OpCode::LOAD_LOCAL_8:
                    ctx.push(frame->slots[8]);
                    break;
                case OpCode::STORE_LOCAL: {
                    const auto slot    = read_u16(frame);
                    frame->slots[slot] = ctx.peek(0);
                    break;
                }
                case OpCode::STORE_LOCAL_1:
                    frame->slots[1] = ctx.peek(0);
                    break;
                case OpCode::STORE_LOCAL_2:
                    frame->slots[2] = ctx.peek(0);
                    break;
                case OpCode::STORE_LOCAL_3:
                    frame->slots[3] = ctx.peek(0);
                    break;
                case OpCode::STORE_LOCAL_4:
                    frame->slots[4] = ctx.peek(0);
                    break;
                case OpCode::STORE_LOCAL_5:
                    frame->slots[5] = ctx.peek(0);
                    break;
                case OpCode::STORE_LOCAL_6:
                    frame->slots[6] = ctx.peek(0);
                    break;
                case OpCode::STORE_LOCAL_7:
                    frame->slots[7] = ctx.peek(0);
                    break;
                case OpCode::STORE_LOCAL_8:
                    frame->slots[8] = ctx.peek(0);
                    break;
                case OpCode::LOAD_UPVALUE: {
                    // 压本闭包第 idx 个 upvalue 的当前值(开/闭两态统一经 value_slot() 取址)。
                    const u8 idx = read_u8(frame);
                    ctx.push(*frame->closure->upvalues()[idx]->value_slot());
                    break;
                }
                case OpCode::STORE_UPVALUE: {
                    // peek-store 到该 upvalue:open 态写穿到栈槽,closed 态写自持。
                    const u8 idx                                   = read_u8(frame);
                    *frame->closure->upvalues()[idx]->value_slot() = ctx.peek(0);
                    break;
                }
                case OpCode::CLOSE_UPVALUE: {
                    // 关闭所有槽址 >= 当前栈顶的开 upvalue,无弹栈 -- 弹栈由前置 POP_N 承担
                    //(弹区槽已位于 top 之上,不 push 不覆写即安全)。
                    ctx.close_upvalues(ctx.stack_top());
                    break;
                }
                case OpCode::DEF_GLOBAL: {
                    // [v] -> []:以常量池 name 为键在当前模块 globals 首次定义(顶层 var 唯一
                    // 创建全局的入口)。栈即根:peek 留 v 跨 set,写完才 drop。
                    ObjString* name = read_name(frame);
                    frame->module->globals().set(Value::from_obj(name), ctx.peek(0));
                    ctx.drop(1); // 写完才弹,栈效应仍为 [v] -> []
                    break;
                }
                case OpCode::LOAD_GLOBAL: {
                    // [] -> [v]:按名查当前模块 globals,miss 回退 vm.builtins_,皆未命中 ->
                    // UndefinedVariable。push 先写栈再 grow,载荷已入栈根后方可能 collect。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    auto        entry = frame->module->globals().find(key);
                    if (entry == nullptr) {
                        entry = vm.builtins_.find(key); // 回退 vm.builtins_
                        if (entry == nullptr) {
                            ctx.raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                            goto unwind_check;
                        }
                    }
                    ctx.push(entry->value);
                    break;
                }
                case OpCode::STORE_GLOBAL: {
                    // [v] -> [v]:peek-store 到模块 globals;未定义 -> UndefinedVariable(赋值不
                    // 隐式创建)。无分配。
                    ObjString*  name  = read_name(frame);
                    const Value key   = Value::from_obj(name);
                    const auto  entry = frame->module->globals().find(key);
                    if (entry == nullptr) {
                        ctx.raise(ErrorCode::UndefinedVariable, "undefined global '{}'", name->view());
                        goto unwind_check;
                    }
                    entry->value = ctx.peek(0);
                    break;
                }
                case OpCode::LOAD_FIELD:
                    // name:u16;[obj] -> [v]
                    if (!run_load_field(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_FIELD:
                    // name:u16;[obj, v] -> [v](单槽下移留 v)
                    if (!run_store_field(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LOAD_INDEX:
                    // [obj, idx] -> [v]
                    if (!run_load_index(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_INDEX:
                    // [obj, idx, v] -> [v](peek-store,值下移两格)
                    if (!run_store_index(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LOAD_THIS_FIELD:
                    // name:u16;[] -> [v]
                    if (!run_load_this_field(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::STORE_THIS_FIELD:
                    // name:u16;[v] -> [v](peek-store)
                    if (!run_store_this_field(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;

                // 算术与逻辑
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
                // 比较
                case OpCode::GREATER:
                    if (!run_binary_operator<OpCode::GREATER>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::GREATER_EQUAL:
                    if (!run_binary_operator<OpCode::GREATER_EQUAL>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LESS:
                    if (!run_binary_operator<OpCode::LESS>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::LESS_EQUAL:
                    if (!run_binary_operator<OpCode::LESS_EQUAL>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                // 算术(五算子共用执行体 run_binary_operator)
                case OpCode::ADD:
                    if (!run_binary_operator<OpCode::ADD>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::SUBTRACT:
                    if (!run_binary_operator<OpCode::SUBTRACT>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MULTIPLY:
                    if (!run_binary_operator<OpCode::MULTIPLY>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::DIVIDE:
                    if (!run_binary_operator<OpCode::DIVIDE>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MOD:
                    if (!run_binary_operator<OpCode::MOD>(ctx)) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::NOT:
                    ctx.push(Value::from_bool(!is_truthy(ctx.pop())));
                    break;
                case OpCode::NEGATE:
                    if (!run_negate(ctx)) {
                        goto unwind_check;
                    }
                    break;

                // 栈操作
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

                // 调试
                case OpCode::NOP:
                    break;

                // 控制流(u16 无符号;前向 JUMP* ip+=off,后向 JUMP_BACK ip-=off;
                // 偏移以读完操作数后的 ip 为基准,同 Disassembler 解码约定)
                case OpCode::JUMP: {
                    const u16 off = read_u16(frame);
                    frame->ip += off;
                    break;
                }
                case OpCode::JUMP_TRUE: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(ctx.pop())) {
                        frame->ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_TRUE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (is_truthy(ctx.peek(0))) {
                        frame->ip += off; // 命中:不弹,被测值即结果
                    } else {
                        ctx.drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_FALSE: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(ctx.pop())) {
                        frame->ip += off;
                    }
                    break;
                }
                case OpCode::JUMP_FALSE_OR_POP: {
                    const u16 off = read_u16(frame);
                    if (!is_truthy(ctx.peek(0))) {
                        frame->ip += off; // 命中:不弹,被测值即结果
                    } else {
                        ctx.drop(1); // 落空:弹掉
                    }
                    break;
                }
                case OpCode::JUMP_BACK: {
                    const u16 off = read_u16(frame);
                    frame->ip -= off;
                    // safe point:循环回边触发回收;maybe_collect 不移动值栈/帧,frame 指针跨调用有效。
                    vm.gc_.maybe_collect();
                    break;
                }
                case OpCode::JUMP_NE: {
                    // 融合形(EQUAL;JUMP_FALSE):[a, b] -> [];判等与 EQUAL 同走 value_equal,
                    // 假则前跳。仅 EQUAL 入融合:比较面里只有它纯结构判等、永不久帧(LESS 可经
                    // 对象 __lt__ 钩子把控制流转移进被调帧,单指令内无法跨帧边界完成跳转判定)。
                    const u16   off = read_u16(frame);
                    const Value b   = ctx.pop();
                    const Value a   = ctx.pop();
                    if (!value_equal(a, b)) {
                        frame->ip += off;
                    }
                    break;
                }

                // 函数与闭包
                case OpCode::CALL: {
                    const u8 argc = read_u8(frame);
                    // 良构不变式:栈上必有 callee + argc 个实参。
                    ASSERT(ctx.stack_size() >= static_cast<usize>(argc) + 1, "malformed stack");
                    if (const Value callee = ctx.peek(argc); !call_value(ctx, callee, argc)) {
                        goto unwind_check;
                    }
                    break;
                }
                case OpCode::CLOSURE: {
                    // fn:u16;[] -> [closure]:取常量池 ObjFunction 现场包 ObjClosure,按捕获描述
                    // 表逐个填:is_local 捕外围帧局部槽(经 capture_upvalue 收「同一局部一份引用」),
                    // 否则复制外围闭包的对应 upvalue(共享同一份引用)。根安全(栈即根):闭包建成
                    // 立即压栈,desc 循环内 new_upvalue 免守卫。
                    const auto idx     = read_u16(frame);
                    const auto fn      = frame->unit->constants[idx].as_obj()->as<ObjFunction>();
                    const auto closure = new_closure(vm.gc_, fn);
                    ctx.push(Value::from_obj(closure)); // 立即入栈:值栈即根,跨 desc 循环免守卫
                    for (const auto& [is_local, index]: fn->upvalue_descs()) {
                        if (is_local) {
                            closure->add_upvalue(ctx.capture_upvalue(vm.gc_, frame->slots + index));
                        } else {
                            closure->add_upvalue(frame->closure->upvalues()[index]);
                        }
                    }
                    break;
                }

                // 类与对象
                case OpCode::MAKE_CLASS:
                    // name:u16;[super] -> [class]:原地写回;非类值 / 内建类 super 为语言可达错误。
                    if (!run_make_class(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MAKE_METHOD: {
                    // name:u16;[class, closure] -> [class]:实例方法注册,仅收闭包(方法性 =
                    // defining class 戳),栈形经 ASSERT 校验(值恒来自上一条 CLOSURE,语言写不出
                    // 违例)。set_field 命中 "init" 同步 init_ + 播戳 -- 一职双任(super 来源 +
                    // 方法性标记,读路径据非空判绑)。
                    const auto klass  = try_as_obj<ObjClass>(ctx.peek(1));
                    const auto method = ctx.peek(0);
                    ASSERT(klass != nullptr, "slot-1 is not a class (malformed stack)");
                    klass->set_field(read_name(frame), method);

                    const auto closure = try_as_obj<ObjClosure>(method);
                    ASSERT(closure != nullptr, "slot-0 is not a closure (method registration is closure-only)");
                    closure->set_defining_class(klass);
                    ctx.drop(1);
                    break;
                }
                case OpCode::MAKE_STATIC: {
                    // name:u16;[class, value] -> [class]:静态成员注册(var 声明与 fun 静态方法同经此;
                    // 不戳 defining class ⟹ 读恒原值)。与 MAKE_METHOD 同形,栈形经 ASSERT 校验。
                    const auto klass = try_as_obj<ObjClass>(ctx.peek(1));
                    ASSERT(klass != nullptr, "slot-1 is not a class (malformed stack)");
                    klass->set_field(read_name(frame), ctx.peek(0));
                    ctx.drop(1); // 弹 value 留 class:[class, value] -> [class]
                    break;
                }
                case OpCode::LOAD_SUPER_FIELD:
                    // name:u16;[] -> [v]
                    if (!run_load_super_field(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::PREPARE_METHOD:
                    // name:u16;[recv] -> [recv, target]
                    if (!run_prepare_method(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::CALL_METHOD:
                    // argc:u8;[recv, target, a1..aN] -> [r]
                    if (!run_call_method(ctx, read_u8(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::MAKE_LIST: {
                    // n:u16;[v1..vn] -> [list]:元素 peek 在栈跨 new_list 顶部 maybe_collect(「栈即
                    // 根」),整段拷入走 trivial 分配不触 GC,拷完 drop n 再 push(窗口内无 GC 点)。
                    const u16  count = read_u16(frame);
                    const auto list  = new_list(vm.gc_);
                    list->elements().copy_from({ctx.stack_top() - count, count});
                    ctx.drop(count);
                    ctx.push(Value::from_obj(list));
                    break;
                }
                case OpCode::MAKE_MAP: {
                    // n:u16;[k1,v1..kn,vn] -> [map]:键值 peek 在栈跨 new_map 顶部 maybe_collect
                    //(栈即根,逐对 set 不触 GC),拷完 drop 2n 再 push(窗口内无 GC 点)。
                    // 重复键后键胜(set 命中原槽覆写)。
                    const u16  count = read_u16(frame);
                    const auto map   = new_map(vm.gc_);
                    const auto base  = ctx.stack_top() - count * 2;
                    for (usize i = 0; i < count; ++i) {
                        map->table().set(base[i * 2], base[i * 2 + 1]);
                    }
                    ctx.drop(count * 2);
                    ctx.push(Value::from_obj(map));
                    break;
                }
                case OpCode::MAKE_RANGE:
                    // flags:u8;[from, to] -> [range](无上界 [from] -> [range])
                    if (!run_make_range(ctx, read_u8(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::BUILD_STRING: {
                    // n:u8;[v1..vn] -> [str]:插值串收拢,段值 peek 在栈(「栈即根」),拼接负载
                    // 在 concat_values(渲染与 str()/println 同源),铸完 drop n 再 push(窗口内
                    // 无 GC 点)。
                    const u8   count  = read_u8(frame);
                    const auto base   = ctx.stack_top() - count;
                    const auto result = concat_values(vm.gc_, {base, count}, {});
                    ctx.drop(count);
                    ctx.push(Value::from_obj(result));
                    break;
                }
                case OpCode::IMPORT:
                    // path:u16;[..., module]
                    if (!run_import(ctx, read_name(frame))) {
                        goto unwind_check;
                    }
                    break;
                case OpCode::THROW: {
                    // 用户 throw:弹抛出值,原值入寄存器(不包 ObjException -- catch 绑原值保类型)后
                    // unwind。
                    ctx.raise(ctx.pop());
                    goto unwind_check;
                }
                case OpCode::RETURN:
                    if (run_return(ctx)) {
                        return true;
                    }
                    break;
                default:
                    UNREACHABLE();
            }
            continue; // 正常路径:break 出 switch 后回循环顶,不落 unwind_check
        // unwind 收口:true = 已派发 handler 落回循环顶重取帧;false = 全链未捕获,载荷留寄存器
        // 本循环即返,善后归调用方。(标签体不引用 frame 的防御同 switch 头注)。
        unwind_check:
            if (!unwind(vm)) {
                return false;
            }
        }
    }

} // namespace aria
