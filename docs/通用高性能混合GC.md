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
4. **屏障模型**：业务执行代码（mutator）如何与 GC 收集器（collector）交互
5. **算法插件模型**：不同 GC 算法如何挂接到统一底座
这样，GC 算法成为可替换模块，运行时平台成为长期稳定资产。

### 0.1 当前仓库对应关系
截至 2026-06-02，仓库中的实现与本文档的关系如下：

- 已落地的起步算法（baseline algorithms）：
  - `marksweep-stw`
  - `gen-copy-ms`
- 已形成的公共基础层（shared substrate）：
  - `core/runtime.c`
  - `core/barrier.c`
  - `core/heap.c`
  - `core/worklist.c`
- 已存在的基础层测试：
  - `test_barrier_substrate`
  - `test_card_table_substrate`
  - `test_bitmap_substrate`
  - `test_heap_substrate`

也就是说，本文档不再只是前瞻设计；其中一部分能力已经在仓库中形成可运行的起步实现。

### 0.2 文档分工
建议将文档按以下方式理解：

- 本文档：**总设计与算法选择依据**
- `docs/xgc-v2.0-架构蓝图.md`：**架构拆分 + 工程路线 + 算法落地细节的一体化主蓝图**
- `src/algo/INTERFACE.md`：**算法插件接口契约**

如果要开始编码，优先同时阅读本文档的第 3/4/5 章，以及 `docs/xgc-v2.0-架构蓝图.md`。

### 0.3 如果你觉得术语太多，先读这一节
这份文档里有不少 GC 领域常见术语。为了避免一上来就被术语压住，可以先用下面这套**白话版理解**：

- **框架**：一套通用骨架，负责把 GC 的公共问题先解决好。
- **算法**：挂在骨架上的具体 GC 实现，比如 `marksweep-stw`、`gen-copy-ms`。
- **公共基础层（shared substrate）**：不同算法都会反复用到的“公共零件”，比如 worklist、bitmap、card table、barrier。
- **对象描述符（descriptor）**：告诉 GC“这个对象里哪些字段是指针、怎么遍历、怎么终结”。
- **写屏障（write barrier）**：每次对象字段写引用时，顺手通知 GC 一声，避免收集时漏看新引用。
- **记忆集（remembered set）**：专门记录“哪些老对象可能指向年轻代”，这样 minor GC 不用把整个老年代全扫一遍。
- **卡表（card table）**：把大对象切成一小块一小块的“卡片”，记录哪几块脏了。
- **slot**：一个“放指针的格子”，比如对象里的 `left`、`right` 字段。
- **pin / handle**：对象可能移动时，用来避免地址失效或提供间接引用的保护手段。
- **mutator**：正常跑业务逻辑、同时创建和修改对象的执行方。
- **collector**：真正执行 GC 的那部分逻辑。
- **tracing**：从 roots 出发，顺着对象引用一路往下找，判断谁还活着。

如果只想先抓住项目主线，可以记住一句话：

> **这个项目的目标，是先把 GC 的公共骨架搭好，再把不同算法当插件接进去。**

再直白一点：

1. `include/` 定义外部怎么用 GC
2. `src/core/` 放公共零件
3. `src/algo/` 放具体算法
4. `tests/` 负责防止公共零件和算法退化

只要按这个顺序看，很多术语就不会显得那么抽象。

### 0.3.1 拿当前仓库举两个最重要的例子
很多概念只看定义还是容易抽象，下面直接拿仓库里的代码举例。

#### 例子 A：为什么需要“记忆集（remembered set）”？
看 `gen-copy-ms` 时，可以把问题想得很简单：

- 年轻代对象很多，而且死得快
- 所以 minor GC 只想重点扫年轻代
- 但老年代对象里，可能藏着指向年轻代的引用

如果没有 remembered set，那么每次 minor GC 都要把整个老年代再扫一遍，成本很高。

所以项目里现在的做法是：

- 写引用时走 `gc_store_ref(...)`
- 写屏障把对应 slot 所在的 card 标脏
- minor GC 只回来看这些 dirty cards

也就是说，**remembered set 本质上就是“帮 minor GC 缩小检查范围”的记录结构**。

