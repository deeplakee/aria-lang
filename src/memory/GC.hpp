#ifndef ARIA_GC_HPP
#define ARIA_GC_HPP

#include <algorithm>
#include <functional>
#include <new>

#include "common.hpp"
#include "error/Error.hpp"
#include "memory/InternPool.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    // GC:解释器统一内存分配器 + mark-sweep 回收器。回收:roots = 临时根 + VM 根(经 std::function 回调,标 modules_ +
    // builtins_ + current_ 沿 previous_ 执行链各上下文的值栈/活动帧/挂起错误寄存器 + open upvalue 开链节点)。 **核心不
    // 变式(承重)**:allocate<T>/reallocate<T> 永不触发 GC,GC 仅在 new_object 顶部与 VM safe point 触发。这是与「link-on
    // -alloc + publish-after」对象模型绑定的定义性约束:new_object 返回的对象白色无根,需发布进某个根才安全,而发布动作
    // 本身就是一次 buffer 分配;若该分配触发 GC 会扫掉白色无根对象致悬垂。 gray_stack_ / temp_roots_ 是 GC 自身
    // scratch,不经 GC 分配器、不计入 bytes_allocated_。
    class GC {
    public:
        GC() noexcept;
        ~GC();

        GC(const GC&)            = delete;
        GC& operator=(const GC&) = delete;
        GC(GC&&)                 = delete;
        GC& operator=(GC&&)      = delete;

        // 分配 count 个 T(= count*sizeof(T) 字节),失败走 fatal_error(OutOfMemory)。
        // **INVARIANT: 永不触发 GC(不调 maybe_collect)**--调用方可裸持白色对象跨本调用
        // (见类注释核心不变式)。
        template<typename T>
        [[nodiscard]]
        T* allocate(usize count);

        template<typename T>
        void deallocate(T* p, usize count) noexcept;

        // realloc 语义:new_count==0 退化为 deallocate;否则新分配 + 拷贝 min(old,new) 个 T + 释放旧。
        // **INVARIANT: 永不触发 GC** -- 同 allocate,调本函数期间裸持的白色对象不会被回收。
        template<typename T>
        [[nodiscard]]
        T* reallocate(T* p, usize old_count, usize new_count);

        // **分配层唯一触发 GC 的入口**:顶部 maybe_collect() 在分配前完成(新对象尚未诞生,
        // 不会被本轮 GC 扫到),再 allocate<u8> + placement-new 构造 + 链入 objects_head_。
        // 返回的对象此刻白色、无根,需调用方发布进某根后才安全(见类注释核心不变式)。
        template<DerivedFromObj T, typename... Args>
        [[nodiscard]]
        T* new_object(Args&&... args);

        void mark_value(Value value) noexcept;
        void mark_object(Object* object) noexcept;

        // bytes_allocated_ >= next_gc_(或 stress 开)时 collect()。
        void maybe_collect() noexcept;

        // mark_roots_ -> trace_gray_ -> intern_.remove_white()(摘白表项,须在 sweep 前) -> sweep_
        // -> 调整 next_gc_。
        void collect();

        // C++ 局部变量持有的、尚未入值栈的对象,在分配序列间保护其不被回收。
        // 对外只暴露 Guard / make_guard RAII API(构造 push,析构 pop;禁拷贝/移动),底层
        // push_temp_root/pop_temp_root 私有。生存期须严格嵌套(temp_roots_ 是朴素栈,析构只从
        // 尾部弹 count_ 个、无归属校验):A push 后 B push、A 先析构会弹掉 B 的根,无断言可拦。
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

        // 累计对象分配次数(new_object 调用数,单调不减)。bytes_allocated() 是**存活**字节、随回收
        // 回落,看不出分配 churn;本计数器给确定性(零抖动)的分配读数,供基准对照「少分配」类改动。
        [[nodiscard]]
        usize allocation_count() const noexcept {
            return allocation_count_;
        }

        // 运行期压力开关(测试用):开启后每次 new_object 强制 collect。
        void set_stress(const bool enabled) noexcept { is_stress_ = enabled; }

        // GC 禁用锁(单线程,计数器实现,支持嵌套):collect() 在 lock_count_>0 时跳过(临界区
        // 不回收);maybe_collect 经 collect() 间接受控。
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

        // Intern 驻留池(weak root,字符串专用),委托给 intern_ 成员(保持 private)。new_string
        // 经此实现驻留:命中返回已有串,未命中 new_object 后 insert。驻留池不进 mark_roots_,collect 在 sweep 前调
        // intern_.remove_white() 摘除白色表项防悬垂。
        [[nodiscard]] ObjString* intern_find(StringView str) const noexcept;

        void intern_insert(ObjString* str);

        // VM 根:解释器级共享状态经 std::function 回调接入 mark_roots_(组合而非继承,GC 不识
        // VM 类型;[this] 仅一指针,落在 std::function SBO 内,零堆分配)。collect -> mark_roots_ 末尾调用。
        void set_vm_roots(std::function<void(GC&)> tracer) noexcept { vm_roots_tracer_ = std::move(tracer); }

    private:
        void mark_roots_() noexcept;
        void trace_gray_() noexcept;
        void sweep_() noexcept;
        void free_all_() noexcept; // ~GC:释放所有残留对象
        // 销毁单个对象:new_object 的逆(虚析构级联释放子内存 + 释放壳)。不含链表摘除,由调用方管。
        void delete_object(Object* obj) noexcept;

        // temp roots 底层(由 Guard 调用):Object* 经 from_obj 装箱为 Value 存储。
        void push_temp_root(Object* object) noexcept;
        void pop_temp_root(usize count = 1) noexcept;

        static constexpr usize kInitialGcThreshold = 1024 * 4;
        static constexpr usize kGcGrowFactor       = 2;

        // 声明序:基本类型在前,scratch List/InternPool/tracer 在后 -- 析构逆序使 scratch List 先析构
        // (其 dtor 不碰 bytes_allocated_,无依赖)。
        Object*        objects_head_;
        usize          bytes_allocated_;
        usize          allocation_count_; // 累计分配次数(单调),基准用确定性读数
        usize          next_gc_;
        bool           is_stress_;
        u32            lock_count_; // GC 禁用计数(>0 禁用,支持嵌套 disable/enable)
        List<Object*>  gray_stack_; // GC scratch,不计入 bytes_allocated_
        List<Value>    temp_roots_; // GC scratch,不计入 bytes_allocated_
        InternPool<GC> intern_;     // 字符串驻留池(weak root,slots_ 计入 bytes_allocated_)
        // VM 根标记回调(modules_ + builtins_ + current_ 执行链上各上下文值栈/活动帧/
        // 挂起错误寄存器;AriaVM 注册,可为空)
        std::function<void(GC&)> vm_roots_tracer_;
    };

    template<typename T>
    T* GC::allocate(const usize count) {
        void* p = ::operator new(count * sizeof(T), std::nothrow);
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
            fatal_error(ErrorCode::OutOfMemory, "failed to reallocate {} bytes", new_count * sizeof(T));
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
        ++allocation_count_;
#ifdef DEBUG_LOG_GC
        log_obj_alloc(obj);
#endif
        return obj;
    }

} // namespace aria

#endif // ARIA_GC_HPP
