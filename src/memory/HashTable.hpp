#ifndef ARIA_HASHTABLE_HPP
#define ARIA_HASHTABLE_HPP

#include <concepts>
#include <type_traits>

#include "common.hpp"
#include "memory/Allocator.hpp"

namespace aria {

    // 限制 HashTable 的 Hash/Eq 模板参数:必须是类/结构体(is_class_v,拒函数指针)、默认可构造
    // (类内多处 Hash{}/Eq{} 调用),且 operator() 为 noexcept 并返回 u32/bool。
    // Hash:u32 operator()(const K&) const noexcept;Eq:bool operator()(const K&, const K&) const noexcept。
    template<typename T, typename K>
    concept HashFunctor = std::is_class_v<T> && std::default_initializable<T> && requires(const T& fn, const K& key) {
        { fn(key) } noexcept -> std::same_as<u32>;
    };

    template<typename T, typename K>
    concept EqFunctor =
            std::is_class_v<T> && std::default_initializable<T> && requires(const T& fn, const K& a, const K& b) {
                { fn(a, b) } noexcept -> std::same_as<bool>;
            };

    // 通用 Swiss Table 哈希表(值无关,不依赖 Value)。
    //
    //   核心:每个槽 1 字节 ctrl 同时编码「占用槽的 7 位部分哈希(h2)」。探测时先比 h2,
    //   不命中就跳过且**不加载 Entry(K+V)**,只在 h2 命中时才加载 Entry 比全键。
    //   故绝大多数探针只读 1 字节、不碰 Entry,缓存友好。
    //
    //   - cap_ 为 2 的幂(或 0),槽索引 = h1(hash) & (cap_-1)。
    //   - 三角探测:pos = (pos + (step++)) & mask;偏移序列 0,1,3,6,10,...
    //   - 7/8 负载因子:count_+tombstones_+1 超 cap_*7/8 则扩容(×2);墓碑超 cap_/8 则原容 compact。
    //     始终保留 >= 1/8 空槽 -> 探针必然在空槽终止。
    //
    //   两块独立分配(ctrl_ + entries_),rehash 时一起重分配、逐占用槽重算 hash 重插、
    //   释放旧两块。rehash 走分配器 allocate/deallocate(GC 下**不触发 GC**,同 Array 的存储)。
    //
    //   持 Alloc* alloc_,dtor 自释放。不可拷贝/不可移动。K/V 必须 trivially-copyable。
    //   分配器经 TrivialAllocator concept 解耦(见 Allocator.hpp):本头不 include GC.hpp,
    //   故不传递地拖入 object/value 树;Alloc 默认为 GC,实例化点须令 GC 完整可见。
    //
    // K    键类型(POD)
    // V    值类型(POD)
    // Hash 键哈希仿函数:u32 operator()(const K&) const noexcept
    // Eq   键相等仿函数:bool operator()(const K&, const K&) const noexcept
    // Alloc Trivial 分配器(默认 GC)
    template<TriviallyCopyable K, TriviallyCopyable V, HashFunctor<K> Hash, EqFunctor<K> Eq,
             TrivialAllocator Alloc = GC>
    class HashTable {

    public:
        // 键值对条目(16B 当 K=V=Value)。find/upsert 返回指向它的指针。
        struct Entry {
            K key;
            V value;
        };

    private:
        // ctrl 字节编码:
        //   0xFF        = 空槽(探针终止)
        //   0xFE        = 墓碑(已删除)
        //   0x00..0x7F  = 占用,低 7 位 = h2(部分哈希)
        // 高位 1 = 特殊(空/墓碑),高位 0 = 占用。target = ctrl_from_hash(hash)(高位 0),
        // 故 byte == target 只会命中占用槽,不会误中 kCtrlEmpty/kCtrlDeleted(它们高位 1)。
        static constexpr u8 kCtrlEmpty   = 0xFF;
        static constexpr u8 kCtrlDeleted = 0xFE;

        [[nodiscard]] static constexpr bool ctrl_is_occupied(const u8 byte) noexcept { return (byte & 0x80) == 0; }

        [[nodiscard]] static constexpr bool ctrl_is_empty(const u8 byte) noexcept { return byte == kCtrlEmpty; }

        [[nodiscard]] static constexpr u8 ctrl_from_hash(const u32 hash) noexcept {
            return static_cast<u8>(ht_h2(hash) & 0x7F);
        }

