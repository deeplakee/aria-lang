#ifndef ARIA_GC_HPP
#define ARIA_GC_HPP

#include <functional>

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/InternPool.hpp"
#include "memory/RawAlloc.hpp"
#include "memory/ShellPool.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    // mark-sweep 回收器，根由临时守卫与宿主 tracer 提供。
    // allocate/reallocate 和壳池不触发 GC；自动回收点仅在 new_object 前及 VM 安全点。
    class GC {
    public:
        GC() noexcept;
        ~GC();

        GC(const GC&)            = delete;
        GC& operator=(const GC&) = delete;
        GC(GC&&)                 = delete;
        GC& operator=(GC&&)      = delete;

        template<typename T>
        [[nodiscard]]
        T* allocate(usize count);

        template<typename T>
        void deallocate(T* p, usize count) noexcept;

        // new_count 为 0 时释放；扩缩容后基址可能变化。
        template<typename T>
        [[nodiscard]]
        T* reallocate(T* p, usize old_count, usize new_count);

        // 分配前可能回收；入参须由调用方根化，工厂负责自己的中间产物。
        // 返回对象尚无根，须在下个 GC 点前发布。
        template<DerivedFromObj T, typename... Args>
        [[nodiscard]]
        T* new_object(Args&&... args);

        void mark_value(Value value) noexcept;
        void mark_object(Object* object) noexcept;

        void maybe_collect() noexcept;

        void collect();

        // 临时根采用栈结构，守卫生存期须严格嵌套；push 的对象须非空。
        class Guard {
        public:
            explicit Guard(GC* gc) noexcept : gc_{gc}, count_{0} {}

            Guard(GC* gc, Object* object) noexcept : gc_{gc}, count_{1} { gc_->push_temp_root(object); }

            ~Guard() {
                if (count_ > 0) {
                    gc_->pop_temp_root(count_);
                }
            }

            Guard(const Guard&)            = delete;
            Guard(Guard&&)                 = delete;
            Guard& operator=(const Guard&) = delete;
            Guard& operator=(Guard&&)      = delete;

            void push(Object* object) noexcept {
                gc_->push_temp_root(object);
                ++count_;
            }

        private:
            GC*   gc_;
            usize count_;
        };

        [[nodiscard]]
        Guard make_guard() noexcept {
            return Guard{this};
        }

        [[nodiscard]]
        Guard make_guard(Object* object) noexcept {
            return Guard{this, object};
        }

        [[nodiscard]]
        usize bytes_allocated() const noexcept {
            return bytes_allocated_;
        }

        // 累计对象分配数，不随回收下降。
        [[nodiscard]]
        usize allocation_count() const noexcept {
            return allocation_count_;
        }

        void set_stress(const bool enabled) noexcept { is_stress_ = enabled; }

        void disable_gc() noexcept { ++lock_count_; }
        void enable_gc() noexcept {
            ASSERT(lock_count_ > 0, "enable_gc without matching disable_gc");
            --lock_count_;
        }
        [[nodiscard]] bool is_gc_disabled() const noexcept { return lock_count_ > 0; }

        class LockGuard {
        public:
            explicit LockGuard(GC* gc) noexcept : gc_{gc} { gc_->disable_gc(); }
            ~LockGuard() { gc_->enable_gc(); }
            LockGuard(const LockGuard&)            = delete;
            LockGuard(LockGuard&&)                 = delete;
            LockGuard& operator=(const LockGuard&) = delete;
            LockGuard& operator=(LockGuard&&)      = delete;

        private:
            GC* gc_;
        };

        [[nodiscard]] LockGuard make_lock() noexcept { return LockGuard{this}; }

        [[nodiscard]] ObjString* intern_find(StringView str, u32 hash) const noexcept;

        [[nodiscard]] ObjString* intern_find(StringView lhs, StringView rhs, u32 hash) const noexcept;

        void intern_insert(ObjString* str);

        void set_vm_roots(std::function<void(GC&)> tracer) noexcept { vm_roots_tracer_ = std::move(tracer); }

    private:
        void mark_roots_() noexcept;
        void trace_gray_() noexcept;
        void sweep_() noexcept;
        void free_all_() noexcept; // ~GC:释放所有残留对象
        // 不负责从对象链表摘除。
        void delete_object(Object* obj) noexcept;

        void push_temp_root(Object* object) noexcept;
        void pop_temp_root(usize count = 1) noexcept;

        static constexpr usize kInitialGcThreshold = 1024 * 4;
        static constexpr usize kGcGrowFactor       = 2;

        Object* objects_head_;
        usize   bytes_allocated_;
        usize   allocation_count_;
        usize   next_gc_;
        bool    is_stress_;
        u32     lock_count_;
        // gray_stack_/temp_roots_ 为 GC 自身 scratch:不经 GC 分配器,不计入 bytes_allocated_。
        List<Object*>            gray_stack_;
        List<Value>              temp_roots_;
        ShellPool                shell_pool_;      // 对象壳池(span 随析构排空)
        InternPool<GC>           intern_;          // 字符串驻留池(weak root,slots_ 计入 bytes_allocated_)
        std::function<void(GC&)> vm_roots_tracer_; // VM 根标记回调(可为空)
    };

    template<typename T>
    T* GC::allocate(const usize count) {
        void* p = mem::alloc(count * sizeof(T));
        if (p == nullptr) {
            fatal_error(ErrorCode::OutOfMemory, "failed to allocate {} bytes", count * sizeof(T));
        }
        bytes_allocated_ += count * sizeof(T);
        return static_cast<T*>(p);
    }

    template<typename T>
    void GC::deallocate(T* p, const usize count) noexcept {
        if (p == nullptr) {
            return;
        }
        bytes_allocated_ -= count * sizeof(T);
        mem::free(p);
    }

    template<typename T>
    T* GC::reallocate(T* p, const usize old_count, const usize new_count) {
        if (new_count == 0) {
            deallocate(p, old_count);
            return nullptr;
        }
        void* q = mem::realloc(p, new_count * sizeof(T));
        if (q == nullptr) {
            fatal_error(ErrorCode::OutOfMemory, "failed to reallocate {} bytes", new_count * sizeof(T));
        }
        bytes_allocated_ += (new_count - old_count) * sizeof(T);
        return static_cast<T*>(q);
    }

    template<DerivedFromObj T, typename... Args>
    T* GC::new_object(Args&&... args) {
        maybe_collect();
        T* obj = new (shell_pool_.alloc<T>()) T{std::forward<Args>(args)...};
        bytes_allocated_ += sizeof(T);
        obj->next_    = objects_head_;
        objects_head_ = obj;
        ++allocation_count_;
#ifdef DEBUG_LOG_GC
        log_obj_alloc(obj);
#endif
        return obj;
    }

} // namespace aria

#endif // ARIA_GC_HPP
