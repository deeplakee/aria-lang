#include "runtime/Movement.hpp"

#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjUpvalue.hpp"

namespace aria {

    // 定义在 .cpp:init_frame_ 解引用 closure 需 ObjClosure 完整类型,头文件仅前向声明即可
    // (避免 Movement.hpp 拖入 object/bytecode 树)。
    void Movement::enter_frame(ObjClosure* closure, const u8 argc) {
        CallFrame& f = frames_.acquire();
        init_frame_(f, closure, argc);
    }

    // 就位刚 acquire 的栈顶空帧:slots 按不变量设为 top - argc - 1(栈顶 [callee, a1..aN]),
    // VM 专有字段从 closure 解引用填充(unit/module 缓存其 function 的,module 供 *_GLOBAL
    // 定位模块 globals,ip 指向 function 字节码起始)。槽 0 语义见 Movement.hpp enter_frame 注释。
    void Movement::init_frame_(CallFrame& f, ObjClosure* closure, const u8 argc) const {
        const auto fn = closure->function();
        f.slots       = top_ - argc - 1;
        f.closure     = closure;
        f.unit        = &fn->unit();
        f.module      = fn->module();
        f.ip          = fn->unit().code.data();
        // 位置锚点占位(= code 起始,等价 offset 0),主循环取指前即覆写。不用 nullptr:
        // 与 data() 相减是 UB。(无 NSDMI,保 trivial 聚合。)
        f.last_ip = f.ip;
    }

    // 捕获单点:「同一局部只有一份引用」不变式由此收口(链序与自愈说明见 Movement.hpp)。
    // 建新路径:new_upvalue 返回白色无根,到插链之间无任何分配点(new_object 顶 maybe_collect
    // 已过,插链纯指针操作),入链后即经 VM 根 tracer 保命。
    ObjUpvalue* Movement::capture_upvalue(GC& gc, Value* slot) noexcept {
        ObjUpvalue* prev = nullptr;
        ObjUpvalue* cur  = open_upvalues_;
        while (cur != nullptr && cur->value_slot() > slot) {
            prev = cur;
            cur  = cur->next_open();
        }
        if (cur != nullptr && cur->value_slot() == slot) {
            return cur; // 同槽已捕获:复用(内外层共享同一份引用)
        }
        ObjUpvalue* uv = new_upvalue(gc, slot);
        uv->set_next_open(cur);
        if (prev == nullptr) {
            open_upvalues_ = uv;
        } else {
            prev->set_next_open(uv);
        }
        return uv;
    }

    // 闭所有指向 >= from 槽址的开指:值迁入各自 closed_(close,location_ 转指自持)并整段摘链。
    // 降序不变式下 >= from 恒为链头连续前缀,遇首个 < from 即停;摘下的节点 next_open_ 清空
    // (close 后节点已离链,陈旧链指针无意义,防误遍历)。
    void Movement::close_upvalues(const Value* from) noexcept {
        ObjUpvalue* uv = open_upvalues_;
        while (uv != nullptr && uv->value_slot() >= from) {
            ObjUpvalue* next = uv->next_open();
            uv->close();
            uv->set_next_open(nullptr);
            uv = next;
        }
        open_upvalues_ = uv;
    }

    // 值栈 2x 扩容:经 Buffer::reserve -> GC reallocate 搬迁(内部 memcpy)。reallocate 释放
    // 旧块,故指入旧块的 top_/活动帧 slots/open upvalue location_ 三类指针须重绑:搬运**前**
    // (old_base 仍存活、指针减法有定义)算好相对 old_base 的槽偏移,搬运**后**用
    // 「新基址 + 偏移」重建,全程不触碰 dangling 指针(对 dangling 指针做指针减法是 UB,
    // [expr.add] p5;偏移须在搬运前算好,见 Buffer::reserve 注释)。
    //
    // open upvalue 链是第三类重绑(M4):链节点是 GC 对象(非移动,mark-sweep 不搬块),链序
    // 两趟间稳定,偏移按链序平行存取;暂存用 List(std::vector,与 GC 自身 scratch 容器同款;
    // push_back 在 noexcept 函数内理论可抛 bad_alloc 终止进程 -- OOM 已是死局,与 Guard::push
    // 的既有取舍一致)。
    void Movement::grow_stack_() noexcept {
        const auto old_base    = buf_.data();
        const auto frame_count = frames_.size();

        // 搬运前:记 top_/各活动帧 slots 相对 old_base 的槽偏移(偏移而非绝对指针 --
        // 搬运后旧块释放,绝对指针成 dangling 不可再用)。
        const auto top_offset = static_cast<usize>(top_ - old_base);

        usize slot_offsets[kFrameMax];
        for (usize i = 0; i < frame_count; ++i) {
            slot_offsets[i] = static_cast<usize>(frames_[i].slots - old_base);
        }

        // 开链偏移:走链记偏移。
        List<usize> upvalue_offsets;
        for (ObjUpvalue* upvalue = open_upvalues_; upvalue != nullptr; upvalue = upvalue->next_open()) {
            upvalue_offsets.push_back(static_cast<usize>(upvalue->value_slot() - old_base));
        }

        buf_.reserve(buf_.capacity() * 2); // 搬迁:旧块释放,新块就位

        const auto new_base = buf_.data();
        if (new_base == old_base) {
            return; // 原地扩容,无需重绑(当前 reallocate 恒换块,此分支为防御性保留)
        }

        // 搬运后:用「新基址 + 偏移」重算指针,不读任何 dangling 指针值。
        top_ = new_base + top_offset;
        for (usize i = 0; i < frame_count; ++i) {
            frames_[i].slots = new_base + slot_offsets[i];
        }
        usize i = 0;
        for (ObjUpvalue* upvalue = open_upvalues_; upvalue != nullptr; upvalue = upvalue->next_open()) {
            upvalue->set_location(new_base + upvalue_offsets[i++]);
        }
    }

} // namespace aria