#### 例子 B：为什么需要 `trace_slots_range`？
以前对象一旦命中 dirty card，常见的偷懒办法是：

- 反正这个对象脏了
- 那就把整个对象全扫一遍

这样虽然正确，但不够细。

现在项目里增加 `trace_slots_range` 后，可以做到：

- 脏的是哪一张 card，就只扫描这张 card 覆盖到的那部分对象内存
- 如果对象描述符支持范围扫描，就只看那一段里的指针 slot

这就是“dirty-card 精细扫描”的意义：

> **不是只知道“这个对象脏了”，而是进一步知道“对象的哪一块脏了”。**

### 0.4 常见术语对照表（白话版）
| 术语 | 更直白的说法 | 在本项目里具体指什么 |
|------|--------------|----------------------|
| substrate | 公共基础层 / 公共零件层 | `src/core/` 中可被多算法复用的能力 |
| descriptor | 对象说明书 | `GcDescriptor`，告诉 GC 如何扫描对象 |
| barrier | 写入通知钩子 | 对象字段写入时，顺手更新 GC 元数据 |
| write barrier | 写屏障 | `gc_store_ref(...)` 后算法会走到的写入通知逻辑 |
| read barrier | 读屏障 | 读取引用时触发的可选钩子，目前多数路径不用 |
| remembered set | “老年代可疑对象记录表” | minor GC 时避免全量扫描 old gen |
| card table | 脏块表 / 卡片表 | 把对象按 card 粒度标脏，再按 dirty card 扫描 |
| dirty card | 脏卡 | 被写过、可能含有新跨代引用的 card |
| slot | 指针槽位 / 指针格子 | 对象中存放 `GcHeader*` 的字段 |
| trace slots | 枚举对象中的指针格子 | 让 GC 能更新对象字段里的引用 |
| trace slots range | 枚举对象某一段里的指针格子 | 用于 dirty-card 精细扫描 |
| young gen | 年轻代 | 放大部分短命对象的区域 |
| old gen | 老年代 | 存活更久的对象所在区域 |
| minor GC | 小回收 | 主要回收年轻代 |
| full GC | 全回收 | 扫描和回收整个堆的主要对象集合 |
| pin | 固定对象 | 暂时不让对象移动 |
| handle | 句柄 | 通过一层间接引用安全拿对象 |
| forwarding | 转发信息 | 对象搬家后，旧地址如何找到新地址 |
| mutator | 业务执行方 | 正在跑业务代码、也在修改对象图的线程/执行流 |
| collector | GC 执行方 | 负责扫描、移动、回收对象的那部分逻辑 |
| baseline | 起步版本 / 最小可运行版本 | 先求正确和闭环，再逐步优化 |
| tracing | 引用追踪式 GC | 从 roots 出发沿引用遍历对象图 |

### 0.5 推荐阅读顺序（开发者最短路径）
如果你是第一次接触这个项目，建议按下面顺序读：

1. 先看本节 `0.3` 和 `0.4`，把术语翻译成白话
2. 再看第 `4` 章，理解整个项目按目录怎么分层
3. 再看第 `5/6/7` 章，理解对象、descriptor、roots
4. 最后看第 `13/17` 章，理解算法演进路线

如果你是准备改代码，建议直接这样跳：

- 想知道“外部怎么接入 GC” → 看 `include/xgc/gc.h`
- 想知道“公共能力应该放哪” → 看 `src/core/`
- 想知道“某种算法怎么工作” → 看 `src/algo/`
- 想知道“改完怎么验证” → 看 `tests/`

### 0.6 全局静态视角：这个项目“摆在桌面上”长什么样
如果不看运行流程，只把整个项目静态地摊开来看，可以把它理解成下面这张图：

