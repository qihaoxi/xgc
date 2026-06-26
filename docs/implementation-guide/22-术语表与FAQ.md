# 22 — 术语表、FAQ 与故障排查

> 实现过程中遇到不懂的术语、困惑的设计、或诡异的 bug 时查这里。

---

## 1. 术语表

| 术语 | 英文 | 含义 | 在 xgc 中 |
|------|------|------|----------|
| 可达性 | reachability | 从根出发能否到达对象 | 决定对象生死的唯一标准（tracing） |
| 根 | root | GC 不能回收的引用起点 | 栈、全局、寄存器、handle |
| 根集 | root set | 所有根的集合 | `scan_roots` 枚举 + `rt->handles` |
| 标记 | mark | 把可达对象标记为存活 | `marked` bit / mark bitmap |
| 清扫 | sweep | 释放未标记对象 | 遍历全对象链表 |
| 疏散 | evacuation | 把存活对象复制到新空间 | gen-copy 的 promote |
| 晋升 | promotion | 对象从年轻代移到老年代 | `gc_gen_promote` |
| 转发 | forwarding | 旧地址→新地址映射 | `GcGenForward` / 转发指针 |
| 写屏障 | write barrier | 堆字段写入时的钩子 | `gc_store_ref` → `algo->write_barrier` |
| 读屏障 | read barrier | 读引用时的钩子 | `gc_load_ref` → `algo->read_barrier` |
| 记忆集 | remembered set | 老→新引用记录 | card table |
| 卡表 | card table | 按卡片粒度记脏 | `src/core/card_table.c` |
| 脏卡 | dirty card | 被写过的卡片 | dirty bitmap |
| 三色标记 | tri-color marking | 白/灰/黑标记法 | 增量/并发标记 |
| 三色不变式 | tri-color invariant | 黑不能直接指白 | 写屏障维护 |
| SATB | snapshot-at-the-beginning | 记录旧值的快照屏障 | 并发标记可选 |
| 增量更新 | incremental update | 记录新引用的屏障 | 并发标记可选 |
| STW | stop-the-world | mutator 全停 | `gc_state` 状态机 |
| 安全点 | safepoint | 可安全暂停的位置 | 字节码边界，协作式轮询 |
| Mutator | mutator | 跑业务、改对象图的线程 | VM 线程 |
| Collector | collector | 执行 GC 的逻辑 | 算法 vtable |
| 引用计数 | reference counting | 记录引用数，归零即死 | rc-cycle |
| 延迟 RC | deferred RC | 栈引用不计数 | rc-cycle，栈在 GC 时补偿扫描 |
| 试删除 | trial deletion | Bacon-Rajan 环检测 | rc-cycle 的 collect |
| 紫色缓冲区 | purple buffer | 环检测嫌疑人列表 | rc-cycle |
| 终结器 | finalizer | 对象死前的清理回调 | `desc->finalize` |
| 复活 | resurrection | finalizer 让死对象重新可达 | 两阶段终结处理 |
| 弱引用 | weak reference | 不阻止回收的引用 | `GC_OBJECT_FLAG_WEAK_REF` |
| 钉住 | pinning | 临时禁止对象移动 | `pin/unpin` + handle |
| 句柄 | handle | 外部 C 持有对象的间接层 | `gc_handle` |
| TLAB | thread-local allocation buffer | 线程私有分配缓冲 | 多线程优化 |
| 描述符 | descriptor | 对象布局说明书 | `gc_descriptor` |
| slot | slot | 存指针的字段 | `gc_header**` |
| substrate | substrate | 算法共用的公共零件 | `src/core/` |
| 工作栈 | worklist | 显式遍历栈（防递归溢出） | `gc_worklist` |
| 内联头 | inline metadata header | 对象前置的算法私有头 | 文档 13 方案 A |

---

## 2. 设计 FAQ

### Q1: 为什么 gc_header 只有 4 个字段，不放 mark/rc/color？
不同算法需要的元数据不同。marksweep 要 mark bit，RC 要 rc+color，copying 要 forwarding。全塞进 header 会让每个对象浪费空间，且耦合算法。所以元数据放 side metadata 或内联头（文档 13）。

### Q2: 为什么写屏障必须经过 gc_store_ref，不能直接写字段？
分代 GC 依赖写屏障记录 old→young 引用（remembered set）。如果直接写字段，minor GC 时这个跨代引用会被漏掉，导致年轻代活对象被误回收。

### Q3: 为什么栈引用不计数（延迟 RC）？
栈操作极其频繁（每次函数调用/局部变量赋值）。如果每次都做原子 inc/dec，热路径开销巨大。延迟 RC 让栈不计数，只在 GC 时扫描一次栈做补偿，把成本从"每次栈操作"降到"每次 GC"。

### Q4: 为什么 marksweep 不需要写屏障？
marksweep 每次都从根全量 trace，不依赖"增量"或"分代"信息。没有需要写屏障维护的不变式。所以它的 `write_barrier` 是空函数。

### Q5: handle 和 root 有什么区别？
- root（通过 scan_roots）：VM 内部的引用（栈、全局），GC 每次扫描时枚举
- handle：外部 C 代码持有的引用，登记在 `rt->handles` 链表，跨 GC 周期稳定

### Q6: 为什么 copying GC 能用转发指针破坏 obj->desc，marksweep 不行？
copying 中旧对象所在的整个 nursery 会被 reset 丢弃，旧对象注定消失，破坏 desc 无害。marksweep 中存活对象原地保留，desc 还要用，不能破坏。

