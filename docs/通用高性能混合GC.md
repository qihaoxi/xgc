# 通用可插拔高性能 GC 框架 — 正式设计文档
> 版本：v2.0  
> 日期：2026-06-02  
> 定位：**纯 C、面向精确根扫描 VM 的多算法通用 GC 框架**
---
## 0. 摘要
本文档描述的不是某一种 GC 算法，而是一个**支持多种 GC 算法切换的运行时框架**。目标是为解释器、虚拟机、运行时系统提供一套统一底座，使下列算法都能在同一框架下实现或共存：
- 纯 Mark-Sweep
- 分代 Tracing
- Copying / Compacting
- Reference Counting
- RC + Cycle Collector
- 并发 / 增量 / 分区式 GC
框架本身不预设“RC 是主算法”或“Tracing 是主算法”。它只定义：
1. **对象模型**：对象如何被描述、如何枚举指针字段、如何终结
2. **根模型**：如何扫描线程栈、寄存器、协程、全局对象根
3. **堆模型**：对象位于哪些 arena / region / generation 中
4. **屏障模型**：mutator 如何与 collector 交互
5. **算法插件模型**：不同 GC 算法如何挂接到统一底座
这样，GC 算法成为可替换模块，运行时平台成为长期稳定资产。

### 0.1 当前仓库对应关系
截至 2026-06-02，仓库中的实现与本文档的关系如下：

- 已落地 baseline 算法：
  - `marksweep-stw`
  - `gen-copy-ms`
- 已形成的 shared substrate：
  - `core/runtime.c`
  - `core/barrier.c`
  - `core/heap.c`
  - `core/worklist.c`
- 已存在的 substrate 测试：
  - `test_barrier_substrate`
  - `test_heap_substrate`

也就是说，本文档不再只是前瞻设计；其中一部分能力已经在仓库中形成 baseline 实现。

### 0.2 文档分工
建议将文档按以下方式理解：

- 本文档：**总设计与算法选择依据**
- `docs/xgc-v2.0-架构蓝图.md`：**架构拆分 + 工程路线 + 算法落地细节的一体化主蓝图**
- `src/algo/INTERFACE.md`：**算法插件接口契约**

如果要开始编码，优先同时阅读本文档的第 3/4/5 章，以及 `docs/xgc-v2.0-架构蓝图.md`。
---
## 1. 设计目标
## 1.1 总目标
构建一个适用于纯 C 运行时的、**可插拔、多算法、可分阶段演进** 的 GC 框架，满足以下要求：
- 对 VM 类型系统无硬编码依赖
- 对单线程、单 OS 线程多协程、多线程运行时均可适配
- 同时支持非移动与移动型 GC
- 同时支持精确 tracing 与 reference counting 家族算法
- 支持从简单 STW 实现演进到分代、增量、并发实现
- 对外提供稳定 API，对内允许多种算法共存与替换
## 1.2 成功标准
一个“真正通用”的 GC 框架，至少应满足：
1. **对象头不绑定单一算法元数据**  
   不能把 `rc`、`gc_ref`、`purple` 等字段固化为所有算法的前提。
2. **移动对象与非移动对象都能表达**  
   如果框架只支持“对象地址永不变化”，那它天然排斥 copying / compacting。
3. **写屏障 / 读屏障是框架能力，而不是某算法私货**  
   不同算法对屏障需求不同，但框架必须为这些算法提供统一挂接点。
4. **根扫描必须精确**  
   想支持 moving collector，根扫描必须能更新 slot，而不只是“看到对象”。
5. **堆布局必须可多态**  
   至少要能表达：
   - malloc/arena 型非移动堆
   - semi-space copying
   - region/segment based heap
   - generational heap
