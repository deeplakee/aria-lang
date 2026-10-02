#ifndef ARIA_OBJ_STRING_ITERATOR_HPP
#define ARIA_OBJ_STRING_ITERATOR_HPP

#include "common.hpp"
#include "object/iterator/ObjIterator.hpp"

namespace aria {

    class ObjString;

    // string 迭代器:持被遍历 string 与字节偏移游标,按码点步进;next 每步产出一个单字符 string。
    class ObjStringIterator final : public ObjIterator {
    public:
        // str 恒非空(ctor ASSERT);offset 出厂 0。
        explicit ObjStringIterator(ObjString* str);

        ~ObjStringIterator() override = default;

        ObjStringIterator(const ObjStringIterator&)            = delete;
        ObjStringIterator& operator=(const ObjStringIterator&) = delete;
        ObjStringIterator(ObjStringIterator&&)                 = delete;
        ObjStringIterator& operator=(ObjStringIterator&&)      = delete;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override;

        [[nodiscard]]
        bool has_next() const noexcept override;

        [[nodiscard]]
        Opt<Value> next(AriaVM& vm) override;

    private:
        ObjString* str_;    // 被遍历者(恒非空;经 trace 标根)
        usize      offset_; // 下一码点的字节偏移(逐码点推进,恒在字符边界)
    };

    // 工厂:分配 ObjStringIterator(单次分配);调用方须已根化 str,返回对象白色无根,建成即写回槽发布。
    [[nodiscard]]
    ObjStringIterator* new_string_iterator(GC& gc, ObjString* str);

} // namespace aria

#endif // ARIA_OBJ_STRING_ITERATOR_HPP
