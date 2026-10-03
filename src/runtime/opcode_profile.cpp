#include "runtime/opcode_profile.hpp"

#ifdef ARIA_OPCODE_PROFILE

    #include <algorithm>
    #include <bit>
    #include <cstdlib>
    #include <utility>

    #include "bytecode/CodeUnit.hpp"
    #include "object/ObjNativeFn.hpp"
    #include "object/ObjString.hpp"
    #include "runtime/AriaVM.hpp"
    #include "runtime/ObjMovement.hpp"
    #include "util/io.hpp"
    #include "util/util.hpp"

namespace aria {

    namespace {

        // 指令字节长 = 1 + 操作数字节数(位宽取 Disassembler 同源的 kOpCodeFormats 表)。
        constexpr usize size_of(const OpCode op) noexcept {
            switch (kOpCodeFormats[std::to_underlying(op)]) {
                case OpFormat::Simple:
                    return 1;
                case OpFormat::U8:
                case OpFormat::ImmI8:
                case OpFormat::RangeFlags:
                case OpFormat::RegU8:
                    return 2;
                default:
                    return 3; // U16 / ConstU16 / JumpFwd / JumpBack / Import
            }
        }

        // 帧切换五指令:下一取指不落在本条结束地址(进被调体/回调用方/进模块体/进 handler),
        // 出边在目的地经 prev_op_ 补记派发对。
        constexpr bool is_frame_switch(const OpCode op) noexcept {
            switch (op) {
                case OpCode::CALL:
                case OpCode::CALL_METHOD:
                case OpCode::RETURN:
                case OpCode::IMPORT:
                case OpCode::THROW:
                    return true;
                default:
                    return false;
            }
        }

        // 直线型指令的 fall-through 配对判定:跳转族出边按走向在各自 case 记,帧切换族出边在
        // 目的地记,HALT 无出边(运行终点)。按格式类别派生,新增直线型 opcode 自动纳入。
        constexpr bool records_fall_pair(const OpCode op) noexcept {
            const auto format = kOpCodeFormats[std::to_underlying(op)];
            return format != OpFormat::JumpFwd && format != OpFormat::JumpBack && !is_frame_switch(op) &&
                   op != OpCode::HALT;
        }

        // 旁路读 u16 操作数(不推进 ip -- 消费仍归各 case 的 read_u16),拼装与 read_u16 同款。
        usize peek_u16(const CallFrame* frame) noexcept { return util::make_u16(frame->ip[0], frame->ip[1]); }

        // 旁路读 ConstU16 操作数指向的驻留名(不推进 ip,消费仍归各 case 的 read_name;良构前提)。
        ObjString* peek_const_name(const CallFrame* frame) noexcept {
            return frame->unit->constants[peek_u16(frame)].as_obj()->as<ObjString>();
        }

    } // namespace

    OpcodeProfiler::~OpcodeProfiler() { maybe_dump(); }

