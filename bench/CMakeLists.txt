# 性能基准(非 gtest):独立可执行文件,链接 aria_core。
# 运行:./build/bench/hashtable_bench
#      ./build/bench/lexer_bench
#      ./build/bench/vm_bench(数字只在优化构建下有意义,见各文件头)
# 源文件级基准(把 lang/ 下的 .aria 当程序逐个起进程计时,可与 .py/.js/.lua/.java 端口对照)不在此处,
# 入口是 bench/lang/lang_bench.py + bench/lang/README.md。
add_executable(hashtable_bench
        hashtable_bench.cpp)
target_link_libraries(hashtable_bench PRIVATE aria_core)

add_executable(lexer_bench
        lexer_bench.cpp)
target_link_libraries(lexer_bench PRIVATE aria_core)

add_executable(vm_bench
        vm_bench.cpp)
target_link_libraries(vm_bench PRIVATE aria_core)
