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

#### 0.x.2.1 图一：`core/` 文件依赖图（更直观版）
如果你更希望看到一张“谁依赖谁”的静态图，可以直接看下面这张：

```text
                           ┌───────────────────────┐
                           │ include/xgc/*.h       │
                           │ 对外 API / 公共类型    │
                           └───────────┬───────────┘
                                       │
                                       ▼
                           ┌───────────────────────┐
                           │ src/gc_internal.h     │
                           │ core/algo 共享内部定义 │
                           └───────┬───────────────┘
                                   │
                  ┌────────────────┼────────────────┐
                  │                │                │
                  ▼                ▼                ▼
        ┌────────────────┐  ┌───────────────┐  ┌────────────────┐
        │ runtime.c      │  │ heap.c        │  │ worklist.c     │
        │ 总调度中枢      │  │ heap 状态记录  │  │ tracing 工作栈 │
        └──────┬─────────┘  └───────────────┘  └────────────────┘
               │
               ├──────────────┐
               │              │
               ▼              ▼
      ┌────────────────┐  ┌────────────────┐
      │ barrier.c      │  │ reclaim.c      │
      │ 屏障外观层      │  │ 终结/释放对象   │
      └──────┬─────────┘  └────────────────┘
             │
             ▼
      ┌────────────────┐
      │ card_table.c   │
      │ dirty-card 管理 │
      └──────┬─────────┘
             │
             ▼
      ┌────────────────┐
      │ bitmap.c       │
      │ 位图底层工具    │
      └────────────────┘

      ┌────────────────┐
      │ descriptor.c   │
      │ 范围扫描 helper │
      └────────────────┘

      ┌──────────────────────────────────────────────────────┐
      │ 预留文件：atomic.c / epoch.c / purple.c / ring.c    │
      │ 对应多线程、并发 GC、RC-cycle、快速分配等未来方向     │
      └──────────────────────────────────────────────────────┘
```

这张图最好记的地方是：

- `runtime.c` 是中枢
- `barrier.c -> card_table.c -> bitmap.c` 是一条 remembered-set 元数据链
- `descriptor.c` 是“精细对象扫描”的公共辅助点
- `worklist.c` 和 `reclaim.c` 是 tracing 算法最常复用的两个基础件

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

#### 0.x.4.1 图二：`gen_copy_ms` 关键时序图
如果你想理解“算法到底怎么转起来的”，这张图比单看函数名更有帮助：

```text
业务代码 / VM
  │
  │ 1. 分配对象
  ▼
gc_alloc_typed(...)
  │
  ▼
runtime.c
  │
  ▼
gen_copy_ms::alloc(...)
  │
  ├─ 小对象 -> young space bump allocate
  └─ 大对象 / pinned -> old object + barrier_register_owner


业务代码 / VM
  │
  │ 2. 写入引用 owner->slot = new_value
  ▼
gc_store_ref(rt, thread, owner, slot, new_value)
  │
  ├─ runtime.c 先执行真实写入
  ▼
gen_copy_ms::write_barrier(...)
  │
  ├─ 如果不是 old -> young 写入：直接返回
  └─ 如果是 old -> young 写入：
        gc_barrier_mark_slot_dirty(rt, owner, slot)
          -> barrier.c
          -> card_table.c
          -> bitmap.c


业务代码 / VM
  │
  │ 3. 触发 minor GC
  ▼
gc_collect_minor(rt)
  │
  ▼
runtime.c
  │
  ▼
gen_copy_ms::collect_minor(...)
  │
  ├─ 扫 handles
  ├─ 扫 VM roots（hooks.scan_roots）
  ├─ 遍历 dirty cards
  │    -> barrier.c
  │    -> card_table.c
  │    -> descriptor.c (trace_slots_range)
  ├─ 命中 young 对象则 promotion
  ├─ promoted 对象入 worklist
  ├─ worklist.c 持续 drain
  ├─ finalize 死掉的 young 对象
  └─ heap.c 重置 young 统计


业务代码 / VM
  │
  │ 4. 触发 full GC
  ▼
gc_collect_full(rt)
  │
  ▼
runtime.c
  │
  ▼
gen_copy_ms::collect_full(...)
  │
  ├─ 先做 minor
  ├─ mark old objects
  ├─ worklist.c drain
  └─ reclaim.c sweep 死亡 old objects
```

这张图最想表达的不是“函数名很多”，而是三件事：

1. **所有外部动作都先经过 `runtime.c`**
2. **写屏障不是直接写一堆私有逻辑，而是尽量走 `barrier/card_table/bitmap` 公共链路**
3. **minor GC 的 remembered-set 扫描已经是“slot 标脏 -> dirty card -> card-range 扫描”这条完整路径**

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

### 0.x.8 为什么会做这个 GC：来自 Rhino / `polyglot-c` 的问题驱动

这个项目并不是“凭空想做一个新 GC 框架”，而是有非常现实的来源：

> `/home/clouder/workspace/raw-spofer-rhino` 里的 VM / `polyglot-c` 实现，其 GC 和对象生命周期设计存在结构性问题，已经影响正确性、可维护性和长期演进。

