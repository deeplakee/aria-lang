#ifndef ARIA_OBJ_EXCEPTION_HPP
#define ARIA_OBJ_EXCEPTION_HPP

#include "error/ErrorCode.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;
    class ObjString;
    class Error;
    class AriaVM;

    // 异常对象:VM 检测到的运行时错误与原生报错的装箱载荷;message_ 存完整烘焙消息,运行期不含位置前缀。
    class ObjException final : public Object {
    public:
        ObjException(i64 code, ObjString* message);
        ~ObjException() override = default;

        ObjException(const ObjException&)            = delete;
        ObjException& operator=(const ObjException&) = delete;
        ObjException(ObjException&&)                 = delete;
        ObjException& operator=(ObjException&&)      = delete;

        // 错误码(ErrorCode 视图 = 数字码转注册表枚举;VM 报错路径恒为注册表码,cast 保真)。
        [[nodiscard]]
        ErrorCode code() const noexcept {
            return static_cast<ErrorCode>(code_);
        }

        // 错误码数字(i64 视图 = 存储原值;语言面 code() 即此值)。
        [[nodiscard]]
        i64 numeric_code() const noexcept {
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

        // 裸读 override:委托 Exception bootstrap 类表直取原生值。
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override:同一查找命中恒绑 this。
        Opt<Value> load_field_bound(AriaVM& vm, ObjString* name) override;

    private:
        i64        code_;    // 错误码数字(单一存储:VM 报错 = 注册表序号;Error 码参 = 用户所给 int)
        ObjString* message_; // 完整烘焙消息串(intern;指针恒非空,内容可空)
    };

    // 工厂:message 须为已烘好的完整消息串(原样存,不经 make_message);内部 new_string 驻留自守,返回对象白色
    // 无根,须立即发布进根(如 raise 入寄存器)。ErrorCode 重载 = VM 报错装箱;i64 重载 = 数字码参直通。
    [[nodiscard]]
    ObjException* new_exception(GC& gc, ErrorCode code, StringView message);

    [[nodiscard]]
    ObjException* new_exception(GC& gc, i64 code, StringView message);

    // Error 便捷重载:收已烘焙 Error,code + message 逐件转发原样装箱不重烘(与 to_error 对称)。
    [[nodiscard]]
    ObjException* new_exception(GC& gc, const Error& error);

} // namespace aria

#endif // ARIA_OBJ_EXCEPTION_HPP