        // h1:hash 高 25 位 -> 槽索引(再 & (cap-1));h2:hash 低 7 位 -> ctrl 字节。
        [[nodiscard]] static constexpr u32 ht_h1(const u32 hash) noexcept { return hash >> 7; }

        [[nodiscard]] static constexpr u32 ht_h2(const u32 hash) noexcept { return hash & 0x7F; }

        Entry* entries_; // alloc_->allocate<Entry>(cap_)
        u8*    ctrl_;    // alloc_->allocate<u8>(cap_),每槽 1 字节
        Alloc* alloc_;
        usize  cap_;        // 2 的幂(或 0)
        usize  count_;      // 占用数
        usize  tombstones_; // 墓碑数

        static constexpr usize kInitialCap = 8;
        static constexpr usize kNpos       = static_cast<usize>(-1);

    public:
        explicit HashTable(Alloc* alloc) noexcept :
            entries_{nullptr}, ctrl_{nullptr}, alloc_{alloc}, cap_{0}, count_{0}, tombstones_{0} {}

        ~HashTable() {
            if (cap_ != 0) {
                alloc_->template deallocate<u8>(ctrl_, cap_);
                alloc_->template deallocate<Entry>(entries_, cap_);
            }
        }

        // 禁拷贝/禁移动:持 GC 堆分配裸指针(entries_/ctrl_),浅 move 会 double-free;
        // 深拷贝要重分配+rehash 且当前无需。资源仅经析构释放,需转移所有权时用指针/就地构造。
        HashTable(const HashTable&)            = delete;
        HashTable& operator=(const HashTable&) = delete;
        HashTable(HashTable&&)                 = delete;
        HashTable& operator=(HashTable&&)      = delete;

        // 查找。命中返回指向 Entry 的指针,未命中 nullptr。
        [[nodiscard]]
        Entry* find(const K& key) noexcept {
            return const_cast<Entry*>(std::as_const(*this).find(key));
        }

        [[nodiscard]]
        const Entry* find(const K& key) const noexcept {
            if (cap_ == 0) {
                return nullptr;
            }
            const u32   hash   = Hash{}(key);
            const usize mask   = cap_ - 1;
            const u8    target = ctrl_from_hash(hash);

            usize pos  = ht_h1(hash); // 可超 cap,首轮循环统一 & mask 归位
            usize step = 0;

            for (usize probe = 0; probe < cap_; ++probe) {
                pos = (pos + (step++)) & mask; // 三角探测:偏移 0,1,3,6,...
                if (const u8 byte = ctrl_[pos]; byte == target) {
                    if (Eq{}(entries_[pos].key, key)) {
                        return &entries_[pos];
                    }
                } else if (ctrl_is_empty(byte)) {
                    return nullptr; // 空槽,探针终止
                }
                // h2 命中但全键不等 / 墓碑 -> 继续探测
            }
            return nullptr; // 安全上限耗尽(不变式下不会到达)
        }

        // find-or-insert:命中返回已有 Entry(保留其 value);未命中插入新 Entry
        // (key 填入,value 值初始化为 V{},调用方负责覆写),返回指向它的指针。
        // 可能触发 rehash(扩容或 compact),rehash 后 entries_/ctrl_ 指针改变。
        // 注意:Value{} 零填充是 f64 0.0 非 nil,需要 nil 的场合调用方显式覆写。
        Entry* upsert(const K& key) {
            if (Entry* entry = find(key)) {
                return entry; // 命中已有,value 保留
            }
            maybe_rehash_for_insert_(); // 确保有空槽(可能重分配)
            const u32   hash   = Hash{}(key);
            const usize mask   = cap_ - 1;
            const u8    target = ctrl_from_hash(hash);

            usize pos  = ht_h1(hash); // 可超 cap,首轮循环统一 & mask 归位
            usize step = 0;
            usize tomb = kNpos;

            for (usize probe = 0; probe < cap_; ++probe) {
                pos           = (pos + (step++)) & mask; // 三角探测:偏移 0,1,3,6,...
                const u8 byte = ctrl_[pos];
                if (ctrl_is_empty(byte)) {
                    const usize insert_pos     = (tomb != kNpos) ? tomb : pos;
                    ctrl_[insert_pos]          = target;
                    entries_[insert_pos].key   = key;
                    entries_[insert_pos].value = V{};
                    if (tomb != kNpos) {
                        --tombstones_; // 复用墓碑
                    }
                    ++count_;
                    return &entries_[insert_pos];
                }
                if (byte == kCtrlDeleted && tomb == kNpos) {
                    tomb = pos; // 记首个墓碑,探到空槽时回退写入
                }
            }
            ASSERT(false, "HashTable::upsert: probe exhausted (invariant violated)");
            return nullptr;
        }