结合 `analysis.md`、`code-quality.md` 和 `polyglot-c/docs/rhino-gc-guide.md`，可以把原始动机概括成下面几条。

#### 0.x.8.1 Rhino 现有实现的核心问题

##### 问题 1：RC 和 tracing GC 是两套互相打架的生命周期模型
`analysis.md` 明确指出，Rhino 继承了 Vim 风格的引用计数（RC）路径，同时又嫁接了一套标记-清扫（mark-sweep）路径，但两者没有统一。

这会导致：

- RC 在某些路径上还在减 refcount
- GC sweep 又按另一套规则释放对象
- 两边都觉得自己“有权处理对象生命周期”

结果就是：

- 有些对象该活时被 sweep 掉
- 有些对象该死时因为环永远不死
- 设计层面很难证明正确性

##### 问题 2：GC 触发逻辑存在，但运行期实际上没真正接通
分析文档指出：

- `try_to_collect_garbage()` 会去调用 `f_garbagecollect()`
- 但 `f_garbagecollect()` 只是设置标志位
- 没有代码真正消费这个标志位去执行完整 GC

这意味着：

> **GC 看起来“有”，但在运行期实际上经常并没有真正工作。**

##### 问题 3：root scanning 不完整，关键 mark 函数还是空实现
`analysis.md` 里列出的几个函数：

- `set_ref_in_previous_funccal()`
- `set_ref_in_call_stack()`
- `set_ref_in_func_args()`
- `free_unref_funccal()`

都还是 stub。

这意味着执行栈、函数参数、调用链等关键 root 根本没有被可靠纳入 GC 可见范围。

换句话说：

> **GC 连“谁是活对象”都看不全，就谈不上正确回收。**

##### 问题 4：对象遍历不完整，只能覆盖部分类型
根据分析文档，当前 sweep 主要只覆盖 `dict` 和 `list`，很多参与引用图的对象类型并没有被完整纳入 tracing 路径，例如：

- `partial_t`
- `userfunc_t`
- `future_t`
- `generator_t`

这意味着只要这些类型参与了环或中间引用链，就可能出现：

- 永久泄漏
- 引用图断裂
- GC 可见性不完整

##### 问题 5：内部对象和用户对象混在同一条 GC 链上
分析文档提到：`dict_alloc()` / `list_alloc()` 无条件进入同一个 GC 链表。

这会导致：

- VM 内部元数据对象
- 用户脚本层对象

被混在同一套回收逻辑里处理。

这在工程上会带来两个很大的问题：

1. GC 规则很难分层
2. 一旦 root/mark 漏掉某条内部引用，内部基础设施也可能被错误处理

##### 问题 6：对象生命周期和 refcount 规则散落在很多类型里
`analysis.md` 列出了大量不统一的 refcount 字段命名：

- `dv_refcount`
- `lv_refcount`
- `pt_refcount`
- `ref_count`
- `refcount`

这反映出一个更深层的问题：

> **对象生命周期没有统一抽象，而是按类型不断追加、不断分叉。**

这会直接导致：

- 很难建立统一的对象模型
- 很难替换 GC 算法
- 很难做精确 root scanning 和移动对象支持

#### 0.x.8.2 这些问题如何映射到 `xgc` 的设计决策

下面这张表，就是“Rhino 的问题”与“`xgc` 的设计决策”之间的对应关系：

| Rhino / `polyglot-c` 暴露的问题 | `xgc` 的对应设计 |
|---|---|
| RC 与 tracing GC 两套生命周期打架 | 统一 `GcRuntime + GcAlgorithmVTable`，算法通过同一入口挂接，而不是混杂在对象类型代码里 |
| GC 触发逻辑不闭环 | `gc_collect_minor/full` 统一从 `runtime.c` 分发，运行链路明确 |
| roots 扫描不完整 | 强调 `GcVmHooks.scan_roots` + slot 级扫描，把 roots 当成第一等接口 |
| 只覆盖部分对象类型 | 通过 `GcDescriptor` 统一对象遍历接口，而不是给每种对象单独拼补丁 |
| 内部对象与用户对象混用 | 设计上强调公共基础层、算法层、VM 接入层分层，逐步把内部基础设施从“业务对象模型”里拆开 |
| refcount 字段与生命周期规则碎片化 | 公共对象头最小化，算法元数据尽量放 side metadata 或算法私有状态 |
| remembered set / barrier 缺失或混乱 | 把 `barrier.c` / `card_table.c` / `bitmap.c` 抽成公共能力 |
| 很难演进到分代 / moving GC | 从一开始要求 descriptor、slot tracing、pin/handle、young/old 空间抽象 |

#### 0.x.8.3 为什么 `xgc` 不是直接“修 Rhino 的现有 GC 代码”

因为从这些分析可以看出，问题并不只是几个 bug，而是**对象模型、生命周期模型、root 模型、GC 触发链路、内部/用户对象边界**都纠缠在一起。

