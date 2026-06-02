# GC 算法插件接口契约（通用版）
本文件描述 `xgc` 中算法模块应遵守的**通用插件接口契约**。
目标不是约束某一个 collector，而是让以下算法都能在同一框架中接入：
- Mark-Sweep
- Generational Tracing
- Copying / Compacting
- Reference Counting
- RC + Cycle Collector
- 未来的并发 / 增量 / 分区式 Collector

## 0.1 当前仓库状态
截至 2026-06-02，本接口契约已经有了两个 baseline 落地对象：

- `marksweep-stw`
- `gen-copy-ms`

并且以下 shared substrate 已开始形成：

- `core/runtime.c`
- `core/barrier.c`
- `core/heap.c`
- `core/worklist.c`

因此，本文件不是纯理论接口说明，而是当前代码库继续演进时必须遵守的约束。
---
## 1. 设计原则
### 1.1 算法模块不拥有公共对象模型
算法模块不能自行定义全局对象 ABI。对象最小公共头、描述符系统、根扫描模型、屏障入口、heap substrate 应由框架层统一提供。
### 1.2 算法模块只实现“策略”，不重写“平台”
算法模块负责：
- 如何分配
- 何时收集
- 收集时如何遍历与回收
- 是否移动对象
- 是否使用 barrier / remembered set / forwarding
算法模块不负责重新定义：
- VM 如何描述对象布局
- roots 如何被枚举
- threads/safepoints 如何接入
---
## 2. 公共前提类型
算法实现默认依赖以下公共类型：
```c
typedef struct GcRuntime       GcRuntime;
typedef struct GcThreadContext GcThreadContext;
typedef struct GcHeader        GcHeader;
typedef struct GcDescriptor    GcDescriptor;
typedef struct GcConfig        GcConfig;
```
对象描述符至少应提供：
- `trace_slots()`
- `trace_edges()`
- `finalize()`
- 大小 / 标志信息
---
## 3. 算法能力声明
每个算法插件必须声明自己的 capability：
```c
typedef struct GcAlgorithmCaps {
    uint8_t supports_moving;
    uint8_t supports_compaction;
    uint8_t supports_generations;
    uint8_t requires_write_barrier;
    uint8_t requires_read_barrier;
    uint8_t supports_concurrent_mark;
    uint8_t supports_concurrent_relocate;
    uint8_t provides_deterministic_release;
} GcAlgorithmCaps;
```
示例：
- `marksweep-stw`：moving=0, generations=0, write_barrier=0
- `gen-copy-ms`：moving=1, generations=1, write_barrier=1
- `rc-cycle`：deterministic_release=1, write_barrier=1
---
## 4. 核心 vtable
```c
typedef struct GcAlgorithmVTable {
    const char *name;
    GcAlgorithmCaps caps;
    void (*global_init)(GcRuntime *rt, const GcConfig *cfg);
    void (*global_destroy)(GcRuntime *rt);
    void (*thread_init)(GcRuntime *rt, GcThreadContext *thread);
    void (*thread_destroy)(GcRuntime *rt, GcThreadContext *thread);
    void *(*alloc)(GcRuntime *rt,
                   GcThreadContext *thread,
                   const GcDescriptor *desc,
                   size_t size,
                   uint32_t alloc_flags);
    void (*post_alloc)(GcRuntime *rt, GcHeader *obj);
    void (*write_barrier)(GcRuntime *rt,
                          GcThreadContext *thread,
                          GcHeader *owner,
                          GcHeader **slot,
                          GcHeader *old_value,
                          GcHeader *new_value);
    GcHeader *(*read_barrier)(GcRuntime *rt,
                              GcThreadContext *thread,
                              GcHeader **slot);
    void (*collect_minor)(GcRuntime *rt);
    void (*collect_major)(GcRuntime *rt);
    void (*collect_full)(GcRuntime *rt);
    void (*pin)(GcRuntime *rt, GcHeader *obj);
    void (*unpin)(GcRuntime *rt, GcHeader *obj);
} GcAlgorithmVTable;
```
---
## 5. 必须遵守的统一语义
## 5.1 分配语义
- `alloc()` 返回的对象必须已绑定 `GcDescriptor`
- 算法可以决定对象进入哪个 space / generation
- 若算法需要 post-allocation 初始化，可在 `post_alloc()` 完成
## 5.2 写屏障语义
框架统一通过：
```c
gc_store_ref(rt, thread, owner, slot, new_value)
```
进行堆字段写入。
算法插件中的 `write_barrier()` 负责：
- RC retain/release
- remembered set/card marking
- SATB / incremental update bookkeeping
- 其他算法私有屏障行为
### 重要约束
算法插件不得要求 VM 直接绕过框架写 slot。
## 5.3 读屏障语义
- 非 moving / 非并发算法可返回 `NULL` 或直接不实现
- 需要 relocation/read barrier 的算法必须通过该接口完成 slot 解析
## 5.4 roots 与 slot 更新语义
所有支持 moving 的算法都依赖：
- roots 以 slot 形式可枚举
- 对象字段以 slot 形式可枚举
如果某算法 `supports_moving=1`，则它必须能处理 root slot 和 object slot 的重写。
---
## 6. 算法分层建议
## 6.1 `marksweep-stw`
至少实现：
- `alloc`
- `collect_full`
可不实现：
- `collect_minor`
- `read_barrier`
- `pin/unpin`（若全非移动可为空实现）
## 6.2 `gen-copy-ms`
至少实现：
- `alloc`
- `write_barrier`
- `collect_minor`
- `collect_major`
- `pin/unpin`
## 6.3 `rc-cycle`
至少实现：
- `alloc`
- `write_barrier`
- `collect_full`（或 cycle-collect 入口）
其 `write_barrier` 内部可执行 retain/release，但不得破坏统一 API 语义。
---
## 7. 框架层应提供的共享设施
算法插件默认可以调用框架提供的以下能力：
- worklist / queue
- mark bitmap
- card table / remembered set
- heap region iteration
- descriptor-based slot tracing
- roots slot scanning
- handle / pin registry
- safepoint / STW coordination
- stats / pressure / trigger hooks
换句话说，`algo/` 不应重复实现这些 substrate。

### 当前工程约束
在当前仓库阶段，允许算法插件暂时在实现文件中保留少量私有状态；但**新功能应优先下沉到 `core/`**，而不是继续让 `algo/` 目录膨胀为另一层 runtime。

特别是以下能力，后续新增时必须优先考虑 shared substrate 方案：

- old/young/large object accounting
- remembered set / dirty card
- mark bitmap / forwarding metadata
- roots/handles 的统一 glue
---
## 8. 推荐的首批算法插件
建议按以下顺序建设：
1. `marksweep-stw`
2. `gen-copy-ms`
3. `gen-copy-compact`
4. `rc-cycle`
5. `regional-concurrent`
这条顺序的好处是：
- 先验证 tracing substrate
- 再验证 moving + barrier
- 最后验证 RC 作为“另一类策略”接入
比“先做 RC 再想办法扩成通用框架”更稳定。
---
## 9. 结论
真正通用的 GC 插件接口，不应长成“某一种算法的接口抽象”，而应长成：
- 公共对象模型最小化
- 描述符驱动的 slot tracing
- capability-based algorithm vtable
- roots / barrier / heap substrate 统一下沉到框架层
只有这样，`src/algo/<name>/` 才真正代表“算法切换”，而不是“不同目录下的一组专用实现”。