```text
┌─────────────────────────────────────────────────────────────┐
│ include/xgc/                                               │
│ 对外公开 API 与公共类型定义                                 │
│ - GcRuntime / GcThreadContext / GcDescriptor / GcConfig    │
│ - gc_alloc_typed / gc_store_ref / gc_collect_*             │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ src/core/                                                  │
│ 公共基础层：所有算法都可能复用的零件                         │
│ - runtime.c     统一入口与调度                              │
│ - heap.c        堆统计/young-old 空间信息                    │
│ - barrier.c     屏障对外 facade                             │
│ - card_table.c  old→young 脏卡记录                           │
│ - bitmap.c      位图基础能力                                 │
│ - worklist.c    显式工作栈                                   │
│ - reclaim.c     对象终结与释放                               │
│ - descriptor.c  descriptor 范围扫描辅助                      │
│ - 其余文件为并发/RC/快速路径预留                             │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ src/algo/                                                  │
│ 具体算法层：决定“怎么分配、怎么回收、何时写屏障、如何扫描”     │
│ - marksweep/                                               │
│ - gen_copy_ms/                                             │
│ - bacon-rajan/                                             │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ tests/ + bench/ + docs/                                    │
│ 验证、压测、解释整个系统                                     │
└─────────────────────────────────────────────────────────────┘
```

这张图表达的是一个很重要的设计原则：

> **对外只有一套 GC API；对内先有公共基础层，再有具体算法。**

也就是说：

- `include/` 负责“别人怎么用这个 GC”
- `src/core/` 负责“公共零件怎么组织”
- `src/algo/` 负责“某个算法怎么利用这些零件跑起来”

### 0.7 全局动态视角：这个项目“跑起来”时发生什么
静态结构解决的是“代码摆在哪里”，动态视角解决的是“调用时谁先谁后”。

整个项目最重要的三条动态主线只有三条：

#### 0.7.1 分配主线
```text
VM / test / example
  -> gc_alloc_typed(...)
  -> src/core/runtime.c
  -> 当前算法的 alloc(...)
  -> 必要时调用 heap.c / barrier.c 等公共能力
  -> 返回对象
```

#### 0.7.2 写引用主线
```text
VM / test / example
  -> gc_store_ref(rt, thread, owner, slot, new_value)
  -> src/core/runtime.c 先真正写入 *slot = new_value
  -> 当前算法的 write_barrier(...)
  -> 若算法需要 remembered set
       -> src/core/barrier.c
       -> src/core/card_table.c
       -> src/core/bitmap.c
```

#### 0.7.3 收集主线
```text
VM / test / example
  -> gc_collect_minor / gc_collect_full
  -> src/core/runtime.c
  -> 当前算法的 collect_* 实现
  -> 过程中调用：
       - hooks.scan_roots
       - worklist.c
       - barrier/card_table/bitmap
       - reclaim.c
       - heap.c
```

所以从“整体是怎么转起来的”这个角度看，可以把它简化成一句话：

> **runtime 负责分发，core 提供公共零件，algo 决定回收策略。**

---
## 0.x 开发者全局总览：静态结构 + 动态流程

这一节专门解决一个问题：

> **如果我是第一次接手这个项目，我该怎么建立“全局的静态和动态认识”？**

答案是：

1. 先看“静态结构”：目录怎么分层、每个文件管什么
2. 再看“动态流程”：一次分配、一次写引用、一次 GC 到底会经过哪些文件

### 0.x.1 `core/` 目录里每个文件分别做什么

下面这张表可以当作 `src/core/` 的总导航：

| 文件 | 主要职责 | 在系统中的位置 |
|------|----------|----------------|
| `runtime.c` | 运行时总入口、线程 attach/detach、分配/写屏障/collect 分发、handle 管理 | `core` 的中枢调度文件 |
| `heap.c` | young/old 使用量、对象计数、young 空间基址与容量记录 | 堆统计与空间状态的公共记录层 |
| `barrier.c` | 屏障 facade，对外暴露 `gc_barrier_*` 接口 | 把算法和 card table 隔开 |
| `card_table.c` | owner 注册、slot→card 映射、dirty card 标记/遍历/清除 | remembered set 的核心实现 |
| `bitmap.c` | 位图 resize/set/test/clear 等底层能力 | `card_table` 等元数据结构的底层工具 |
| `worklist.c` | 显式栈 push/pop | tracing 算法遍历对象图的基础工具 |
| `reclaim.c` | finalizer 调用、对象 free、统计回退 | 回收阶段的公共收尾逻辑 |
| `descriptor.c` | descriptor 范围扫描 helper | 为 dirty-card 精细扫描提供统一入口 |
| `atomic.c` | 原子能力占位文件 | 多线程支持的预留点 |
| `epoch.c` | 并发/后台 GC 协调预留 | 多线程/并发 GC 的预留点 |
| `purple.c` | RC-cycle 候选集预留 | RC 家族算法的预留点 |
| `ring.c` | ring buffer 快速分配预留 | 未来快速分配路径的预留点 |

