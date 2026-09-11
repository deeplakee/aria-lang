#ifndef ARIA_OBJ_STRING_HPP
#define ARIA_OBJ_STRING_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;

    // 字符串对象:SSO(短串内联 / 长串独立 buffer)。
    //
    //   - 长度 <= kShortCapacity(15):内联 short_chars_[16](15 字符 + NUL),无额外分配。
    //   - 长度  > kShortCapacity:long_chars_ 指向 gc.allocate<char>(length_+1) 的独立 buffer,
    //     ~ObjString 时 gc_->deallocate<char> 释放。壳本身定长(sizeof(ObjString))。
    //
    //   is_long() 由 length_ > kShortCapacity 派生(不存标志位,省 1 字节 + 填充)。
    //   trace() 空(纯字节)。哈希(FNV-1a 32-bit)构造时算出,存 Object::hash_。
    //   持 GC* gc_ 供 ~ObjString 释放 long_chars_(非 Array 子内存的释放统一走虚析构)。
    //
    //   Phase 2 起接 intern 驻留池:new_string 先查 GC 的 InternPool,命中返回已有串,
    //   未命中才 new_object + insert。等价内容的串共享同一 ObjString*。
    class ObjString final : public Object {
    public:
        static constexpr usize kShortCapacity = 15;

    private:
        GC* gc_; // 供 ~ObjString 释放 long_chars_(仅长串用到)
        union {
            char  short_chars_[kShortCapacity + 1]; // 16 字节,与 char* 取大
            char* long_chars_;
        };
        usize length_;

    public:
        ObjString(GC& gc, StringView src);
        ~ObjString() override;

        // 借出内容视图:指向 SSO 内联 buffer 或 long_chars_。非移动 GC 对象地址与缓冲恒定,
        // view 在 ObjString 存活期内有效;但串被 sweep 回收后 view 即悬垂,勿长期持有跨 collect
        // 的 view(经 intern 持串则随持串者保命)。
        [[nodiscard]]
        StringView view() const noexcept;

        // 内容相等(==):先比指针(intern 命中快速路径),再比 view() 字符内容。
        // override Object::equals(默认地址相等)。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 可读描述(显示位):字符内容原文(无引号),如 hello。override Object::to_string
        // (基类默认委托 debug_repr,本类型显示与调试分叉故两者都 override)。
        [[nodiscard]]
        String to_string() const override;

        // 调试渲染(repr 位):字面量形式 `"<转义内容>"`--util::escape_string 转义内部、外层
        // 补双引号(反汇编常量池等调试上下文的字符串约定形态);显示与调试分叉故两者都 override。
        [[nodiscard]]
        String debug_repr() const override;

        [[nodiscard]]
        usize length() const noexcept {
            return length_;
        }

        [[nodiscard]]
        bool is_long() const noexcept {
            return length_ > kShortCapacity;
        }

        // 无 Value 子节点。
        void trace(GC&) const noexcept override {}

        // 壳定长(长串 buffer 不计入壳,由 ~ObjString 单独释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjString);
        }
    };

    // 工厂:返回内容等于 src 的 ObjString*。经 GC 驻留池:命中返回已有串,未命中分配+驻留。
    [[nodiscard]]
    ObjString* new_string(GC& gc, StringView src);

} // namespace aria

#endif // ARIA_OBJ_STRING_HPP
