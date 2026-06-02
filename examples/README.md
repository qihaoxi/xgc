# xgc examples

## marksweep demo

使用 `marksweep` 算法构建并运行：

```bash
cmake -S . -B build-ms -DGC_ALGORITHM=marksweep
cmake --build build-ms -j
./build-ms/examples/marksweep_demo
```

这个示例会：

1. 创建一个三节点链表
2. 先在 root 存在时执行一次 full collect
3. 再清空 root 并执行一次 full collect
4. 观察 finalizer 输出，确认 `marksweep-stw` 真正回收不可达对象

## gen-copy-ms demo

使用 `gen_copy_ms` 算法构建并运行：

```bash
cmake -S . -B build-gen -DGC_ALGORITHM=gen_copy_ms
cmake --build build-gen -j
./build-gen/examples/gen_copy_ms_demo
```

这个示例会：

1. 在 nursery 中分配一个三节点链表
2. 执行一次 minor collect，观察 root 是否发生移动
3. 清空 root 后执行 full collect
4. 观察 finalizer 输出，确认年轻代复制与老年代回收都已接入新框架

