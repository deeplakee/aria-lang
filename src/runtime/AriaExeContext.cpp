#include "runtime/AriaExeContext.hpp"

#include <format>

#include "object/ObjFnProto.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjList.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/Eval.hpp"

namespace aria {
    namespace {

        String arity_phrase(const u32 count) {
            if (count == 0) {
                return "no arguments";
            }
            if (count == 1) {
                return "1 argument";
            }
            return std::format("{} arguments", count);
        }

    } // namespace

    FailSignal AriaExeContext::arity_error(const u32 argc, const u32 expected) {
        return fail(ErrorCode::WrongArity, "function expects {}, got {}", arity_phrase(expected), argc);
    }

    FailSignal AriaExeContext::arity_error_range(const u32 argc, const u32 low, const u32 high) {
        return fail(ErrorCode::WrongArity, "function expects {} or {} arguments, got {}", low, high, argc);
    }

    FailSignal AriaExeContext::arity_error_at_least(const u32 argc, const u32 low) {
        return fail(ErrorCode::WrongArity, "function expects at least {}, got {}", arity_phrase(low), argc);
    }

    bool AriaExeContext::check_arity(const ObjFnProto* proto, const u8 argc) {
        const auto arity     = proto->arity();
        const auto min_arity = proto->min_arity();
        if (proto->is_varargs()) {
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

    u8 AriaExeContext::prepare_call_args(const ObjFnProto* proto, const u8 argc) {
        const auto arity = proto->arity();

        const auto missing = argc < arity ? static_cast<u8>(arity - argc) : u8{0};
        for (u8 i = 0; i < missing; ++i) {
            push(Value::from_obj(host_->default_mark()));
        }

        if (proto->is_varargs()) {
            const auto extras = argc > arity ? static_cast<u8>(argc - arity) : u8{0};
            const auto rest   = new_list(gc());
            rest->elements().copy_from({stack_top() - extras, extras});
            drop(extras);
            push(Value::from_obj(rest));
            return static_cast<u8>(arity + 1);
        }
        return arity;
    }

    Opt<Value> AriaExeContext::execute_function(ObjFunction* function, const ArgList args) {
        return Eval::execute_function(*this, Value::from_obj(function), function, args);
    }

    Opt<Value> AriaExeContext::execute_method(const Value receiver, ObjFunction* method, const ArgList args) {
        return Eval::execute_function(*this, receiver, method, args);
    }

    void AriaExeContext::enter_frame(ObjFunction* function, const u8 argc) {
        CallFrame::init(frames_.acquire(), function, top_, argc);
    }

    ObjUpvalue* AriaExeContext::capture_upvalue(Value* slot) noexcept {
        ObjUpvalue* prev = nullptr;
        ObjUpvalue* cur  = open_upvalues_;
        while (cur != nullptr && cur->value_slot() > slot) {
            prev = cur;
            cur  = cur->next_open();
        }
        if (cur != nullptr && cur->value_slot() == slot) {
            return cur;
        }
        ObjUpvalue* uv = new_upvalue(gc(), slot);
        uv->set_next_open(cur);
        if (prev == nullptr) {
            open_upvalues_ = uv;
        } else {
            prev->set_next_open(uv);
        }
        return uv;
    }

    void AriaExeContext::close_upvalues(const Value* from) noexcept {
        ObjUpvalue* uv = open_upvalues_;
        while (uv != nullptr && uv->value_slot() >= from) {
            ObjUpvalue* next = uv->next_open();
            uv->close();
            uv->set_next_open(nullptr);
            uv = next;
        }
        open_upvalues_ = uv;
    }

    // 须在 reserve 前记录 top、帧槽和 upvalue 的偏移，搬迁后从新基址重建。
    // 旧指针已失效，不能在搬迁后相减求偏移。
    void AriaExeContext::grow_stack_() noexcept {
        const auto old_base    = buf_.data();
        const auto frame_count = frames_.size();

        const auto top_offset = static_cast<usize>(top_ - old_base);

        usize slot_offsets[kFrameMax];
        for (usize i = 0; i < frame_count; ++i) {
            slot_offsets[i] = static_cast<usize>(frames_[i].slots - old_base);
        }

        List<usize> upvalue_offsets;
        for (ObjUpvalue* upvalue = open_upvalues_; upvalue != nullptr; upvalue = upvalue->next_open()) {
            upvalue_offsets.push_back(static_cast<usize>(upvalue->value_slot() - old_base));
        }

        buf_.reserve(buf_.capacity() * 2);

        const auto new_base = buf_.data();
        if (new_base == old_base) {
            return;
        }

        top_ = new_base + top_offset;
        for (usize i = 0; i < frame_count; ++i) {
            frames_[i].slots = new_base + slot_offsets[i];
        }
        usize i = 0;
        for (ObjUpvalue* upvalue = open_upvalues_; upvalue != nullptr; upvalue = upvalue->next_open()) {
            upvalue->set_location(new_base + upvalue_offsets[i++]);
        }
    }

    ObjString* AriaExeContext::char_string(const u8 byte) const noexcept { return host_->char_string(byte); }

    ObjString* AriaExeContext::str(const usize index) const noexcept { return host_->str(index); }

    ObjClass* AriaExeContext::object_class() const noexcept { return host_->object_class(); }

    ObjClass* AriaExeContext::exception_class() const noexcept { return host_->exception_class(); }

    ObjClass* AriaExeContext::iterator_class() const noexcept { return host_->iterator_class(); }

    ObjClass* AriaExeContext::list_class() const noexcept { return host_->list_class(); }

    ObjClass* AriaExeContext::map_class() const noexcept { return host_->map_class(); }

    ObjClass* AriaExeContext::string_class() const noexcept { return host_->string_class(); }

    ObjClass* AriaExeContext::range_class() const noexcept { return host_->range_class(); }

    ObjClass* AriaExeContext::function_class() const noexcept { return host_->function_class(); }

    ObjClass* AriaExeContext::bound_method_class() const noexcept { return host_->bound_method_class(); }

    ObjClass* AriaExeContext::module_class() const noexcept { return host_->module_class(); }

    ObjClass* AriaExeContext::coroutine_class() const noexcept { return host_->coroutine_class(); }

    ObjClass* AriaExeContext::class_class() const noexcept { return host_->class_class(); }

    GC& AriaExeContext::gc() const noexcept { return host_->gc(); }

} // namespace aria
