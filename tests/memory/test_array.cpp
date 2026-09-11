#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "memory/Array.hpp" // 分配器解耦,不传递 GC.hpp(见 Allocator.hpp)
#include "memory/GC.hpp"    // 显式 include:本测用 GC 作分配器(默认 Alloc=GC)

using aria::Array;
using aria::GC;
using aria::List;
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

TEST(Array, CopyFromList) {
    GC         gc;
    Array<int> buf{&gc};
    List<int>  src{10, 20, 30}; // List(std::vector) 隐式转 Span<const T>,整段一次拷入
    buf.copy_from(src);
    ASSERT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[0], 10);
    EXPECT_EQ(buf[1], 20);
    EXPECT_EQ(buf[2], 30);
}

TEST(Array, CopyFromAppendsAfterExisting) {
    GC              gc;
    Array<int>      buf{&gc};
    const List<int> src{2, 3, 4};
    buf.push(1);
    buf.copy_from(src); // append 语义:接在已有元素之后,不改写
    ASSERT_EQ(buf.size(), 4u);
    for (usize i = 0; i < 4; ++i) {
        EXPECT_EQ(buf[i], static_cast<int>(i) + 1);
    }
}

TEST(Array, CopyFromTriggersGrowth) {
    GC         gc;
    Array<int> buf{&gc};
    buf.push(-1);
    List<int> src(100); // fill 构造走小括号({100} 会成单元素 initializer_list)
    for (usize i = 0; i < 100; ++i) {
        src[i] = static_cast<int>(i) * 3;
    }
    buf.copy_from(src); // 8 -> 128 跨多次几何扩容,reallocate 搬迁后数据须完好
    ASSERT_EQ(buf.size(), 101u);
    EXPECT_EQ(buf[0], -1);
    for (usize i = 0; i < 100; ++i) {
        EXPECT_EQ(buf[i + 1], static_cast<int>(i) * 3);
    }
    EXPECT_GE(buf.capacity(), 101u);
}

TEST(Array, CopyFromEmptyNoOp) {
    GC              gc;
    Array<int>      buf{&gc};
    const List<int> empty{};
    buf.push(7);
    const usize cap_before = buf.capacity();
    buf.copy_from(empty); // 空 src 零操作:不扩容不改内容
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0], 7);
    EXPECT_EQ(buf.capacity(), cap_before);
}
