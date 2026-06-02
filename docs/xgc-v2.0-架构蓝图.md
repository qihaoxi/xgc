# xgc v2.1 架构与实现蓝图
> 日期：2026-06-02  
> 面向目标：多算法通用 GC 框架，而非单算法库  
> 用途：**把架构拆分、工程路线、算法实现细则与验收标准合并为一份主蓝图**

---
## 0. 本文档解决什么问题
这份文档同时回答四类问题：

1. **框架应该长成什么样**
2. **当前仓库已经做到哪一步**
3. **接下来应该先做哪些 substrate，而不是继续堆新算法**
4. **每种算法应该如何在统一框架下落地**

它取代原先分开的“架构蓝图”和“实现线路图”，以后 `docs/` 中应只维护这一份主蓝图。

### 0.1 当前状态快照
截至 2026-06-02，仓库已经部分进入本蓝图描述的状态：

- `src/core/` 已存在，并承载：
  - `runtime.c`
  - `barrier.c`
  - `heap.c`
  - `worklist.c`
- `src/algo/` 中已有两个真实 baseline：
  - `marksweep/`
  - `gen_copy_ms/`
- shared substrate 测试已存在：
  - `test_barrier_substrate`
  - `test_heap_substrate`

这意味着当前工作重点不再是“证明多算法框架可行”，而是：

> **把已经出现的 shared substrate 真正稳定下来，并继续抽象化、性能化。**

### 0.2 与其他文档的关系
- `docs/通用高性能混合GC.md`：总设计、算法选择依据、工业参考
- 本文档：模块拆分、工程顺序、实现线路、算法落地细节、测试与 benchmark 路线
- `src/algo/INTERFACE.md`：算法插件接口契约

---
## 1. 目标代码结构
建议把当前代码结构演进为：

```text
include/xgc/
    gc.h                 // 对外统一 API
    gc_types.h           // 公共类型、descriptor、flags、config
    gc_barrier.h         // barrier API
    gc_handle.h          // pin/handle API
    gc_algorithm.h       // algorithm capabilities + vtable
src/
    core/
        runtime.c        // GcRuntime 生命周期
        heap.c           // heap/spaces/regions 管理
        arena.c          // arena/page 分配
        descriptor.c     // descriptor 注册与校验
        barrier.c        // 写屏障/读屏障分发
        rootscan.c       // roots slot scanning glue
        safepoint.c      // STW/safepoint 基础设施
        stats.c          // 统计与压力模型
        worklist.c       // 通用工作栈/队列
        bitmap.c         // mark bitmap / side metadata
        card_table.c     // remembered set / dirty card
        handle.c         // pin / handle / external roots
        bench_hooks.c    // benchmark / metrics glue
    algo/
        marksweep/
            ms.c
        gen_copy_ms/
            young_copy.c
            old_ms.c
        gen_copy_compact/
            young_copy.c
            old_compact.c
            relocate.c
        rc_cycle/
            rc.c
            cycle.c
        regional_concurrent/
            mark.c
            evac.c
            scheduler.c
tests/
    test_barrier_substrate.c
    test_heap_substrate.c
    ...
bench/
    alloc_small.c
    old_to_young_writes.c
    full_collect_graph.c
```

这个结构的重点是：

