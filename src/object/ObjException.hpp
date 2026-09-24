#ifndef ARIA_OBJ_EXCEPTION_HPP
#define ARIA_OBJ_EXCEPTION_HPP

#include "error/ErrorCode.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;
    class ObjString;
    class Error;

    // 异常对象:VM 检测到的运行时错误 / 原生报错的装箱载荷(ObjType::EXCEPTION)。
    //   - message_ 存**完整烘焙消息**(与 Error::message() 同形,含 "Category: Name" 前缀;运行期装箱不含位置前缀 -- 位置
    //     由未捕获出口的 at 行给出,经 load_module 透传的编译期消息自带 path:line:col: 保留)。烘焙在 raise 侧完成(Error
    //     ::make_message),本对象原样持有;指针恒非空(intern;ctor ASSERT),内容可空(空串兜底)。code_ 是机器标识。
    //   - to_error():经 Error::from_baked **原样**回传(跳过 make_message 重烘,否则双重前缀)。VM 未捕获出口不经本方法,
    //     在 AriaVM::take_uncaught_error 拆件后 from_baked 物化。地址哈希型、final;注意本类承载**解释器报告的错误
    //     **,aria 的 throw 抛任意 Value。
    class ObjException final : public Object {
    public:
        // message = 完整烘焙消息串(intern;指针恒非空 -- ASSERT;内容可空)。
        ObjException(ErrorCode code, ObjString* message);
        ~ObjException() override = default;

        ObjException(const ObjException&)            = delete;
        ObjException& operator=(const ObjException&) = delete;
        ObjException(ObjException&&)                 = delete;
        ObjException& operator=(ObjException&&)      = delete;

        // 所属错误码(机器标识)。
        [[nodiscard]]
        ErrorCode code() const noexcept {
            return code_;
        }

        // 完整烘焙消息串(intern;指针恒非空,内容可空)。
        [[nodiscard]]
        ObjString* message() const noexcept {
            return message_;
        }

        // 还原为边界 Error(message_ 经 from_baked 原样回传,不再重烘);供测试 / 嵌入方断言。
        [[nodiscard]]
        Error to_error() const;

        // 标 message_(唯一 GC 子节点)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(无外挂子内存;message_ 归 GC 管)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjException);
        }

        // 调试渲染:消息原文(无引号);显示同文案,catch 的 println(e)/str(e) 即此文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ErrorCode  code_;    // 错误码(机器标识)
        ObjString* message_; // 完整烘焙消息串(intern;指针恒非空,内容可空)
    };

    // 工厂:message 须为**已烘好的**完整消息串(原样存,不经 make_message);内部 new_string 驻留
    // 并自行守卫,调用方传 StringView 即可。返回对象白色无根,须立即发布进根(如 raise 入寄存器)。
    [[nodiscard]]
    ObjException* new_exception(GC& gc, ErrorCode code, StringView message);

    // Error 便捷重载:收**已烘焙完整消息**的 Error,code + message 逐件转发、原样装箱不重烘(
    // 否则双重前缀);与 to_error() 的 from_baked 反向桥对称。
    [[nodiscard]]
    ObjException* new_exception(GC& gc, const Error& error);

} // namespace aria

#endif // ARIA_OBJ_EXCEPTION_HPP