这类问题如果直接在旧代码上打补丁，通常会出现：

- 修掉一个泄漏，又引入另一条悬空引用
- 修掉一个 sweep 误释放，又让某些环永久无法回收
- 新增一个对象类型，又要补很多分散的 refcount / mark / free 逻辑

所以 `xgc` 的出发点更像是：

> **先把 GC 的通用框架和公共零件重建出来，再考虑怎样把旧 VM 的对象系统逐步迁移上来。**

换句话说，`xgc` 不是“对 Rhino 现有 GC 的小修小补”，而是一次**架构重整后的可复用 GC 底座**。

#### 0.x.8.4 用一句话总结这层动机

如果要把这层背景压缩成一句最容易记住的话，就是：

> **`xgc` 的存在，不是因为“想做一个很酷的新 GC”，而是因为旧 VM 的对象生命周期和 GC 代码已经难以通过局部修补继续演进。**
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
- `RC`：最适合强调”资源要尽快释放”的场景
- 并发/增量 GC：是后续优化方向，不是 v1 第一目标

也就是说，这一章不是要求你一次吃透所有 GC 学术术语，而是帮助你回答一个更实际的问题：

> **当前项目应该先实现哪种算法，为什么？**

---
### 3.0 GC 算法基础概念（所有算法的共同前提）

在深入每个算法之前，需要先理解贯穿所有 GC 算法的几个核心概念。

#### 3.0.1 可达性与活跃性

GC 的核心问题是判断**哪些对象还”活着”**。判断标准不是”对象是否被使用过”，而是**从根集出发能否到达**：

```text
根集（roots）
  ├── 线程栈上的局部变量
  ├── 全局变量 / 静态变量
  ├── 寄存器中的对象引用
  └── 外部 API 持有的句柄（handles）

活跃对象 = 从任意根出发，沿引用关系可以直接或间接到达的所有对象
垃圾对象 = 不可达的对象（即使它们之间互相引用形成环）
```

这个定义的关键后果是：**即使一个对象环内所有对象互相引用，只要没有任何根能到达环中任意节点，整个环就是垃圾。**

```
根 → A → B → C → A（A/B/C 永不回收）
根 → D（D 存活）
    X → Y → X（X/Y 是垃圾 — 没有任何根可达）
```

#### 3.0.2 三色抽象（Tri-Color Marking）

几乎所有 tracing GC 都可以用三色抽象来理解。它把对象分成三类：

| 颜色 | 含义 | 状态 |
|------|------|------|
| **白色** | 尚未被扫描到 | 可能是垃圾（标记完成后仍为白色 = 垃圾） |
| **灰色** | 已被标记为存活，但其子节点尚未扫描 | 处于 worklist 中等待处理 |
| **黑色** | 已被标记为存活，且其所有子节点已扫描 | 确认存活，不再需要处理 |

算法流程：

```
1. 所有对象初始为白色
2. 将从根直接可达的对象染为灰色，放入 worklist
3. while worklist 非空:
     a. 取出一个灰色对象 obj
     b. 遍历 obj 的每个子对象 child:
          if child 是白色:
              将 child 染为灰色，放入 worklist
     c. 将 obj 染为黑色
4. 所有仍为白色的对象 = 垃圾，可回收
```

**三色不变式（Tri-Color Invariant）**：在任何时刻，黑色对象不能直接指向白色对象。所有从黑色对象到白色对象的引用路径必须经过灰色对象。如果 mutator 在 GC 标记期间修改了引用关系，写屏障的作用就是维护这个不变式。

#### 3.0.3 精确 vs 保守 GC

| | 精确 GC（Precise） | 保守 GC（Conservative） |
|---|---|---|
| 如何识别指针 | VM 提供对象布局元数据 | 扫描内存，猜测哪些值是”像指针”的整数 |
| 根扫描 | 编译器/解释器明确告知哪些位置是指针 | 扫描整个栈和寄存器 |
| 移动对象 | ✅ 可以（知道所有指针位置就能更新它们） | ❌ 不能（可能把整数误更新为指针） |
| 实现复杂度 | 需要 descriptor 和精确 root map | 实现简单但语义粗糙 |
| 本项目选择 | **精确 GC** | 不考虑 |

本项目选择精确 GC 路线。这解释了为什么 `GcDescriptor`、`trace_slots`、`scan_roots` 这些基础设施是框架级别的一等公民。

#### 3.0.4 STW、并发、增量

| 模式 | 含义 | 暂停时间 | 实现复杂度 |
|------|------|---------|-----------|
| **STW（Stop-The-World）** | mutator 完全暂停，GC 独占执行 | 等于 GC 总时间 | 最简单 |
| **增量（Incremental）** | GC 分多步执行，每步后 mutator 可运行 | 多次小暂停 | 中等 |
| **并发（Concurrent）** | mutator 和 GC 真正同时运行 | 接近零（只有极短的 safepoint） | 最高 |

本项目从 STW 开始，逐步演进。

#### 3.0.5 Safepoint

