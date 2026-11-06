# 性能基准(非 gtest):独立可执行文件,链接 aria_core;数字只在优化构建下有意义(见各文件头)。
# 运行:./build/bench/{hashtable_bench,lexer_bench,vm_bench}
# 源文件级基准(逐 .aria 起进程计时,可跨语言对照)入口在 bench/lang/lang_bench.py + 其 README。
add_executable(hashtable_bench
        hashtable_bench.cpp)
target_link_libraries(hashtable_bench PRIVATE aria_core)

add_executable(lexer_bench
        lexer_bench.cpp)
target_link_libraries(lexer_bench PRIVATE aria_core)

add_executable(vm_bench
        vm_bench.cpp)
target_link_libraries(vm_bench PRIVATE aria_core)
