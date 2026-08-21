# 性能基准(非 gtest):独立可执行文件,链接 aria_core。
# 运行:./build/bench/hashtable_bench
add_executable(hashtable_bench
        hashtable_bench.cpp)
target_link_libraries(hashtable_bench PRIVATE aria_core)