Safepoint 是代码中**可以安全暂停 mutator 执行**的位置。在本项目中，safepoint 采用**协作式轮询**：在分配入口和循环回跳指令处插入轻量检查。线程到达 safepoint 时，它的栈帧完整、寄存器状态清洁，GC 可以精确扫描它的根。

#### 3.0.6 分代假设（Generational Hypothesis）

分代 GC 的理论基础是一个经验观察：

> **大多数对象在创建后很快就变成垃圾（”婴儿死亡率高”），而存活较久的对象倾向于继续存活。**

具体数据：
- 约 80-98% 的新对象在一次 GC 内就死亡
- 存活超过一次 GC 的对象，大概率长期存活

分代 GC 利用这个假设：
- **年轻代（young generation）**：频繁收集，因为大部分对象死在这里
- **老年代（old generation）**：低频收集，因为老对象大概率还活着
- 对象从年轻代”晋升”（promote）到老年代的条件：存活了若干轮 GC

#### 3.0.7 记忆集（Remembered Set）与卡表（Card Table）

分代 GC 面临一个问题：minor GC 只扫年轻代，但老年代对象可能包含指向年轻代的引用。如果每次 minor GC 都扫描整个老年代，分代的优势就没了。

**Remembered Set** 记录”老年代中哪些位置可能指向年轻代”。**Card Table** 是实现 remembered set 的一种常见方式：

```
老年代空间被切成固定大小的”卡片”（card，通常是 128-512 字节）
当一个 slot 被写入，且写入是 old→young 引用时：
    card_table[slot_addr / card_size] = DIRTY

minor GC 时：
    只扫描 dirty cards 对应的老年代区域
    而不是扫描整个老年代
```

这大幅缩小了 minor GC 的扫描范围。xgc 的 `src/core/card_table.c` 实现了这个机制。

#### 3.0.8 写屏障（Write Barrier）的分类

写屏障是 GC 的核心机制。按目的分，主要有三种：

| 类型 | 目的 | 何时触发 | 使用方 |
|------|------|---------|--------|
| **Remembered-set barrier** | 记录 old→young 引用 | `gc_store_ref(owner(old), slot, new_value(young))` | 分代 GC |
| **SATB barrier** | 记录被覆盖的旧值 | 写入前保存旧值快照 | G1, 并发标记 |
| **Incremental-update barrier** | 把新引入的引用重新标灰 | 写入后标记新值 | 增量标记 |
| **RC barrier** | retain/release | 每次赋值 | 引用计数 |

xgc 选择统一入口 `gc_store_ref(...)`，由当前算法内部决定 barrier 行为。

#### 3.0.9 移动对象（Moving GC）的前提条件

要让 GC 能安全移动对象，必须满足以下全部前提：

1. **精确根**：所有指向被移动对象的根引用都必须是 GC 已知的 slot，以便更新地址
2. **精确对象布局**：对象的每个引用字段的位置和类型都是已知的
3. **转发机制**：旧地址到新地址的映射，用于在移动过程中处理”已被移走但旧地址仍被引用”的时刻
4. **Pinning/Handle**：外部 C 代码持有的裸指针需要固定对象或通过句柄间接访问

xgc 在 `gen-copy-ms` 算法中实践了这些机制：nursery space 支持 bump-pointer 分配和 promotion 复制，`GcHandle` 为外部引用提供了间接层。

---
## 3.1 Mark-Sweep：最经典的 Tracing GC

### 3.1.1 算法机理

Mark-Sweep 是 GC 算法的”最小公分母”。几乎每一个 GC 框架的第一个基准实现都应该是它。

**核心思想**：两阶段。Mark 阶段从根出发遍历对象图，标记所有存活对象。Sweep 阶段遍历整个堆，回收未标记的对象。

```
算法：STW Mark-Sweep

Phase 1 — Mark（标记）：
    将所有对象标记为”未访问”
    worklist = []
    for each root_slot in roots:
        obj = *root_slot
        if obj 未标记:
            标记 obj
            worklist.push(obj)

    while worklist 非空:
        obj = worklist.pop()
        for each child_slot in obj（通过 descriptor->trace_slots）:
            child = *child_slot
            if child 未标记:
                标记 child
                worklist.push(child)

Phase 2 — Sweep（清除）：
    for each object in 全堆对象列表:
        if object 已标记:
            清除标记（为下一轮 GC 做准备）
        else:
            调用 descriptor->finalize(object)
            free(object)
```

### 3.1.2 关键数据结构

```c
// Mark-Sweep 的算法私有状态（见 xgc src/algo/marksweep/ms.c）
typedef struct GcMsNode {
    GcHeader*        obj;     // 指向堆对象
    uint8_t          marked;  // 本轮是否标记
    struct GcMsNode* next;    // 全对象链表
} GcMsNode;

typedef struct {
    GcMsNode* objects;        // 全对象链表头
} GcMarksweepState;
```

### 3.1.3 实现要点

1. **全对象链表**：最简单的实现是维护一个全局链表，记录所有已分配对象。`alloc` 时插入链表，`sweep` 时遍历链表。

