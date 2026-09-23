// bench/hashtable_bench.cpp
//
// HashTable 性能基准:测量 set / find-hit / find-miss / erase 的吞吐,
// 对比良好哈希(IntHash)与弱哈希(WeakHash,探测链更长),并附非计时正确性校验。
//
// 仅用 GC 作 Trivial 分配器(allocate/deallocate,不触发 GC 回收 -- 本测不调 new_object),
// 故测的是 HashTable 本身的瑞士表(Swiss Table)探测与 rehash 性能,无 GC 噪声。
//
// 构建:cmake --build build --target hashtable_bench -j
// 运行:./build/bench/hashtable_bench

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <limits>

#include "memory/GC.hpp"
#include "memory/HashTable.hpp"
#include "util/io.hpp"

using aria::GC;
using aria::HashTable;
using aria::u32;
using aria::usize;
using aria::io::print;
using aria::io::println;

namespace {

    // 良好分布(Knuth 乘法):h1/h2 随键充分变化,探测链短。
    struct IntHash {
        u32 operator()(int k) const noexcept { return static_cast<u32>(k) * 0x9E3779B9u; }
    };

    // 弱哈希(k*8):仅低 3 位移位,h1 扩散不足 -> 同簇键聚集、探测链变长(非病理性)。
    // 用于验证 HashTable 在哈希质量下降时的退化是否可接受。
    struct WeakHash {
        u32 operator()(int k) const noexcept { return static_cast<u32>(k) * 8u; }
    };

    struct IntEq {
        bool operator()(int a, int b) const noexcept { return a == b; }
    };

    template<class Hash>
    using IntTable = HashTable<int, int, Hash, IntEq>;

// 始终生效的校验(NDEBUG 下 ASSERT 被消除,故自管 BENCH_CHECK)。
#define BENCH_CHECK(cond, msg)                                                            \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            print(stderr, "[bench] CHECK failed: {} ({}:{})\n", msg, __FILE__, __LINE__); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (false)

    template<class Fn>
    [[nodiscard]] double elapsed_nanos(Fn&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        std::forward<Fn>(fn)();
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(t1 - t0).count();
    }

    // ---- 正确性校验(非计时)----
    template<class Hash>
    void verify_correctness(usize n) {
        GC             gc;
        IntTable<Hash> ht{&gc};
        for (usize i = 0; i < n; ++i) {
            ht.set(static_cast<int>(i), static_cast<int>(i) * 7);
        }
        BENCH_CHECK(ht.size() == n, "size after insert");

        long sum_k = 0, sum_v = 0, cnt = 0;
        for (const auto& entry: ht) {
            sum_k += entry.key;
            sum_v += entry.value;
            ++cnt;
        }
        const long expected = static_cast<long>(n) * (static_cast<long>(n) - 1) / 2;
        BENCH_CHECK(cnt == static_cast<long>(n), "iteration count");
        BENCH_CHECK(sum_k == expected, "sum of keys");
        BENCH_CHECK(sum_v == expected * 7, "sum of values");

        for (usize i = 0; i < n; ++i) {
            auto e = ht.find(static_cast<int>(i));
            BENCH_CHECK(e != nullptr && e->value == static_cast<int>(i) * 7, "find hit value");
        }
        for (usize i = n; i < n + 16; ++i) { // 抽样 miss
            BENCH_CHECK(ht.find(static_cast<int>(i)) == nullptr, "find miss");
        }
        for (usize i = 0; i < n; ++i) {
            BENCH_CHECK(ht.erase(static_cast<int>(i)), "erase existing");
        }
        BENCH_CHECK(ht.size() == 0, "size after full erase");
        for (usize i = 0; i < n; ++i) {
            BENCH_CHECK(ht.find(static_cast<int>(i)) == nullptr, "find after erase");
        }
    }

    // ---- 计时场景 ----
    // 插入含分摊 rehash(随容量增长触发);find/erase 在预建好的表上测纯查询/删除开销。
    // 用 volatile sink 防止编译器消除 find 的返回值。

    template<class Hash>
    double bench_insert(usize n, int trials) {
        double best = std::numeric_limits<double>::max();
        for (int t = 0; t < trials; ++t) {
            GC             gc;
            IntTable<Hash> ht{&gc};
            best = std::min(best, elapsed_nanos([&] {
                                for (usize i = 0; i < n; ++i) {
                                    ht.set(static_cast<int>(i), static_cast<int>(i));
                                }
                            }));
        }
        return best / static_cast<double>(n);
    }

