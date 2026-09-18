# 性能基准(非 gtest):独立可执行文件,链接 aria_core。
# 运行:./build/bench/hashtable_bench
#      ./build/bench/lexer_bench(数字只在优化构建下有意义,见文件头)
add_executable(hashtable_bench
        hashtable_bench.cpp)
target_link_libraries(hashtable_bench PRIVATE aria_core)

add_executable(lexer_bench
        lexer_bench.cpp)
target_link_libraries(lexer_bench PRIVATE aria_core)