2. **Mark 位可以放在对象头或 side metadata 中**。xgc 的 `marksweep-stw` 把 `marked` 放在 `GcMsNode` 中（算法私有状态），而非 `GcHeader` 中（公共最小头）。

3. **显式 worklist 而非递归**：对象图可能很深。用 `GcWorklist`（`src/core/worklist.c`）迭代替代 C 函数递归，防止栈溢出。

4. **Descriptors 驱动的子引用遍历**：Mark 阶段不硬编码对象类型（”如果是 dict 就这样扫，如果是 list 就那样扫”），而是通过 `desc->trace_slots(obj, visit_slot, ctx)` 统一遍历。这使 GC 核心代码与对象类型解耦。

5. **Sweep 阶段不要忘记处理 finalizer**：在释放对象之前调用 `desc->finalize(obj)`，让 VM 有机会释放对象持有的非 GC 资源（如文件描述符、网络连接）。

### 3.1.4 在 xgc 中的对应实现

- **代码位置**：`src/algo/marksweep/ms.c`（约 309 行）
- **vtable 注册**：`xgc_algorithm_marksweep_stw()`
- **Capability 声明**：`supports_moving=0, supports_generations=0, requires_write_barrier=0`
- **测试**：`tests/test_marksweep_smoke.c` — 包含链表可达性、handle pinning、自环回收等场景

### 3.1.5 优缺点总结

| 优点 | 缺点 |
|------|------|
| 实现最简单，适合验证框架正确性 | 非移动，长期运行会碎片化 |
| 对象头最小（只需一个 mark bit） | 每次 GC 要遍历”所有存活对象”而不只是”可能变化的部分” |
| 天然处理环 | 没有分代时短命对象吞吐量低 |
| 不要求精确根也能工作（保守式变体） | sweep 阶段要遍历整个堆，含大量死亡对象 |

### 3.1.6 工业参考

- **Boehm-Demers-Weiser GC**：C/C++ 的保守式 Mark-Sweep，不要求精确根信息
- **Lua 5.0-5.3**：以增量 Mark-Sweep 为核心，逐步演化
- **CPython 的循环检测**：虽然不是全局 Mark-Sweep，但其 cycle collector 使用了类似的”标记 + 扫描”逻辑

---
## 3.2 Copying GC（半空间复制）

### 3.2.1 算法机理

Copying GC 是”移动对象”最简单的实现。它将堆分成两个等大的半空间：**from-space** 和 **to-space**。分配总是在 from-space 中通过 bump pointer 进行。GC 时：

```
算法：Semi-Space Copying GC（Cheney 算法）

// 初始
from-space: [A B C D E F _ _ _ _ _ _ _ _ _ _]
            ^bump pointer

to-space:   [_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _]

// GC 开始 — 交换 from/to，清空 to-space
swap(from, to)
to-space 初始为空

// 从根开始复制
scan = to-space 起始
free = to-space 起始
for each root_slot:
    *root_slot = copy_to_to_space(*root_slot)  // 把根指向的对象复制到 to-space

// 扫描 to-space 中的对象，更新子引用
while scan < free:
    obj = (GcHeader*)scan
    for each child_slot in obj:
        *child_slot = copy_to_to_space(*child_slot)
    scan += obj->size

// copy_to_to_space 的语义:
GcHeader* copy_to_to_space(GcHeader* obj):
    if obj 已在 to-space 中（看 forwarding 标记）:
        return obj 在 to-space 中的新地址
    // 否则复制:
    new_obj = (GcHeader*)free
    memcpy(new_obj, obj, obj->size)
    在 obj 的旧地址记录 forwarding 信息：obj → new_obj
    free += obj->size
    return new_obj
```

### 3.2.2 关键数据结构

```c
// Forwarding：对象移动后，旧地址指向新地址
// 最简单的实现：在旧对象的字段中临时存放新地址

// 判断对象是否已被复制：
//   如果旧对象头部被改写为 forwarding 地址，则已复制
//   否则，旧对象还在 from-space

// Bump-pointer 分配：
//   free 指针始终指向 to-space 中的下一个空闲位置
//   分配时 free += size，O(1) 时间
```

### 3.2.3 Cheney 算法的优雅之处

Cheney 扫描算法把 worklist 和 to-space 合并为一个结构：

- `free` 指针：下一个对象复制到 to-space 的位置
- `scan` 指针：下一个需要扫描子引用的对象位置
- 当 `scan == free` 时，所有存活对象的子引用都已处理完毕

这不需要额外的 worklist 数据结构——to-space 本身就是 worklist。

### 3.2.4 优缺点

| 优点 | 缺点 |
|------|------|
| 分配 = bump pointer，极其快 | 浪费一半堆内存（to-space 在非 GC 期间空闲） |
| GC 后 from-space 完全清空，零碎片 | 大对象复制成本高 |
| Cheney 算法不需要显式 worklist | 必须精确知道所有指针位置 |
| 自然处理碎片（连续分配） | 对象地址变化，需要跟踪所有引用 |

