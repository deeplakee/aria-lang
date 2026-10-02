#ifndef ARIA_SHELLPOOL_HPP
#define ARIA_SHELLPOOL_HPP

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/RawAlloc.hpp"

namespace aria {

    // 对象壳池:按槽尺寸类(8 B 一档)的定长空壳仓库,同尺寸死壳就地复用,后端只见 span 大块流量。
    // 槽尺寸 = sizeof 上取整到 8,超过 kMaxPooledSlotBytes 不池化、直连后端(正确性不变);永不触发 GC。
    class ShellPool {
    public:
        ShellPool() noexcept : classes_{} {}
        ~ShellPool() { drain_(); }

        ShellPool(const ShellPool&)            = delete;
        ShellPool& operator=(const ShellPool&) = delete;
        ShellPool(ShellPool&&)                 = delete;
        ShellPool& operator=(ShellPool&&)      = delete;

        // 取一个 T 壳的存储(未构造)。sizeof(T) 编译期定格,格下标/bump 步长/span 槽容量全编译期折叠,
        // 超限分支对池化 T 不实例化;永不返回 null。
        template<typename T>
        [[nodiscard]]
        void* alloc() {
            static_assert(alignof(T) <= kSlotGranularity, "shell type alignment exceeds slot granularity");
            constexpr usize slot_bytes = slot_size_for(sizeof(T));
            if constexpr (slot_bytes == 0) {
                return alloc_oversized_(sizeof(T));
            } else {
                return alloc_pooled_<slot_bytes>();
            }
        }

        // 归还死壳(析构已跑完,内存按字节复用):按精确 sizeof 落格,与 alloc 侧同源恒命中同格;超上限直连后端。
        void push(const usize shell_bytes, void* shell) noexcept {
            const usize slot_bytes = slot_size_for(shell_bytes);
            if (slot_bytes == 0) {
                mem::free(shell);
                return;
            }
            auto& [free_head, _] = classes_[slot_index_for(slot_bytes)];
            const auto node      = static_cast<FreeNode*>(shell);
            node->next           = free_head;
            free_head            = node;
        }

    private:
        // 空壳链节点:寄生在死壳内存头 8 B(壳已析构,字节已死,下次分配由构造函数覆写)。
        struct FreeNode {
            FreeNode* next;
        };

        // span 头:同格 span 链 + bump 切槽游标;块前 sizeof(SpanHeader) 字节作头,其余切成槽。
        struct SpanHeader {
            SpanHeader* next;
            u8*         bump;
            usize       remaining;
        };

        // 单尺寸格状态:空壳链 + 本格 span 链(链头即当前 bump 中的 span)。
        struct SlotClass {
            FreeNode*   free_head;
            SpanHeader* spans;
        };

        static constexpr usize kSlotGranularity    = 8;
        static constexpr usize kMaxPooledSlotBytes = 256;
        static constexpr usize kSlotClassCount     = kMaxPooledSlotBytes / kSlotGranularity;
        static constexpr usize kSpanBytes          = 64 * 1024;

        static_assert(sizeof(SpanHeader) % kSlotGranularity == 0, "span header must keep slot offsets aligned");
        static_assert(sizeof(SpanHeader) < kSpanBytes, "span header must leave room for slots");

        // 壳字节数落格:超过池上限返回 0 作「不池化」标记。
        static constexpr usize slot_size_for(const usize shell_bytes) noexcept {
            if (shell_bytes > kMaxPooledSlotBytes) {
                return 0;
            }
            return (shell_bytes + kSlotGranularity - 1) / kSlotGranularity * kSlotGranularity;
        }

        static constexpr usize slot_index_for(const usize slot_bytes) noexcept {
            return slot_bytes / kSlotGranularity - 1;
        }

        template<usize SlotBytes>
        void* alloc_pooled_() {
            SlotClass& slot_class = classes_[slot_index_for(SlotBytes)];
            if (slot_class.free_head != nullptr) {
                FreeNode* node       = slot_class.free_head;
                slot_class.free_head = node->next;
                return node;
            }
            SpanHeader* span = slot_class.spans;
            if (span == nullptr || span->remaining == 0) {
                span = acquire_span_<SlotBytes>(slot_class);
            }
            u8* shell = span->bump;
            span->bump += SlotBytes;
            --span->remaining;
            return shell;
        }

        template<usize SlotBytes>
        static SpanHeader* acquire_span_(SlotClass& slot_class) {
            void* block = mem::alloc(kSpanBytes);
            if (block == nullptr) {
                fatal_error(ErrorCode::OutOfMemory, "failed to allocate {} bytes", kSpanBytes);
            }
            const auto span  = static_cast<SpanHeader*>(block);
            span->next       = slot_class.spans;
            span->bump       = reinterpret_cast<u8*>(span) + sizeof(SpanHeader);
            span->remaining  = (kSpanBytes - sizeof(SpanHeader)) / SlotBytes;
            slot_class.spans = span;
            return span;
        }

        static void* alloc_oversized_(const usize shell_bytes) {
            void* pointer = mem::alloc(shell_bytes);
            if (pointer == nullptr) {
                fatal_error(ErrorCode::OutOfMemory, "failed to allocate {} bytes", shell_bytes);
            }
            return pointer;
        }

        // 排空:全部 span 逐块还后端,各格状态复位。
        void drain_() noexcept {
            for (auto& [free_head, spans]: classes_) {
                auto span = spans;
                while (span != nullptr) {
                    const auto next = span->next;
                    mem::free(span);
                    span = next;
                }
                spans     = nullptr;
                free_head = nullptr;
            }
        }

        SlotClass classes_[kSlotClassCount];
    };

} // namespace aria

#endif // ARIA_SHELLPOOL_HPP
