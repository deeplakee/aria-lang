#ifndef ARIA_OBJ_ITERATOR_HPP
#define ARIA_OBJ_ITERATOR_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class ObjString;

    // 迭代器基类(ObjType::ITERATOR):每种可迭代源一个小子类,各自持游标并实现 has_next/next/trace。
    class ObjIterator : public Object {
    public:
        ~ObjIterator() override = default;

        ObjIterator(const ObjIterator&)            = delete;
        ObjIterator& operator=(const ObjIterator&) = delete;
        ObjIterator(ObjIterator&&)                 = delete;
        ObjIterator& operator=(ObjIterator&&)      = delete;

        void trace(GC& gc) const noexcept override = 0;

        [[nodiscard]]
        usize size() const noexcept override = 0;

        // 调试渲染:"<iterator>";显示同文案。
        [[nodiscard]]
        String repr() const override;

        // 从所属类载入:自 Iterator bootstrap 类链起查(本类无值本地状态)。
        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 裸读 override:Iterator bootstrap 类链直取(find_field 纯查询),miss 就地类措辞 fail。
        [[nodiscard]]
        Opt<Value> load_field(AriaExeContext& ctx, ObjString* name) override;

        // 绑定读 override:命中恒绑 this。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaExeContext& ctx, ObjString* name) override;

        // 是否还有下一个元素。纯查询,无分配、不报错。
        [[nodiscard]]
        virtual bool has_next() const noexcept = 0;

        // 取下一元素并推进游标;遍历结束时报 IterationExhausted(返回 nullopt 表示已报错)。
        [[nodiscard]]
        virtual Opt<Value> next(AriaExeContext& ctx) = 0;

    protected:
        // 仅子类可造;地址哈希型。
        ObjIterator();
    };

} // namespace aria

#endif // ARIA_OBJ_ITERATOR_HPP
