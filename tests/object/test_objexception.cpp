#include <format>
#include <gtest/gtest.h>

#include <utility>

#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "object/ObjString.hpp"
#include "value/Value.hpp"

using aria::ErrorCode;
using aria::GC;
using aria::i64;
using aria::new_exception;
using aria::new_string;
using aria::Object;
using aria::ObjException;
using aria::StringView;

namespace {

    // 工厂返回对象未根:测试若跨分配继续用 e,须 make_guard 根化(见 new_exception 头注释)。
    ObjException* make_exception(GC& gc, const ErrorCode code, const StringView message) {
        auto e     = new_exception(gc, code, message);
        auto guard = gc.make_guard(e); // 守卫随函数退出释放:调用方拿到的 e 未根,语义同工厂
        return e;
    }

} // namespace

TEST(ObjException, Basics) {
    GC   gc;
    auto e     = make_exception(gc, ErrorCode::TypeMismatch, "Runtime: TypeMismatch boom");
    auto guard = gc.make_guard(e); // 下方 new_string(intern 比较用)可能 collect,先入根
    EXPECT_TRUE(e->is<ObjException>());
    EXPECT_EQ(e->type(), aria::ObjType::EXCEPTION);
    EXPECT_EQ(e->code(), ErrorCode::TypeMismatch);
    ASSERT_NE(e->message(), nullptr); // 指针恒非空
    // message_ 原样存调用方传的完整烘焙串(工厂不经 make_message,不做任何加工)。
    EXPECT_EQ(e->message()->view(), "Runtime: TypeMismatch boom");
    // message 经 new_string 驻留:与同内容另建的串同指针(intern 命中)。
    EXPECT_EQ(e->message(), new_string(gc, "Runtime: TypeMismatch boom"));
}

TEST(ObjException, EmptyMessageIsNonNullInterned) {
    GC   gc;
    auto e = make_exception(gc, ErrorCode::DivisionByZero, "");
    ASSERT_NE(e->message(), nullptr); // 无消息错误:空串 intern 兜底,无合法指针空态
    EXPECT_TRUE(e->message()->view().empty());
}

TEST(ObjException, NumericCodeCarriedVerbatim) {
    // 数字码单一存储(i64):用户所给 int 原样携带(numeric_code 视图);ErrorCode 重载取注册
    // 表序号,两个视图同源(code() = numeric_code() 的枚举 cast,VM 报错路径 cast 保真)。
    GC   gc;
    auto e     = new_exception(gc, static_cast<i64>(1001), "boom");
    auto guard = gc.make_guard(e);
    EXPECT_EQ(e->numeric_code(), 1001);

    auto typed  = make_exception(gc, ErrorCode::TypeMismatch, "Runtime: TypeMismatch boom");
    auto tguard = gc.make_guard(typed);
    EXPECT_EQ(typed->numeric_code(), static_cast<i64>(std::to_underlying(ErrorCode::TypeMismatch)));
    EXPECT_EQ(typed->code(), ErrorCode::TypeMismatch);
}

TEST(ObjException, ToStringRendersBakedMessage) {
    GC   gc;
    auto e = make_exception(gc, ErrorCode::WrongArity, "Runtime: WrongArity function expects 2 arguments, got 1");
    // 渲染完整烘焙消息(M3 catch 的 println(e) 即 "Category: Name detail" 同款文案)。
    EXPECT_EQ(e->to_string(), "Runtime: WrongArity function expects 2 arguments, got 1");
}

TEST(ObjException, ToErrorCopiesBakedMessageVerbatim) {
    // to_error 经 Error::from_baked **原样**回传已烘焙消息,不再重烘前缀(经 from_detail 会把
    // "Runtime: TypeMismatch " 再烘一遍成双重前缀,故 from_baked 跳过 make_message)。
    GC   gc;
    auto e         = make_exception(gc, ErrorCode::TypeMismatch,
                                    "Runtime: TypeMismatch operator '+' requires numbers, got nil and string");
    auto converted = e->to_error();
    EXPECT_EQ(converted.code(), ErrorCode::TypeMismatch);
    EXPECT_EQ(converted.message(), "Runtime: TypeMismatch operator '+' requires numbers, got nil and string");
}

TEST(ObjException, ToErrorRoundTrip) {
    // 全链路回环:Error::from_detail 烘焙 -> message_ 原样存 -> to_error 原样回传,
    // 与 from_detail 直构的 Error 逐字一致。
    GC   gc;
    auto err = aria::Error::from_detail(
            ErrorCode::TypeMismatch,
            std::format("operator '+' requires numbers, got {} and {}", StringView{"nil"}, StringView{"string"}));
    auto e         = make_exception(gc, err.code(), err.message());
    auto converted = e->to_error();
    EXPECT_EQ(converted.code(), err.code());
    EXPECT_EQ(converted.message(), err.message());
    EXPECT_EQ(converted.message(), "Runtime: TypeMismatch operator '+' requires numbers, got nil and string");
}

TEST(ObjException, ToErrorLeavesGc) {
    // Error 自有 String、不持对象引用:to_error 的产物脱离 GC 存活(异常本体被回收后仍可读)。
    GC   gc;
    auto e         = make_exception(gc, ErrorCode::StackOverflow, "Runtime: StackOverflow call frame stack overflow");
    auto converted = e->to_error(); // 先取走 Error 副本
    // e 无根 -> 回收;intern 的 message 无其它根 -> 一并摘除
    gc.collect();
    EXPECT_EQ(converted.code(), ErrorCode::StackOverflow);
    EXPECT_EQ(converted.message(), "Runtime: StackOverflow call frame stack overflow");
}

TEST(ObjException, TraceKeepsMessageAlive) {
    // e 经 guard 标根 -> collect -> e.trace 标 message_,异常与其消息串全存活
    // (collect 末尾对存活对象 unmark 复位,故断言内容存活而非 is_marked,同 ObjModule 测试)。
    GC   gc;
    auto e     = new_exception(gc, ErrorCode::KeyError, "Runtime: KeyError missing key 'k'");
    auto guard = gc.make_guard(e);
    gc.collect();
    EXPECT_EQ(e->code(), ErrorCode::KeyError);
    EXPECT_EQ(e->message()->view(), "Runtime: KeyError missing key 'k'");
}
