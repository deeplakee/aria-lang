#ifndef ARIA_INTERN_POOL_HPP
#define ARIA_INTERN_POOL_HPP

#include <cstring>

#include "common.hpp"
#include "memory/Allocator.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

namespace aria {

    // 字符串驻留池(intern pool):字符串专用 set(键即串内容,值即 ObjString* 自身)。
    //
    //   用比通用 HashTable 更省的特化表示:裸 ObjString** slots_,8 字节/槽,无 ctrl、无 h2
    //   (靠内容比较)。低位标签区分槽状态(对象指针经 ::operator new 是 max_align_t 对齐,
    //   低 4 位全 0,0x1 空闲):
    //     nullptr           = 空槽(探针终止)
    //     (ObjString*)0x1   = 墓碑
    //     真 ObjString*      = 占用
    //
    //   lookup:hash 串内容(FNV-1a,同 ObjString)-> 三角探测,占用槽比 view()==query,
    //   空槽终止,墓碑跳过。insert:首个墓碑或空槽写入(复用墓碑);调用方先 find 查重。
    //
    //   **weak root**:不进 GC::mark_roots_(否则驻留串永生)。GC::collect 在 sweep 前调
    //   remove_white() 摘除指向白色(未标 is_marked)ObjString* 的表项,避免 sweep 后悬垂。
    //
    //   slots_ 经 alloc_->allocate<ObjString*> 分配,在 rehash 或 ~InternPool 释放。InternPool
    //   本身是 GC 的普通值成员(非 Object、不被 trace/sweep)。分配器解耦见 Allocator.hpp
    //   (实例化点须令 GC 完整可见)。**元素类型固定 ObjString***:特化表示依赖其缓存哈希
    //   (hash())、内容视图(view())与 weak root 清理用的 is_marked(),未参数化(YAGNI)。
    //
    //   **Alloc 约束的位置**:不放在模板头,而在 ctor 体内 static_assert。InternPool<GC> 是
    //   GC 的**值成员**,在 GC 类体内实例化--此刻 GC 尚不完整,而模板头约束在 Allocator.hpp
    //   上下文做名字查找(那里 GC 仅前向声明、看不到 allocate 成员),会判定不满足而报错;
    //   ctor 体内随具现化检查(GC.cpp 构造 intern_ 时 GC 已完整),既保留概念强制又能作成员。
    //
    //   头循环同因:本头只前向声明 GC(供默认模板实参)、include ObjString.hpp(方法体调
    //   view/hash/is_marked);调 allocate/deallocate 的成员在实例化点具现化,届时 Alloc 必已
    //   完整(同 HashTable)。
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
            // 延迟到 ctor 具现化点检查(见类注释「Alloc 约束的位置」):此时 Alloc(GC)已完整。
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
        ObjString* find(const StringView src) const noexcept {
            if (cap_ == 0) {
                return nullptr;
            }

            const usize mask = cap_ - 1;

            usize pos  = util::hash_str(src);
            usize step = 0;

            for (usize probe = 0; probe < cap_; ++probe) {
                pos          = (pos + (step++)) & mask; // 三角探测:偏移 0,1,3,6,...
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
            // 确保有空槽(同 HashTable 策略):cap_==0 初始分配;插入后(count+tomb+1)超 7/8 扩容×2;
            // 墓碑超 cap/8 原容 compact。任一情形都经 grow_and_rehash_。
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
                pos                  = (pos + (step++)) & mask; // 三角探测:偏移 0,1,3,6,...
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
            ASSERT(false, "InternPool::insert: probe exhausted (invariant violated)");
        }

        // weak root 清理:遍历 slots_,把指向白色(未标 is_marked)ObjString* 的占用槽置墓碑。
        // 在 GC::collect 的 trace 后、sweep 前调用,防 sweep 释放后 slots_ 悬垂。
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

        [[nodiscard]]
        usize size() const noexcept {
            return count_;
        }

        [[nodiscard]]
        usize capacity() const noexcept {
            return cap_;
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
                    pos = (pos + (step++)) & mask; // 三角探测:偏移 0,1,3,6,...
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