### Q7: trace_slots 和 trace_edges 有什么区别？
- `trace_slots(obj, visit_slot)`：visit 收到 `gc_header**`（字段地址），可以**改写**字段 → 用于 moving GC 更新引用
- `trace_edges(obj, visit_obj)`：visit 收到 `gc_header*`（对象），只读 → 用于 mark/统计

### Q8: 为什么 gc_ref 可能为负还判为垃圾？
环内对象被同一个对象多次引用时（如 list 有重复元素），trial deletion 的 subtract 会执行多次，gc_ref 减成负数。负数说明内部引用 ≥ 实际引用，即没有外部引用，是垃圾。CPython 也这么处理。

---

## 3. 故障排查表

| 症状 | 最可能原因 | 排查步骤 |
|------|-----------|---------|
| **use-after-free（ASan 报）** | trace_slots 漏了某字段 → 活对象被回收 | 1. `-DGC_DEBUG=ON` 看 SWEEP_OBJECT 事件<br>2. 确认被释放对象是否真的不可达<br>3. 检查它的引用者的 descriptor.trace_slots 是否覆盖了这个 slot |
| **段错误在 free()** | 内联头法 free 了 obj 而非 block 起始 | 检查 sweep 是否 `free(meta)` 而非 `free(obj)`（文档 13 §2.3） |
| **内存只增不减** | finalize 没释放内部 malloc / 对象未被回收 | 1. dump stats 看 total_allocated<br>2. 检查 descriptor.finalize<br>3. 确认 GC 真的被触发 |
| **对象值错乱（非崩溃）** | 对象被移动但引用没更新 | 1. 检查是否绕过 gc_store_ref<br>2. 检查 trace_slots 是否传 `gc_header**`（不是 `gc_header*`） |
| **GC 后活对象消失** | scan_roots 漏了某个根 | 1. collect 前 dump 对象链表<br>2. 确认所有持有引用的位置都在 scan_roots 中 |
| **多线程卡死** | safepoint poll 缺失，某线程不响应 STW | 检查解释循环/分配路径是否有 GC_SAFEPOINT_POLL |
| **invariant 失败** | 某个计数器与实际不一致 | 看失败的具体 invariant，定位是 heap/barrier/handle 哪个计数错了 |
| **环不被回收（RC）** | purple buffer 没满，没触发 trial deletion | 手动调 gc_collect_full 或降低 purple 容量阈值 |
| **double-free** | 同一对象被回收两次 | 检查对象是否同时在两个管理结构中（如既在 nursery 又在 old list） |
| **realloc 后旧指针失效** | worklist 扩容后用了旧 data 指针 | 扩容后重新读 wl->data |

---

## 4. 调试命令速查

```bash
# 开启调试构建 + 事件日志
cmake -B build-dbg -DGC_DEBUG=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dbg
GC_DEBUG_LOG=gc.log ./build-dbg/tests/test_xxx

# ASan 定位内存错误
cmake -B build-asan -DSAN_ADDRESS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
./build-asan/tests/test_xxx   # ASan 自动报错带栈回溯

# 只看特定事件（如只看 collect）
GC_DEBUG_MASK=0C ./build-dbg/tests/test_xxx

# gdb 定位崩溃
gdb -batch -ex run -ex bt ./build-dbg/tests/test_xxx

# 单独跑一个测试
ctest --test-dir build -R test_marksweep_smoke --output-on-failure
```

---

## 5. "我该改哪个文件"决策表

| 我要做的事 | 改哪里 |
|-----------|--------|
| 加公共 API | `include/xgc/gc.h` + `src/core/runtime.c` |
| 加共享 substrate | `src/core/<new>.c` + `gc_internal.h` 声明 |
| 加新算法 | `src/algo/<name>/` + `gc_algorithm.h` 工厂声明 |
| 改对象元数据存储 | 文档 13 选方案 + 对应算法文件 |
| 加单元测试 | `tests/test_<x>_substrate.c` + `cmake/tests.cmake` |
| 加算法 smoke 测试 | `tests/test_<algo>_smoke.c` + `cmake/tests.cmake` 条件注册 |
| 加示例 | `examples/<x>_demo.c` + `cmake/examples.cmake` |
| 加 benchmark | `bench/<x>.c` + `cmake/benchmarks.cmake` |
| 改编译选项 | `cmake/compiler-options.cmake` |
| 加调试插桩 | 对应文件 + 文档 06 的事件类型 |

---

## 6. 实现完成度自检清单

每完成一个算法，对照此清单确认达到工业级标准：

```
正确性：
  □ smoke test 全部场景通过
  □ ASan 零报错（no leak, no UAF, no overflow）
  □ 环引用正确回收
  □ handle/pin 正确保护对象
  □ finalize 正确调用且只调一次
  □ 弱引用目标死亡后被清 NULL
  □ 空堆/全活/全死 边界正确

健壮性：
  □ 所有公共函数 NULL 输入不崩溃
  □ OOM 时返回 NULL 而非崩溃
  □ worklist 溢出有保底（全量重扫）
  □ 重入防护（finalize/collect 中不递归 GC）

性能（可选，优化阶段）：
  □ 有对应 benchmark
  □ 分配路径无不必要的原子操作
  □ 写屏障是条件式（大部分写入不触发工作）

可观测：
  □ 关键路径有 GC_DEBUG 事件插桩
  □ invariant 检查覆盖该算法的私有状态
  □ stats 正确统计 alloc/reclaim/promote
```
