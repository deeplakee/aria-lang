#include <gtest/gtest.h>

#include <cstring>
#include <string_view>
#include <tuple>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"

using aria::GC;
using aria::new_string;
using aria::ObjString;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;

namespace {

    constexpr StringView kShort = "hello";
    constexpr StringView kLong  = "this is a string longer than fifteen chars";

    static_assert(kShort.size() <= ObjString::kShortCapacity);
    static_assert(kLong.size() > ObjString::kShortCapacity);

    // 测试用 Object 子类型:走地址哈希默认 ctor(Object{ObjType})。
    class ObjDummy : public aria::Object {
    public:
        ObjDummy() : Object{aria::ObjType::BASE} {}
        void        trace(aria::GC&) const noexcept override {}
        aria::usize size() const noexcept override { return sizeof(ObjDummy); }
    };

} // namespace

TEST(GcAlloc, BytesCounted) {
    GC          gc;
    const usize before = gc.bytes_allocated();
    u8*         p      = gc.allocate<u8>(100);
    EXPECT_EQ(gc.bytes_allocated(), before + 100);
    gc.deallocate<u8>(p, 100);
    EXPECT_EQ(gc.bytes_allocated(), before);
}

// allocation_count 是累计分配次数(单调不减,不随回收回落):它是性能基准对照「少分配」类改动的
// 确定性读数(bytes_allocated 是存活字节,看不出 churn)。
TEST(GcAlloc, AllocationCountMonotonic) {
    GC gc;
    EXPECT_EQ(gc.allocation_count(), 0u); // 空 GC 起步

    auto* rooted = gc.new_object<ObjDummy>();
    EXPECT_EQ(gc.allocation_count(), 1u);
    gc.mark_object(rooted); // 标根后再回收:对象存活,计数只增不减
    gc.collect();
    EXPECT_EQ(gc.allocation_count(), 1u);

    std::ignore = gc.new_object<ObjDummy>(); // 与上者一起在下次 collect 被扫(此后均不再解引用)
    gc.collect();
    EXPECT_EQ(gc.allocation_count(), 2u);
}

TEST(GcAlloc, FreeNullIsNoop) {
    GC          gc;
    const usize before = gc.bytes_allocated();
    gc.deallocate<u8>(nullptr, 100);
    EXPECT_EQ(gc.bytes_allocated(), before);
}

TEST(GcAlloc, ReallocCopiesAndAdjusts) {
    GC          gc;
    const usize before = gc.bytes_allocated();
    char*       p      = gc.allocate<char>(4);
    std::memcpy(p, "abcd", 4);
    char* q = gc.reallocate<char>(p, 4, 8);
    EXPECT_EQ(std::memcmp(q, "abcd", 4), 0);
    EXPECT_EQ(gc.bytes_allocated(), before + 8); // 4 -> 8, 净增 4
    gc.deallocate<char>(q, 8);
}

TEST(GcAlloc, ReallocToZeroFrees) {
    GC          gc;
    u8*         p      = gc.allocate<u8>(16);
    const usize before = gc.bytes_allocated();
    u8*         q      = gc.reallocate<u8>(p, 16, 0);
    EXPECT_EQ(q, nullptr);
    EXPECT_LT(gc.bytes_allocated(), before);
}

TEST(ObjString, ShortIsInline) {
    GC   gc;
    auto s = new_string(gc, kShort);
    EXPECT_FALSE(s->is_long());
    EXPECT_EQ(s->length(), kShort.size());
    EXPECT_EQ(s->view(), kShort);
}

TEST(ObjString, LongIsSeparate) {
    GC   gc;
    auto s = new_string(gc, kLong);
    EXPECT_TRUE(s->is_long());
    EXPECT_EQ(s->length(), kLong.size());
    EXPECT_EQ(s->view(), kLong);
}

TEST(ObjString, HashStableForEqualContent) {
    GC   gc;
    auto a = new_string(gc, "same content here!!!!");
    auto b = new_string(gc, "same content here!!!!");
    EXPECT_EQ(a->hash(), b->hash());
}

TEST(GcCollect, EmptyCollectIsNoop) {
    GC gc;
    gc.collect(); // 无对象无根,不崩
    SUCCEED();
}