- **core/** 只放算法无关 substrate
- **algo/** 只放算法差异实现
- **tests/** 同时覆盖 substrate 与 collector
- **bench/** 用来度量分配、屏障、minor/full GC 成本

---
## 2. 顶层模块职责

## 2.1 `runtime`
负责：
- 创建 / 销毁 `GcRuntime`
- 绑定算法 vtable
- 持有 heap / scheduler / barrier set / stats
- 作为 VM 侧唯一入口

## 2.2 `heap`
负责：
- 定义 spaces / generations / regions
- 对接 page/arena 管理
- 提供 heap walk、region census、space iteration
- 提供共享 young/old/large object 统计 API

## 2.3 `descriptor`
负责：
- 维护对象类型描述符
- 暴露 `trace_slots` / `trace_edges` / `finalize`
- 校验 descriptor 的一致性

## 2.4 `barrier`
负责：
- 提供统一写屏障 API
- 将屏障行为分发给当前算法
- 维护 remembered set / dirty owner / dirty card substrate

## 2.5 `rootscan`
负责：
- 连接 VM 的 roots 枚举 SPI
- 提供 slot 级根扫描
- 统一处理线程栈、协程、全局根、handle 根

## 2.6 `handle`
负责：
- pin/unpin
- 长生命周期 C 外部引用
- moving collector 下的稳定引用桥接

## 2.7 `bitmap` / `card_table`
负责：
- mark bits
- age/generation side metadata
- dirty cards / remembered set
- forwarding / relocation 预留位

这些设施都不应绑定到某一个算法文件中。

---
## 3. 统一 API 蓝图

## 3.1 VM 初始化
```c
GcRuntime *gc_runtime_create(const GcConfig *cfg,
                             const GcAlgorithmVTable *algo,
                             const GcVmHooks *hooks);
```

其中 `GcVmHooks` 包含：
- root scanning
- descriptor registry
- thread attach/detach hooks
- finalizer error policy

## 3.2 分配 API
```c
void *gc_alloc_typed(GcRuntime *rt,
                     GcThreadContext *thread,
                     const GcDescriptor *desc,
                     size_t size,
                     uint32_t alloc_flags);
```

## 3.3 写屏障 API
```c
void gc_store_ref(GcRuntime *rt,
                  GcThreadContext *thread,
                  GcHeader *owner,
                  GcHeader **slot,
                  GcHeader *new_value);
```

### 统一含义
- 这是唯一合法的“堆字段写入对象引用”的入口
- 对 RC 算法，它内部做 retain/release
- 对 generational 算法，它内部做 remembered-set update
- 对 incremental/concurrent 算法，它内部维护 tri-color/SATB 不变式

## 3.4 Root / Handle API
```c
GcHandle *gc_handle_acquire(GcRuntime *rt, GcHeader *obj, uint32_t flags);
GcHeader *gc_handle_get(GcHandle *h);
void gc_handle_release(GcHandle *h);
```

这是为 moving collector 准备的长期稳定引用机制。

---
## 4. 当前阶段的总路线
当前推荐总路线如下：

1. **Phase A — `marksweep-stw`**
2. **Phase B — `gen_copy_ms`**
3. **Phase C — `core/heap + core/barrier + side metadata` 完整化**
4. **Phase D — `gen_copy_compact`**
5. **Phase E — `rc-cycle`**
6. **Phase F — incremental / concurrent / regional collector**

### 为什么是这个顺序
- `marksweep-stw` 最适合验证 descriptor / roots / finalizer / STW
- `gen_copy_ms` 最适合验证 moving / barrier / remembered set / pin
- `gen_copy_compact` 建立在 moving + forwarding + relocation 之上
- `rc-cycle` 是另一条算法家族，应该作为插件接入，而不是框架底色
- concurrent collector 复杂度最高，必须最后做

---
## 5. Phase A：`marksweep-stw`

### 5.1 目标
构建一个**最简单但语义完整**的 tracing baseline collector。

### 5.2 依赖 substrate
必须已有：
- `GcHeader`
- `GcDescriptor`
- `trace_slots()`
- root slot scanning
- worklist
- finalizer path
- non-moving allocation path

### 5.3 算法结构
采用标准 STW mark-sweep：

1. Stop-the-world
2. 从 roots / handles 开始 mark
3. descriptor 驱动字段遍历
4. sweep 不可达对象

### 5.4 最小数据结构建议
- 全对象链表 / 节点链表
- 显式 worklist
- `marked` bit（对象侧或 side metadata 均可）

### 5.5 关键实现点
- `alloc()`：加入对象链表
- `collect_full()`：完整 mark + sweep
- `write_barrier()`：可为空实现
- `pin/unpin()`：可先是 no-op / flag-only

### 5.6 当前状态
**已实现 baseline。** 下一步不应为它单独堆太多算法私货，而应优先让它复用更成熟的 `bitmap` / `heap` substrate。

---
## 6. Phase B：`gen_copy_ms`

### 6.1 目标
构建一个**年轻代 copying + 老年代 mark-sweep** 的分代 collector，用于验证框架真的支持 moving collector。

### 6.2 必备 substrate
- exact roots
- slot-based root updates
- slot-based object field tracing
- `GcHandle`
- write barrier
- remembered set
- young/old 空间区分

### 6.3 推荐 heap 布局
最小可用形态：

- `young space`：bump pointer nursery
- `old space`：non-moving object list / arena
- `large object path`：先退化到 old non-moving

### 6.4 推荐 minor GC 时序
1. 暂停 mutator（v1 先 STW）
2. 扫 roots
3. 扫 handles
4. 扫 remembered set（old→young）
5. evacuation：young live objects promote/copy to old
6. 更新 roots/slots
7. 清空 nursery

### 6.5 推荐 full GC 时序
1. 先做一次 minor
2. 再对 old generation 做 mark-sweep
3. 清理 old 不可达对象

### 6.6 forwarding 设计
最小实现可采用：
- forwarding table（from → to）
- 或复制后临时映射链表

在 baseline 中，链表映射可接受；性能版再切到 side metadata / forwarding word。

### 6.7 remembered set 演进

#### v1：dirty owner table
- old 对象写入 young 引用时，记录 owner dirty
- minor 时只扫 dirty owners

#### v2：card table
- old space 按 card 粒度脏化
- minor 时只扫 dirty cards

#### v3：region remembered set
- 每个 region 维护跨代引用索引
- 为 regional collector 打基础

### 6.8 pinning 语义
- v1：young pinned 对象直接分配进 old / non-moving path
- v2：young pinned 留在单独 pin-set，minor 不移动
- v3：支持 pin-aware evacuation strategy

### 6.9 当前状态
**已实现 baseline，并已开始使用 shared barrier/heap substrate。**

当前下一步重点不是再加新功能，而是：
- card-table 化
- LOS
- promotion policy
- 继续去掉算法文件里的私有 bookkeeping

---
## 7. Phase C：`core/` substrate 完整化
这是当前最优先的工作。

### 7.1 `core/heap`
目标：从“计数字段”进化为真正的 heap substrate。

#### 需要提供
- young / old / large object space 抽象
- allocation placement policy
- page / arena / region 的基础结构
- heap walk / iteration
- usage statistics

#### 建议文件布局
- `core/heap.c`
- `core/arena.c`
- `core/page.c`
- `core/space.c`

### 7.2 `core/barrier`
目标：从 dirty-owner table 进化到 card-table。

#### 需要提供
- owner registration
- dirty card marking
- card iteration
- clear dirty cards
- remembered-set query API

### 7.3 `core/bitmap`
为以下算法准备 side metadata：
- marksweep
- gen-copy-compact
- incremental tracing
- concurrent mark

#### 需要提供
- mark bit set/clear/query
- forwarding bit / relocation bit 预留
- per-object or per-card metadata

### 7.4 `core/rootscan`
后续应形成统一 glue：
- thread roots
- coroutine roots
- handle roots
- global roots
- VM-specific registered roots

### 7.5 `core/stats` / `bench`
这里是性能路线的起点，不是附属品。

#### 至少要能记录
- alloc throughput
- pause time
- bytes promoted / reclaimed
- dirty cards scanned
- old objects scanned

### 7.6 Phase C 验收标准
- `gen_copy_ms` 不再私有维护 remembered set 影子实现
- `marksweep` / `gen_copy_ms` 都能复用 heap accounting
- substrate 自身拥有独立单测
- 出现最小 benchmark 目录并能稳定回归

---
## 8. Phase D：`gen_copy_compact`

### 8.1 目标
在 `gen_copy_ms` 的基础上，把 old generation 从 non-moving 提升为 selective compacting。

### 8.2 前提
- moving roots/slots 机制稳定
- pinning 已存在
- forwarding/relocation substrate 已存在
- heap / barrier metadata 已能支撑对象重定位

### 8.3 推荐实现
1. young 仍保持 copying
2. old major collect 改为：
   - mark
   - 计算 relocation
   - 设置 forwarding
   - update roots/slots
   - compact space

### 8.4 核心难点
- old pinned objects
- large objects 不宜频繁 compact
- relocation 顺序与更新次序
- finalizer / weak refs 处理时机

---
## 9. Phase E：`rc-cycle`

### 9.1 目标
把 deterministic release 家族算法以插件形式接入，而不是把框架重新 RC 化。

### 9.2 必须坚持的设计原则
- RC 不是公共对象 ABI
- RC 状态优先考虑 side metadata / 算法私有 state
- 仍然必须走统一 `gc_store_ref()`

### 9.3 推荐实现拆分
- `algo/rc_cycle/rc.c`
- `algo/rc_cycle/cycle.c`

### 9.4 `write_barrier()` 中做什么
- retain new child
- release old child
- 记录 suspect / purple candidate
- 需要时触发 cycle collect

### 9.5 cycle collector建议路径
1. simple trial deletion
2. Bacon-Rajan purple buffer
3. deferred RC + cycle collector
4. 多线程 concurrent cycle collector

---
## 10. Phase F：incremental / concurrent / regional collector

### 10.1 进入这一阶段的前提
只有当以下全部成立后才值得开始：
- exact roots 稳定
- moving young generation 稳定
- barrier substrate 稳定
- card-table / remembered set 稳定
- pin / handle 语义稳定
- heap region/page substrate 稳定

### 10.2 推荐先做哪一种
建议先做：
1. incremental mark-sweep
2. incremental generational tracing
3. regional concurrent collector

而不是一上来做 ZGC/Shenandoah 风格读屏障重定位。

---
## 11. 文件级蓝图

### 11.1 `include/xgc/gc_types.h`
定义：
- `GcHeader`
- `GcDescriptor`
- `GcConfig`
- `GcFlags`
- `GcHandle`
- `GcAlgorithmCaps`

### 11.2 `include/xgc/gc_algorithm.h`
定义：
- `GcAlgorithmVTable`
- 算法注册与 capability 查询 API

### 11.3 `src/core/heap.c`
实现：
- spaces/regions 初始化
- object placement policy
- page/arena allocation
- heap stats

### 11.4 `src/core/barrier.c`
实现：
- 统一 `gc_store_ref()` 的支撑能力
- card/owner dirty tracking
- per-algo write barrier substrate

### 11.5 `src/core/rootscan.c`
实现：
- roots slot enumeration
- thread/coroutine/global roots glue
- handle roots integration

### 11.6 `src/algo/marksweep/ms.c`
实现：
- mark phase
- sweep phase
- full collection

### 11.7 `src/algo/gen_copy_ms/*`
实现：
- nursery allocation
- minor collection
- promotion
- old generation mark-sweep

### 11.8 `src/algo/rc_cycle/*`
实现：
- retain/release fast path
- cycle detection/collection
- deterministic release path

---
## 12. 最小测试矩阵

### 12.1 substrate 层测试
- barrier/card-table
- heap accounting
- bitmap correctness
- rootscan glue
- handle / pin correctness

### 12.2 `marksweep-stw`
- 线性对象图
- 深链 / 深树
- finalizer 顺序
- weak roots

### 12.3 `gen_copy_ms`
- old -> young remembered set
- promotion
- pinned young object
- nursery evacuation root update

### 12.4 `gen_copy_compact`
- forwarding correctness
- slot update correctness
- large object exemption
- pinned object compaction skip

### 12.5 `rc-cycle`
- acyclic immediate release
- self-cycle / bi-cycle
- deferred roots compensation
- multi-thread off / on compile modes

---
## 13. Benchmark 规划
不要只看一个吞吐数，而要按负载类型测。

### 指标
- alloc throughput
- pause time (p50/p95/p99)
- max RSS
- fragmentation ratio
- objects promoted / copied / reclaimed
- write barrier cost
- dirty-card / dirty-owner scan cost

### 场景
- 短命对象密集
- 长寿命图结构
- 资源对象析构频繁
- 协程切换频繁
- 大堆长时间运行

---
## 14. 当前最推荐的下一步
如果目标是“继续完善通用框架，而不是推进某个具体算法”，当前最推荐的下一步是：

> 把 `GcBarrierSet` 从 dirty-owner 表继续推进到真正 card-table / remembered-set substrate。

### 下一步具体任务
1. `core/card_table.c`
2. `core/bitmap.c`
3. `GcBarrierSet` 改为 card 粒度 dirty
4. `gen_copy_ms` minor 扫描改为 dirty-card scan
5. 增加 `test_card_table_substrate`
6. 增加最小 `bench/` 目录

---
## 15. 推荐结论
如果目标是“真正意义上的多算法通用”，最合理的工程路线不是先把某一种算法做到极致，而是：

1. **先建立 descriptor + root slot + heap substrate + barrier 的公共地基**
2. **先落地一个最简单 tracing collector（marksweep-stw）**
3. **再落地一个真正会移动对象的 collector（gen-copy-ms）**
4. **把性能优化下沉为 shared substrate，而不是算法私货**
5. **最后再把 RC 家族、compacting、并发 collector 作为插件扩展**

对当前仓库来说，这条路线最能保证：
- 文档不是“修补某个前案”
- 蓝图不是“只服务于 RC + Bacon-Rajan”
- 架构能真实容纳 Mark-Sweep、Generational、Copying/Compacting、RC 等多类算法
- 下一阶段工作聚焦在**通用框架完善、抽象稳定与性能 substrate**
