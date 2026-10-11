#include <gtest/gtest.h>

#include "runtime/AriaVM.hpp"
#include "runtime/NativeCallInfo.hpp"
#include "value/Value.hpp"

using aria::AriaExeContext;
using aria::AriaVM;
using aria::i64;
using aria::NativeCallInfo;
using aria::u8;
using aria::usize;
using aria::Value;

namespace {

    TEST(NativeCallInfo, ReadsCallRegionByOffset) {
        AriaVM vm;
        auto&  ctx = *vm.current_context();
        ctx.push(Value::from_int(7));   // 槽 0 = self
        ctx.push(Value::from_int(11));  // a1
        ctx.push(Value::from_f64(2.5)); // a2

        const NativeCallInfo info{ctx, 2};
        EXPECT_EQ(info.size(), u8{2});
        EXPECT_EQ(&info.ctx(), &ctx);
        EXPECT_EQ(info.self().as_int(), 7);
        EXPECT_EQ(info[0].as_int(), 11);
        EXPECT_EQ(info[1].as_f64(), 2.5);
    }

    TEST(NativeCallInfo, ReadsSurviveStackGrowth) {
        AriaVM vm;
        auto&  ctx = *vm.current_context();
        ctx.push(Value::from_int(7));  // 槽 0 = self
        ctx.push(Value::from_int(11)); // a1
        ctx.push(Value::from_int(13)); // a2

        const NativeCallInfo info{ctx, 2};

        for (usize i = 0; i < 2048; ++i) {
            ctx.push(Value::from_int(static_cast<i64>(i))); // 压过初始容量,触发 2x 增长搬迁
        }
        EXPECT_GT(ctx.stack_capacity(), AriaExeContext::kStackInit); // 增长确已发生

        // 访问时按缓冲下标重取基址:搬迁后原调用区读数不变。
        EXPECT_EQ(info.self().as_int(), 7);
        EXPECT_EQ(info[0].as_int(), 11);
        EXPECT_EQ(info[1].as_int(), 13);
    }

    TEST(NativeCallInfo, SetReturnValWritesSlot0) {
        AriaVM vm;
        auto&  ctx = *vm.current_context();
        ctx.push(Value::from_int(7));  // 槽 0 = self
        ctx.push(Value::from_int(11)); // a1

        const NativeCallInfo info{ctx, 1};
        info.set_return_val(Value::from_int(42));
        EXPECT_EQ(info.self().as_int(), 42); // 槽 0 兼返回槽,self 读数随之翻转
    }

} // namespace
