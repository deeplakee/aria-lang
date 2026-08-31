# 测试目标：链接 aria_core（提供被测 util 头文件）与 GTest
add_executable(aria_tests
        # util
        util/test_utf8.cpp
        util/test_source_file.cpp
        util/test_fs.cpp
        util/test_cli.cpp
        # value
        value/test_value.cpp
        value/test_aria_containers.cpp
        # compile
        compile/test_lexer.cpp
        compile/test_ast.cpp
        compile/test_parser.cpp
        compile/test_astvisitor.cpp
        compile/test_codegen.cpp
        compile/test_compiler.cpp
        # bytecode
        bytecode/test_codeunit.cpp
        # runtime
        runtime/test_ariavm.cpp
        runtime/test_interpret.cpp
        # object
        object/test_objfunction.cpp
        object/test_objmodule.cpp
        # memory
        memory/test_array.cpp
        memory/test_gc.cpp
        memory/test_hashtable.cpp
        memory/test_internpool.cpp
)

target_include_directories(aria_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aria_tests PRIVATE aria_core gtest_main)

# 通过 ctest 运行
include(GoogleTest)
gtest_discover_tests(aria_tests)