cmake_minimum_required(VERSION 3.20)
project(aria
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 值表示选择（见 src/common.hpp）：默认 NaN-boxing；ON 时以 TagValue 编译全部目标与测试。
option(ARIA_USE_TAGVALUE "用 TagValue（tag+union, 16B）替代 NaN-boxing 作为 Value 表示" OFF)
if(ARIA_USE_TAGVALUE)
    add_compile_definitions(ARIA_USE_TAGVALUE)
endif()

add_subdirectory(external/isocline)


add_library(aria_core STATIC
        src/sys.hpp
        src/type.hpp
        src/common.hpp
        src/aria.hpp
        src/value/AriaArray.hpp
        src/value/AriaHashTable.hpp
        src/value/NanBoxing.hpp
        src/value/ObjBridge.hpp
        src/value/TagValue.hpp
        src/value/Value.cpp
        src/value/Value.hpp
        src/util/util.hpp
        src/util/io.hpp
        src/util/fs.hpp
        src/util/utf8.hpp
        src/util/source_file.hpp
        src/util/cli.hpp
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
        src/compile/TokenType.hpp
        src/compile/Lexer.cpp
        src/compile/Lexer.hpp
        src/compile/Parser.cpp
        src/compile/Parser.hpp
        src/compile/ast.hpp
        src/compile/ast.cpp
        src/compile/AstVisitor.hpp
        src/compile/FnKind.hpp
        src/compile/FunctionCtx.cpp
        src/compile/FunctionCtx.hpp
        src/compile/ModuleCtx.cpp
        src/compile/ModuleCtx.hpp
        src/compile/CodeGen.cpp
        src/compile/CodeGen.hpp
        src/compile/Compiler.cpp
        src/compile/Compiler.hpp
        src/runtime/AriaVM.cpp
        src/runtime/AriaVM.hpp
        src/runtime/Builtins.cpp
        src/runtime/Builtins.hpp
        src/runtime/FrameStack.hpp
        src/runtime/IteratorMethods.cpp
        src/runtime/IteratorMethods.hpp
        src/runtime/ListMethods.cpp
        src/runtime/ListMethods.hpp
        src/runtime/MapMethods.cpp
        src/runtime/MapMethods.hpp
        src/runtime/Movement.cpp
        src/runtime/Movement.hpp
        src/runtime/value_register.hpp
        src/error/ErrorCode.hpp
        src/error/Error.hpp
        src/error/AriaException.hpp
        src/object/EqualGuard.hpp
        src/object/Object.cpp
        src/object/Object.hpp
        src/object/ObjBoundMethod.cpp
        src/object/ObjBoundMethod.hpp
        src/object/ObjClass.cpp
        src/object/ObjClass.hpp
        src/object/ObjClosure.cpp
        src/object/ObjClosure.hpp
        src/object/ObjString.cpp
        src/object/ObjString.hpp
        src/object/ObjFunction.cpp
        src/object/ObjFunction.hpp
        src/object/ObjInstance.cpp
        src/object/ObjInstance.hpp
        src/object/ObjList.cpp
        src/object/ObjList.hpp
        src/object/ObjMap.cpp
        src/object/ObjMap.hpp
        src/object/ObjModule.cpp
        src/object/ObjModule.hpp
        src/object/ObjNativeFn.cpp
        src/object/ObjNativeFn.hpp
        src/object/ObjException.cpp
        src/object/ObjException.hpp
        src/object/ObjUpvalue.cpp
        src/object/ObjUpvalue.hpp
        src/object/PrintGuard.hpp
        src/object/iterator/ObjIterator.cpp
        src/object/iterator/ObjIterator.hpp
        src/object/iterator/ObjListIterator.cpp
        src/object/iterator/ObjListIterator.hpp
        src/object/iterator/ObjMapIterator.cpp
        src/object/iterator/ObjMapIterator.hpp
        src/bytecode/Disassembler.cpp
        src/bytecode/Disassembler.hpp
)

target_include_directories(aria_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

if (NOT WIN32)
    target_link_libraries(aria_core PRIVATE m)
endif ()
target_compile_features(aria_core PUBLIC cxx_std_23)

# GC 调试日志(经 DEBUG_LOG_GC 宏,被 log_obj_alloc / GC::collect 使用)
option(ARIA_DEBUG_GC "Enable GC debug logging" OFF)
if (ARIA_DEBUG_GC)
    target_compile_definitions(aria_core PUBLIC DEBUG_LOG_GC)
endif ()

# 编译产物反汇编打印(经 DEBUG_PRINT_COMPILED_CODE 宏,被 CodeGen 在每个函数编译完成时
# 打印其 CodeUnit 反汇编,供调试/核对字节码发射)
option(ARIA_DEBUG_PRINT_CODE "Enable compiled code disassembly printing" OFF)
if (ARIA_DEBUG_PRINT_CODE)
    target_compile_definitions(aria_core PUBLIC DEBUG_PRINT_COMPILED_CODE)
endif ()

# 执行跟踪(经 DEBUG_TRACE_EXECUTION 宏,被 AriaVM 主循环在每条指令执行前打印字节码/栈/帧/模块信息,
# 供调试字节码执行)
option(ARIA_DEBUG_TRACE_EXECUTION "Enable bytecode execution tracing" OFF)
if (ARIA_DEBUG_TRACE_EXECUTION)
    target_compile_definitions(aria_core PUBLIC DEBUG_TRACE_EXECUTION)
endif ()

add_executable(aria src/main.cpp src/interpreter.cpp src/interpreter.hpp)
target_include_directories(aria PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aria PRIVATE aria_core)
target_link_libraries(aria PRIVATE isocline)

# === LTO：Release 默认开启（跨平台，可经 -DARIA_ENABLE_LTO=OFF 关闭）===
# Clang 走 thin LTO（构建快、效果接近 full LTO），链接依赖 lld；macOS 例外（Apple ld64
# 原生支持 LTO，无需 -fuse-ld）。GCC 走 -flto（g++ 驱动链接时自动选链接器）；MSVC 走 /GL+/LTCG。
# 用 $<CONFIG:Release> 生成器表达式，兼容单配置（MinGW/Unix Makefiles/Ninja）与多配置
# （VS / Ninja Multi-Config）生成器。仅作用于 aria_core 与 aria，不影响 isocline/gtest。
option(ARIA_ENABLE_LTO "Enable link-time optimization in Release build" ON)
if (ARIA_ENABLE_LTO)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        if (APPLE)
            target_compile_options(aria_core PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            target_compile_options(aria PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            target_link_options(aria PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            message(STATUS "LTO (Release): Clang thin LTO via Apple ld64")
        else()
            target_compile_options(aria_core PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            target_compile_options(aria PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            target_link_options(aria PRIVATE $<$<CONFIG:Release>:-flto=thin> $<$<CONFIG:Release>:-fuse-ld=lld>)
            message(STATUS "LTO (Release): Clang thin LTO via lld")
        endif()
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(aria_core PRIVATE $<$<CONFIG:Release>:-flto>)
        target_compile_options(aria PRIVATE $<$<CONFIG:Release>:-flto>)
        target_link_options(aria PRIVATE $<$<CONFIG:Release>:-flto>)
        message(STATUS "LTO (Release): GCC -flto")
    elseif (MSVC)
        target_compile_options(aria_core PRIVATE $<$<CONFIG:Release>:/GL>)
        target_compile_options(aria PRIVATE $<$<CONFIG:Release>:/GL>)
        target_link_options(aria PRIVATE $<$<CONFIG:Release>:/LTCG>)
        message(STATUS "LTO (Release): MSVC /GL + /LTCG")
    else()
        message(STATUS "LTO (Release): unsupported compiler ${CMAKE_CXX_COMPILER_ID}, skipped")
    endif()
endif()

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
