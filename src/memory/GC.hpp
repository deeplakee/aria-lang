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

    // GC:解释器统一内存分配器 + mark-sweep 回收器。
    //
    // 两层分配:
    //   - 类型化 trivial 模板(allocate<T>/deallocate<T>/reallocate<T>):count 个 T,
    //     计入 bytes_allocated_,**永不触发 GC**。供 Array<T> / ObjString long_chars_ 等使用。
    //   - Object(new_object<T>):带 Object 头的可追踪对象,链入 objects_head_,
    //     分配前 maybe_collect() 可能触发回收。
    //
    // 回收:三色 mark-sweep。roots = 临时根(Phase 1)+ VM 根(M2 起用,经 std::function 回调,
    //   标 modules_ + builtins_ + current_ 沿 previous_ 执行链各上下文的值栈/活动帧/挂起错误
    //   寄存器;M4 起并标 open upvalue 开链节点);M6 升 Movement 为 Object。
    //   mark_roots_ -> trace_gray_ -> intern_.remove_white() -> sweep_;sweep_ 对未标对象调虚析构(级联释放
    //   子内存:Array 成员自释放 / ObjString long_chars_ 在 ~ObjString 释放)再释放壳。
    //
    // **核心不变式(承重)**:allocate<T>/reallocate<T> 永不触发 GC,GC 仅在 new_object 顶部
    //   与 VM safe point 触发。这不是性能取舍,而是与「link-on-alloc + publish-after」对象
    //   模型绑定的定义性约束:new_object 返回的对象此时已在 objects_head_、白色、无任何根
    //   指向,它要被「发布」进某个根(常量池/intern 池/值栈/globals 表)才真正安全,而发布动作
    //   本身就是一次 buffer 分配(Array::push->reallocate / InternPool::insert->allocate /
    //   HashTable::upsert->allocate)。若该 buffer 分配会触发 GC,此刻白色无根对象会被
    //   sweep,发布进去的即悬垂指针。故 add_constant(new_string(...)) / intern_insert /
    //   globals().upsert 等「fresh 对象裸持跨一次 buffer 分配再发布」的写法全靠此不变式
    //   免守卫。打破它(给 allocate/reallocate 加 maybe_collect)会让所有此类未守卫站点
    //   同时悬垂。此为不变式契约(见各函数注释),靠 review 守;allocate/reallocate 是叶函数,
    //   无间接触发 GC 的现实路径。注意另一方向--裸持白色对象跨真 GC 点(new_object/
    //   new_string/emit_expr)漏 make_guard--本不变式不管,靠显式守卫 + stress GC 测试守。
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
        // 分配 count 个 T(= count*sizeof(T) 字节),失败走 fatal_error(OutOfMemory)。
        // **INVARIANT: 永不触发 GC(不调 maybe_collect)**。调用方据此可裸持白色对象跨本调用
        // (Array::push / InternPool::insert / HashTable::upsert / ObjString 构造子分配等全靠此)。
        // 在此加 maybe_collect 会让所有「fresh 对象 -> 发布进结构」未守卫站点悬垂(见类注释核心不变式)。
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

        // ---- object allocation(可追踪层)----
        // **分配层唯一触发 GC 的入口**:顶部 maybe_collect() 在分配前完成(此刻新对象尚未诞生,
        // 不会被本轮 GC 扫到),再 allocate<u8> + placement-new 构造 + 链入 objects_head_。
        // 返回的对象此刻白色、无根,需调用方发布进某根后才安全(见类注释核心不变式)。
        template<DerivedFromObj T, typename... Args>
        [[nodiscard]]
        T* new_object(Args&&... args);

        // ---- tracing ----
        void mark_value(Value value) noexcept;
        void mark_object(Object* object) noexcept;

        // bytes_allocated_ >= next_gc_(或 stress 开)时 collect()。
        void maybe_collect() noexcept;

        // mark_roots_ -> trace_gray_ -> intern_.remove_white()(摘白表项,须在 sweep 前) -> sweep_
        // -> 调整 next_gc_。
        void collect();

        // ---- temp roots ----
        // C++ 局部变量持有的、尚未入值栈的对象/值,在分配序列间保护其不被回收。
        // 对外只暴露 Guard / make_guard RAII API;底层 push_temp_root/pop_temp_root 为私有,
        // 由 Guard 内部调用(GC 的嵌套类可访问外层私有成员)。
        // RAII 临时根:构造时 push,析构时 pop。禁拷贝/移动(make_guard 经 prvalue 必然复制消除)。
        // 生存期须严格嵌套(temp_roots_ 是朴素栈,析构只从尾部弹 count_ 个、无归属校验):
        // A push 后 B push、A 先析构会弹掉 B 的根,无断言可拦。
        class Guard {
        public:
            explicit Guard(GC* gc) noexcept : gc_{gc}, count_{0} {}

            Guard(GC* gc, Value value) noexcept : gc_{gc}, count_{1} { gc_->push_temp_root(value); }

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

            void push(Value value) noexcept {
                gc_->push_temp_root(value);
                ++count_;
            }

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
        Guard make_guard(Value value) noexcept {
            return Guard{this, value};
        }

        [[nodiscard]]
        Guard make_guard(Object* object) noexcept {
            return Guard{this, object};
        }

        [[nodiscard]]
        usize bytes_allocated() const noexcept {
            return bytes_allocated_;
        }

        // 运行期压力开关(测试用):开启后每次 new_object 强制 collect。
        void set_stress(bool enabled) noexcept { is_stress_ = enabled; }

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
        void push_temp_root(Value value) noexcept;
        void push_temp_root(Object* object) noexcept;
        void pop_temp_root(usize count = 1) noexcept;

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
        std::function<void(GC&)> vm_roots_tracer_; // VM 根标记回调(modules_ + builtins_ + current_
                                                   // 执行链上各上下文值栈/活动帧/挂起错误寄存器;AriaVM 注册,可为空)
    };

    // ---- 模板实现 ----

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
#ifdef DEBUG_LOG_GC
        log_obj_alloc(obj);
#endif
        return obj;
    }

} // namespace aria

#endif // ARIA_GC_HPP