    template<class Hash>
    double bench_find_hit(usize n, int trials) {
        GC             gc;
        IntTable<Hash> ht{&gc};
        for (usize i = 0; i < n; ++i) {
            ht.set(static_cast<int>(i), static_cast<int>(i));
        }
        double best = std::numeric_limits<double>::max();
        for (int t = 0; t < trials; ++t) {
            best = std::min(best, elapsed_nanos([&] {
                                volatile long sink = 0;
                                long          acc  = 0;
                                for (usize i = 0; i < n; ++i) {
                                    auto e = ht.find(static_cast<int>(i));
                                    acc += e ? e->value : -1;
                                }
                                sink = acc;
                            }));
        }
        return best / static_cast<double>(n);
    }

    template<class Hash>
    double bench_find_miss(usize n, int trials) {
        GC             gc;
        IntTable<Hash> ht{&gc};
        for (usize i = 0; i < n; ++i) {
            ht.set(static_cast<int>(i), static_cast<int>(i));
        }
        double best = std::numeric_limits<double>::max();
        for (int t = 0; t < trials; ++t) {
            best = std::min(best, elapsed_nanos([&] {
                                volatile long sink = 0;
                                long          acc  = 0;
                                for (usize i = 0; i < n; ++i) {
                                    auto e = ht.find(static_cast<int>(i + n)); // 必不在表中
                                    acc += e ? e->value : -1;
                                }
                                sink = acc;
                            }));
        }
        return best / static_cast<double>(n);
    }

    template<class Hash>
    double bench_erase(usize n, int trials) {
        double best = std::numeric_limits<double>::max();
        for (int t = 0; t < trials; ++t) {
            GC             gc;
            IntTable<Hash> ht{&gc};
            for (usize i = 0; i < n; ++i) { // 构建不计入计时
                ht.set(static_cast<int>(i), static_cast<int>(i));
            }
            best = std::min(best, elapsed_nanos([&] {
                                for (usize i = 0; i < n; ++i) {
                                    ht.erase(static_cast<int>(i));
                                }
                            }));
        }
        return best / static_cast<double>(n);
    }

    // 构建一次,报告容量与负载因子(解读 find 性能的参照)。
    template<class Hash>
    void report_capacity(const char* hash_name, usize n) {
        GC             gc;
        IntTable<Hash> ht{&gc};
        for (usize i = 0; i < n; ++i) {
            ht.set(static_cast<int>(i), 0);
        }
        const double load = static_cast<double>(n) / static_cast<double>(ht.capacity());
        println("# hash={:<8} N={:<8} cap={:<8} load={:.3f}", hash_name, n, ht.capacity(), load);
    }

    void print_header() {
        println("{:<12} {:<8} {:>10} {:>10} {:>10}", "scenario", "hash", "N", "ns/op", "Mops/s");
        println("{:-<12} {:-<8} {:->10} {:->10} {:->10}", "", "", "", "", "");
    }

    void print_row(const char* scenario, const char* hash, usize n, double ns_per_op) {
        // ops/s = 1e9 / ns_per_op;Mops/s = 1e3 / ns_per_op。
        const double mops = 1e3 / ns_per_op;
        println("{:<12} {:<8} {:>10} {:>10.2f} {:>10.1f}", scenario, hash, n, ns_per_op, mops);
    }

    template<class Hash>
    void run_for_hash(const char* hash_name, const usize* sizes, usize num_sizes, int trials) {
        for (usize s = 0; s < num_sizes; ++s) {
            const usize n = sizes[s];
            verify_correctness<Hash>(n);
            report_capacity<Hash>(hash_name, n);
            print_row("insert", hash_name, n, bench_insert<Hash>(n, trials));
            print_row("find-hit", hash_name, n, bench_find_hit<Hash>(n, trials));
            print_row("find-miss", hash_name, n, bench_find_miss<Hash>(n, trials));
            print_row("erase", hash_name, n, bench_erase<Hash>(n, trials));
            println("");
        }
    }

} // namespace

int main() {
    constexpr int   kTrials  = 5;
    constexpr usize kSizes[] = {10'000, 100'000, 1'000'000};

    println("HashTable performance benchmark (trials={}, best-of)", kTrials);
    println("");
    print_header();
    run_for_hash<IntHash>("IntHash", kSizes, std::size(kSizes), kTrials);
    run_for_hash<WeakHash>("WeakHash", kSizes, std::size(kSizes), kTrials);
    println("done.");
    return 0;
}
