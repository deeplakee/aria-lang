#include "memory/GC.hpp"

namespace aria {

    GC::GC() noexcept :
        objects_head_{nullptr}, bytes_allocated_{0}, next_gc_{kInitialGcThreshold}, is_stress_{false}, lock_count_{0},
        gray_stack_{}, temp_roots_{}, intern_{this}, vm_roots_tracer_{} {}

    GC::~GC() { free_all_(); }

    void GC::mark_value(Value v) noexcept {
        if (v.is_obj()) {
            mark_object(v.as_obj());
        }
    }

    void GC::mark_object(Object* o) noexcept {
        if (o == nullptr || o->is_marked()) {
            return;
        }
        o->mark();
        gray_stack_.push_back(o);
    }

    void GC::maybe_collect() noexcept {
        if (is_stress_ || bytes_allocated_ >= next_gc_) {
            collect();
        }
    }

    void GC::collect() {
        if (lock_count_ > 0) {
            return; // GC 禁用(临界区)
        }
#ifdef DEBUG_LOG_GC
        const usize before = bytes_allocated_;
        io::println(stderr, "-- gc start ({} bytes) --", before);
#endif
        mark_roots_();
        trace_gray_();
        intern_.remove_white(); // weak root 清理:摘除指向白色 ObjString* 的表项,防 sweep 后悬垂
        sweep_();
        next_gc_ = bytes_allocated_ * kGcGrowFactor;
#ifdef DEBUG_LOG_GC
        io::println(stderr, "-- gc end: {} -> {} (next {}) --", before, bytes_allocated_, next_gc_);
#endif
    }

    void GC::mark_roots_() noexcept {
        for (const Value& v: temp_roots_) {
            mark_value(v);
        }
        // VM 根(M2 起用):经 std::function 回调,标 modules_ + current_ 执行链上各上下文的值栈/活动帧 function/module。
        // M4 再接 open upvalues,M6 升 Movement 为 Object。
        if (vm_roots_tracer_) {
            vm_roots_tracer_(*this);
        }
    }

    void GC::trace_gray_() noexcept {
        while (!gray_stack_.empty()) {
            Object* o = gray_stack_.back();
            gray_stack_.pop_back();
            o->trace(*this);
        }
    }

    void GC::delete_object(Object* obj) noexcept {
        ASSERT(obj != nullptr, "null object");
        const usize sz = obj->size();                   // 虚调用,必须在 ~Object 前
        obj->~Object();                                 // 级联释放子内存(Array / long_chars_)
        deallocate<u8>(reinterpret_cast<u8*>(obj), sz); // 释放壳
    }

    void GC::sweep_() noexcept {
        Object** slot = &objects_head_;
        while (*slot != nullptr) {
            Object* obj = *slot;
            if (obj->is_marked()) {
                obj->unmark(); // 复位,为下轮准备
                slot = &obj->next_;
            } else {
                *slot = obj->next_; // 从链表摘除
                delete_object(obj); // 销毁(new_object 的逆)
            }
        }
    }

    void GC::free_all_() noexcept {
        Object* obj = objects_head_;
        while (obj != nullptr) {
            Object* next = obj->next_;
            delete_object(obj);
            obj = next;
        }
        objects_head_ = nullptr;
    }

    void GC::push_temp_root(Value v) noexcept { temp_roots_.push_back(v); }

    void GC::push_temp_root(Object* o) noexcept { temp_roots_.push_back(Value::from_obj(o)); }

    void GC::pop_temp_root(usize n) noexcept {
        ASSERT(n <= temp_roots_.size(), "pop_temp_root: n exceeds count");
        temp_roots_.resize(temp_roots_.size() - n);
    }

    ObjString* GC::intern_find(const StringView src) const noexcept { return intern_.find(src); }

    void GC::intern_insert(ObjString* s) { intern_.insert(s); }

} // namespace aria
