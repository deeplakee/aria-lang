-- startup_floor 参照端口(与 startup_floor.aria 同规模:空负载)。Lua 无单调墙钟,os.clock() 是
-- CPU 时间(纯计算负载上与本进程墙钟接近,见 lang-bench-notes.md「端口纪律」)。
assert(1 + 1 == 2)