    OpCode OpcodeProfiler::fetch(const AriaVM& vm, CallFrame* frame) {
        const auto start = frame->ip;
        const auto op    = static_cast<OpCode>(*frame->ip++); // read_u8 同款:取走 opcode 字节
        const auto index = std::to_underlying(op);
        ++total_;
        ++counts_[index];

        // 帧切换边在目的地记账:上一条是帧切换指令时,本次取指即其派发目的地;prev_op_ 哨兵
        // = HALT("没有上一条"),首条取指与 REPL 行边界自然落空。
        if (is_frame_switch(prev_op_)) {
            ++jt_[std::to_underlying(prev_op_)][index];
        }
        prev_op_ = op;

        // 逐指令记账(与 dispatch_loop 同形):操作数直方图旁路读不推进 ip;调用面在取指点
        // peek、跳转走向在取指点判定(栈形是编译器不变式),出边按实际落点记 -- 落空记 pairs_
        //(可融合面),跳走记 jt_。
        switch (op) {
            // u8 操作数族(槽位/元数/寄存器格位/range flags)
            case OpCode::LOAD_UPVALUE:
            case OpCode::STORE_UPVALUE:
            case OpCode::POP_N:
            case OpCode::LOAD_REG:
            case OpCode::MAKE_RANGE:
                note_operand(index, frame->ip[0]);
                break;
            // u16 操作数族(槽位/常量池索引/元素数)
            case OpCode::LOAD_CONST:
            case OpCode::LOAD_LOCAL:
            case OpCode::STORE_LOCAL:
            case OpCode::DEF_GLOBAL:
            case OpCode::LOAD_GLOBAL:
            case OpCode::STORE_GLOBAL:
            case OpCode::LOAD_FIELD:
            case OpCode::STORE_FIELD:
            case OpCode::LOAD_THIS_FIELD:
            case OpCode::STORE_THIS_FIELD:
            case OpCode::CLOSURE:
            case OpCode::MAKE_CLASS:
            case OpCode::MAKE_METHOD:
            case OpCode::MAKE_STATIC:
            case OpCode::LOAD_SUPER_FIELD:
            case OpCode::MAKE_LIST:
            case OpCode::MAKE_MAP:
                note_operand(index, peek_u16(frame));
                break;
            case OpCode::LOAD_IMM: {
                isize imm = std::bit_cast<i8>(frame->ip[0]);
                imm       = imm < -24 ? -24 : (imm > 24 ? 24 : imm); // 先夹再移,负向越界不得回绕
                note_operand(index, static_cast<usize>(imm + 24));   // [-24, 24] -> [0, 48]
                break;
            }
            // 调用面:CALL/CALL_METHOD 的被调值在 peek(argc)。
            case OpCode::CALL:
            case OpCode::CALL_METHOD: {
                note_operand(index, frame->ip[0]);
                if (const usize argc = frame->ip[0]; vm.current_->stack_size() > argc) {
                    const Value callee = vm.current_->peek(argc);
                    note_callee(callee.is_obj() ? callee.as_obj() : nullptr); // 非对象 callee 是语言可达态
                }
                break;
            }
            // 调用面:PREPARE_METHOD 的栈顶即接收者(实参尚未求值),名字在常量池。
            case OpCode::PREPARE_METHOD:
                note_operand(index, peek_u16(frame));
                if (vm.current_->stack_size() > 0) {
                    if (const Value recv = vm.current_->peek(0); recv.is_obj()) {
                        note_method(peek_const_name(frame), recv.as_obj()->type());
                    }
                }
                break;
            // 无条件跳转:目标 = 读完操作数后的 ip ± 偏移,目标处一个字节即后继 opcode。
            case OpCode::JUMP:
                ++jt_[index][start[3 + peek_u16(frame)]];
                break;
            case OpCode::JUMP_BACK:
                ++jt_[index][*(start + 3 - peek_u16(frame))];
                break;
            // 条件跳转:条件在栈顶(编译器不变式),取指点按极性判走向 -- 命中率 + 实际落点对。
            case OpCode::JUMP_TRUE:
            case OpCode::JUMP_TRUE_OR_POP:
                if (is_truthy(vm.current_->peek(0))) {
                    ++branch_taken_[index];
                    ++jt_[index][start[3 + peek_u16(frame)]]; // 跳走:目标是 target 处的 opcode
                } else {
                    ++pairs_[index][start[3]]; // 落空:fall-through 配对
                }
                break;
            case OpCode::JUMP_FALSE:
            case OpCode::JUMP_FALSE_OR_POP:
                if (!is_truthy(vm.current_->peek(0))) {
                    ++branch_taken_[index];
                    ++jt_[index][start[3 + peek_u16(frame)]];
                } else {
                    ++pairs_[index][start[3]];
                }
                break;
            // 融合条件跳转(JUMP_NE):u16 操作数,但条件要等判等执行后才在栈上
            // 形成,取指点无从按极性判走向 -- 不记命中率与出边对(该指令的存在本身即吸收了
            // EQUAL;JUMP_FALSE 对),指令量照常入 counts_。
            case OpCode::JUMP_NE:
                break;
            // 无账指令:无操作数,且出边由目的地记账(帧切换)或不存在(运算/栈操作/运行终点)。
            case OpCode::HALT:
            case OpCode::LOAD_NIL:
            case OpCode::LOAD_TRUE:
            case OpCode::LOAD_FALSE:
            case OpCode::LOAD_LOCAL_1:
            case OpCode::LOAD_LOCAL_2:
            case OpCode::LOAD_LOCAL_3:
            case OpCode::LOAD_LOCAL_4:
            case OpCode::LOAD_LOCAL_5:
            case OpCode::LOAD_LOCAL_6:
            case OpCode::LOAD_LOCAL_7:
            case OpCode::LOAD_LOCAL_8:
            case OpCode::STORE_LOCAL_1:
            case OpCode::STORE_LOCAL_2:
            case OpCode::STORE_LOCAL_3:
            case OpCode::STORE_LOCAL_4:
            case OpCode::STORE_LOCAL_5:
            case OpCode::STORE_LOCAL_6:
            case OpCode::STORE_LOCAL_7:
            case OpCode::STORE_LOCAL_8:
            case OpCode::CLOSE_UPVALUE:
            case OpCode::LOAD_INDEX:
            case OpCode::STORE_INDEX:
            case OpCode::EQUAL:
            case OpCode::NOT_EQUAL:
            case OpCode::STRICT_EQUAL:
            case OpCode::STRICT_NOT_EQUAL:
            case OpCode::GREATER:
            case OpCode::GREATER_EQUAL:
            case OpCode::LESS:
            case OpCode::LESS_EQUAL:
            case OpCode::ADD:
            case OpCode::SUBTRACT:
            case OpCode::MULTIPLY:
            case OpCode::DIVIDE:
            case OpCode::MOD:
            case OpCode::NOT:
            case OpCode::NEGATE:
            case OpCode::POP:
            case OpCode::DUP:
            case OpCode::DUP2:
            case OpCode::NOP:
            case OpCode::RETURN:
            case OpCode::IMPORT:
            case OpCode::THROW:
                break;
        }

        // 直线型指令的 fall-through 配对在源头记:下一取指恒落在指令结束地址,该处一个字节即
        // 后继 opcode;单元以 RETURN/HALT 终止恒有后继,无越界之虞。异常出口路径会多记一次,
        // 健康程序为零。
        if (records_fall_pair(op)) {
            ++pairs_[index][start[size_of(op)]];
        }
        return op;
    }

