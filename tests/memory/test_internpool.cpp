#include <gtest/gtest.h>

#include <format>
#include <string_view>
#include <tuple>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

using aria::GC;
using aria::new_string;
using aria::ObjString;
using aria::String;
using aria::StringView;
using aria::usize;
using aria::util::hash_str;

// InternPool 是 GC 的 private 成员,经 new_string + collect 的可观测行为
// (指针身份 / 字节数 / 存活)间接测试。

TEST(InternPool, SameContentReturnsSamePtr) {
    GC   gc;
    auto a = new_string(gc, "hello world");
    auto b = new_string(gc, "hello world");
    EXPECT_EQ(a, b); // 驻留:等价内容共享同一 ObjString*
}

TEST(InternPool, DifferentContentDifferentPtr) {
    GC   gc;
    auto a = new_string(gc, "aaa");
    auto b = new_string(gc, "bbb");
    EXPECT_NE(a, b);
}

TEST(InternPool, LongStringInterning) {
    GC   gc;
    auto a = new_string(gc, "this is a long string over fifteen chars");
    auto b = new_string(gc, "this is a long string over fifteen chars");
    EXPECT_TRUE(a->is_long());
    EXPECT_EQ(a, b); // 长串也驻留
    EXPECT_EQ(a->view(), "this is a long string over fifteen chars");
}

TEST(InternPool, ManyStringsRehash) {
    GC                     gc;
    auto                   lock = gc.make_lock(); // 禁用 GC:本测关注 intern rehash,持裸指针跨分配
    aria::List<ObjString*> strs;
    for (int i = 0; i < 50; ++i) {
        String s = std::format("string-{}", i);
        strs.push_back(new_string(gc, s));
    }
    // 50 个不同串触发多次 rehash;全部驻留 -> 再 make 返回同指针
    for (int i = 0; i < 50; ++i) {
        String s = std::format("string-{}", i);
        EXPECT_EQ(strs[static_cast<aria::usize>(i)], new_string(gc, s));
    }
}

TEST(InternPool, RootedStaysInternedAcrossGc) {
    GC gc;
    gc.set_stress(true);
    auto s1     = new_string(gc, "rooted content here!");
    auto guard  = gc.make_guard(s1);
    std::ignore = new_string(gc, "trigger"); // GC:s1 标记存活
    // find 命中存活表项
    auto s2 = new_string(gc, "rooted content here!");
    EXPECT_EQ(s1, s2); // 同指针(驻留 + 存活)
}

TEST(InternPool, UnrootedInternedCollected) {
    GC gc;
    std::ignore        = new_string(gc, "long unrooted string here!!"); // 无根,intern 后未引用
    const usize before = gc.bytes_allocated();
    gc.collect(); // 白色 -> remove_white 摘表项 -> sweep 释放
    EXPECT_LT(gc.bytes_allocated(), before);
}

// remove_white 的关键验证:无根驻留串被 collect 回收后,其表项必须被摘除,
// 否则再次 make 同内容会 find 命中指向已释放内存的悬垂表项。
// 用字节数判定:remove_white 生效 -> find miss -> 新分配 -> 字节增加。
TEST(InternPool, RemoveWhiteClearsEntry) {
    GC                   gc;
    constexpr StringView content = "long string to intern then collect";
    std::ignore                  = new_string(gc, content); // 无根
    // 释放 + remove_white 摘除表项
    gc.collect();
    const usize before = gc.bytes_allocated();
    auto        s2     = new_string(gc, content); // find 应 miss(表项已摘) -> 新分配
    ASSERT_GT(gc.bytes_allocated(), before);      // 新分配 => remove_white 生效(否则 find 命中悬垂旧串,无新分配)
    EXPECT_EQ(s2->view(), content);               // 新串内容正确
}

TEST(InternPool, MixedRootingSelectiveSurvival) {
    GC gc;
    gc.set_stress(true);
    auto kept   = new_string(gc, "kept-string-content-here");
    auto guard  = gc.make_guard(kept);
    std::ignore = new_string(gc, "dropped-string-content"); // 无根
    // GC:kept 存活,dropped 回收
    std::ignore = new_string(gc, "trigger");
    EXPECT_EQ(kept->view(), "kept-string-content-here"); // kept 存活
    // kept 仍驻留:再 make 同内容返回同指针
    EXPECT_EQ(kept, new_string(gc, "kept-string-content-here"));
    // dropped 已回收:再 make 同内容是新分配(find miss)
    const usize before_drop = gc.bytes_allocated();
    std::ignore             = new_string(gc, "dropped-string-content");
    EXPECT_GT(gc.bytes_allocated(), before_drop);
}

// 两段重载(先查后拼):内容等于 lhs+rhs,哈希经 hash_str(hash_str(lhs), rhs) 续算。
TEST(InternPool, TwoPartFindHitsExistingFullString) {
    GC   gc;
    auto full = new_string(gc, "abcdef"); // 先驻留全串
    // 两段查命中同一指针:拼接方免先拼出整段即可复用已有串
    const auto hit = new_string(gc, "abc", "def", hash_str(hash_str("abc"), "def"));
    EXPECT_EQ(hit, full);
}

TEST(InternPool, TwoPartFindMissMintsThenSelfInterns) {
    GC         gc;
    const auto h = hash_str(hash_str("xy"), "z1");
    auto       s = new_string(gc, "xy", "z1", h); // 未命中 -> 铸造
    EXPECT_EQ(s->view(), "xyz1");
    EXPECT_EQ(s->hash(), hash_str("xyz1"));      // 哈希与单段全算一致
    EXPECT_EQ(new_string(gc, "xy", "z1", h), s); // 自驻留:同参数再查同指针
    EXPECT_EQ(new_string(gc, "xyz1"), s);        // 单段查同内容也命中(跨入口一致)
}

TEST(InternPool, TwoPartFindEmptySegments) {
    GC   gc;
    auto abc = new_string(gc, "abc");
    EXPECT_EQ(new_string(gc, "abc", "", hash_str(hash_str("abc"), "")), abc); // rhs 空
    EXPECT_EQ(new_string(gc, "", "abc", hash_str(hash_str(""), "abc")), abc); // lhs 空
}

TEST(InternPool, TwoPartMintUnderStressGc) {
    GC gc;
    gc.set_stress(true);
    // 未命中腿跨 new_object 的 maybe_collect:lhs/rhs 宿主串须保活(真实调用点靠栈根,测试以
    // guard 等价);stress 下铸造各串之间的分配也触发 GC,故每串诞生即根化。
    auto a     = new_string(gc, "long-prefix-content!!");
    auto guard = gc.make_guard(a);
    auto b     = new_string(gc, "suffix-tail!!");
    auto gb    = gc.make_guard(b);
    auto s     = new_string(gc, a->view(), b->view(), hash_str(a->hash(), b->view()));
    EXPECT_EQ(s->view(), "long-prefix-content!!suffix-tail!!");
}