## 1.3 非目标
本文档不追求：
- 在 v1 就落地工业级 ZGC / Shenandoah 级别实现
- 让任意 C 原始指针在 moving GC 下完全无约束工作
- 在没有精确根信息的前提下安全支持 compacting
- 用一个固定对象头同时最优支持所有算法
---
## 2. 需求矩阵
| 场景 | 必需能力 | 对框架的要求 |
|------|----------|--------------|
| 单线程解释器 | 低复杂度、可调试 | 支持最简单 STW collector |
| 单线程多协程 | 扫描挂起协程根 | 根扫描 API 必须覆盖 continuation/scheduler |
| 多线程 VM | Safepoint / STW / 线程本地分配 | 明确 mutator 生命周期和线程上下文 |
| 资源敏感型对象 | 可预测释放或 finalizer | 支持 RC 或显式 finalization 语义 |
| 临时对象非常多 | 年轻代高吞吐 | 需要 generational + copying young gen |
| 碎片敏感 | heap compaction | 需要 moving / compacting 能力 |
| 低暂停延迟 | 增量 / 并发 | 需要 barrier、card table、snapshot 语义 |
| 插件化算法 | 算法热切换/编译切换 | 需要 capability-based algorithm vtable |
---
## 3. 算法版图与选择理由
## 3.1 Reference Counting 家族
### 代表算法
- Naive RC
- Deferred RC
- RC + Trial Deletion / Bacon-Rajan
- Generational RC
### 优点
- 大量无环对象可即时释放
- 资源析构语义清晰
- 小型运行时实现直观
### 缺点
- 热路径有引用计数开销
- 天然不能处理环，需要额外 cycle collector
- 多线程下原子 RC 成本高
- 移动对象支持困难
### 适用场景
- 资源生命周期强依赖及时释放
- 对象图相对浅、环比例不高
- 解释器本身已有类似 retain/release 语义
### 工业参考
- CPython：RC + cycle collector
- Swift / ObjC ARC：以 RC 为主，辅以 autorelease / runtime 优化
---
## 3.2 Mark-Sweep
### 代表算法
- Stop-The-World Mark-Sweep
- Incremental Tri-Color Mark-Sweep
- Conservative Mark-Sweep（如 Boehm）
### 优点
- 实现简单、通用性强
- 对对象头要求少
- 天然支持环
- 适合作为统一框架下的第一个 tracing baseline
### 缺点
- 非移动，容易碎片化
- 每次回收要遍历可达对象
- 没有分代时短命对象吞吐一般
### 适用场景
- 作为框架的第一个通用算法实现
- C 运行时早期版本
- 需要先验证 root/barrier/heap substrate 是否正确
### 工业参考
- Boehm-Demers-Weiser GC（偏保守式）
- Lua 早期及其增量标记-清扫演化路线
---
## 3.3 Generational Tracing
### 代表算法
- Young copying + old mark-sweep
- Young copying + old mark-compact
- Two-generation / multi-generation collectors
### 优点
- 极适合大量短命对象场景
- 吞吐量高
- 可分阶段演进：先年轻代移动，老年代不移动
### 缺点
- 需要 remembered set / write barrier
- 需要更复杂的堆布局
- 调试复杂度高于单一 Mark-Sweep
### 适用场景
- 脚本语言 / 动态语言解释器
- 编译器中间表示、AST、临时对象很多
- 服务端高分配速率场景
### 工业参考
- HotSpot JVM：Serial/Parallel/G1 均体现分代思想
- .NET GC：分代收集
- V8 Orinoco：分代、增量、并发组合
- OCaml minor heap：复制式年轻代
---
## 3.4 Copying / Compacting
### Copying 的意义
- 分配只需 bump pointer
- 年轻代回收效率极高
- 能自然消除碎片
### Compacting 的意义
- 老年代长期运行后可压缩碎片
- 提高对象局部性与缓存命中率
### 成本
- 需要更新所有指针槽位
- 需要精确根和精确对象布局
- 需要 pinning / handle / forwarding 机制之一
### 工业参考
- Cheney semispace copying
- HotSpot Serial/Parallel compaction
- V8 compaction phases
- .NET compacting generations
---
## 3.5 并发 / 增量 / 分区式 GC
### 目标
- 降低 STW 暂停时间
- 将标记或重定位工作与 mutator 交错执行
### 常见技术
- Incremental tri-color
- Snapshot-at-the-beginning (SATB)
- Incremental update barrier
- Region-based evacuation
- Load barrier / colored pointer
### 工业参考
- JVM G1：分区 + remembered set + SATB
- Shenandoah：并发疏散、读屏障
- ZGC：染色指针、并发 relocation
- Go GC：并发 tri-color + assist + pacer
### 结论
这类算法不应成为框架 v1 的前提，但框架必须**为它们预留结构性扩展点**。
---
## 4. 框架的总架构
一个真正多算法通用的设计，应拆成五层：
```text
┌──────────────────────────────────────────────┐
│ VM / Runtime Integration Layer               │
│ roots / descriptors / handles / finalizers   │
├──────────────────────────────────────────────┤
│ GC Coordination Layer                        │
│ safepoint / scheduling / trigger / pressure  │
├──────────────────────────────────────────────┤
│ Heap Substrate Layer                         │
│ arenas / regions / pages / generations       │
├──────────────────────────────────────────────┤
│ Barrier & Metadata Layer                     │
│ card table / mark bits / forwarding / pin    │
├──────────────────────────────────────────────┤
│ Algorithm Plugin Layer                       │
│ marksweep / generational / rc / compacting   │
└──────────────────────────────────────────────┘
```
GC 框架稳定的是前四层；算法可替换的是最后一层。
---
## 5. 对象模型：真正通用的关键
## 5.1 为什么不能把对象头设计成 RC 专用
如果公共对象头里固定包含：
- `rc`
- `gc_ref`
- `color`
- `purple flag`
那么：
- 对 Mark-Sweep 来说，有大量无用字段
- 对 Copying / Compacting 来说，仍然缺少 forwarding / pin / region metadata
- 对并发算法来说，很多状态更适合 side metadata 而不是对象头
所以真正通用的框架必须把：
- **对象描述（layout/type）**
- **算法状态（mark bit / rc / age / forwarding）**
- **堆布局信息（region/generation）**
分层处理，而不是全部塞进固定 header。
## 5.2 推荐的最小公共对象头
推荐只保留所有算法都可能需要的最小信息：
```c
typedef struct GcHeader {
    const struct GcDescriptor *desc;  // 类型描述符：trace/finalize/size/layout
    uint32_t                  size;   // 对象总大小
    uint16_t                  flags;  // pin/finalizer/weak/immutable 等通用位
    uint16_t                  kind;   // VM 可选对象种类标签
} GcHeader;
```
### 为什么是 `desc`
`desc` 使对象布局从 GC 中抽离。GC 不需要知道 `dict`、`list`、`closure` 的结构，只需要：
- 如何枚举引用 slot
- 如何终结
- 是否可移动
- 大小是多少
## 5.3 算法元数据放在哪里
真正多算法通用时，算法元数据有三种承载方式：
### 方案 A：对象头扩展字段
优点：
- 访问快
- 局部性好
缺点：
- 容易和某个算法耦合
- 头部越来越大
### 方案 B：side metadata（推荐）
在 page / region / arena 旁维护：
- mark bitmap
- age / generation bits
- card table
- forwarding table
- pinning table
优点：
- 更适合 moving / tracing / region-based collectors
- 公共 header 保持小而稳定
### 方案 C：algorithm-private overlay
对象头后接算法私有区，或 arena 中挂 algorithm-specific meta。
优点：
- 可以兼容 RC 类特殊需求
### 推荐结论
**公共 header 最小化 + side metadata 为主 + 少量 algorithm-private overlay**。
这才是能同时承载 RC 和 tracing 家族的设计。
---
## 6. 描述符系统（Descriptor System）
## 6.1 核心思想
所有精确 GC 都离不开对象布局信息。
因此每个对象类型必须有一份 `GcDescriptor`：
```c
typedef struct GcDescriptor {
    const char *name;
    uint32_t    fixed_size;
    uint32_t    flags;
    void (*trace_slots)(GcHeader *obj,
                        void (*visit_slot)(GcHeader **slot, void *ctx),
                        void *ctx);
    void (*trace_edges)(GcHeader *obj,
                        void (*visit_obj)(GcHeader *child, void *ctx),
                        void *ctx);
    void (*finalize)(GcHeader *obj);
} GcDescriptor;
```
## 6.2 为什么同时保留 `trace_slots` 和 `trace_edges`
- `trace_edges`：适合只读遍历，如 mark、count、统计
- `trace_slots`：适合 copying、compacting、root update、reclaim 断边
真正通用的框架必须有 `trace_slots`。没有它就很难安全支持移动对象。
---
## 7. 根模型
## 7.1 根的种类
框架要支持以下 root sources：
- 线程栈根
- 寄存器或 VM frame 局部变量
- 全局变量 / 模块表 / package 表
- 挂起协程 / continuation
- C API handle / pinned handle
- 弱引用表 / finalizer queue / remembered roots
## 7.2 根扫描接口不应只暴露对象，而应能暴露 slot
对于 non-moving collectors，只看到 `GcHeader *root` 常常够用；但对于 moving collectors，必须能更新 root slot。
因此真正通用的根扫描 SPI 应该更像：
```c
typedef void (*GcScanRootsFn)(void *vm_ctx,
                              void (*visit_root_slot)(GcHeader **slot, void *ctx),
                              void *ctx);
```
这比“只传 root object”更通用。
## 7.3 多协程场景
如果一个 OS 线程上有多个协程，那么“线程上下文”不是唯一 root container。必须扫描：
- 当前协程栈
- 所有挂起协程保存栈
- continuation / scheduler 保存的根
否则 moving GC 和 tracing GC 都可能错收活对象。
---
## 8. 堆模型
真正多算法通用的设计，不能把堆等同于 `malloc + 全局链表`。推荐抽象为：
```text
Heap
 ├─ Space / Generation / Region
 │   ├─ Page / Arena / Segment
 │   │   └─ Objects
```
## 8.1 推荐支持的空间类型
### Non-moving space
- 用于 Mark-Sweep
- 也可作为 generational old generation
- 对外部 C 指针最友好
### Copying semispace
- 用于 nursery / young generation
- 分配快，回收快
### Region space
- 用于 G1 风格或将来并发疏散
- 便于按 region 统计存活率、碎片率、remembered set
### Large object space
- 大对象单独管理
- 避免复制和频繁移动
## 8.2 为什么推荐“年轻代可移动、老年代默认不移动”
这是一条工程上非常稳的折中路线：
- 既能获得 copying nursery 的分配和回收效率
- 又不强迫整个 VM 立刻全面接受“对象地址随时变化”
- 对纯 C 运行时尤其现实
工业上也大量采用这条路线。
---
## 9. 屏障模型
## 9.1 写屏障为什么必须框架化
下列算法都需要写屏障，但目的不同：
- Generational GC：记录 old -> young 引用
- Incremental tri-color：维护三色不变式
- SATB：记录旧值
- RC：统一堆字段赋值入口以做 INC/DEC
因此“写屏障”不属于某一个算法，而是框架共性设施。
## 9.2 统一写屏障接口
```c
void gc_write_barrier_slot(GcRuntime *rt,
                           GcThreadContext *thread,
                           GcHeader *owner,
                           GcHeader **slot,
                           GcHeader *new_value);
```
这个接口有四层语义：
1. 实际写入 slot
2. 调用当前算法的 post-write hook
3. 若有 card table，则脏化 card
4. 若算法需要 RC，则在内部做 retain/release
### 关键结论
**屏障应该是统一 API，具体行为由算法插件决定。**
## 9.3 读屏障
只在需要时启用，例如：
- 并发 compaction
- 染色指针
- forwarding during evacuation
所以读屏障应为可选 capability，而不是所有算法的基础负担。
---
## 10. 移动对象支持：指针、slot、handle、pin
这是“真正多算法通用”里最关键也最容易被忽略的设计点。
## 10.1 如果要支持 copying / compacting，必须解决地址变化问题
需要四类能力至少其一：
- 精确 slot 更新
- handle indirection
- pinning API
- forwarding pointer / relocation table
## 10.2 推荐策略
### v1/v2 推荐
- VM 内部对象访问使用直接指针
- 所有 GC root 和堆字段都可精确枚举 slot
- 提供显式 `pin/unpin` 接口处理外部 C 长期持有指针
- 先实现“nursery 可移动，old gen 默认不移动”
这条路线最适合纯 C 运行时。
### 不推荐一开始就全句柄化
handle 模式最通用，但会带来：
- 二次间接访问
- API 侵入性大
- 大量现有 VM 代码需要重写
所以更适合作为后续可选模式，而不是 v1 必选。
---
## 11. 统一算法插件模型
## 11.1 Capability-based 设计
每个算法模块必须声明自己的能力：
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
## 11.2 核心 vtable
```c
typedef struct GcAlgorithmVTable {
    const char *name;
    GcAlgorithmCaps caps;
    void (*global_init)(GcRuntime *rt, const GcConfig *cfg);
    void (*thread_init)(GcRuntime *rt, GcThreadContext *thread);
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
    void (*destroy)(GcRuntime *rt);
} GcAlgorithmVTable;
```
## 11.3 为什么这样设计
- Mark-Sweep：只实现 `collect_full`
- Generational：实现 `collect_minor` + `collect_major`
- RC：`write_barrier` 中做 retain/release，`collect_full` 可做 cycle collector
- Copying：`alloc` 使用 bump pointer，`collect_minor` 做 evacuation
- Compacting：`collect_major` 做 mark-compact
统一入口，不统一内部细节。
---
## 12. 统一运行时（Runtime Core）
## 12.1 推荐顶层结构
```c
typedef struct GcRuntime {
    GcConfig                 cfg;
    const GcAlgorithmVTable *algo;
    GcHeap                   heap;
    GcScheduler              sched;
    GcBarrierSet             barriers;
    GcStats                  stats;
    GcScanRootsFn            scan_roots;
    void                    *vm_ctx;
} GcRuntime;
```
GC 框架对 VM 暴露的是 `GcRuntime`；算法模块在其内部挂接。
## 12.2 为什么不直接暴露算法私有 global context
因为一旦公开 API 直接绑定 `GcGlobalContext_RC`、`GcGlobalContext_MS` 这种结构，算法切换就不再透明。
真正通用的框架应对外只暴露：
- `GcRuntime`
- `GcThreadContext`
- `GcDescriptor`
- roots/barrier/handle 接口
算法私有状态应藏在 runtime 内部或 plugin private state 中。
---
## 13. 推荐的内置算法组合
真正“多算法通用”不意味着第一天就实现所有算法；而是设计上允许它们自然加入。建议内置算法按如下顺序演进。
## 13.1 算法 A：`marksweep-stw`
### 角色
- 基线 collector
- 校验 descriptor / root scanning / heap iteration / finalizer 流程
### 特性
- 非移动
- STW
- 最简单 tracing collector
### 工业价值
所有更复杂算法最终都依赖这套 tracing substrate 是否正确。
---
## 13.2 算法 B：`gen-copy-ms`
### 结构
- young gen：copying semispace
- old gen：mark-sweep
### 原因
- 这是动态语言最有性价比的路线之一
- 对纯 C VM 来说，可先只移动年轻代
- 通过 remembered set / card table 训练屏障子系统
### 工业参考
- OCaml minor heap
- 多数现代 VM 的经典分代路线
---
## 13.3 算法 C：`gen-copy-compact`
### 结构
- young gen：copying
- old gen：mark-compact / region compaction
### 作用
- 解决碎片问题
- 提高长期运行场景的 locality
### 代价
- 需要更完整的 pin/update/forwarding 基础设施
---
## 13.4 算法 D：`rc-cycle`
### 结构
- 写屏障中 retain/release
- cycle collector 独立触发
### 作用
- 提供 deterministic-release 风格选项
- 满足资源析构要求很强的运行时
### 重要说明
RC 应作为**可插拔算法之一**，而不是整个框架的默认形态。
---
## 13.5 算法 E：`regional-concurrent`（远期）
### 结构
- region heap
- SATB / incremental update barrier
- 并发 mark
- evacuation / relocation
### 作用
- 面向低暂停与大堆
### 工业参考
- G1
- Shenandoah
- ZGC
- Go GC（虽不是 region evacuating，但调度与 barrier 思路可借鉴）
---
## 14. 算法选择指南
## 14.1 如果你优先要“最早能跑”
选：**STW Mark-Sweep**
理由：
- 对对象模型要求最少
- 最适合先验证精确 roots 与 descriptor
- 是后续 tracing 家族的最小公共子集
## 14.2 如果你优先要“吞吐量”
选：**Generational copying young + non-moving old**
理由：
- 对临时对象极友好
- 是多数现代 VM 的现实起点
- 工程复杂度远低于全堆 compacting/concurrent
## 14.3 如果你优先要“及时析构”
选：**RC + cycle collector**
理由：
- 资源型对象释放更即时
- 但应作为算法插件，而不是框架核心假设
## 14.4 如果你优先要“低暂停”
选：**分区式并发 collector**
理由：
- 但只能在准确 roots、屏障、heap metadata 完整之后再做
---
## 15. 工业级实现参考与可借鉴点
| 运行时 | 算法 | 借鉴点 | 不宜直接照搬的点 |
|--------|------|--------|------------------|
| CPython | RC + cycle collector | 即时释放 + 环检测 | 强绑定 Python 对象模型与 GIL |
| Swift/ObjC ARC | 编译器辅助 RC | 资源语义清晰 | 依赖编译期插桩和语言特性 |
| Lua 5.4 | 增量 / 分代 tracing | 小型 VM 的工程权衡 | 与 Lua 对象布局深耦合 |
| Boehm GC | conservative mark-sweep | C 环境下的可用性 | 保守式不适合 moving 精确 GC |
| HotSpot JVM | 分代/分区/并发多算法共存 | 最成熟的“多算法框架化”案例 | 体量巨大，复杂度极高 |
| V8 | 分代 + 增量 + compaction | 动态语言对象模型与屏障体系 | 引擎工程复杂、JIT 深耦合 |
| Go GC | 并发 tri-color | assist/pacer/scheduler 设计 | Go 的 goroutine 栈与 runtime 特性特殊 |
| .NET GC | 分代 + compacting + background | 代际与 server/workstation 模式 | CLR 深度集成，不适合直接照搬 |
| OCaml | minor copying + major non-moving/compaction | 年轻代移动、老年代保守迁移的折中 | 语言运行时约束较强 |
### 核心借鉴结论
1. **工业级系统几乎都把“框架层”和“算法层”分开**  
2. **绝大多数高性能 VM 都至少有分代思想**  
3. **移动 GC 的前提永远是精确 roots + 可更新 slot**  
4. **并发 GC 的前提是完善的 barrier 和调度基础设施**
---
## 16. 推荐架构决策
如果目标是“真正多算法通用”，推荐如下总体决策：
### 决策 1：公共对象头最小化
不要把算法私有字段写死进所有对象头。
### 决策 2：descriptor + precise slot tracing 作为框架核心
这是同时支持 RC、Mark-Sweep、Copying、Compacting 的共同前提。
### 决策 3：heap substrate 抽象为 spaces / generations / regions
不要把 heap 固定成“全局链表 + malloc”。
### 决策 4：barrier 作为框架服务，不作为某算法实现细节
写屏障必须统一；读屏障按 capability 可选。
### 决策 5：算法通过 capability-based vtable 注册
而不是用大量 `#ifdef ALGO_X` 把整个工程编译期撕裂。
### 决策 6：先实现一条 tracing 主线，再引入 RC 插件
从工程成功率看：
- 先做 `marksweep-stw`
- 再做 `gen-copy-ms`
- 再做 `gen-copy-compact`
- 再做 `rc-cycle`
- 最后做并发 collector
这比先从 RC 内核向外扩更容易得到真正多算法框架。
---
## 17. 演进路线
### Phase 0：统一 substrate
建立：
- `GcDescriptor`
- roots slot scanning
- heap spaces/arenas
- worklist / mark bitmap / card table / pin API
- `GcAlgorithmVTable`
### Phase 1：`marksweep-stw`
目标：让 tracing 家族的基本闭环跑通。
### Phase 2：`gen-copy-ms`
目标：把年轻代移动与写屏障体系跑通。
### Phase 3：`gen-copy-compact`
目标：把 forwarding / slot update / pinning 跑通。
### Phase 4：`rc-cycle`
目标：引入 deterministic-release 风格算法作为插件。
### Phase 5：concurrent / regional collector
目标：低暂停与大堆优化。
---
## 18. 最终结论
如果你要的是一份**真正意义上的多算法通用 GC 设计文档**，那么核心不是“先选 RC 还是先选 Bacon-Rajan”，而是先回答以下五个架构问题：
1. **对象布局如何被精确描述？**
2. **根和堆字段是否都能以 slot 形式更新？**
3. **堆是否允许多个 space / generation / region 共存？**
4. **屏障是否由框架统一调度？**
5. **算法是否通过 capability + vtable 插件化？**
只要这五个问题设计正确，Mark-Sweep、分代 Tracing、Copying/Compacting、RC 家族、甚至并发 collector 都能在同一底座上演进。
这才是“真正多算法通用”的框架设计。