    OpcodeProfiler::NameEntry& OpcodeProfiler::find_or_add(List<NameEntry>& table, ObjString* name,
                                                           const ObjType kind) {
        for (auto& entry: table) {
            if (entry.name == name && entry.kind == kind) {
                return entry;
            }
        }
        return table.emplace_back(NameEntry{name, kind, 0});
    }

    void OpcodeProfiler::note_method(ObjString* name, const ObjType kind) {
        ++find_or_add(method_calls_, name, kind).count;
    }

    void OpcodeProfiler::note_callee(Object* callee) {
        static_assert(std::to_underlying(ObjType::MOVEMENT) < kCallKindSlots, "ObjType outgrew kCallKindSlots");
        if (callee == nullptr) {
            ++call_non_obj_;
            return;
        }
        const auto kind = callee->type();
        ++call_kinds_[std::to_underlying(kind)];
        if (kind == ObjType::NATIVE_FN) {
            ++find_or_add(native_calls_, callee->as<ObjNativeFn>()->name(), kind).count;
        }
    }

    void OpcodeProfiler::note_operand(const usize index, const usize value) {
        ++operands_[index][value < kOperandLastBucket ? value : kOperandLastBucket];
    }

    // 产出 dump(ARIA_OPCODE_STATS 环境变量非空才打):stderr 上一段 [opprofile] 行块,计数行
    // 全部降序、只给原始计数。
    void OpcodeProfiler::maybe_dump() {
        if (const auto flag = std::getenv("ARIA_OPCODE_STATS"); flag == nullptr || flag[0] == '\0') {
            return;
        }

        io::println(stderr, "[opprofile] begin");
        io::println(stderr, "[opprofile] total {}", total_);

        List<usize> ops;
        for (usize i = 0; i < kOpCodeCount; ++i) {
            if (counts_[i] > 0) {
                ops.push_back(i);
            }
        }
        std::ranges::sort(ops, [this](const usize a, const usize b) { return counts_[a] > counts_[b]; });
        for (const auto i: ops) {
            io::println(stderr, "[opprofile] op {} {}", counts_[i], kOpCodeNames[i]);
        }

        // 相邻矩阵共用一套"降序展平"打印:tag 区分 pair(直线 / 落空的 fall-through 可融合面)
        // 与 jt(跳转目标边 + 帧切换派发边)。
        const auto dump_matrix = [](const char* tag, const u64(&matrix)[kOpCodeCount][kOpCodeCount]) {
            struct Triple {
                u64   count;
                usize from;
                usize to;
            };
            List<Triple> hits;
            for (usize i = 0; i < kOpCodeCount; ++i) {
                for (usize j = 0; j < kOpCodeCount; ++j) {
                    if (matrix[i][j] > 0) {
                        hits.push_back(Triple{matrix[i][j], i, j});
                    }
                }
            }
            std::ranges::sort(hits, [](const Triple& a, const Triple& b) { return a.count > b.count; });
            for (const auto& [count, from, to]: hits) {
                io::println(stderr, "[opprofile] {} {} {}|{}", tag, count, kOpCodeNames[from], kOpCodeNames[to]);
            }
        };
        dump_matrix("pair", pairs_);
        dump_matrix("jt", jt_);

        for (const auto i: ops) {
            bool any = false;
            for (usize j = 0; j < kOperandBuckets && !any; ++j) {
                any = operands_[i][j] != 0;
            }
            if (!any) {
                continue;
            }
            io::print(stderr, "[opprofile] operand {} {}", kOpCodeNames[i], operands_[i][0]);
            for (usize j = 1; j < kOperandBuckets; ++j) {
                io::print(stderr, " {}", operands_[i][j]);
            }
            io::print(stderr, "\n");
        }

        for (usize i = 0; i < kOpCodeCount; ++i) {
            if (branch_taken_[i] > 0) {
                io::println(stderr, "[opprofile] branch {} {}", branch_taken_[i], kOpCodeNames[i]);
            }
        }

        // (类型, 名) 两张表共用一套"降序展平"打印。
        const auto dump_names = [](const char* tag, const List<NameEntry>& table) {
            List<const NameEntry*> hits;
            for (const auto& entry: table) {
                hits.push_back(&entry);
            }
            std::ranges::sort(hits, [](const NameEntry* a, const NameEntry* b) { return a->count > b->count; });
            for (const auto hit: hits) {
                io::println(stderr, "[opprofile] {} {} {} {}", tag, hit->count, to_string(hit->kind),
                            hit->name->view());
            }
        };
        dump_names("mcall", method_calls_);
        dump_names("ncall", native_calls_);

        for (usize i = 0; i < kCallKindSlots; ++i) {
            if (call_kinds_[i] > 0) {
                io::println(stderr, "[opprofile] call {} {}", call_kinds_[i], to_string(static_cast<ObjType>(i)));
            }
        }
        if (call_non_obj_ > 0) {
            io::println(stderr, "[opprofile] call {} NonObject", call_non_obj_);
        }
        io::println(stderr, "[opprofile] end");
    }

} // namespace aria

#endif // ARIA_OPCODE_PROFILE
