#ifndef ARIA_OBJ_FUNCTION_HPP
#define ARIA_OBJ_FUNCTION_HPP

#include <variant>

#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "object/Object.hpp"
#include "runtime/NativeCallInfo.hpp"

namespace aria {

    class ObjFnProto;
    class ObjUpvalue;
    class ObjClass;
    class ObjString;

    // 运行期函数值:原生与 aria 载荷共用 FUNCTION 标签、方法标记和类型成员面。
    // 载荷就地构造后不切换,variant 自身的判别信息是实现种类的唯一来源。
    class ObjFunction final : public Object {
    public:
        ObjFunction(GC& gc, ObjFnProto* proto);
        ObjFunction(ObjString* name, NativeFn fn);
        ~ObjFunction() override = default;

        ObjFunction(const ObjFunction&)            = delete;
        ObjFunction& operator=(const ObjFunction&) = delete;
        ObjFunction(ObjFunction&&)                 = delete;
        ObjFunction& operator=(ObjFunction&&)      = delete;

        [[nodiscard]]
        bool is_native() const noexcept {
            return std::holds_alternative<NativeData>(payload_);
        }

        [[nodiscard]]
        ObjString* name() const noexcept;

        // 原生专用访问口;aria 函数不持函数指针。
        [[nodiscard]]
        NativeFn fn() const noexcept;

        // aria 专用访问口;原型与捕获数组在函数创建时确定,捕获按 CLOSURE 指令后填。
        [[nodiscard]]
        ObjFnProto* proto() const noexcept;

        [[nodiscard]]
        const Array<ObjUpvalue*>& upvalues() const noexcept;

        [[nodiscard]]
        ObjClass* defining_class() const noexcept {
            return defining_class_;
        }

        [[nodiscard]]
        bool is_method() const noexcept {
            return defining_class_ != nullptr;
        }

        void add_upvalue(ObjUpvalue* upvalue);

        void set_defining_class(ObjClass* klass) noexcept { defining_class_ = klass; }

        // 标定义类,再按载荷标原生名称或 aria 原型与捕获数组。
        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjFunction);
        }

        [[nodiscard]]
        String repr() const override;

        // 两种载荷统一从 Function 类链查询,继承 Object 的绑定策略。
        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

    private:
        struct NativeData {
            ObjString* name;
            NativeFn   fn;
        };

        struct AriaData {
            AriaData(GC& gc, ObjFnProto* proto) : proto{proto}, upvalues{&gc} {}

            ObjFnProto*        proto;
            Array<ObjUpvalue*> upvalues;
        };

        std::variant<NativeData, AriaData> payload_;
        ObjClass*                          defining_class_;
    };

    // aria 工厂:调用方根化 proto;捕获数组初始为空。
    [[nodiscard]]
    ObjFunction* new_function(GC& gc, ObjFnProto* proto);

    // 原生工厂:ObjString* 名称由调用方根化;其他重载内部驻留并自守名称。
    [[nodiscard]]
    ObjFunction* new_function(GC& gc, ObjString* name, NativeFn fn);

    [[nodiscard]]
    ObjFunction* new_function(GC& gc, NativeFn fn);

    [[nodiscard]]
    ObjFunction* new_function(GC& gc, StringView name, NativeFn fn);

} // namespace aria

#endif // ARIA_OBJ_FUNCTION_HPP
