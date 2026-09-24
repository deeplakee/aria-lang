// startup_floor 参照端口(与 startup_floor.aria 同规模:空负载)。JVM 启动成本较大,这一行的总耗时
// 就是它;无负载段故不打印 bench-time。
class Bench_startup_floor {
    public static void main(String[] args) {
        if (1 + 1 != 2) {
            System.exit(1);
        }
    }
}
