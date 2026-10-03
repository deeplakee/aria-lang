#include <gtest/gtest.h>

#include <string>

#include "memory/GC.hpp"
#include "memory/StringBuilder.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

using aria::GC;
using aria::new_string;
using aria::ObjString;
using aria::String;
using aria::StringBuilder;
using aria::StringView;
using aria::util::hash_str;

TEST(StringBuilder, AppendGrowsExactWithoutReserve) {
    GC            gc;
    StringBuilder sb{gc};
    const String  piece(100, 'x'); // 未 reserve 逐片追加:扩容精确按需
    const String  expected(1000, 'x');
    for (int i = 0; i < 10; ++i) {
        sb.append(piece);
    }
    EXPECT_EQ(sb.view().size(), 1000);
    EXPECT_EQ(sb.view(), StringView{expected});
    EXPECT_EQ(gc.bytes_allocated(), 1001); // 无空余:终态账 = 内容 + 终态 '\0' 位
}

TEST(StringBuilder, TakeStringLongHandsOffBuffer) {
    GC            gc;
    const String  content(1000, 'y');
    StringBuilder sb{gc};
    sb.append(content);
    auto* s = sb.take_string();
    EXPECT_EQ(s->view(), StringView{content});
    EXPECT_EQ(s->hash(), hash_str(StringView{content}));
    EXPECT_TRUE(s->is_long());
    EXPECT_EQ(new_string(gc, content), s); // 已进驻留池:单段工厂命中同指针
}

TEST(StringBuilder, TakeStringHitsInternPool) {
    GC            gc;
    auto          existing = new_string(gc, "already interned content");
    StringBuilder sb{gc};
    sb.append("already interned content");
    EXPECT_EQ(sb.take_string(), existing); // 命中驻留:零拷贝返回已有串
}

TEST(StringBuilder, TakeStringShortGoesSso) {
    GC            gc;
    StringBuilder sb{gc};
    sb.append("short str");
    auto* s = sb.take_string();
    EXPECT_FALSE(s->is_long());
    EXPECT_EQ(s->view(), "short str");
    EXPECT_EQ(s->hash(), hash_str("short str"));
}

TEST(StringBuilder, EmptyTakeYieldsInternedEmptyString) {
    GC            gc;
    StringBuilder sb{gc};
    auto*         s1 = sb.take_string();
    EXPECT_EQ(s1->view(), "");
    auto* s2 = sb.take_string(); // 复用后再次空 take:命中空串驻留
    EXPECT_EQ(s1, s2);
}

TEST(StringBuilder, TakeThenReuse) {
    GC            gc;
    StringBuilder sb{gc};
    sb.append("first round content");
    auto* first = sb.take_string();
    EXPECT_EQ(first->view(), "first round content");
    sb.append("second round");
    sb.append("!");
    auto* second = sb.take_string();
    EXPECT_EQ(second->view(), "second round!");
    EXPECT_EQ(second->hash(), hash_str("second round!"));
}

TEST(StringBuilder, ReserveAvoidsRegrowth) {
    GC            gc;
    StringBuilder sb{gc};
    sb.reserve(5000);
    const auto   after_reserve = gc.bytes_allocated();
    const String piece(100, 'z');
    for (int i = 0; i < 50; ++i) {
        sb.append(piece); // 5000 字节内追加不再分配
        EXPECT_EQ(gc.bytes_allocated(), after_reserve);
    }
    EXPECT_EQ(sb.view().size(), 5000);
}

TEST(StringBuilder, ReserveSmallerThanCurrentIsNoOp) {
    GC            gc;
    StringBuilder sb{gc};
    sb.append(String(200, 'a'));
    const auto before = gc.bytes_allocated();
    sb.reserve(10); // 只扩不缩
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(sb.view().size(), 200);
}

TEST(StringBuilder, TakeUnderStressGc) {
    GC gc;
    gc.set_stress(true);
    const String  content(2000, 'q');
    StringBuilder sb{gc}; // raw buffer 不受 GC 管理,stress 下 take 安全
    sb.append(content);
    auto* s     = sb.take_string();
    auto  guard = gc.make_guard(s);
    std::ignore = new_string(gc, "trigger");
    EXPECT_EQ(s->view(), StringView{content});
}

TEST(StringBuilder, MutableIterationRewritesInPlace) {
    GC            gc;
    StringBuilder sb{gc};
    sb.append("hello");
    for (char& c: sb) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    EXPECT_EQ(sb.size(), 5);
    EXPECT_EQ(sb.view(), StringView{"HELLO"});
    EXPECT_EQ(sb.take_string()->view(), "HELLO"); // 就地改写不涉哈希失效:take 按当时内容重算
}

TEST(StringBuilder, TakeStringWithKnownHash) {
    GC            gc;
    const String  content(1000, 'y');
    StringBuilder sb{gc};
    sb.reserve(content.size()); // 精确 reserve:分配口径含终态位,take 长臂为同尺寸 realloc
    sb.append(content);
    auto* s = sb.take_string(hash_str(StringView{content})); // 调用方已持内容终态哈希
    EXPECT_EQ(s->hash(), hash_str(StringView{content}));
    EXPECT_EQ(new_string(gc, content), s); // 与免重算的整算版驻留同指针
}

TEST(StringBuilder, SeedConstructorAppendsContent) {
    GC            gc;
    StringBuilder empty{gc, StringView{}}; // 空种串 = 空态
    EXPECT_EQ(empty.size(), 0);
    StringBuilder sb{gc, "seed"};
    EXPECT_EQ(sb.size(), 4);
    EXPECT_EQ(sb.view(), StringView{"seed"});
    sb.append("-more"); // 种后可续 append
    EXPECT_EQ(sb.view(), StringView{"seed-more"});
    EXPECT_EQ(sb.take_string()->view(), "seed-more");
}
