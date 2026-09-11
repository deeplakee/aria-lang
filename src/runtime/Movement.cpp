#include "runtime/Movement.hpp"

#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjUpvalue.hpp"

namespace aria {

    // 进帧 = acquire 空帧 + 就位该帧。定义在此(.cpp)而非头文件:init_frame_ 解引用 closure
    // 需 ObjClosure 完整类型(function() 取 unit/module),头文件仅前向声明即可,
    // 避免 Movement.hpp 拖入 object/bytecode 树。
    void Movement::enter_frame(ObjClosure* closure, const u8 argc) {
        CallFrame& f = frames_.acquire();
        init_frame_(f, closure, argc);
    }

    // 修改栈顶帧(刚 acquire 的空帧)为对 closure 的调用。slots 按不变量设为 top - argc - 1
    // (栈顶 [callee, a1..aN]:slots 指向槽 0);VM 专有字段从 closure 解引用填充
    // (unit/module 缓存其 function 的,ip 指向 function 字节码起始)。
    // 槽 0 的内容由调用方在进帧前写定:普通函数帧 = 闭包自身(栈上的 callee)、
    // 方法帧 = this(接收者替代 bound 对象;闭包经 frame.closure 携带不上栈)。
    // module 缓存 fn->module(),供 LOAD/STORE/DEF_GLOBAL 定位当前模块 globals;
    // ip 指向 fn 字节码起始(RETURN/异常时按 ip 算 offset)。
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

    // 捕获单点(单趟):「同一局部只有一份引用」不变式(「捕获即引用」的共享保证)由此收口。
    // 沿降序链一趟同时完成两件事 -- 走到首个槽址 <= slot 的节点即停:等值即复用(命中返回,
    // 不再走第二趟找插入点);更小/链尾即在该处插链。若不变式被破出现同槽双节点,本实现命中
    // 复用而非再插一个(自愈,不放大错误)。建新路径:gc 由调用方(VM 驱动)传入,Movement
    // 不自持分配器创建对象;new_upvalue 返回白色无根,到插链之间无任何分配点(new_object 顶
    // maybe_collect 已过,插链纯指针操作),入链后即经 VM 根 tracer 保命。
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
    // 旧块,故 top_ 与各活动帧的 slots(原指旧块)须重绑到新块。重绑策略:在搬运**前**(old_base
    // 仍存活、指针减法有定义)算好 top_ 与各 slots 相对 old_base 的槽偏移(纯整数),搬运**后**
    // 用「新基址 + 偏移」重算指针。如此搬运后不再触碰任何 dangling 指针 -- 旧块释放后 top_/
    // slots/old_base 皆成 dangling,对其做指针减法(如 new_base + (p - old_base))是 UB
    // ([expr.add] p5),读其值为实现定义(非 UB)但亦无必要;故偏移必须在搬运前算好(见
    // Buffer::reserve 注释)。活动帧的 slots 必为指入旧块的有效指针(非空),偏移在 [0, cap)
    // 内。若 reallocate 原地扩容(new_base == old_base)则无需重绑、直接返回 -- 当前
    // GC::reallocate 为「先分配新块再释放旧块」,new_base 不可能等于 old_base,此分支为防御性
    // 保留,供将来支持原地扩容的 reallocate。
    // open upvalue 链是第三类重绑(M4):链上 location_ 同样指入旧块,同法偏移两趟 -- 搬运前
    // 走链记 location_ - old_base,搬运后逐节点 set_location(new_base + offset)。链节点是 GC
    // 对象(非移动,mark-sweep 不搬块),链序两趟间稳定,偏移按链序平行存取;暂存用 List
    // (std::vector,与 GC 自身 scratch gray_stack_/temp_roots_ 同款,计划原文即 List<usize>;
    // push_back 在 noexcept 函数内理论可抛 bad_alloc 终止进程 -- OOM 已是死局,与 Guard::push
    // (noexcept + push_back) 的既有取舍一致,顺带免去数节点一趟,链两趟即完)。
    void Movement::grow_stack_() noexcept {
        const auto old_base    = buf_.data();
        const auto frame_count = frames_.size();

        // 搬运前:old_base 仍存活,此时把 top_ 与各活动帧 slots 到 old_base 的偏移算成整数。
        // 偏移而非绝对指针 -- 搬运后旧块释放,绝对指针成 dangling 不可再用。

        // 栈顶偏移(栈实际占用大小)
        const auto top_offset = static_cast<usize>(top_ - old_base);

        // 各活动帧 slots 偏移
        usize slot_offsets[kFrameMax];
        for (usize i = 0; i < frame_count; ++i) {
            slot_offsets[i] = static_cast<usize>(frames_[i].slots - old_base);
        }

        // 开链偏移:走链记偏移(节点非移动,链序两趟间稳定)。
        List<usize> uv_offsets;
        for (ObjUpvalue* uv = open_upvalues_; uv != nullptr; uv = uv->next_open()) {
            uv_offsets.push_back(static_cast<usize>(uv->value_slot() - old_base));
        }

        buf_.reserve(buf_.capacity() * 2); // 搬迁:旧块释放,新块就位

        const auto new_base = buf_.data();
        if (new_base == old_base) {
            return; // 原地扩容,基址未变,无需重绑(偏移随 vector 析构释放)
        }

        // 搬运后:用「新基址 + 偏移」重算指针,不读任何 dangling 指针值。
        top_ = new_base + top_offset;
        for (usize i = 0; i < frame_count; ++i) {
            frames_[i].slots = new_base + slot_offsets[i];
        }
        usize i = 0;
        for (ObjUpvalue* uv = open_upvalues_; uv != nullptr; uv = uv->next_open()) {
            uv->set_location(new_base + uv_offsets[i++]);
        }
    }

} // namespace aria
