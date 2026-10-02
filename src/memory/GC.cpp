#include "memory/GC.hpp"

namespace aria {

    GC::GC() noexcept :
        objects_head_{nullptr}, bytes_allocated_{0}, allocation_count_{0}, next_gc_{kInitialGcThreshold},
        is_stress_{false}, lock_count_{0}, gray_stack_{}, temp_roots_{}, intern_{this}, vm_roots_tracer_{} {}

    GC::~GC() { free_all_(); }

    void GC::mark_value(const Value value) noexcept {
        if (value.is_obj()) {
            mark_object(value.as_obj());
        }
    }

    void GC::mark_object(Object* object) noexcept {
        if (object == nullptr || object->is_marked()) {
            return;
        }
        object->mark();
        gray_stack_.push_back(object);
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
        intern_.remove_white(); // weak root 摘白
        sweep_();
        next_gc_ = bytes_allocated_ * kGcGrowFactor;
#ifdef DEBUG_LOG_GC
        io::println(stderr, "-- gc end: {} -> {} (next {}) --", before, bytes_allocated_, next_gc_);
#endif
    }

    void GC::mark_roots_() noexcept {
        for (const Value& value: temp_roots_) {
            mark_value(value);
        }
        if (vm_roots_tracer_) {
            vm_roots_tracer_(*this);
        }
    }

    void GC::trace_gray_() noexcept {
        while (!gray_stack_.empty()) {
            Object* object = gray_stack_.back();
            gray_stack_.pop_back();
            object->trace(*this);
        }
    }

    void GC::delete_object(Object* obj) noexcept {
        ASSERT(obj != nullptr, "null object");
        // 虚调用,必须在 ~Object 前
        const usize sz = obj->size();
        // 级联释放子内存
        obj->~Object();
        bytes_allocated_ -= sz;
        shell_pool_.push(sz, obj);
    }

    void GC::sweep_() noexcept {
        Object** slot = &objects_head_;
        while (*slot != nullptr) {
            Object* obj = *slot;
            if (obj->is_marked()) {
                obj->unmark(); // 复位,为下轮准备
                slot = &obj->next_;
            } else {
                *slot = obj->next_;
                delete_object(obj);
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

    void GC::push_temp_root(Object* object) noexcept { temp_roots_.push_back(Value::from_obj(object)); }

    void GC::pop_temp_root(const usize count) noexcept {
        ASSERT(count <= temp_roots_.size(), "count exceeds size");
        temp_roots_.resize(temp_roots_.size() - count);
    }

    ObjString* GC::intern_find(const StringView str, const u32 hash) const noexcept { return intern_.find(str, hash); }

    void GC::intern_insert(ObjString* str) { intern_.insert(str); }

} // namespace aria
