#include "compile/FunctionCtx.hpp"

#include <ranges>

#include "object/ObjFunction.hpp"

namespace aria {

    FunctionCtx::FunctionCtx(ObjFunction* fn, FunctionCtx* enclosing, const FnKind kind) :
        enclosing_{enclosing}, fn_{fn}, kind_{kind}, scope_depth_{0} {
        // 非方法族的哑元 callee 空名，词法不可达、不可引用。
        const auto name = is_method(kind) ? kThisName : StringView{};
        locals_.push_back(Local{.name = name, .depth = 0, .is_captured = false});
    }

    u16 FunctionCtx::add_local(const StringView name) {
        locals_.push_back(Local{.name = name, .depth = scope_depth_, .is_captured = false});
        return static_cast<u16>(locals_.size() - 1);
    }

    u16 FunctionCtx::add_constant(const Value value) {
        if (const auto entry = constant_index_.find(value); entry != constant_index_.end()) {
            return entry->second;
        }
        // 先入池后登记：键的保活只靠池，入池为 trivial 分配不触 GC。
        const auto index = fn_->unit().add_constant(value);
        constant_index_.emplace(value, index);
        return index;
    }

    bool FunctionCtx::is_defined_in_scope(const StringView name) const {
        for (const auto& local: std::views::reverse(locals_)) {
            if (local.depth < scope_depth_) {
                return false;
            }
            if (local.name == name) {
                return true;
            }
        }
        return false;
    }

    Opt<u16> FunctionCtx::find_local(const StringView name) const {
        // 从内向外找，首个命中即最内层；倒数计数器代替 views::enumerate（libc++ 未有），--i 即原始 slot。
        usize i = locals_.size();
        for (const auto& local: std::views::reverse(locals_)) {
            --i;
            if (local.name == name) {
                return static_cast<u16>(i);
            }
        }
        return std::nullopt;
    }

    void FunctionCtx::begin_scope() { ++scope_depth_; }

    void FunctionCtx::end_scope() {
        --scope_depth_;
        while (!locals_.empty() && locals_.back().depth > scope_depth_) {
            locals_.pop_back();
        }
    }

    Opt<u8> FunctionCtx::add_upvalue(const UpvalueDesc desc) {
        for (usize i = 0; i < upvalues_.size(); ++i) {
            if (upvalues_[i] == desc) {
                return static_cast<u8>(i);
            }
        }
        if (upvalues_.size() > kMaxUpvalues) {
            return std::nullopt;
        }
        upvalues_.push_back(desc);
        return static_cast<u8>(upvalues_.size() - 1);
    }

} // namespace aria
