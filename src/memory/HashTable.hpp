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
    //   释放旧两块;rehash 走分配器 allocate/deallocate(GC 下**不触发 GC**)。
    //   持 Alloc* alloc_,dtor 自释放。不可拷贝/不可移动(理由同 Buffer:浅 move 会 double-free)。
    //   K/V 必须 trivially-copyable;分配器解耦见 Allocator.hpp(实例化点须令 GC 完整可见)。
    template<TriviallyCopyable K, TriviallyCopyable V, HashFunctor<K> Hash, EqFunctor<K> Eq,
             TrivialAllocator Alloc = GC>
    class HashTable {

    public:
        // 键值对条目(16B 当 K=V=Value)。find 返回指向它的指针。
        struct Entry {
            K key;
            V value;
        };

    private:
        // ctrl 字节编码:
        //   0xFF        = 空槽(探针终止)
        //   0xFE        = 墓碑(已删除)
        //   0x00..0x7F  = 占用,低 7 位 = h2(部分哈希)
        //
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

        // 从 from 起(含)找下一占用槽的槽位索引,无则 cap_(与 end 哨兵同值,迭代器推进
        // 单点共用:begin 首扫与 const_iterator::operator++)。只看 ctrl 不加载非占用
        // Entry,空/墓碑槽里是垃圾也安全。
        [[nodiscard]]
        usize next_occupied_from_(const usize from) const noexcept {
            for (usize i = from; i < cap_; ++i) {
                if (ctrl_is_occupied(ctrl_[i])) {
                    return i;
                }
            }
            return cap_;
        }

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
            }
            return nullptr; // 安全上限耗尽(不变式下不会到达)
        }

        // 写入:命中覆写 value(原槽更新,find 路径零分配);未命中插入 (key, value)
        // (可能触发 rehash 扩容或 compact,rehash 后 entries_/ctrl_ 指针改变)。
        void set(const K& key, const V& value) {
            if (Entry* entry = find(key)) {
                entry->value = value; // 命中:原槽覆写,无分配
                return;
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
                    entries_[insert_pos].value = value;
                    if (tomb != kNpos) {
                        --tombstones_; // 复用墓碑
                    }
                    ++count_;
                    return;
                }
                if (byte == kCtrlDeleted && tomb == kNpos) {
                    tomb = pos; // 记首个墓碑,探到空槽时回退写入
                }
            }
            ASSERT(false, "HashTable::set: probe exhausted (invariant violated)");
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

        // 只读槽位迭代器(begin/end 语义,消费场景全只读故不设非 const 版):跳过空槽与
        // 墓碑,operator* 取占用槽 Entry。迭代序 = 槽位序(map 语言面迭代序 unspecified,
        // 契约见计划 D4/ObjMapIterator);失效语义同 std::unordered_map 惯例 --erase 使
        // 被删元素失效,rehash/compact 搬迁槽位使全部迭代器失效,迭代中变更容器不设防。
        // 对称基线:Array 的 begin/end/cbegin/cend。
        class const_iterator {
        public:
            const Entry& operator*() const {
                ASSERT(ht_->ctrl_is_occupied(ht_->ctrl_[slot_]), "HashTable::const_iterator: slot is not occupied");
                return ht_->entries_[slot_];
            }

            const Entry* operator->() const { return &operator*(); }

            const_iterator& operator++() {
                slot_ = ht_->next_occupied_from_(slot_ + 1);
                return *this;
            }

            [[nodiscard]]
            bool operator==(const const_iterator& rhs) const noexcept {
                return slot_ == rhs.slot_;
            }

            [[nodiscard]]
            bool operator!=(const const_iterator& rhs) const noexcept {
                return slot_ != rhs.slot_;
            }

            // 仅 HashTable 的 begin/end 构造;slot 语义不对外承诺(越界/非占用槽行为由
            // ASSERT 钉住)。
            explicit const_iterator(const HashTable* ht, const usize slot) noexcept : ht_{ht}, slot_{slot} {}

        private:
            const HashTable* ht_;
            usize            slot_;
        };

        [[nodiscard]]
        const_iterator begin() const noexcept {
            return const_iterator{this, next_occupied_from_(0)};
        }

        [[nodiscard]]
        const_iterator end() const noexcept {
            return const_iterator{this, cap_};
        }

    private:
        // 插入前确保有空槽(阈值见类注释):cap_==0 初始分配;过满扩容×2;墓碑过多原容 compact。
        // 任一情形都经 grow_and_rehash_。
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
