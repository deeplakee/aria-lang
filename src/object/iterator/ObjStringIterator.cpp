#include "object/iterator/ObjStringIterator.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "util/utf8.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjStringIterator::ObjStringIterator(ObjString* str) : ObjIterator{}, str_{str}, offset_{0} {
        // str_ 恒非空:铸造点(fn_iter)解出的即对象,无空态语义。
        ASSERT(str != nullptr, "str must not be null");
    }

    void ObjStringIterator::trace(GC& gc) const noexcept {
        gc.mark_object(str_); // 纯字节串,无子节点,标根即收口
    }

    usize ObjStringIterator::size() const noexcept { return sizeof(ObjStringIterator); }

    bool ObjStringIterator::has_next() const noexcept { return offset_ < str_->length(); }

    Opt<Value> ObjStringIterator::next(AriaVM& vm) {
        // 越界 fail-fast:forIn 靠 has_next 把关,仅绕过协议的手写滥用触此(同 list/map)。
        if (offset_ >= str_->length()) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        // 解码当前码点并按其字节宽推进:offset < length 下 decode_one 恒返宽度 >= 1
        //(非法序列返 replacement 且宽 1),游标必进、不死循环;产出 1-char string。
        const auto [cp, width] = utf8::decode_one(str_->view(), offset_);
        offset_ += width;
        // GC 走查:迭代器在调用方 slots[0] 于栈根、str 经其 trace 可达;new_string 的
        // maybe_collect 在新串诞生前完成,新串随返回值写回槽发布,窗口内无失根对象。
        return Value::from_obj(new_string(vm.gc(), utf8::encode(cp)));
    }

    ObjStringIterator* new_string_iterator(GC& gc, ObjString* str) {
        // 工厂无守卫义务(见头注释)。
        return gc.new_object<ObjStringIterator>(str);
    }

} // namespace aria
