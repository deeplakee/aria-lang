# 测试目标：链接 aria_core（提供被测 util 头文件）与 GTest
add_executable(aria_tests
        test_utf8.cpp
        test_source_file.cpp
        test_fs.cpp
        test_cli.cpp
        test_lexer.cpp
        test_ast.cpp
        test_parser.cpp
        test_astvisitor.cpp
        test_array.cpp
        test_gc.cpp
        test_hashtable.cpp
        test_internpool.cpp
        test_aria_containers.cpp
        test_value.cpp
        test_codeunit.cpp
        test_objfunction.cpp
        test_objmodule.cpp
        test_ariavm.cpp
        test_codegen.cpp
        test_compiler.cpp
        test_interpret.cpp
)

target_include_directories(aria_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aria_tests PRIVATE aria_core gtest_main)

# 通过 ctest 运行
include(GoogleTest)
gtest_discover_tests(aria_tests)
