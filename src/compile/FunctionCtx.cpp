#include "compile/FunctionCtx.hpp"

#include <ranges>

#include "object/ObjFunction.hpp"

namespace aria {

    FunctionCtx::FunctionCtx(ObjFunction* fn, FunctionCtx* enclosing, const FnKind kind) :
        enclosing_{enclosing}, fn_{fn}, kind_{kind}, scope_depth_{0} {
        // 槽 0 语义见 ctor 头注与 kThisName 注；非方法族的哑元 callee 空名、词法不可达、不可引用。
        auto name  = is_method(kind) ? String{kThisName} : String{};
        auto slot0 = Local{.name = std::move(name), .depth = 0, .is_captured = false};
        locals_.push_back(std::move(slot0));
    }

    u16 FunctionCtx::add_local(const StringView name) {
        auto local = Local{.name = String{name}, .depth = scope_depth_, .is_captured = false};
        locals_.push_back(std::move(local));
        return static_cast<u16>(locals_.size() - 1);
    }

    u16 FunctionCtx::add_constant(const Value value) {
        if (const auto entry = constant_index_.find(value); entry != constant_index_.end()) {
            return entry->second;
        }
        // 先入池后登记:索引不是 GC 根,键的保活只靠池(入池是 trivial 分配,不触 GC,见 CodeGen 类首注)。
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
        // 从内向外查找（高索引 = 更内层作用域），首个命中即最内层同名局部，早退。
        // std::views::reverse 从末尾向前遍历；i 从 size 倒数，--i 后即当前元素原始索引（slot）。
        // （libc++ 尚未提供 std::views::enumerate，故用 reverse + 倒数计数器。）
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
        // 弹出 depth > 新 scope_depth_ 的局部（不变式与 break/continue 例外见头文件 end_scope 注）。
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
        // 容量判定 > kMaxUpvalues 的语义见 FunctionCtx.hpp kMaxUpvalues 注。
        if (upvalues_.size() > kMaxUpvalues) {
            return std::nullopt;
        }
        upvalues_.push_back(desc);
        return static_cast<u8>(upvalues_.size() - 1);
    }

} // namespace aria