        // 擦除命中槽(置墓碑)。返回是否确实擦除。无需 nil-out entries_,trace 按 ctrl 跳过非占用槽。
        bool erase(const K& key) noexcept {
            Entry* entry = find(key);
            if (entry == nullptr) {
                return false;
            }
            const auto idx = static_cast<usize>(entry - entries_);
            ctrl_[idx]     = kCtrlDeleted;
            --count_;
            ++tombstones_;
            return true;
        }

        // 清空(全 ctrl 置 kEmpty)。不释放容量。entries_ 内容废弃(trivial,无需析构)。
        void clear() noexcept {
            if (cap_ == 0) {
                return;
            }
            std::memset(ctrl_, kCtrlEmpty, cap_);
            count_      = 0;
            tombstones_ = 0;
        }

        [[nodiscard]]
        usize size() const noexcept {
            return count_;
        }

        [[nodiscard]]
        usize capacity() const noexcept {
            return cap_;
        }

        [[nodiscard]]
        bool empty() const noexcept {
            return count_ == 0;
        }

        // 遍历所有占用槽(供 trace / 调试)。fn 接收 (const K&, const V&)。
        // 只看 ctrl,不加载非占用 Entry,故空/墓碑槽里是垃圾也安全。
        template<typename Fn>
        void for_each_occupied(Fn&& fn) const noexcept {
            for (usize i = 0; i < cap_; ++i) {
                if (ctrl_is_occupied(ctrl_[i])) {
                    fn(entries_[i].key, entries_[i].value);
                }
            }
        }

    private:
        // 插入前确保有空槽:cap_==0 初始分配;插入后(count+tomb+1)超 7/8 则扩容×2;
        // 墓碑超 cap/8 则原容 compact(清墓碑)。任一情形都经 grow_and_rehash_。
        void maybe_rehash_for_insert_() {
            if (cap_ == 0) {
                grow_and_rehash_(kInitialCap);
                return;
            }
            if (const usize projected = count_ + tombstones_ + 1; projected * 8 > cap_ * 7) {
                grow_and_rehash_(cap_ * 2); // 太满 -> 扩容
                return;
            }
            if (tombstones_ * 8 > cap_) {
                grow_and_rehash_(cap_); // 墓碑过多 -> 原容 compact
                return;
            }
        }

        // 重分配 ctrl_(全 kEmpty)+ entries_,逐个占用槽重算 hash 重插,释放旧两块。
        // 墓碑丢弃。new_cap 为 2 的幂。
        void grow_and_rehash_(const usize new_cap) {
            Entry*      old_entries = entries_;
            u8*         old_ctrl    = ctrl_;
            const usize old_cap     = cap_;

            entries_ = alloc_->template allocate<Entry>(new_cap);
            ctrl_    = alloc_->template allocate<u8>(new_cap);
            std::memset(ctrl_, kCtrlEmpty, new_cap);
            cap_        = new_cap;
            count_      = 0; // 重插时累加
            tombstones_ = 0;

            const usize mask = new_cap - 1;
            for (usize i = 0; i < old_cap; ++i) {
                if (!ctrl_is_occupied(old_ctrl[i])) {
                    continue; // 跳过空/墓碑
                }

                const u32 hash   = Hash{}(old_entries[i].key);
                const u8  target = ctrl_from_hash(hash);

                usize pos  = ht_h1(hash);
                usize step = 0;

                do {
                    pos = (pos + (step++)) & mask; // 三角探测:偏移 0,1,3,6,...
                } while (ctrl_is_occupied(ctrl_[pos])); // 新表无墓碑,只撞占用槽
                ctrl_[pos]    = target;
                entries_[pos] = old_entries[i];
                ++count_;
            }

            if (old_cap != 0) {
                alloc_->template deallocate<u8>(old_ctrl, old_cap);
                alloc_->template deallocate<Entry>(old_entries, old_cap);
            }
        }
    };

} // namespace aria

#endif // ARIA_HASHTABLE_HPP
