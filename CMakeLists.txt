cmake_minimum_required(VERSION 3.20)
project(aria
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#add_library(fmt INTERFACE)
#target_include_directories(fmt INTERFACE third/fmt)
#target_compile_definitions(fmt INTERFACE FMT_HEADER_ONLY)

add_subdirectory(third/isocline)


add_library(aria_core STATIC
        src/sys.hpp
        src/type.hpp
        src/common.hpp
        src/value/AriaArray.hpp
        src/value/AriaHashTable.hpp
        src/value/NanBoxing.hpp
        src/value/TagValue.hpp
        src/value/Value.cpp
        src/value/Value.hpp
        src/util/util.hpp
        src/util/io.hpp
        src/util/fs.hpp
        src/util/utf8.hpp
        src/util/source_file.hpp
        src/bytecode/code.hpp
        src/bytecode/CodeUnit.cpp
        src/bytecode/CodeUnit.hpp
        src/memory/Allocator.hpp
        src/memory/Array.hpp
        src/memory/Buffer.hpp
        src/memory/GC.cpp
        src/memory/GC.hpp
        src/memory/HashTable.hpp
        src/memory/InternPool.hpp
        src/compile/Token.cpp
        src/compile/Token.hpp
        src/compile/Lexer.cpp
        src/compile/Lexer.hpp
        src/compile/Parser.cpp
        src/compile/Parser.hpp
        src/compile/ast.hpp
        src/compile/ast.cpp
        src/compile/AstVisitor.hpp
        src/compile/FunctionCtx.cpp
        src/compile/FunctionCtx.hpp
        src/compile/ModuleCtx.cpp
        src/compile/ModuleCtx.hpp
        src/compile/CodeGen.cpp
        src/compile/CodeGen.hpp
        src/runtime/AriaVM.cpp
        src/runtime/AriaVM.hpp
        src/runtime/FrameStack.hpp
        src/runtime/Movement.cpp
        src/runtime/Movement.hpp
        src/error/ErrorCode.hpp
        src/error/Error.hpp
        src/error/AriaException.hpp
        src/object/Object.cpp
        src/object/Object.hpp
        src/object/ObjString.cpp
        src/object/ObjString.hpp
        src/object/ObjFunction.cpp
        src/object/ObjFunction.hpp
        src/object/ObjModule.cpp
        src/object/ObjModule.hpp
        src/object/ObjNativeFn.cpp
        src/object/ObjNativeFn.hpp
        src/bytecode/Disassembler.cpp
        src/bytecode/Disassembler.hpp
)

target_include_directories(aria_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

#target_link_libraries(aria_core fmt)
if (NOT WIN32)
    target_link_libraries(aria_core PRIVATE m)
endif ()
target_compile_features(aria_core PUBLIC cxx_std_23)

# GC 调试日志(经 DEBUG_LOG_GC 宏,被 log_obj_alloc / GC::collect 使用)
option(ARIA_DEBUG_GC "Enable GC debug logging" OFF)
if (ARIA_DEBUG_GC)
    target_compile_definitions(aria_core PUBLIC DEBUG_LOG_GC)
endif ()

add_executable(aria src/main.cpp)
target_include_directories(aria PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aria PRIVATE aria_core)
target_link_libraries(aria PRIVATE isocline)

# === 测试 ===
enable_testing()

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Windows: 避免与 gtest 的 main 冲突
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(tests)

# === 性能基准(非 gtest,独立可执行) ===
add_subdirectory(bench)
