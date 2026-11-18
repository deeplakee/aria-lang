#ifndef ARIA_OBJ_EXCEPTION_HPP
#define ARIA_OBJ_EXCEPTION_HPP

#include "error/ErrorCode.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;
    class ObjString;
    class Error;

    // 运行期错误载荷，持完整消息；位置由未捕获出口补充。
    class ObjException final : public Object {
    public:
        ObjException(i64 code, ObjString* message);
        ~ObjException() override = default;

        ObjException(const ObjException&)            = delete;
        ObjException& operator=(const ObjException&) = delete;
        ObjException(ObjException&&)                 = delete;
        ObjException& operator=(ObjException&&)      = delete;

        // 内部枚举视图；用户传入的数字码可能不在注册表内。
        [[nodiscard]]
        ErrorCode code() const noexcept {
            return static_cast<ErrorCode>(code_);
        }

        [[nodiscard]]
        i64 numeric_code() const noexcept {
            return code_;
        }

        [[nodiscard]]
        ObjString* message() const noexcept {
            return message_;
        }

        [[nodiscard]]
        Error to_error() const;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjException);
        }

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

    private:
        i64        code_;    // 错误码数字(单一存储:VM 报错 = 注册表序号;Error 码参 = 用户所给 int)
        ObjString* message_; // 完整烘焙消息串(intern;指针恒非空,内容可空)
    };

    // message 须为完整消息，原样存储；返回对象须在下个 GC 点前发布进根。
    [[nodiscard]]
    ObjException* new_exception(GC& gc, ErrorCode code, StringView message);

    [[nodiscard]]
    ObjException* new_exception(GC& gc, i64 code, StringView message);

    [[nodiscard]]
    ObjException* new_exception(GC& gc, const Error& error);

} // namespace aria

#endif // ARIA_OBJ_EXCEPTION_HPP