### 3.2.5 在 xgc 中的对应

xgc 的 `gen-copy-ms` 算法在年轻代使用了 copying 技术：

- Nursery space 使用 bump-pointer 分配
- Minor GC 时，存活对象被复制到老年代空间（promotion）
- 使用 `GcGenForward` 链表记录 forwarding 信息：`{from, to, next}`
- 根和 handle 更新通过 `gc_gen_evacuate_slot` 回调完成

### 3.2.6 工业参考

- **OCaml minor heap**：经典的年轻代 copying GC
- **HotSpot Serial/Parallel GC**：年轻代使用 copying
- **Cheney 1970 论文**：半空间复制算法的原始论文

---
## 3.3 Mark-Compact：压缩式 GC

### 3.3.1 为什么需要 Compaction

Copying GC 完美处理了碎片，但浪费了一半内存。Mark-Compact 在 Mark-Sweep 的基础上增加一步：**标记存活对象后，把它们向堆的一端滑动（slide/compact），释放连续的大块空间**。

### 3.3.2 三种常见 Compaction 策略

**（1）Two-Finger Compaction**

- 一个指针从堆低端向上扫，找空闲区域
- 一个指针从堆高端向下扫，找存活对象
- 将高端存活对象移动到低端空闲区域
- 需要为每个对象记录 forwarding 地址
- 优点：简单；缺点：对象顺序被打乱，可能破坏局部性

**（2）LISP2 Compaction**

```
Phase 1 — Mark：标记所有存活对象
Phase 2 — Compute Forwarding：
    从头到尾扫描堆，计算每个存活对象压缩后的地址
    forwarding[obj] = 新地址
Phase 3 — Update References：
    遍历所有存活对象，根据 forwarding 表更新其内部所有引用
    遍历所有根，更新根引用
Phase 4 — Move Objects：
    将每个存活对象 memmove 到新地址
```

- 需要额外的 forwarding 表（可以是 side metadata）
- 对象保持原有顺序

**（3）Table-Based Compaction（如 HotSpot 使用）**

- 将堆分成固定大小的块
- 维护每块的”存活对象偏移表”
- 通过偏移表并行更新引用和移动对象
- 适合多线程并行 GC

### 3.3.3 在 xgc 中的位置

xgc 目前没有独立的 compacting collector，但以下基础设施已经为它准备好了：

- `GcBitmap`（`src/core/bitmap.c`）：可以存储 mark bits 和 forwarding bits
- `GcHandle` 和 `pin/unpin` API：用于处理不能移动的对象
- `trace_slots`：精确枚举对象中的所有引用 slot，这是 updating references 的前提

在 xgc 的演进路线中，`gen-copy-compact` 是 Phase D，在 `gen-copy-ms` 之后。

---
## 3.4 Generational GC（分代垃圾回收）

### 3.4.1 为什么分代有效

回顾分代假设（§3.0.6）：大多数对象很快死亡，长活对象倾向继续长活。

如果一个 GC 每次都对整个堆做 Mark-Sweep，它反复遍历大量”老而不死”的对象，而这些对象在这次 GC 中几乎不可能变成垃圾。**分代 GC 的关键洞察**是：把精力集中在”更可能产生垃圾”的区域。

### 3.4.2 分代 GC 的结构

```
┌──────────────────────────────────────────┐
│ 年轻代（Young / Nursery）                  │
│ - bump-pointer 分配（Copying semispace）   │
│ - 频繁的 minor GC                         │
│ - 晋升阈值：存活 N 轮 → 进老年代           │
│ - 大小：通常几 MB                          │
├──────────────────────────────────────────┤
│ 老年代（Old / Tenured）                    │
│ - 标记-清扫 或 标记-压缩                   │
│ - 低频的 major/full GC                     │
│ - 大小：剩余堆空间                         │
└──────────────────────────────────────────┘
```

### 3.4.3 Minor GC 完整流程

以 xgc 的 `gen-copy-ms` 为例：

```
minor_collect():
    1. 停止 mutator（STW）

    2. 扫描根（roots）：
       for each root_slot:
           if *root_slot 在 nursery 中:
               *root_slot = promote_to_old(*root_slot)
       # promote_to_old：复制对象到 old gen，建立 forwarding

    3. 扫描 handles：
       for each handle:
           if handle->target 在 nursery 中:
               handle->target = promote_to_old(handle->target)

    4. 扫描 dirty cards（remembered set）：
       for each dirty_card in card_table:
           # 只扫描这张 card 覆盖的老年代对象的 slot 范围
           for each slot in 该 card 覆盖范围:
               if *slot 在 nursery 中:
                   *slot = promote_to_old(*slot)

    5. drain worklist：
       # promoted 对象可能本身就包含指向其他 nursery 对象的引用
       while worklist 非空:
           obj = worklist.pop()
           for each child_slot in obj:
               if *child_slot 在 nursery 中:
                   *child_slot = promote_to_old(*child_slot)

    6. 清理：
       for each 原 nursery 中的对象:
           if 未被 promote（dead）且有 finalizer:
               finalize(obj)
       清空 nursery space
       清除所有 dirty cards
```

