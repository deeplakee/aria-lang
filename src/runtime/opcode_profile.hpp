#ifndef ARIA_OPCODE_PROFILE_HPP
#define ARIA_OPCODE_PROFILE_HPP

// dispatch_loop 取指行的唯一接入点(常态构建展开式与原取指表达式逐字一致;探针构建展开为
// 成员 opcode_profiler_.fetch(*this, frame),AriaVM.cpp 除该行外零探针代码)。
#ifndef ARIA_OPCODE_PROFILE

    #define ARIA_FETCH_OPCODE(frame) static_cast<OpCode>(read_u8(frame))

#else

    // 探针构建取指:仅可在 AriaVM 成员函数内展开(依赖成员 opcode_profiler_);fetch 按 read_u8
    // 同款语义取走 opcode 字节并返回,操作数消费仍归各 case。
    #define ARIA_FETCH_OPCODE(frame) opcode_profiler_.fetch(*this, frame)

    #include "bytecode/code.hpp"
    #include "common.hpp"

namespace aria {

    class AriaVM;
    struct CallFrame;
    class Object;
    class ObjString;
    enum class ObjType : u8;

    // 指令频度探针:计数状态全部本类自持(每 VM 一份,不跨 VM 混账);解码与 Disassembler 同源
    // -- 位宽取 kOpCodeFormats、名字渲染用 kOpCodeNames。
    class OpcodeProfiler {
    public:
        // 析构即产出:门控在析构而非 run() 出口,使 AriaVM 侧除取指行外无任何探针代码。
        ~OpcodeProfiler();

        // 取指 + 记账:先按 read_u8 同款语义取走 opcode 并返回(调用方与普通取指逐位一致),
        // 再在与 dispatch_loop 同形的 switch 里记自己的账 -- 直线型记 pairs_(下一取指恒落在
        // 指令结束地址,该处一个字节即后继 opcode);条件跳转按栈顶极性判走向(命中率
        // branch_taken_,落空记 pairs_、跳走记 jt_);无条件跳转目标记 jt_;帧切换五指令的落点
        // 在下一取指经 prev_op_ 补记派发对。另有操作数直方图与调用面记账。
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

        // 上一条 opcode,哨兵 = HALT(其后的行首边本就不是相邻执行);帧切换五指令的落点经它补记。
        // 只存值不存地址 -- REPL 行边界上一行单元可能已被 GC 回收,解引用上一条地址是 UB,
        // 而补记只需要“上一条是谁”。
        OpCode prev_op_ = OpCode::HALT;
    };

} // namespace aria

#endif // ARIA_OPCODE_PROFILE

#endif // ARIA_OPCODE_PROFILE_HPP
