#ifndef ARIA_OBJ_EXCEPTION_HPP
#define ARIA_OBJ_EXCEPTION_HPP

#include "error/ErrorCode.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;
    class ObjString;
    class Error;

    // 异常对象:VM 检测到的运行时错误 / 原生函数报错的装箱载荷(ObjType::EXCEPTION),M3 异常
    // (try/catch/throw)统一异常通道的运行时载体之一。持 ErrorCode + 错误消息串。
    //
    //   - code_ / message_:message_ 存**完整烘焙消息** -- 与 Error::message() 同形,含
    //     "Category: Name" 前缀(运行期装箱经 make_message 另烘 "path:line: " 位置前缀,
    //     编译期为 path:line:col:)。烘焙发生在
    //     raise 侧(经 Error::make_message 烘齐,from_detail 同源经它),本对象原样持有,
    //     自身不做加工;
    //     位置不丢、catch 里 print(e)/str(e) 渲染完整消息不退化。code_ 保留机器标识:
    //     to_error() 与 catch 类型判定(M3)经它取。message_ 指针恒非空(ctor ASSERT;内容可空
    //     -- 无细节的错误以空串兜底),经 intern 驻留同指针。设计定稿见
    //     .claude/reference/runtime/exception-implementation-pitfalls.md 坑 #7(单寄存器模型)。
    //   - to_error():经 Error::from_baked 把 message_ **原样**回传(跳过 make_message 重烘 --
    //     否则双重前缀),供测试 / 嵌入方取 Error 断言。VM 未捕获出口不经本方法,经
    //     AriaVM.cpp uncaught_error_parts 直读 code_/message_ 拆件后 from_baked 一次物化。
    //   - trace():标 message_(唯一 GC 子节点)。
    //   - to_string():渲染完整烘焙消息(M3 catch 的 print(e) 显示 "Category: Name detail")。
    //
    // 地址哈希型可变对象(走 Object{ObjType::EXCEPTION} ctor);equals 保持默认地址相等。
    // final,不再派生。注意区分:本类承载 **解释器报告的错误**,aria 语言自身的 throw(M3)
    // 抛任意 Value,不限定 ObjException。
    class ObjException final : public Object {
    public:
        // message = 完整烘焙消息串(intern;指针恒非空 -- ASSERT;内容可空)。
        ObjException(ErrorCode code, ObjString* message);
        ~ObjException() override = default;

        ObjException(const ObjException&)            = delete;
        ObjException& operator=(const ObjException&) = delete;
        ObjException(ObjException&&)                 = delete;
        ObjException& operator=(ObjException&&)      = delete;

        // 所属错误码(机器标识:分类/名称经 to_string(ErrorCode) / category_of 再取,同 Error)。
        [[nodiscard]]
        ErrorCode code() const noexcept {
            return code_;
        }

        // 完整烘焙消息串(intern;指针恒非空,内容可空)。
        [[nodiscard]]
        ObjString* message() const noexcept {
            return message_;
        }

        // 还原为边界 Error:message_ 已是完整烘焙串(与 Error::message() 同形),经
        // Error::from_baked 原样回传、不再重烘前缀。经本方法脱离 GC(Error 自有 String,无对象引用)。
        // 供测试 / 嵌入方断言;VM 未捕获出口经 uncaught_error_parts 直读两件拆解,不中转 Error 对象。
        [[nodiscard]]
        Error to_error() const;

        // 标 message_(唯一 GC 子节点)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(无外挂子内存;message_ 归 GC 管)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjException);
        }

        // 调试渲染:消息原文(无引号),同 ObjString 显示(to_string)风格;基类 to_string 默认委托本方法,
        // catch 的 print(e)/str(e) 显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ErrorCode  code_;    // 错误码(机器标识)
        ObjString* message_; // 完整烘焙消息串,与 Error::message() 同形(intern;指针恒非空,内容可空)
    };

    // 工厂:完整烘焙消息串以 StringView 传入(**原样存,不经 make_message 烘焙** -- 调用方须传
    // Error::message() / Error::make_message 产物那种已烘好的串,常见形如 "Runtime: TypeMismatch ...")。
    // 内部 new_string 驻留并自行守卫跨下方 new_object 顶 maybe_collect(工厂守「自己创建的」),
    // 调用方传 StringView 即可,无需手动建串根化。返回对象白色无根:调用方须立即发布进某根
    // (如 ctx.raise(Value::from_obj(...)) -- raise 到寄存器后由 VM 根 tracer 标 pending_error 保命)。
    [[nodiscard]]
    ObjException* new_exception(GC& gc, ErrorCode code, StringView message);

} // namespace aria

#endif // ARIA_OBJ_EXCEPTION_HPP