从“优先级”来看，目前最核心、最值得先读的 `core` 文件是：

1. `runtime.c`
2. `heap.c`
3. `barrier.c`
4. `card_table.c`
5. `bitmap.c`
6. `worklist.c`
7. `reclaim.c`

### 0.x.2 `core/` 各文件之间怎么交互

如果只看 `core/` 内部关系，可以把它理解成下面这个小图：

```text
runtime.c
 ├─ 调用 heap.c        维护 heap 状态
 ├─ 调用 barrier.c     维护写屏障相关状态
 ├─ 调用 worklist.c    给算法遍历提供工作栈
 ├─ 调用 reclaim.c     在回收阶段释放对象
 └─ 调用算法 vtable     把具体决策交给 algo/

barrier.c
 └─ 调用 card_table.c  实现 old→young remembered set

card_table.c
 └─ 调用 bitmap.c      存储 dirty card 位图

algo/gen_copy_ms
 ├─ 调用 runtime 提供的入口和 hooks
 ├─ 调用 heap.c        更新 young/old 统计
 ├─ 调用 barrier.c     标脏 slot / 遍历 dirty cards
 ├─ 调用 descriptor.c  做 card-range 精细扫描
 ├─ 调用 worklist.c    推进 tracing
 └─ 调用 reclaim.c     回收死亡对象
```

也就是说，当前系统的一个非常重要的层级关系是：

> **算法一般不应该直接跳过 `barrier.c` 去手写 remembered set 逻辑，而是优先复用 `core` 里的公共实现。**

### 0.x.3 `runtime.c` 为什么是“总中枢”

如果你只能先读一个文件，那最值得先读的是 `src/core/runtime.c`。

原因很简单：它统一承接了对外最重要的几个入口：

- `gc_runtime_create`
- `gc_thread_attach`
- `gc_alloc_typed`
- `gc_store_ref`
- `gc_collect_minor`
- `gc_collect_full`
- `gc_handle_acquire/release`

这些入口基本就覆盖了“外部如何使用 GC”的全部主线。

可以把 `runtime.c` 理解成：

> **把外部 API 翻译成内部调用链的总调度器。**

它自己并不想知道“某个算法具体怎么收集”，而是做两件事：

1. 维护统一运行时状态
2. 把请求转发给当前算法的 vtable

### 0.x.4 `gen_copy_ms` 是怎么“转起来”的

为了形成动态认识，最值得拿来举例的是 `gen-copy-ms`，因为它已经把：

- young/old 分代
- 写屏障
- remembered set
- dirty-card 扫描
- worklist tracing

这些关键部件串起来了。

它的大致运行方式可以概括成下面这样：

#### 分配阶段
- 小对象优先分到 young space
- 放不下或 pinned 的对象直接进 old
- old 对象会注册到 barrier/card table owner 集合里

#### 写引用阶段
- 所有写引用优先走 `gc_store_ref(...)`
- `runtime.c` 先真的把指针写进去
- 然后进入算法的 `write_barrier(...)`
- 如果是 old → young 写入：
  - `gc_barrier_mark_slot_dirty(...)`
  - card table 把对应 card 标脏

#### minor GC 阶段
- 先扫描 handles
- 再扫描 VM roots
- 再扫描 dirty cards 对应的 old 对象局部范围
- 看到 young 对象就 promotion / evacuation
- promoted 对象进入 worklist
- 不断 drain worklist，直到所有新复制对象的子引用都处理完
- 最后清空 young space 统计并 finalize 死掉的 young 对象

#### full GC 阶段
- 先做一次 minor，保证 young 部分先整理干净
- 然后对 old objects 做 mark
- 最后 sweep 掉 old 中不可达对象

### 0.x.5 从三个入口建立动态心智模型

如果你想快速理解“算法到底怎么转起来”，最好的方法不是从头到尾死读代码，而是盯住三个入口：

