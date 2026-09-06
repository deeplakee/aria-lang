#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "memory/Array.hpp" // Array 经 TrivialAllocator concept 与分配器解耦,不再传递 GC.hpp
#include "memory/GC.hpp"    // 显式 include:本测用 GC 作分配器(默认 Alloc=GC)

using aria::Array;
using aria::GC;
using aria::usize;
using aria::Value;

TEST(Array, PushAndIndex) {
    GC         gc;
    Array<int> buf{&gc};
    EXPECT_TRUE(buf.empty());
    for (int i = 0; i < 10; ++i) {
        buf.push(i);
    }
    EXPECT_EQ(buf.size(), 10u);
    EXPECT_FALSE(buf.empty());
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(buf[i], i);
    }
    EXPECT_EQ(buf.top(), 9);
    buf.pop();
    EXPECT_EQ(buf.size(), 9u);
    EXPECT_EQ(buf.top(), 8);
}

TEST(Array, CapacityGrowsGeometric) {
    GC         gc;
    Array<int> buf{&gc};
    EXPECT_EQ(buf.capacity(), 0u);
    buf.push(1);
    EXPECT_GE(buf.capacity(), 1u); // 至少 kInitialCapacity(8)
    const usize cap_after_1 = buf.capacity();
    // 填满至当前容量,再 push 一个 -> 触发 2 倍扩容
    while (buf.size() < buf.capacity()) {
        buf.push(0);
    }
    buf.push(0);
    EXPECT_GT(buf.capacity(), cap_after_1);
}

TEST(Array, ReserveAndResize) {
    GC         gc;
    Array<int> buf{&gc};
    buf.reserve(20);
    EXPECT_GE(buf.capacity(), 20u);
    EXPECT_EQ(buf.size(), 0u);
    buf.resize(5, 42);
    EXPECT_EQ(buf.size(), 5u);
    for (usize i = 0; i < 5; ++i) {
        EXPECT_EQ(buf[i], 42);
    }
    buf.resize(3); // 收缩
    EXPECT_EQ(buf.size(), 3u);
}

TEST(Array, TruncateAndClear) {
    GC         gc;
    Array<int> buf{&gc};
    for (int i = 0; i < 5; ++i) {
        buf.push(i);
    }
    buf.truncate(3);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[2], 2);
    buf.clear();
    EXPECT_TRUE(buf.empty());
}

TEST(Array, EmptyDtorNoCrash) {
    GC gc;
    {
        Array<int> buf{&gc};
    } // 空 array 析构不崩
}

TEST(Array, ValueElement) {
    GC           gc;
    Array<Value> buf{&gc};
    buf.push(Value::nil_val());
    buf.push(Value::from_i32(7));
    EXPECT_TRUE(buf[0].is_nil());
    EXPECT_EQ(buf[1].as_int(), 7);
}

TEST(Array, IteratorRangeFor) {
    GC         gc;
    Array<int> buf{&gc};
    // 空态:begin == end(nullptr),range-for 零次循环
    EXPECT_EQ(buf.begin(), buf.end());
    int sum = 0;
    for (int v: buf) {
        ADD_FAILURE() << "empty array 不应迭代";
        sum += v;
    }
    EXPECT_EQ(sum, 0);

    for (int i = 0; i < 5; ++i) {
        buf.push(i * 10);
    }
    int expect = 0;
    for (int v: buf) {
        EXPECT_EQ(v, expect * 10);
        ++expect;
    }
    EXPECT_EQ(expect, 5);
}

TEST(Array, IteratorMutable) {
    GC         gc;
    Array<int> buf{&gc};
    buf.push(3);
    buf.push(1);
    buf.push(2);
    std::sort(buf.begin(), buf.end());
    EXPECT_EQ(buf[0], 1);
    EXPECT_EQ(buf[1], 2);
    EXPECT_EQ(buf[2], 3);
    for (int& v: buf) {
        v *= 2;
    }
    EXPECT_EQ(buf[0], 2);
    EXPECT_EQ(buf[1], 4);
    EXPECT_EQ(buf[2], 6);
}

TEST(Array, ConstIterator) {
    GC         gc;
    Array<int> buf{&gc};
    buf.push(7);
    buf.push(8);
    const Array<int>& cbuf = buf;
    int               sum  = 0;
    for (int v: cbuf) {
        sum += v;
    }
    EXPECT_EQ(sum, 15);
    EXPECT_EQ(*cbuf.cbegin(), 7);
    EXPECT_EQ(cbuf.cend() - cbuf.cbegin(), 2);
    // 非 const 对象上 cbegin/cend 也可用,元素不可写
    static_assert(std::is_const_v<std::remove_reference_t<decltype(*buf.cbegin())>>);
}
