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

    // GC:解释器统一内存分配器 + mark-sweep 回收器。roots = 临时根 + VM 根(经 std::function 回调标五类:modules_、
    // builtins_、registers_、string_constants_、current_ 执行链的值栈/活动帧/open upvalue 链/挂起错误寄存器)。
    // **核心不变式**:allocate/reallocate 永不触发 GC,GC 仅在 new_object 顶部与 VM safe point 触发。
    class GC {
    public:
        GC() noexcept;
        ~GC();

        GC(const GC&)            = delete;
        GC& operator=(const GC&) = delete;
        GC(GC&&)                 = delete;
        GC& operator=(GC&&)      = delete;

        // 分配 count*sizeof(T) 字节,失败走 fatal_error(OutOfMemory)。**INVARIANT: 永不触发 GC**--
        // 调用方可裸持白色对象跨本调用。
        template<typename T>
        [[nodiscard]]
        T* allocate(usize count);

        template<typename T>
        void deallocate(T* p, usize count) noexcept;

        // realloc 语义:new_count==0 退化为 deallocate;否则后端原生 realloc(扩缩容都允许,基址可能不变)。
        // **INVARIANT: 永不触发 GC**--调本函数期间裸持的白色对象不会被回收。
        template<typename T>
        [[nodiscard]]
        T* reallocate(T* p, usize old_count, usize new_count);

        // 分配层唯一触发 GC 的入口:maybe_collect 在分配前完成(新对象尚未诞生,不会被本轮扫到)。
        // 工厂守卫纪律(**每方只守自己创建的**):调用方入参不守、调用前自行根化,工厂内部产物
        // 经 make_guard 自守;返回对象白色无根,发布进根才安全,发布动作自身也不触发 GC(否则白色
        // 对象会被扫掉)。
        template<DerivedFromObj T, typename... Args>
        [[nodiscard]]
        T* new_object(Args&&... args);

        void mark_value(Value value) noexcept;
        void mark_object(Object* object) noexcept;

        void maybe_collect() noexcept;

        void collect();

        // 保护 C++ 局部变量持有的、尚未入值栈的对象跨分配序列:构造 push、析构 pop,底层 push/pop 私有。
        // 生存期须严格嵌套(temp_roots_ 是朴素栈,无归属校验):A push 后 B push、A 先析构会弹掉 B 的根,无断言可拦。
        // 前置条件:object 非空 -- null 无可守,调用方自行判空跳过挂守卫(from_obj 的 DEBUG 断言兜底)。
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

        // 累计 new_object 调用数(单调不减;bytes_allocated() 是存活字节、随回收回落,看不出分配 churn)。
        [[nodiscard]]
        usize allocation_count() const noexcept {
            return allocation_count_;
        }

        // 运行期压力开关(测试用):开启后每次 new_object 强制 collect。
        void set_stress(const bool enabled) noexcept { is_stress_ = enabled; }

        // GC 禁用锁(计数器实现,支持嵌套):lock_count_ > 0 时 collect() 直跳过。
        void disable_gc() noexcept { ++lock_count_; }
        void enable_gc() noexcept {
            ASSERT(lock_count_ > 0, "enable_gc without matching disable_gc");
            --lock_count_;
        }
        [[nodiscard]] bool is_gc_disabled() const noexcept { return lock_count_ > 0; }

        // RAII 禁用 GC:构造 disable、析构 enable;禁拷贝/移动无碍(make_lock 为 prvalue,必然复制消除)。
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

        // 驻留池 API:命中返回已有串,未命中 new_object 后 insert。
        [[nodiscard]] ObjString* intern_find(StringView str, u32 hash) const noexcept;

        // 两段查找(内容等于 lhs+rhs 拼接):拼接方免先拼整段即可查驻留。
        [[nodiscard]] ObjString* intern_find(StringView lhs, StringView rhs, u32 hash) const noexcept;

        void intern_insert(ObjString* str);

        // VM 根经 std::function 回调接入(组合而非继承,GC 不识 VM 类型;[this] 恰落 std::function SBO,零堆分配)。
        void set_vm_roots(std::function<void(GC&)> tracer) noexcept { vm_roots_tracer_ = std::move(tracer); }

    private:
        void mark_roots_() noexcept;
        void trace_gray_() noexcept;
        void sweep_() noexcept;
        void free_all_() noexcept; // ~GC:释放所有残留对象
        // 销毁单个对象:new_object 的逆(虚析构级联释放子内存 + 归还壳给壳池)。不含链表摘除,由调用方管。
        void delete_object(Object* obj) noexcept;

        // temp roots 底层:Object* 经 from_obj 装箱为 Value 存储。
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
        // 原生 realloc:失败时旧块仍有效,但本出口即 fatal_error 进程退出,无需回收旧块。
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
