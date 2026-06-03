# xgc benchmarks

当前提供两个最小 microbenchmark：

- `alloc_small`：观察小对象分配吞吐
- `old_to_young_writes`：观察 old→young 写入与 minor GC 的 remembered-set 路径

## 构建

```bash
cmake -S . -B build-bench -DGC_ALGORITHM=gen_copy_ms -DENABLE_BENCHMARKS=ON
cmake --build build-bench -j
```

## 运行

```bash
./build-bench/bench/bench_old_to_young_writes
./build-bench/bench/bench_old_to_young_writes 500000 512
./build-bench/bench/bench_alloc_small
./build-bench/bench/bench_alloc_small 800000 1024
```

位置参数含义：

1. `iterations`：写入次数，默认 `200000`
2. `minor_every`：每隔多少次写入触发一次 `minor collect`，默认 `256`

`bench_alloc_small` 的位置参数：

1. `iterations`：分配次数，默认 `500000`
2. `collect_every`：每隔多少次分配触发一次 `minor collect`，默认 `0`（不主动触发）

这个 benchmark 主要覆盖：

- old object 上的大量 slot 写入
- 写屏障把 old→young 写入记录到 card table
- minor GC 通过 dirty-card remembered set 扫描老年代对象

`old_to_young_writes` 现在还会额外输出：

- `minor_runs`
- `avg_minor_ms`
- `minor_collections`
- `dirty_old_objects`
- `dirty_cards`
- `young_used_bytes`
- `old_object_count`
- `old_object_bytes`
- `peak_allocated`

`alloc_small` 主要覆盖：

- 小对象分配吞吐
- 每对象分配成本
- `minor collect` 对分配路径的干扰
- young/old 堆使用状态

