#ifndef ARIA_INTERN_POOL_HPP
#define ARIA_INTERN_POOL_HPP

#include <cstring>

#include "common.hpp"
#include "memory/Allocator.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

namespace aria {

    // 字符串驻留池：字符串专用集合，键即串内容（等价内容共享同一 ObjString*）。
    // weak root：表项不保证保命，GC 在 sweep 前经 remove_white() 摘除指向白色串的表项。
    template<typename Alloc = GC>
    class InternPool {
        ObjString** slots_;
        Alloc*      alloc_;
        usize       cap_; // 2 的幂(或 0)
        usize       count_;
        usize       tombstones_;

        static constexpr usize kInitialCap = 8;
        static constexpr usize kNpos       = static_cast<usize>(-1);

        [[nodiscard]]
        static ObjString* tombstone() noexcept {
            return reinterpret_cast<ObjString*>(static_cast<usize>(0x1));
        }

        [[nodiscard]]
        static bool is_tombstone(const ObjString* p) noexcept {
            return p == tombstone();
        }

    public:
        explicit InternPool(Alloc* alloc) noexcept :
            slots_{nullptr}, alloc_{alloc}, cap_{0}, count_{0}, tombstones_{0} {
            // static_assert 不在模板头而在 ctor:类体实例化(InternPool<GC> 为 GC 的值成员)时 GC 尚不完整,
            // 模板头约束会误判不满足;ctor 具现化点 Alloc 已完整。
            static_assert(TrivialAllocator<Alloc>, "InternPool: Alloc must satisfy TrivialAllocator");
        }

        ~InternPool() {
            if (cap_ != 0) {
                alloc_->template deallocate<ObjString*>(slots_, cap_);
            }
        }

        InternPool(const InternPool&)            = delete;
        InternPool& operator=(const InternPool&) = delete;
        InternPool(InternPool&&)                 = delete;
        InternPool& operator=(InternPool&&)      = delete;

        // 查找内容等于 src 的驻留串。命中返回其 ObjString*,未命中 nullptr。
        [[nodiscard]]
        ObjString* find(const StringView src, const u32 hash) const noexcept {
            ASSERT(hash == util::hash_str(src), "hash must be hash_str(src)");
            if (cap_ == 0) {
                return nullptr;
            }

            const usize mask = cap_ - 1;

            usize pos  = hash;
            usize step = 0;

            for (usize probe = 0; probe < cap_; ++probe) {
                pos          = (pos + (step++)) & mask; // 三角探测(偏移序见类头注)
                ObjString* s = slots_[pos];
                if (s == nullptr) {
                    return nullptr; // 空槽,探针终止
                }
                if (!is_tombstone(s) && s->view() == src) {
                    return s; // 占用且内容相等
                }
            }
            return nullptr; // 安全上限耗尽(不变式下不会到达)
        }

        // 插入 s(假定其内容未驻留:调用方先 find 查重,未命中才 insert)。可能触发 rehash。
        void insert(ObjString* s) {
            ASSERT(s != nullptr && !is_tombstone(s), "invalid slot for insert");
            // 确保有空槽(阈值见类头注,含 cap_==0 初始分配)。
            if (cap_ == 0) {
                grow_and_rehash_(kInitialCap);
            } else {
                if (const usize projected = count_ + tombstones_ + 1; projected * 8 > cap_ * 7) {
                    grow_and_rehash_(cap_ * 2);
                } else if (tombstones_ * 8 > cap_) {
                    grow_and_rehash_(cap_);
                }
            }

            const usize mask = cap_ - 1;

            usize pos  = s->hash();
            usize step = 0;
            usize tomb = kNpos;

            for (usize probe = 0; probe < cap_; ++probe) {
                pos                  = (pos + (step++)) & mask; // 三角探测(偏移序见类头注)
                const ObjString* cur = slots_[pos];
                if (cur == nullptr) {
                    const usize insert_pos = (tomb != kNpos) ? tomb : pos;
                    slots_[insert_pos]     = s;
                    if (tomb != kNpos) {
                        --tombstones_; // 复用墓碑
                    }
                    ++count_;
                    return;
                }
                if (is_tombstone(cur) && tomb == kNpos) {
                    tomb = pos; // 记首个墓碑,探到空槽时回退写入
                }
            }
            ASSERT(false, "probe exhausted (invariant violated)");
        }

        // weak root 清理:把指向白色(未标 is_marked)串的占用槽置墓碑;在 trace 后、sweep 前调用,防 slots_ 悬垂。
        void remove_white() noexcept {
            for (usize i = 0; i < cap_; ++i) {
                const ObjString* s = slots_[i];
                if (s == nullptr || is_tombstone(s)) {
                    continue;
                }
                if (!s->is_marked()) {
                    slots_[i] = tombstone(); // 白色(未标)-> 墓碑
                    --count_;
                    ++tombstones_;
                }
            }
        }

    private:
        void grow_and_rehash_(const usize new_cap) {
            ObjString** old_slots = slots_;
            const usize old_cap   = cap_;

            slots_ = alloc_->template allocate<ObjString*>(new_cap);
            std::memset(slots_, 0, new_cap * sizeof(ObjString*)); // 全 nullptr(空槽)
            cap_        = new_cap;
            count_      = 0; // 重插时累加
            tombstones_ = 0;

            const usize mask = new_cap - 1;
            for (usize i = 0; i < old_cap; ++i) {
                ObjString* s = old_slots[i];
                if (s == nullptr || is_tombstone(s)) {
                    continue; // 跳过空/墓碑(墓碑丢弃)
                }
                usize pos  = s->hash();
                usize step = 0;
                do {
                    pos = (pos + (step++)) & mask; // 三角探测(偏移序见类头注)
                } while (slots_[pos] != nullptr); // 新表无墓碑,只撞占用槽
                slots_[pos] = s;
                ++count_;
            }

            if (old_cap != 0) {
                alloc_->template deallocate<ObjString*>(old_slots, old_cap);
            }
        }
    };

} // namespace aria

#endif // ARIA_INTERN_POOL_HPP