#### 入口一：`gc_alloc_typed`
它回答的是：

> 新对象怎么出生？由谁决定进 young 还是 old？

#### 入口二：`gc_store_ref`
它回答的是：

> 对象之间的引用一旦改变，GC 是怎么知道的？

#### 入口三：`gc_collect_minor` / `gc_collect_full`
它回答的是：

> 真正开始收集时，扫描顺序和回收顺序是什么？

只要把这三条线看懂，整个项目的动态认识就会迅速清晰很多。

### 0.x.6 建议的阅读顺序（建立全局认识专用）

如果你的目标不是立刻改代码，而是先建立完整的全局认识，推荐按下面顺序读：

1. 本文档的 `0.6`、`0.7`、`0.x`
2. `include/xgc/gc.h`
3. `include/xgc/gc_types.h`
4. `src/gc_internal.h`
5. `src/core/runtime.c`
6. `src/core/barrier.c` + `src/core/card_table.c` + `src/core/bitmap.c`
7. `src/core/worklist.c` + `src/core/heap.c` + `src/core/reclaim.c`
8. `src/algo/gen_copy_ms/young_copy.c`
9. `tests/test_gen_copy_ms_smoke.c`

为什么最后要看测试？

因为测试通常是“动态流程最短的可执行说明书”。

例如：

- `test_barrier_substrate.c` 让你理解 barrier/card table 的静态行为
- `test_bitmap_substrate.c` 让你理解位图元数据怎么工作
- `test_gen_copy_ms_smoke.c` 让你看到一次真正的算法闭环

### 0.x.7 一句话总结这个项目的全局心智模型

如果要把整个项目压缩成一句最容易记住的话，可以记成：

> **外部通过统一 API 与 runtime 交互，runtime 把请求分发给算法；算法尽量复用 `core` 的公共零件完成分配、写屏障和收集。**

这句话基本就把静态结构和动态流程都串起来了。
---
## 1. 设计目标
## 1.1 总目标
构建一个适用于纯 C 运行时的、**可插拔、多算法、可分阶段演进** 的 GC 框架，满足以下要求：
- 对 VM 类型系统无硬编码依赖
- 对单线程、单 OS 线程多协程、多线程运行时都能适配
- 同时支持非移动与移动型 GC
- 同时支持精确的“引用追踪式 GC（tracing）”与 reference counting 家族算法
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
   想支持对象移动的 GC，根扫描必须能更新 slot，而不只是“看到对象”。
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
如果你看到这一章开始觉得概念又变多了，可以先只抓这四句话：

- `Mark-Sweep`：最适合先把框架跑通
- `gen-copy-ms`：最适合大量短命对象场景
- `RC`：最适合强调“资源要尽快释放”的场景
- 并发/增量 GC：是后续优化方向，不是 v1 第一目标

也就是说，这一章不是要求你一次吃透所有 GC 学术术语，而是帮助你回答一个更实际的问题：