### 3.4.4 写屏障在分代 GC 中的角色

如果在 minor GC 之间，mutator 执行了 `old_object->some_field = young_object`，写屏障必须**记录这个写入**，否则下次 minor GC 时这个 young_object 可能被当作垃圾回收（因为没有根或 handle 指向它，只有老年代中的引用）。

```
// gc_store_ref 内部：
*slot = new_value;                           // 真实写入

// 写屏障：
if (owner 在老年代 && new_value 在年轻代):
    card_table_mark_dirty(owner, slot);      // 记录这个 old→young 引用
```

### 3.4.5 晋升策略（Promotion Policy）

对象何时从年轻代晋升到老年代？常见策略：

| 策略 | 描述 |
|------|------|
| **Age-based** | 对象每存活一次 minor GC，age++；`age >= threshold` 时晋升 |
| **Size-based** | 大对象直接分配进老年代（避免复制成本） |
| **Survival-based** | 本次 GC 存活率过高时，提升阈值避免频繁 GC |
| **Pinned** | 被 pin 的对象不移动，直接分入老年代 |

xgc 的 `gen-copy-ms` 当前使用简化策略：**pinned 或超过 nursery 一半大小的对象直接进老年代**；其余在 nursery 中分配，minor GC 时全部 evacuate 到老年代。

### 3.4.6 在 xgc 中的完整实现路径

```
分配：gc_alloc_typed → runtime.c → gen_copy_ms::alloc
  ├── 小对象 + 非pinned → nursery bump-pointer
  └── 大对象 / pinned → calloc + gc_gen_add_old_node + gc_barrier_register_owner

写引用：gc_store_ref → runtime.c → gen_copy_ms::write_barrier
  └── 如果是 old→young → gc_barrier_mark_slot_dirty
                                → barrier.c → card_table.c → bitmap.c

minor GC：gc_collect_minor → runtime.c → gen_copy_ms::collect_minor
  ├── gc_gen_minor_scan_handles
  ├── hooks.scan_roots → gc_gen_evacuate_slot → gc_gen_promote
  ├── gc_barrier_visit_dirty_cards → gc_gen_minor_visit_dirty_card
  │       └── gc_trace_object_slots_in_range → 只扫描 card 覆盖的 slot 范围
  ├── gc_gen_minor_drain（worklist 遍历）
  └── gc_gen_finalize_dead_nursery + gc_heap_reset_young
```

### 3.4.7 工业参考

- **OCaml**：nursery copying + old mark-sweep（最接近 xgc 当前实现）
- **HotSpot Serial GC**：young copying（semispace）+ old mark-compact
- **V8 Orinoco**：young semispace + old mark-compact + 增量标记

---
## 3.5 Reference Counting（引用计数）家族

### 3.5.1 核心原理

与 Mark-Sweep 从根出发”找活对象”不同，RC 的视角是：

> **每个对象记录”有多少个其他对象引用了我”。当这个数字降为零时，对象就是垃圾。**

```c
// 最简单的 RC 实现
typedef struct {
    uint32_t rc;         // 引用计数
    // ... 其他字段
} RcObject;

// retain：有人开始引用我了
void retain(RcObject *obj) {
    if (obj) obj->rc++;
}

// release：有人不再引用我了
void release(RcObject *obj) {
    if (obj && --obj->rc == 0) {
        // 我变成垃圾了 — 先释放我的子对象
        for each child in obj:
            release(child);
        // 最后释放自己
        free(obj);
    }
}
```

### 3.5.2 RC 的核心问题：无法回收环

```
A <──> B    （A.rc=1, B.rc=1，互相引用）
  ↕
  根（无根可达）

A 和 B 的 rc 永远不小于 1，永远不会被 release 回收。
即使没有任何根指向它们，它们仍然互相”活着”。
```

这就是为什么纯 RC 需要**额外的环检测器（Cycle Collector）**。

### 3.5.3 两种主要的 RC 变体

**（1）Immediate RC**

- 每次赋值立即执行 retain/release
- 即时释放，延迟低
- 每次赋值都有开销

**（2）Deferred RC**

- 赋值时只记录增减，不立即检查归零
- 定期批量处理
- 减少热路径原子操作
- Swift/ObjC ARC 在编译器优化层面使用了类似思想

### 3.5.4 Bacon-Rajan Cycle Collection

Bacon 和 Rajan 在 2001 年提出了一种高效的 RC 环检测算法，核心思想是：

```
核心洞察：
  环内的引用都是”内部引用”（internal edges）。
  如果从环内对象的 RC 中减去所有内部引用，
  剩下的就是”外部引用”（external references）。
  如果外部引用 = 0，整个环就是垃圾。

算法阶段（Trial Deletion）：
Phase 1 — 标记嫌疑集：
    每当一个对象的 RC 减少但未归零，将其加入紫色缓冲区（purple buffer）
    紫色 = “可能是环的一部分”

Phase 2 — 减去内部引用：
    以紫色对象为起点，遍历整张子图
    对子图内每条边：目标对象的 gc_ref--

Phase 3 — 判定：
    遍历嫌疑集：
        gc_ref > 0 → 有外部引用 → 存活（染黑）
        gc_ref == 0 → 纯内部引用 → 垃圾（染白）

Phase 4 — 释放：
    回收白色对象
    黑色对象重回正常状态
```

