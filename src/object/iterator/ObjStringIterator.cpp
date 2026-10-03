#include "object/iterator/ObjStringIterator.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "util/utf8.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjStringIterator::ObjStringIterator(ObjString* str) : ObjIterator{}, str_{str}, offset_{0} {
        ASSERT(str != nullptr, "str must not be null");
    }

    void ObjStringIterator::trace(GC& gc) const noexcept {
        gc.mark_object(str_); // 纯字节串,无子节点,标根即收口
    }

    usize ObjStringIterator::size() const noexcept { return sizeof(ObjStringIterator); }

    bool ObjStringIterator::has_next() const noexcept { return offset_ < str_->length(); }

    Opt<Value> ObjStringIterator::next(AriaVM& vm) {
        // 越界 fail-fast:forIn 靠 has_next 把关,仅手写滥用触此。
        if (offset_ >= str_->length()) {
            return vm.fail(ErrorCode::IterationExhausted, "iterator exhausted");
        }
        // decode_one 恒返宽度 >= 1(offset < length 下,非法序列返 replacement 且宽 1),游标必进、不死循环。
        // 内容零重生(同 fn_chars):坏字节(宽 1 却非 ASCII)走 encode(U+FFFD) 冷臂;宽 1 必 ASCII
        // 走单字符常量表;多字节切 str 原字节。
        const usize start      = offset_;
        const auto [cp, width] = utf8::decode_one(str_->view(), offset_);
        offset_ += width;
        // GC 时序:迭代器于调用方 slots[0] 栈根、str 经其 trace 可达;新串铸后即随返回值写回槽
        // 发布,窗口内无失根对象(命中单字符常量表时无铸造、无 GC 点)。宽 1 内判码点:ASCII 走
        // 常量表直读、非 ASCII 即坏字节走 encode(U+FFFD) 冷臂。
        if (width == 1) {
            if (cp >= 0x80) {
                return Value::from_obj(new_string(vm.gc(), utf8::encode(cp)));
            }
            return Value::from_obj(vm.char_string(static_cast<u8>(cp)));
        }
        return Value::from_obj(new_string(vm.gc(), str_->view().substr(start, width)));
    }

    ObjStringIterator* new_string_iterator(GC& gc, ObjString* str) { return gc.new_object<ObjStringIterator>(str); }

} // namespace aria
