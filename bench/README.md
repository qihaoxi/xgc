# xgc benchmarks

当前先提供一个最小 microbenchmark，用来观察 old→young 写入与 minor GC 的 remembered-set 路径。

## 构建

```bash
cmake -S . -B build-bench -DGC_ALGORITHM=gen_copy_ms -DENABLE_BENCHMARKS=ON
cmake --build build-bench -j
```

## 运行

```bash
./build-bench/bench/bench_old_to_young_writes
./build-bench/bench/bench_old_to_young_writes 500000 512
```

位置参数含义：

1. `iterations`：写入次数，默认 `200000`
2. `minor_every`：每隔多少次写入触发一次 `minor collect`，默认 `256`

这个 benchmark 主要覆盖：

- old object 上的大量 slot 写入
- 写屏障把 old→young 写入记录到 card table
- minor GC 通过 dirty-card remembered set 扫描老年代对象