### 3.5.5 在 xgc 中的位置

xgc 的 `bacon-rajan` 算法目录目前是骨架实现，预留了 `write_barrier`（RC retain/release 的入口）和 `collect_full`（cycle collector 的触发点）。`src/core/purple.c` 中的 `gc_push_purple_adaptive` 是为 RC cycle collector 预备的嫌疑对象缓冲区。

### 3.5.6 工业参考

- **CPython**：全量 RC（包括栈）+ 分代 cycle collector（Trial Deletion 变体）
- **Swift/ObjC ARC**：编译器辅助的 Deferred RC + autorelease pool
- **PHP 7+**：RC 为主，辅以少量 tracing
- **Mozilla'sServo**：RC 作为主要内存管理策略

---
## 3.6 并发与增量 GC 基础

### 3.6.1 为什么需要并发/增量

经典 STW GC 有一个根本问题：堆越大，暂停越长。对于 GUI 应用、游戏、实时系统，>10ms 的暂停是不可接受的。

增量 GC 把一次完整的 GC 拆成多个小步骤，每步交替执行 mutator 代码。并发 GC 更进一步：mark 线程和 mutator 线程真正同时运行。

### 3.6.2 并发标记的挑战：Mutator 在修改对象图

假设 GC 线程正在标记对象 A（灰色），并扫描 A 的子节点：

```
时刻 1：GC 扫描 A.child，看到 B 是白色
时刻 2：Mutator 把 B 赋值到 根变量 root 中
时刻 3：Mutator 把 A.child 设为 NULL
时刻 4：GC 继续扫描，认为已经”处理过 A→B 这条边了”
时刻 5：标记结束，B 仍是白色 → 被当作垃圾释放！
```

这就是**并发修改导致活对象被漏标**的问题。解决方案是**写屏障**。

### 3.6.3 两种写屏障策略

**（1）SATB（Snapshot-At-The-Beginning）**

在标记开始时对对象图拍一个逻辑快照。如果 mutator 覆盖了一个引用，barrier 会把旧值记录下来：

```
write_barrier_satb(owner, slot, old_value, new_value):
    if GC 正在标记 && old_value 是白色:
        old_value 染灰色，放入标记队列   // “这个对象在快照时刻是存活的”
    *slot = new_value
```

- 用方：G1 GC
- 特点：保守（某些实际已死的对象也会被标活，但不会漏标）

**（2）Incremental Update**

如果 mutator 在黑色对象中添加了一个白色引用，barrier 把黑色对象重新染灰：

```
write_barrier_incremental(owner, slot, old_value, new_value):
    *slot = new_value
    if GC 正在标记 && owner 是黑色 && new_value 是白色:
        owner 重新染灰                    // “这个黑色对象现在指向了我们还没扫描到的东西”
```

- 用方：CMS（Concurrent Mark Sweep）
- 特点：需要重新扫描灰色对象的子节点

### 3.6.4 并发/增量 GC 在 xgc 中的预留

xgc 已经为并发/增量 GC 预留了以下基础设施：

| 能力 | 位置 | 作用 |
|------|------|------|
| `GcAlgorithmCaps.supports_concurrent_mark` | `gc_algorithm.h` | 声明算法支持并发标记 |
| `read_barrier` vtable 函数 | `gc_algorithm.h` | 并发 relocation 需要读屏障 |
| `GcScheduler` | `gc_internal.h` | 并发调度预留 |
| `gc_signal_background_thread` | `core/epoch.c` | 唤醒后台 GC 线程 |
| Card table + bitmap | `core/card_table.c` + `core/bitmap.c` | SATB 和 incremental update 都需要的元数据 |

### 3.6.5 工业参考

- **Go GC**：并发 tri-color + write barrier（incremental update 风格）+ GC assist + pacer
- **JVM G1**：SATB + remembered set + concurrent mark + STW evacuation
- **JVM ZGC**：染色指针 + concurrent relocation + load barrier
- **JVM Shenandoah**：concurrent evacuation + Brooks-style forwarding pointer

---
## 3.7 算法选择速查表

| 需求 | 推荐算法 | 当前 xgc 状态 |
|------|---------|--------------|
| “我要最快跑起来” | marksweep-stw | ✅ 已实现 |
| “大量临时对象，要高吞吐” | gen-copy-ms | ✅ 已实现 |
| “资源要即时释放” | rc-cycle | ⚠️ 骨架占位 |
| “长期运行要压缩碎片” | gen-copy-compact | 🔜 规划中 |
| “大堆要低暂停” | regional-concurrent | 🔜 远期规划 |
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
