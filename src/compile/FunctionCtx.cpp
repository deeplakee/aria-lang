#include "compile/FunctionCtx.hpp"

#include <ranges>

#include "object/ObjFunction.hpp"

namespace aria {

    // ============================================================
    // 构造
    // ============================================================

    FunctionCtx::FunctionCtx(ObjFunction& fn) : enclosing_{nullptr}, fn_{&fn}, scope_depth_{0} {
        auto this_ = Local{.name = String{}, .depth = 0, .is_captured = false, .is_initialized = true};
        locals_.push_back(std::move(this_)); // 哑元：slot 0 = callee
    }

    FunctionCtx::FunctionCtx(FunctionCtx& enclosing, ObjFunction& fn) :
        enclosing_{&enclosing}, fn_{&fn}, scope_depth_{0} {
        auto this_ = Local{.name = String{}, .depth = 0, .is_captured = false, .is_initialized = true};
        locals_.push_back(std::move(this_)); // 哑元：slot 0 = callee
    }

    // ============================================================
    // 局部 / 作用域
    // ============================================================

    u16 FunctionCtx::add_local(const StringView name) {
        auto local = Local{.name = String{name}, .depth = scope_depth_, .is_captured = false, .is_initialized = false};
        locals_.push_back(std::move(local));
        return static_cast<u16>(locals_.size() - 1);
    }

    bool FunctionCtx::is_defined_in_scope(const StringView name) const {
        for (const auto& local: std::views::reverse(locals_)) {
            if (local.depth < scope_depth_) {
                return false; // 外层同名允许 shadow
            }
            if (local.name == name) {
                return true;
            }
        }
        return false;
    }

    void FunctionCtx::mark_initialized(const u16 slot) { locals_[slot].is_initialized = true; }

    bool FunctionCtx::is_initialized(const u16 slot) const { return locals_[slot].is_initialized; }

    Opt<u16> FunctionCtx::find_local(const StringView name) const {
        // 从内向外查找（高索引 = 更内层作用域），首个命中即最内层同名局部，早退。
        // 同名局部必处不同作用域（is_defined_in_scope 禁同作用域重名），内层后声明故索引更高。
        // std::views::reverse 从末尾（内层）向前遍历；i 从 size 倒数，--i 后即当前元素的原始索引（slot）。
        // （std::views::enumerate 可一步配对索引但本环境 libc++ 尚未提供，故用 reverse + 倒数计数器。）
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

    u32 FunctionCtx::end_scope_pop_count() {
        --scope_depth_; // 退出当前作用域；之后弹「比新 scope_depth_ 更深的局部」即原 scope 的局部
        return pop_locals_deeper_than(scope_depth_);
    }

    u32 FunctionCtx::pop_locals_deeper_than(const u32 target_depth) {
        // 仅弹 depth > target 的局部;slot 0 哑元(depth=0)因 0 <= 任意 u32 target_depth 而
        // 永不满足弹出条件,故 !empty() 足够--与 count_locals_deeper_than 共用同一深度不变式。
        u32 count = 0;
        while (!locals_.empty() && locals_.back().depth > target_depth) {
            locals_.pop_back();
            ++count;
        }
        return count;
    }

    u32 FunctionCtx::count_locals_deeper_than(const u32 target_depth) const {
        // 与 pop_locals_deeper_than 同形,但仅计数不弹出:break/continue 后的语句仍在作用域内,
        // 编译期 locals_ 须保持完整(只有 end_scope 才真正移除)。从末尾(最内层)向前数,遇 depth<=target 即停
        // (活局部按 depth 非递减序排列,见 pop_locals_deeper_than 的不变式)。
        u32 count = 0;
        for (const auto& local: std::views::reverse(locals_)) {
            if (local.depth <= target_depth) {
                return count;
            }
            ++count;
        }
        return count;
    }

} // namespace aria
