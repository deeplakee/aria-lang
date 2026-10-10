#include "runtime/ObjMovement.hpp"

#include <format>

#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjList.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/Eval.hpp"

namespace aria {
    namespace {

        // 元数规格措辞:0 -> "no arguments"、1 -> "1 argument"、N -> "N arguments"(单数只在恰好 1)。
        String arity_phrase(const usize count) {
            if (count == 0) {
                return "no arguments";
            }
            if (count == 1) {
                return "1 argument";
            }
            return std::format("{} arguments", count);
        }

    } // namespace

    // 帧成员 mark_object 的基类转换需完整类型,故住 .cpp。
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

    void ObjMovement::enter_frame(ObjClosure* closure, const u8 argc) {
        CallFrame::init(frames_.acquire(), closure, top_, argc);
    }

    // 「同一局部只有一份引用」不变式由此收口;建新到插链之间无任何分配点,入链后即随本对象
    // trace 保命。
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

    // 关闭所有指向 >= from 槽址的开指:值迁入各自 closed_ 并整段摘链,降序不变式下 >= from 为
    // 链头连续前缀,遇首个 < from 即停;摘下的节点 next_open_ 清空 -- 已离链的陈旧链指针防误遍历。
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

    // 值栈 2x 扩容:旧基址一律失效,指入旧块的三类指针 top_/活动帧 slots/开指 location_ 须重绑
    // -- 调 reserve 前算好相对旧基址的槽偏移、调后用「新基址 + 偏移」重建,全程不读 dangling
    // 指针(对 dangling 指针做指针减法是 UB);暂存用 List(与 GC 自身 scratch 容器同款)。
    void ObjMovement::grow_stack_() noexcept {
        const auto old_base    = buf_.data();
        const auto frame_count = frames_.size();

        // 扩容前:记 top_/各活动帧 slots 相对 old_base 的槽偏移(偏移而非绝对指针 --
        // 扩容后旧基址失效,绝对指针成 dangling 不可再用)。
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

        buf_.reserve(buf_.capacity() * 2); // 扩容:旧基址失效,新基址就位(可能原地)

        const auto new_base = buf_.data();
        if (new_base == old_base) {
            return; // 原地扩展,无需重绑(realloc 允许原地;此分支现是活路径,非防御性保留)
        }

        // 搬迁后:用「新基址 + 偏移」重算指针,不读任何 dangling 指针值。
        top_ = new_base + top_offset;
        for (usize i = 0; i < frame_count; ++i) {
            frames_[i].slots = new_base + slot_offsets[i];
        }
        usize i = 0;
        for (ObjUpvalue* upvalue = open_upvalues_; upvalue != nullptr; upvalue = upvalue->next_open()) {
            upvalue->set_location(new_base + upvalue_offsets[i++]);
        }
    }

    ObjMovement* new_movement(GC& gc, AriaVM& host) { return gc.new_object<ObjMovement>(&gc, &host); }

    // ctx 服务面:报错族真身在此(写本上下文自己的寄存器),其余委托宿主公有面。

    FailSignal ObjMovement::arity_error(const usize argc, const usize expected) {
        return fail(ErrorCode::WrongArity, "function expects {}, got {}", arity_phrase(expected), argc);
    }

    FailSignal ObjMovement::arity_error_range(const usize argc, const usize low, const usize high) {
        // 区间恒复数(n or m arguments):"0 or 1 argument" 这类读起来像单数,不成句。
        return fail(ErrorCode::WrongArity, "function expects {} or {} arguments, got {}", low, high, argc);
    }

    FailSignal ObjMovement::arity_error_at_least(const usize argc, const usize low) {
        return fail(ErrorCode::WrongArity, "function expects at least {}, got {}", arity_phrase(low), argc);
    }

    bool ObjMovement::check_arity(const ObjFunction* fn, const u8 argc) {
        // 契约见 ObjMovement.hpp;检查纯读,无分配。
        const auto arity     = fn->arity();
        const auto min_arity = fn->min_arity();
        if (fn->is_varargs()) {
            if (argc < min_arity) {
                return arity_error_at_least(argc, min_arity);
            }
            return true;
        }
        if (argc < min_arity || argc > arity) {
            if (min_arity == arity) {
                return arity_error(argc, arity);
            }
            return arity_error_range(argc, min_arity, arity);
        }
        return true;
    }

    u8 ObjMovement::prepare_call_args(const ObjFunction* fn, const u8 argc) {
        // GC 走查:new_list 为唯一分配点 -- 额外实参 peek 在栈(栈即根),copy_from 走 trivial
        // 分配不触 GC,白色 list 随即 drop+push 入值栈根,窗口内无 GC 点;垫充压寄存器单例无分配。
        const auto arity = fn->arity();

        // ① 缺省垫充(仅当实参不足固定参数数;varargs 的 argc 可超 arity,差值不可作 u8 减)。
        const auto missing = argc < arity ? static_cast<u8>(arity - argc) : u8{0};
        for (u8 i = 0; i < missing; ++i) {
            push(register_value(kDefaultMarkOffset));
        }

        // ② varargs 打包:rest 槽深 = arity + 1;普通函数槽深 = arity。
        if (fn->is_varargs()) {
            const auto extras = argc > arity ? static_cast<u8>(argc - arity) : u8{0};
            const auto rest   = new_list(gc());
            rest->elements().copy_from({stack_top() - extras, extras});
            drop(extras);
            push(Value::from_obj(rest));
            return static_cast<u8>(arity + 1);
        }
        return arity;
    }

    Opt<Value> ObjMovement::execute_closure(ObjClosure* closure, const Span<const Value> args) {
        return Eval::execute_call(*this, Value::from_obj(closure), closure, args);
    }

    Opt<Value> ObjMovement::execute_method(const Value receiver, ObjClosure* method, const Span<const Value> args) {
        return Eval::execute_call(*this, receiver, method, args);
    }

    ObjString* ObjMovement::char_string(const u8 byte) const noexcept { return host_->char_string(byte); }

    ObjString* ObjMovement::string_constant(const usize index) const noexcept { return host_->string_constant(index); }

    Value ObjMovement::register_value(const u8 offset) const noexcept { return host_->register_value(offset); }

    ObjClass* ObjMovement::object_class() const noexcept { return host_->object_class(); }

    ObjClass* ObjMovement::exception_class() const noexcept { return host_->exception_class(); }

    ObjClass* ObjMovement::iterator_class() const noexcept { return host_->iterator_class(); }

    ObjClass* ObjMovement::list_class() const noexcept { return host_->list_class(); }

    ObjClass* ObjMovement::map_class() const noexcept { return host_->map_class(); }

    ObjClass* ObjMovement::string_class() const noexcept { return host_->string_class(); }

    ObjClass* ObjMovement::range_class() const noexcept { return host_->range_class(); }

    GC& ObjMovement::gc() noexcept { return host_->gc(); }

} // namespace aria
