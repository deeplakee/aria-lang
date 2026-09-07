#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjString.hpp"

using aria::GC;
using aria::new_string;
using aria::ObjFunction;
using aria::ObjString;

// Object::try_as<T>(is+as 合一):动态类型匹配返回转型指针,否则 nullptr(含 null 入参)。

TEST(ObjectTryAs, MatchReturnsPointer) {
    GC   gc;
    auto s = new_string(gc, "hello");
    EXPECT_EQ(aria::Object::try_as<ObjString>(s), s);
}

TEST(ObjectTryAs, MismatchReturnsNull) {
    GC   gc;
    auto s = new_string(gc, "hello");
    EXPECT_EQ(aria::Object::try_as<ObjFunction>(s), nullptr);
}

TEST(ObjectTryAs, NullObjectReturnsNull) {
    EXPECT_EQ(aria::Object::try_as<ObjString>(static_cast<aria::Object*>(nullptr)), nullptr);
}

// const 重载:const Object* -> const T*。
TEST(ObjectTryAs, ConstOverload) {
    GC                  gc;
    auto                s = new_string(gc, "hello");
    const aria::Object* o = s;
    EXPECT_EQ(aria::Object::try_as<ObjString>(o), s);
    EXPECT_EQ(aria::Object::try_as<ObjFunction>(o), nullptr);
}
