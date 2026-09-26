#include "runtime/ObjMovement.hpp"

#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjUpvalue.hpp"

namespace aria {

    // 清单见 ObjMovement.hpp trace 注释;帧成员 mark_object 的基类转换需完整类型,故住 .cpp。
    void ObjMovement::trace(GC& gc) const noexcept {
        for (auto p = buf_.data(); p < top_; ++p) {
            gc.mark_value(*p); // mark_value 对非对象 Value no-op,栈槽含 int/f64/bool/nil 安全
        }
        for (const auto& frame: frames_.span()) {
            gc.mark_object(frame.closure); // trace 级联标 function/upvalues;容 nullptr
            gc.mark_object(frame.module);
        }
        // 开链节点可能仅被本链引用(闭包已死),须单独标(mark 幂等,双标无害)。
        for (auto upvalue = open_upvalues_; upvalue != nullptr; upvalue = upvalue->next_open()) {
            gc.mark_object(upvalue);
        }
        if (const auto& pending = pending_error()) {
            gc.mark_value(*pending);
        }
        gc.mark_object(previous_); // resume 链链上成员经灰栈级联,容 nullptr
    }

    // 定义在 .cpp:init_frame_ 解引用 closure 需 ObjClosure 完整类型,头文件仅前向声明即可。
    void ObjMovement::enter_frame(ObjClosure* closure, const u8 argc) {
        CallFrame& f = frames_.acquire();
        init_frame_(f, closure, argc);
    }

    // 就位刚 acquire 的栈顶空帧:slots 按不变量设为 top - argc - 1(栈顶 [callee, a1..aN]),
    // VM 专有字段从 closure 解引用填充。槽 0 语义见 ObjMovement.hpp enter_frame 注释。
    void ObjMovement::init_frame_(CallFrame& f, ObjClosure* closure, const u8 argc) const {
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

    // 捕获单点:「同一局部只有一份引用」不变式由此收口(链序与自愈说明见 ObjMovement.hpp)。
    // 建新路径:new_upvalue 返回白色无根,到插链之间无任何分配点,入链后即随本对象 trace 保命。
    ObjUpvalue* ObjMovement::capture_upvalue(GC& gc, Value* slot) noexcept {
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
    // 降序不变式下 >= from 恒为链头连续前缀,遇首个 < from 即停;摘下的节点 next_open_ 清空(close
    // 后节点已离链,陈旧链指针无意义,防误遍历)。
    void ObjMovement::close_upvalues(const Value* from) noexcept {
        ObjUpvalue* uv = open_upvalues_;
        while (uv != nullptr && uv->value_slot() >= from) {
            ObjUpvalue* next = uv->next_open();
            uv->close();
            uv->set_next_open(nullptr);
            uv = next;
        }
        open_upvalues_ = uv;
    }

    // 值栈 2x 扩容:经 Buffer::reserve -> GC reallocate 搬迁。reallocate 释放旧块,故指入旧块的
    // top_/活动帧 slots/open upvalue location_ 三类指针须重绑:搬运**前**(old_base 仍存活)算好
    // 相对 old_base 的槽偏移,搬运**后**用「新基址 + 偏移」重建,全程不触碰 dangling 指针(对
    // dangling 指针做指针减法是 UB,[expr.add] p5;偏移须在搬运前算好,见 Buffer::reserve 注释)。
    // open upvalue 链偏移按链序平行存取;暂存用 List(与 GC 自身 scratch 容器同款)。
    void ObjMovement::grow_stack_() noexcept {
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

    ObjMovement* new_movement(GC& gc) { return gc.new_object<ObjMovement>(&gc); }

} // namespace aria
