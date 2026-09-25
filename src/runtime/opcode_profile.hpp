#ifndef ARIA_OPCODE_PROFILE_HPP
#define ARIA_OPCODE_PROFILE_HPP

// 指令频度探针的唯一接入点:dispatch_loop 取指行的 switch 初始化语句改用本宏取 opcode。
// 本宏唯一使用点即该行(read_u8 住 AriaVM.cpp 匿名命名空间,别处不可用)。
//
// 常态构建:展开式与原取指表达式逐字一致,零探针痕迹。
// 探针构建(-DARIA_OPCODE_PROFILE=ON):展开为成员 opcode_profiler_.fetch(*this, frame) --
// vm 经 this 隐式传递。OpcodeProfiler 以 VM 成员挂载(生命周期随 VM)、是 AriaVM 的友元
//(取 current_ 等私有状态),fetch 按 read_u8 同款语义取走 opcode 字节并返回后就地记账,
// 析构时按 ARIA_OPCODE_STATS 环境变量门控 dump 到 stderr(AriaVM.cpp 除取指行外零探针代码)。
#ifndef ARIA_OPCODE_PROFILE

    #define ARIA_FETCH_OPCODE(frame) static_cast<OpCode>(read_u8(frame))

#else

    // 探针构建的取指:vm 经 this 隐式传递(fetch 兼按 read_u8 同款语义取走 opcode 字节并返回),
    // 仅可在 AriaVM 成员函数内展开(依赖成员 opcode_profiler_ 与 *this)。
    #define ARIA_FETCH_OPCODE(frame) opcode_profiler_.fetch(*this, frame)

    #include "bytecode/code.hpp"
    #include "common.hpp"

namespace aria {

    class AriaVM;
    struct CallFrame;
    class Object;
    class ObjString;
    enum class ObjType : u8;

    // 指令频度探针:计数状态全部本类自持(每 VM 一份,不跨 VM 混账)。指令解码与 Disassembler
    // 同源 -- 操作数位宽按 kOpCodeFormats(OpFormat)取、名字渲染用 kOpCodeNames;产出格式由
    // bench/profile/opcode_profile.py 消费。
    class OpcodeProfiler {
    public:
        // 析构即产出:门控在析构而非 run() 出口,使 AriaVM 侧除取指行外无任何探针代码。
        ~OpcodeProfiler();

        // 取指 + 记账:frame->ip 指向 opcode 字节,先按 read_u8 同款语义取走并返回(调用方看到的
        // ip 状态与普通取指逐位一致,操作数消费仍归各 case),再借 frame 与 vm(友元)在与
        // dispatch_loop 同形的逐指令 switch 里记自己的账:
        //   直线型     -- fall-through 配对记 pairs_(下一取指恒落在指令结束地址,该处一个字节即
        //                 后继 opcode);
        //   条件跳转   -- 条件在栈顶(编译器不变式),取指点按极性判走向:命中率记 branch_taken_,
        //                 不落空记 pairs_、跳走记 jt_(目标处一个字节即后继 opcode);
        //   无条件跳转 -- 目标对记 jt_;
        //   帧切换(CALL/CALL_METHOD/RETURN/IMPORT/THROW) -- 落点取指时才知,目的地经 prev_op_
        //                 在下一取指补记派发对;
        // 另有操作数直方图与调用面(PREPARE_METHOD 的(接收者类型, 名)、CALL/CALL_METHOD 的
        // callee 分发面,栈形是编译器不变式,就地 peek 免散布探针)。
        OpCode fetch(const AriaVM& vm, CallFrame* frame);

    private:
        // (类型, 名) 计数条目:方法面按接收者 ObjType + 名,native 调用再按名;名指针即键
        //(intern 串身份恒一)。
        struct NameEntry {
            ObjString* name;
            ObjType    kind;
            u64        count;
        };

        static NameEntry& find_or_add(List<NameEntry>& table, ObjString* name, ObjType kind);

        void note_method(ObjString* name, ObjType kind);

        void note_callee(Object* callee);

        // 操作数入桶:>= 末桶归末桶(LOAD_IMM 已预移位到 [0, 48] 域)。
        void note_operand(usize index, usize value);

        void maybe_dump();

        // 操作数直方图桶:非负操作数(u8 槽位/元数、u16 索引)0..47 精确 + 末桶(>= 48);
        // LOAD_IMM 的 i8 夹到 [-24, 24] 再 +24 落同表(24 即 0)。跳转偏移与 IMPORT 路径不记。
        static constexpr usize kOperandBuckets    = 49;
        static constexpr usize kOperandLastBucket = kOperandBuckets - 1;

        // call_kinds_ 槽数(ObjType 现有 16 值,倍余量;越界由 .cpp 的 static_assert 把关)。
        static constexpr usize kCallKindSlots = 32;

        u64             total_                                   = 0;
        u64             counts_[kOpCodeCount]                    = {};
        u64             pairs_[kOpCodeCount][kOpCodeCount]       = {};
        u64             jt_[kOpCodeCount][kOpCodeCount]          = {};
        u64             operands_[kOpCodeCount][kOperandBuckets] = {};
        u64             branch_taken_[kOpCodeCount]              = {};
        u64             call_kinds_[kCallKindSlots]              = {};
        u64             call_non_obj_                            = 0; // CALL/CALL_METHOD 收到非对象 callee
        List<NameEntry> method_calls_;
        List<NameEntry> native_calls_;

        // 上一条指令的 opcode,哨兵 = HALT(即"没有上一条",其后的行首边本就不是相邻执行)。
        // 帧切换五指令的落点取指时才知,目的地凭本字段补记派发对;只存值不存地址 -- REPL 行
        // 边界上一行单元可能已被 GC 回收,解引用上一条地址是 UB,而帧切换边只需要"上一条是谁"。
        OpCode prev_op_ = OpCode::HALT;
    };

} // namespace aria

#endif // ARIA_OPCODE_PROFILE

#endif // ARIA_OPCODE_PROFILE_HPP