> **当前项目应该先实现哪种算法，为什么？**

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
│ VM 接入层（VM / Runtime Integration Layer）   │
│ roots / descriptors / handles / finalizers   │
├──────────────────────────────────────────────┤
│ GC 协调层（GC Coordination Layer）            │
│ safepoint / scheduling / trigger / pressure  │
├──────────────────────────────────────────────┤
│ 堆基础层（Heap Substrate Layer）              │
│ arenas / regions / pages / generations       │
├──────────────────────────────────────────────┤
│ 屏障与元数据层（Barrier & Metadata Layer）    │
│ card table / mark bits / forwarding / pin    │
├──────────────────────────────────────────────┤
│ 算法插件层（Algorithm Plugin Layer）          │
│ marksweep / generational / rc / compacting   │
└──────────────────────────────────────────────┘
```
GC 框架稳定的是前四层；算法可替换的是最后一层。

如果觉得“五层架构”听起来太抽象，可以把它理解成一句更口语的话：

> **上层负责“怎么接入”，中层负责“公共零件”，下层负责“具体算法”。**

再换个更像工程实践的说法：

- `include/`：给外部开发者看的入口
- `src/core/`：给所有算法共用的公共实现
- `src/algo/`：每个 GC 算法自己的实现
- `tests/` / `bench/` / `docs/`：用来验证、解释、评估前面三层

### 4.1 结合当前仓库的整体分层图
对当前仓库，可以把“抽象分层”和“代码目录”一一对应起来理解：

```text
┌───────────────────────────────────────────────────────────────────────┐
│ ① VM 接入层 / Public API / Integration Layer                          │
│   目的：给 VM、解释器、测试、示例提供稳定入口                           │
│   代码：include/xgc/gc.h                                              │
│        examples/*.c                                                   │
│        tests/*_smoke.c                                                │
│   关键概念：GcRuntime / GcThreadContext / GcDescriptor / GcVmHooks    │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 通过 public API 进入 runtime
┌───────────────────────────────────────────────────────────────────────┐
│ ② Runtime 协调层 / Runtime Coordination Layer                         │
│   目的：统一生命周期、调度、分发、根扫描回调、collect 入口               │
│   代码：src/core/runtime.c                                            │
│        src/core/epoch.c                                               │
│   关键职责：                                                          │
│   - 创建/销毁 runtime                                                 │
│   - attach/detach thread                                              │
│   - dispatch 到当前算法 vtable                                        │
│   - 挂接 hooks.scan_roots                                             │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 调用共享 substrate
┌───────────────────────────────────────────────────────────────────────┐
│ ③ 公共基础层 / Shared Substrate Layer                                 │
│   目的：沉淀“算法无关但 GC 必需”的公共零件                              │
│   代码：src/core/heap.c                                               │
│        src/core/worklist.c                                            │
│        src/core/reclaim.c                                             │
│        src/core/barrier.c                                             │
│        src/core/bitmap.c                                              │
│        src/core/card_table.c                                          │
│        src/gc_internal.h                                              │
│   关键职责：                                                          │
│   - heap / young-old 统计                                             │
│   - 显式 worklist                                                     │
│   - dirty owner / dirty card 相关公共能力                              │
│   - side metadata 基础抽象                                            │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 被具体算法组合使用
┌───────────────────────────────────────────────────────────────────────┐
│ ④ 算法插件层 / Algorithm Plugin Layer                                 │
│   目的：实现具体 GC 算法，而不是重新发明 runtime                       │
│   代码：src/algo/marksweep/ms.c                                       │
│        src/algo/gen_copy_ms/young_copy.c                              │
│        src/algo/bacon-rajan/*                                         │
│   关键职责：                                                          │
│   - alloc / collect_minor / collect_full                              │
│   - write_barrier / read_barrier                                      │
│   - 算法私有状态与对象遍历策略                                         │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 结果通过 hooks / handles / API 返回给 VM
┌───────────────────────────────────────────────────────────────────────┐
│ ⑤ 验证与文档层 / Validation / Benchmark / Docs Layer                  │
│   目的：验证 substrate 正确性、评估吞吐/暂停、同步设计意图               │
│   代码：tests/*.c                                                     │
│        docs/*.md                                                      │
│        bench/*   （建议逐步补齐）                                      │
└───────────────────────────────────────────────────────────────────────┘
```

这张图的核心含义是：

- **`include/` 定义稳定边界**：VM 优先只依赖公共 API，而不是直接耦合算法内部。
- **`src/core/` 沉淀共享能力**：能抽象成通用设施的能力，优先放进公共基础层（shared substrate），而不是散落到某个算法文件里。
- **`src/algo/` 只表达算法差异**：算法文件负责“如何收集”，而不是“框架是什么”。
- **`tests/` 与 `docs/` 是架构的一部分**：不是收尾工作，而是保证长期演进不退化的护栏。

### 4.2 每一层应该解决什么问题

#### 4.2.1 VM / Public API / Integration Layer
这一层回答的问题是：**VM 要如何接入 GC，而不需要知道具体算法内部细节？**

开发者在这一层主要处理：

- 定义对象 descriptor
- 提供根扫描 `scan_roots`
- 使用 `gc_alloc_typed` / `gc_store_ref` / `gc_collect_*`
- 通过 handle / pin API 与移动对象安全交互

如果你在做的是“让业务 VM 能正确接 GC”，优先看：

- `include/xgc/gc.h`
- `tests/test_gen_copy_ms_smoke.c`
- `examples/*.c`

#### 4.2.2 Runtime Coordination Layer
这一层回答的问题是：**一次分配、一次写屏障、一次 GC 请求，最终是如何被路由到当前算法上的？**

它不直接实现某个 collector，但负责：

- 管理 `GcRuntime` 生命周期
- 管理 `GcThreadContext`
- 统一 collect 入口
- 在 runtime 与 algorithm vtable 之间做桥接
- 保持公共 API 语义稳定

如果你要：

- 增加一个新的公共 API
- 调整 runtime 初始化流程
- 接新的 VM hook

那么通常应该从 `src/core/runtime.c` 开始，而不是直接改具体算法。

#### 4.2.3 公共基础层（Shared Substrate Layer）
这一层回答的问题是：**不同算法都反复需要、但又不该重复实现的能力，应该放在哪里？**

当前仓库中，这一层已经开始形成的能力包括：

- `heap.c`：young/old 统计、space 基础抽象
- `worklist.c`：显式遍历栈
- `bitmap.c`：side metadata bitset
- `card_table.c`：owner + card remembered set substrate
- `barrier.c`：对外 facade，屏蔽 remembered-set 细节
- `reclaim.c`：回收与 finalization 相关公共逻辑

这层的设计原则是：

1. **先抽象稳定语义，再考虑具体算法优化**
2. **接口要能被多算法复用**
3. **尽量让测试直接覆盖 substrate 本身**

也就是说，如果你发现某段逻辑未来 `marksweep`、`gen_copy_ms`、`gen-copy-compact` 都会需要，那么优先考虑沉淀到 `src/core/`。

如果还是觉得“substrate”这个词别扭，可以直接把它理解成：

> **给多个算法共用的一层公共零件。**

例如：

- `bitmap.c` 不是某个算法独占的，它是公共零件
- `card_table.c` 不是某个算法独占的，它也是公共零件
- `barrier.c` 用来统一对外接口，本质上也是公共零件的一部分

所以今后看到文档里的 `substrate`，都可以先在脑子里翻译成“公共基础层”或“公共零件层”。

#### 4.2.4 Algorithm Plugin Layer
这一层回答的问题是：**某一种 GC 算法，如何使用 runtime + substrate 拼出自己的收集流程？**

这里应该放：

- 算法私有状态结构
- minor/full collection phase 逻辑
- 算法特有的 barrier 语义
- promotion / marking / evacuation / sweeping 细节

这里不应该放：

- 与算法无关的公共 API
- 本可抽象成共享 substrate 的通用 metadata 结构
- “其他算法也可能要抄一遍”的公共工具逻辑

一个实用判断标准是：

> 如果你把当前算法目录删掉，`src/core/` 仍应保持“可被另一套算法复用”的骨架完整性。

### 4.3 运行时调用链：开发者应如何读代码

如果你第一次进入这个仓库，推荐按“调用链”而不是按文件名读代码。

#### 4.3.1 分配路径
```text
VM / test / example
  -> gc_alloc_typed(...)
  -> src/core/runtime.c
  -> rt->algo->alloc(...)
  -> src/algo/<algorithm>/*.c
  -> 必要时调用 heap substrate 记账
```

读这条路径，可以理解：

- 公共 API 如何保持稳定
- 为什么同一个 `gc_alloc_typed` 可以切换不同算法实现
- heap 统计与对象头初始化是在哪一层完成的

#### 4.3.2 写引用路径
```text
VM / test / example
  -> gc_store_ref(rt, thread, owner, slot, new_value)
  -> src/core/runtime.c 先执行真实写入
  -> rt->algo->write_barrier(...)
  -> src/algo/<algorithm>/*.c 决定是否记录 remembered set
  -> src/core/barrier.c / card_table.c / bitmap.c
```

这条路径最重要，因为它直接决定：

- old -> young 引用是否会在 minor GC 时被重新看到
- 增量/并发算法将来如何接入 SATB / incremental-update barrier
- RC 家族算法将来如何复用统一赋值入口

#### 4.3.3 minor / full collection 路径
```text
gc_collect_minor / gc_collect_full
  -> src/core/runtime.c
  -> rt->algo->collect_minor / collect_full
  -> src/algo/<algorithm>/*.c
       -> scan handles
       -> scan VM roots
       -> scan remembered set / dirty cards
       -> drain worklist
       -> reclaim / sweep / reset young space
```

这条路径帮助开发者区分：

- 哪些步骤是 tracing 家族共有的
- 哪些步骤是某个算法自己的 phase
- 哪些 metadata 应该沉淀到 substrate

### 4.4 如果要改代码，应该优先改哪一层

下面给出一个“改动入口表”，帮助开发者决定第一刀应该落在哪：

| 你要做的事情 | 优先修改位置 | 原因 |
|-------------|--------------|------|
| 新增/调整公共 API | `include/xgc/gc.h` + `src/core/runtime.c` | 这是稳定边界，算法不应直接暴露给外部 |
| 增加 remembered set / bitmap / side metadata 抽象 | `src/core/bitmap.c` / `src/core/card_table.c` / `src/core/barrier.c` | 这是共享 substrate，未来多算法可复用 |
| 调整 minor GC 扫描顺序 | `src/algo/gen_copy_ms/young_copy.c` | 属于算法 phase 编排 |
| 调整对象枚举/trace 语义 | descriptor 定义处 + 对应测试 | 布局信息属于对象模型，不应偷偷写死在 collector 里 |
| 新增某种 heap/space 统计 | `src/core/heap.c` | 算法无关的 heap substrate 应集中维护 |
| 增加新算法 | `src/algo/<new_algo>/` + vtable 接入 | 算法差异应通过 plugin layer 表达 |
| 增加 safepoint / 并发调度 | `src/core/runtime.c` / `src/core/epoch.c` | 属于协调层，而不是某个单独 collector 的私货 |
| 验证 substrate 正确性 | `tests/test_*_substrate.c` | 先测 substrate，再测算法 smoke |

### 4.5 推荐的开发顺序：先 substrate，后 algorithm

这个项目最容易走偏的地方，是把所有能力都先塞进某个具体算法里，最后导致：

- `marksweep` 有一套自己的 metadata
- `gen_copy_ms` 又有一套自己的 remembered set
- 将来再做 `gen-copy-compact` 时只能复制粘贴

更稳的做法是：

1. **先判断需求是不是“算法共享能力”**
2. 如果是，先放到 `src/core/`
3. 给它配 substrate test
4. 再让具体算法接入
5. 最后补 smoke test / benchmark / 文档

当前仓库特别适合沿着下面这条路线继续演进：

```text
bitmap / card-table / barrier substrate
    -> gen_copy_ms remembered set 正确接入
    -> more precise card-range scan
    -> mark bitmap / forwarding metadata substrate
    -> compacting / regional collector
```

### 4.6 开发者的实际操作建议

如果你要自己实现一个功能，建议按下面的顺序推进：

#### 场景 A：我要做一个共享抽象
例如：bitmap、card table、mark bits、forwarding table、region stats。

建议步骤：

1. 在 `src/gc_internal.h` 定义最小内部数据结构和函数声明
2. 在 `src/core/` 增加实现文件或扩展现有 substrate
3. 在 `tests/test_*_substrate.c` 增加针对抽象本身的单元测试
4. 让某个算法先最小接入，验证能闭环
5. 再考虑更细粒度优化

#### 场景 B：我要优化某个具体算法
例如：让 `gen_copy_ms` 从 dirty-owner 升级到 dirty-card remembered set。

建议步骤：

1. 先确认共享 substrate 是否已具备足够能力
2. 只在 `src/algo/<algorithm>/` 中调整 phase 逻辑
3. 不要把本应共享的 metadata 私藏到算法目录
4. 先保持语义正确，再逐步做范围扫描等性能优化
5. 用 smoke test 验证行为，用 benchmark 验证收益

#### 场景 C：我要接一个新的 VM
建议优先实现：

1. descriptor
2. root slot scanning
3. `gc_store_ref` 的统一赋值入口
4. pinned/handle 约束
5. 最小 smoke test

结论是：

> **开发者应该把这个仓库理解成“runtime + substrate + algorithm plugin + tests/docs”的组合系统，而不是一堆独立 C 文件。**

一旦按层理解，很多“应该改哪里”的问题会非常清晰。
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
