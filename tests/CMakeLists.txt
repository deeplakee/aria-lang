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
        bytecode/test_disassembler.cpp
        # runtime
        runtime/test_ariavm.cpp
        runtime/test_interpret.cpp
        runtime/test_interpreter.cpp
        # object
        object/test_object.cpp
        object/test_objboundmethod.cpp
        object/test_objclass.cpp
        object/test_objclosure.cpp
        object/test_objfunction.cpp
        object/test_objinstance.cpp
        object/test_objiterator.cpp
        object/test_objlist.cpp
        object/test_objmodule.cpp
        object/test_objnativefn.cpp
        object/test_objexception.cpp
        object/test_objupvalue.cpp
        # memory
        memory/test_array.cpp
        memory/test_gc.cpp
        memory/test_hashtable.cpp
        memory/test_internpool.cpp
        # language（aria 脚本级端到端语料 runner；.aria 脚本本体见 language/ 目录）
        language/test_language_corpus.cpp
)

target_include_directories(aria_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aria_tests PRIVATE aria_core gtest_main)

# language 语料根：runner（test_language_corpus.cpp）经该绝对路径递归发现 .aria 脚本
target_compile_definitions(aria_tests PRIVATE ARIA_LANG_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/language")

# interpreter.cpp 实现 cli_dispatch（test_interpreter 驱动它）；它属 CLI 层，不在 aria_core
# 而归 aria 可执行文件（见根 CMakeLists.txt），故此处单独编译进测试目标以提供符号。
target_sources(aria_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/interpreter.cpp)

# 通过 ctest 运行。
# DISCOVERY_MODE PRE_TEST：ctest 启动时（而非构建后）枚举测试——language 语料是
# 文件驱动参数化（新增 .aria 脚本不触发重链接），构建期枚举会漏新脚本。
include(GoogleTest)
gtest_discover_tests(aria_tests DISCOVERY_MODE PRE_TEST)