#ifndef ARIA_AST_ARENA_HPP
#define ARIA_AST_ARENA_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/RawAlloc.hpp"

#include <memory>
#include <new>
#include <utility>

namespace aria {

    // 首块容量：常规源整棵 AST 落前几个块内。
    inline constexpr usize kAstArenaFirstBlockBytes = 256 * 1024;

    // 扩块量子（2 的幂）：放不下时新块 = 请求字节数取整到量子；新块首分配必落 offset 0，无对齐垫。
    // bump 扩块无搬移成本可摊销，几何倍增只剩尾块闲置；量子不取小值，小块在分配器 large 类下有逐块常驻开销。
    inline constexpr usize kAstArenaBlockQuantum = 1024 * 1024;

    static_assert((kAstArenaBlockQuantum & (kAstArenaBlockQuantum - 1)) == 0, "block quantum must be a power of two");

    // AST 专用 bump 分配器：节点与列表缓冲自大块顺序分配，析构时整批释放、不跑任何析构函数。
    // 后备经 mem::alloc/free 同族三口（随 ARIA_USE_MIMALLOC 开关），分配失败 fatal_error(OutOfMemory)、
    // 全程不抛（先例 GC::allocate）。前提（Ast.hpp 节点形态保证）：节点成员全为平凡类型（名字/字符串
    // 借源缓冲的 StringView、子节点为裸指针、列表缓冲为 Span 视图），节点的虚析构仅为多态保留、
    // 从不单独调用；投机解析弃掉的节点不单独回收，留到整批释放统一归还。
    class AstArena {
    public:
        explicit AstArena(const usize first_block_bytes = kAstArenaFirstBlockBytes) { append_block(first_block_bytes); }

        ~AstArena() {
            Block* block = head_;
            while (block != nullptr) {
                Block* next = block->next;
                mem::free(block);
                block = next;
            }
        }

        AstArena(const AstArena&)            = delete;
        AstArena& operator=(const AstArena&) = delete;
        AstArena(AstArena&&) noexcept        = delete;
        AstArena& operator=(AstArena&&)      = delete;

        // 构造一个节点（构造语义同 make_unique，内存自当前块顺序分配）。
        template<typename T, typename... Args>
        T* make(Args&&... args) {
            ++node_count_;
            return new (allocate(sizeof(T), alignof(T))) T{std::forward<Args>(args)...};
        }

        // 把 source 的元素连续拷入 arena 缓冲，返回 Span 视图；空表零分配。
        // 节点列表字段的唯一生产口：Span 会自容器隐式转换，局部容器不得直接喂字段（别名悬垂）。
        // 元素只拷入、不逐个析构，故须平凡可拷贝。
        template<typename T>
        Span<T> make_list(const List<T>& source) {
            static_assert(std::is_trivially_copyable_v<T>, "make_list elements must be trivially copyable");
            if (source.empty()) {
                return Span<T>{};
            }
            const auto elements = static_cast<T*>(allocate(sizeof(T) * source.size(), alignof(T)));
            const auto count    = source.size();
            std::uninitialized_copy(source.begin(), source.end(), elements);
            return Span<T>{elements, count};
        }

        // 已构造节点数（make 调用计数，列表不计）。
        [[nodiscard]] usize node_count() const noexcept { return node_count_; }

        // 已提交的块内存总量（含块头）。
        [[nodiscard]] usize allocated_bytes() const noexcept { return allocated_bytes_; }

    private:
        struct Block {
            Block* next     = nullptr;
            u8*    data     = nullptr;
            usize  capacity = 0;
            usize  used     = 0;

            static Block* new_block(const usize capacity, Block* next) {
                const auto bytes = sizeof(Block) + capacity;
                const auto block = static_cast<Block*>(mem::alloc(bytes));
                if (block == nullptr) {
                    fatal_error(ErrorCode::OutOfMemory, "failed to allocate {} bytes", bytes);
                }
                block->next     = next;
                block->data     = reinterpret_cast<u8*>(block + 1);
                block->capacity = capacity;
                block->used     = 0;
                return block;
            }
        };

        void* allocate(const usize bytes, const usize alignment) {
            const auto align_up = [alignment](const usize value) { return (value + alignment - 1) & ~(alignment - 1); };
            usize      offset   = align_up(head_->used);
            if (offset + bytes > head_->capacity) {
                append_block((bytes + kAstArenaBlockQuantum - 1) & ~(kAstArenaBlockQuantum - 1));
                offset = align_up(head_->used);
            }
            head_->used = offset + bytes;
            return head_->data + offset;
        }

        void append_block(const usize capacity) {
            head_ = Block::new_block(capacity, head_);
            allocated_bytes_ += sizeof(Block) + capacity;
        }

        // 块链头兼当前填充块：新块恒头插、只从头分配，两者是同一成员；析构沿链整批释放。
        Block* head_            = nullptr;
        usize  node_count_      = 0;
        usize  allocated_bytes_ = 0;
    };

} // namespace aria

#endif // ARIA_AST_ARENA_HPP