TEST(GcCollect, UnrootedShortSwept) {
    GC gc;
    std::ignore        = new_string(gc, kShort); // 无根
    const usize before = gc.bytes_allocated();
    gc.collect(); // 显式触发:kShort 被回收(无新分配,字节数严格下降)
    EXPECT_LT(gc.bytes_allocated(), before);
}

TEST(GcCollect, UnrootedLongSwept) {
    GC gc;
    std::ignore        = new_string(gc, kLong); // 无根(壳 + long_chars_)
    const usize before = gc.bytes_allocated();
    gc.collect(); // 显式触发:kLong 壳与 buffer 均回收
    EXPECT_LT(gc.bytes_allocated(), before);
}

TEST(GcCollect, TempRootSurvives) {
    GC gc;
    gc.set_stress(true);
    auto s      = new_string(gc, kLong);
    auto guard  = gc.make_guard(s);          // 保护
    std::ignore = new_string(gc, "trigger"); // 触发 GC:s 被标根 -> 存活
    EXPECT_EQ(s->view(), kLong);             // 未被释放,访问安全
}

TEST(GcCollect, SweepResetsMarks) {
    GC gc;
    gc.set_stress(true);
    auto s      = new_string(gc, kLong);
    auto guard  = gc.make_guard(s);
    std::ignore = new_string(gc, "trigger"); // GC:s 存活,is_marked 复位
    EXPECT_FALSE(s->is_marked());
}

TEST(GcCollect, GuardBalancesTempRoots) {
    GC gc;
    gc.set_stress(true);
    auto s = new_string(gc, kLong);
    {
        auto guard  = gc.make_guard(s);
        std::ignore = new_string(gc, "trigger"); // GC:s 存活
        EXPECT_EQ(s->view(), kLong);
    } // guard 析构 pop 临时根 -> s 不再受保护
    const usize before = gc.bytes_allocated();
    std::ignore        = new_string(gc, "trigger2"); // GC:s 被回收
    EXPECT_LT(gc.bytes_allocated(), before);
}

TEST(Object, AddressHashCtor) {
    GC   gc;
    auto a = gc.new_object<ObjDummy>();
    auto b = gc.new_object<ObjDummy>();
    EXPECT_EQ(a->hash(), a->hash()); // 同一对象哈希稳定
    EXPECT_NE(a->hash(), b->hash()); // 不同对象不同地址 -> 不同哈希
}

TEST(GcLock, DisablePreventsCollect) {
    GC gc;
    gc.set_stress(true);
    gc.disable_gc();
    std::ignore        = new_string(gc, kLong); // 无根;stress 本应回收,但 GC 禁用 -> 保留
    const usize before = gc.bytes_allocated();
    std::ignore        = new_string(gc, "trigger"); // stress 触发 collect,但锁住 -> 不回收
    EXPECT_GE(gc.bytes_allocated(), before);        // 未回收
    gc.enable_gc();
    std::ignore = new_string(gc, "trigger2"); // 恢复 GC,stress 触发 -> kLong/trigger 回收
    EXPECT_LT(gc.bytes_allocated(), before);
}

TEST(GcLock, ExplicitCollectRespectsLock) {
    GC gc;
    std::ignore = new_string(gc, kLong); // 无根
    gc.disable_gc();
    const usize before = gc.bytes_allocated();
    // 显式,但锁住 -> no-op
    gc.collect();
    EXPECT_EQ(gc.bytes_allocated(), before); // 未回收
    gc.enable_gc();
    gc.collect(); // 恢复 -> 回收
    EXPECT_LT(gc.bytes_allocated(), before);
}

TEST(GcLock, NestingAndGuard) {
    GC gc;
    gc.disable_gc();
    gc.disable_gc(); // 嵌套
    EXPECT_TRUE(gc.is_gc_disabled());
    gc.enable_gc();
    EXPECT_TRUE(gc.is_gc_disabled()); // 仍禁用(计数 1)
    gc.enable_gc();
    EXPECT_FALSE(gc.is_gc_disabled()); // 恢复(计数 0)
    {
        auto lock = gc.make_lock(); // RAII
        EXPECT_TRUE(gc.is_gc_disabled());
    }
    EXPECT_FALSE(gc.is_gc_disabled()); // guard 析构恢复
}
