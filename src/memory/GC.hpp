#ifndef ARIA_GC_HPP
#define ARIA_GC_HPP

#include <algorithm>
#include <format>
#include <functional>
#include <new>

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/InternPool.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    // GC:解释器统一内存分配器 + mark-sweep 回收器。
    //
    // 两层分配:
    //   - 类型化 trivial 模板(allocate<T>/deallocate<T>/reallocate<T>):count 个 T,
    //     计入 bytes_allocated_,不触发 GC。供 Array<T> / ObjString long_chars_ 等使用。
    //   - Object(new_object<T>):带 Object 头的可追踪对象,链入 objects_head_,
    //     分配前 maybe_collect() 可能触发回收。
    //
    // 回收:三色 mark-sweep。roots = 临时根(Phase 1)+ VM 根(M2 起用,经函数指针回调,
    //   当前为模块表);Phase 4 再接 Movement 根(值栈/帧/open upvalues)。
    //   mark_roots_ -> trace_gray_ -> sweep_;sweep_ 对未标对象调虚析构(级联释放
    //   子内存:Array 成员自释放 / ObjString long_chars_ 在 ~ObjString 释放)再释放壳。
    //
    // 不变式:allocate/reallocate 永不触发 GC,故对象构造期内的子分配不会回收正在
    //   构造的对象。GC 仅在 new_object 顶部与 VM safe point 触发。
    //
    // gray_stack_ / temp_roots_ 是 GC 自身 scratch,用 List(std::vector)实现,
    // 不经 GC 分配器、不计入 bytes_allocated_(GC overhead 与 managed heap 分离)。
    class GC {
    public:
        GC() noexcept;
        ~GC();

        GC(const GC&)            = delete;
        GC& operator=(const GC&) = delete;
        GC(GC&&)                 = delete;
        GC& operator=(GC&&)      = delete;

        // ---- 类型化 trivial 分配 ----
        // 分配 count 个 T(= count*sizeof(T) 字节),失败走 fatal_error(OutOfMemory)。不触发 GC。
        template<typename T>
        [[nodiscard]]
        T* allocate(usize count);

        template<typename T>
        void deallocate(T* p, usize count) noexcept;

        // realloc 语义:new_count==0 退化为 deallocate;否则新分配 + 拷贝 min(old,new) 个 T + 释放旧。
        template<typename T>
        [[nodiscard]]
        T* reallocate(T* p, usize old_count, usize new_count);

        // ---- object allocation(可追踪层)----
        // maybe_collect -> allocate<u8>(sizeof(T)) -> placement-new 构造 -> 链入 objects_head_。
        template<DerivedFromObj T, typename... Args>
        [[nodiscard]]
        T* new_object(Args&&... args);

        // ---- tracing ----
        void mark_value(Value v) noexcept;
        void mark_object(Object* o) noexcept;

        // bytes_allocated_ >= next_gc_(或 stress 开)时 collect()。
        void maybe_collect() noexcept;

        // mark_roots_ -> trace_gray_ -> sweep_ -> 调整 next_gc_。
        void collect();

        // ---- temp roots ----
        // C++ 局部变量持有的、尚未入值栈的对象/值,在分配序列间保护其不被回收。
        // 对外只暴露 Guard / make_guard RAII API;底层 push_temp_root/pop_temp_root 为私有,
        // 由 Guard 内部调用(GC 的嵌套类可访问外层私有成员)。
        // RAII 临时根:构造时 push,析构时 pop。禁拷贝/移动(make_guard 经 prvalue 必然复制消除)。
        class Guard {
        public:
            explicit Guard(GC* gc) noexcept : gc_{gc}, count_{0} {}

            Guard(GC* gc, Value v) noexcept : gc_{gc}, count_{1} { gc_->push_temp_root(v); }

            Guard(GC* gc, Object* o) noexcept : gc_{gc}, count_{1} { gc_->push_temp_root(o); }

            ~Guard() {
                if (count_ > 0) {
                    gc_->pop_temp_root(count_);
                }
            }

            Guard(const Guard&)            = delete;
            Guard(Guard&&)                 = delete;
            Guard& operator=(const Guard&) = delete;
            Guard& operator=(Guard&&)      = delete;

            void push(Value v) noexcept {
                gc_->push_temp_root(v);
                ++count_;
            }

            void push(Object* o) noexcept {
                gc_->push_temp_root(o);
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
        Guard make_guard(Value v) noexcept {
            return Guard{this, v};
        }

        [[nodiscard]]
        Guard make_guard(Object* o) noexcept {
            return Guard{this, o};
        }

        [[nodiscard]]
        usize bytes_allocated() const noexcept {
            return bytes_allocated_;
        }

        // 运行期压力开关(测试用):开启后每次 new_object 强制 collect。
        void set_stress(bool b) noexcept { is_stress_ = b; }

        // ---- GC 禁用锁(单线程,计数器实现,支持嵌套)----
        // collect() 在 lock_count_>0 时跳过(临界区不回收);maybe_collect 经 collect() 间接受控。
        void disable_gc() noexcept { ++lock_count_; }
        void enable_gc() noexcept {
            ASSERT(lock_count_ > 0, "enable_gc without matching disable_gc");
            --lock_count_;
        }
        [[nodiscard]] bool is_gc_disabled() const noexcept { return lock_count_ > 0; }

        // RAII 禁用 GC:构造 disable,析构 enable。禁拷贝/移动(make_lock 经 prvalue 必然复制消除)。
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

        // ---- intern 驻留池(weak root,字符串专用)----
        // 委托给 intern_ 成员(保持 private)。new_string 经此实现驻留:命中返回已有串,
        // 未命中 new_object 后 insert。驻留池不进 mark_roots_,collect 在 sweep 前调
        // intern_.remove_white() 摘除白色表项防悬垂。
        [[nodiscard]] ObjString* intern_find(StringView src) const noexcept;

        void intern_insert(ObjString* s);

        // ---- VM 根(M2 起用)----
        // 解释器级共享状态(模块表等)经 std::function 回调接入 mark_roots_(组合而非继承:
        // GC 不识 VM 类型,避免双向 include)。调用方(AriaVM)注册一个 lambda(通常捕获 this,
        // 内部 trace 自己的 roots);collect -> mark_roots_ 末尾调用。std::function 比
        // 裸函数指针 + void* ctx + 静态 thunk 干净:无静态适配器、无 void*、无 static_cast,
        // 且 lambda 直写标记逻辑。[this] 仅一指针,落在 std::function SBO 内,零堆分配。
        void set_vm_roots(std::function<void(GC&)> tracer) noexcept { vm_roots_tracer_ = std::move(tracer); }

    private:
        void mark_roots_() noexcept;
        void trace_gray_() noexcept;
        void sweep_() noexcept;
        void free_all_() noexcept; // ~GC:释放所有残留对象
        // 销毁单个对象:new_object 的逆(虚析构级联释放子内存 + 释放壳)。不含链表摘除,由调用方管。
        void delete_object(Object* obj) noexcept;

        // ---- temp roots 底层(由 Guard 调用)----
        // Value 与 Object* 双重载,内部统一存为 Value(Object* 经 from_obj 装箱)。
        void push_temp_root(Value v) noexcept;
        void push_temp_root(Object* o) noexcept;
        void pop_temp_root(usize n = 1) noexcept;

        static constexpr usize kInitialGcThreshold = 1024 * 4;
        static constexpr usize kGcGrowFactor       = 2;

        Object*                  objects_head_;
        usize                    bytes_allocated_;
        usize                    next_gc_;
        bool                     is_stress_;
        u32                      lock_count_;      // GC 禁用计数(>0 禁用,支持嵌套 disable/enable)
        List<Object*>            gray_stack_;      // GC scratch,不计入 bytes_allocated_
        List<Value>              temp_roots_;      // GC scratch,不计入 bytes_allocated_
        InternPool<GC>           intern_;          // 字符串驻留池(weak root,slots_ 计入 bytes_allocated_)
        std::function<void(GC&)> vm_roots_tracer_; // VM 根标记回调(M2:模块表;AriaVM 注册,可为空)
    };

    // ---- 模板实现 ----

    template<typename T>
    T* GC::allocate(const usize count) {
        void* p = ::operator new(count * sizeof(T), std::nothrow);
        if (p == nullptr) {
            fatal_error(ErrorCode::OutOfMemory, std::format("failed to allocate {} bytes", count * sizeof(T)));
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
        ::operator delete(p);
    }

    template<typename T>
    T* GC::reallocate(T* p, const usize old_count, const usize new_count) {
        if (new_count == 0) {
            deallocate(p, old_count);
            return nullptr;
        }
        void* q = ::operator new(new_count * sizeof(T), std::nothrow);
        if (q == nullptr) {
            fatal_error(ErrorCode::OutOfMemory, std::format("failed to reallocate {} bytes", new_count * sizeof(T)));
        }
        if (p != nullptr) {
            std::memcpy(q, p, std::min(old_count, new_count) * sizeof(T));
            deallocate(p, old_count);
        }
        bytes_allocated_ += new_count * sizeof(T);
        return static_cast<T*>(q);
    }

    template<DerivedFromObj T, typename... Args>
    T* GC::new_object(Args&&... args) {
        maybe_collect();
        T* obj        = new (allocate<u8>(sizeof(T))) T{std::forward<Args>(args)...};
        obj->next_    = objects_head_;
        objects_head_ = obj;
#ifdef DEBUG_LOG_GC
        log_obj_alloc(obj);
#endif
        return obj;
    }

} // namespace aria

#endif // ARIA_GC_HPP
