# xgc — 通用可插拔高性能 GC 框架设计文档

> 版本：v2.1  
> 日期：2026-06-08  
> 定位：**纯 C、面向精确根扫描 VM 的多算法通用 GC 框架**

---

## 0. 概览与导航

### 0.1 这是什么

xgc 不是某一种 GC 算法，而是一个**支持多种 GC 算法编译时切换的运行时框架**。
它定义了统一的**对象模型、根模型、堆模型、屏障模型、算法插件模型**，使下列算法能在同一底座上演进：

- 纯 Mark-Sweep（已实现）
- 分代 Tracing — young copying + old mark-sweep（已实现）
- Mark-Compact、Copying/Compacting（规划中）
- Reference Counting + Cycle Collector（骨架占位）
- 并发 / 增量 / 分区式 GC（远期）

### 0.2 仓库结构速览

```
include/xgc/          公共 API — gc.h / gc_types.h / gc_algorithm.h / gc_config.h
src/core/             共享基础设施 — runtime / heap / barrier / card_table / bitmap / worklist / ...
src/algo/             算法插件 — marksweep / gen_copy_ms / bacon-rajan
tests/                单元测试 + 集成测试
bench/                微基准
docs/                 设计文档 + 架构蓝图 + 接口契约
```

一句话定位：**`include/` 定义边界，`core/` 沉淀公共能力，`algo/` 表达算法差异。**

### 0.3 术语速查

| 术语 | 直白说法 | 本项目中的含义 |
|------|---------|--------------|
| substrate | 公共零件层 | `src/core/` 中可被多算法复用的能力 |
| descriptor | 对象说明书 | `GcDescriptor`，告诉 GC 对象的引用字段在哪、如何遍历、如何终结 |
| write barrier | 写屏障 | `gc_store_ref()` 写入后触发的钩子，算法借此记录 remembered set / 更新 RC |
| read barrier | 读屏障 | 读取引用时触发的可选钩子（moving/concurrent GC 才需要） |
| remembered set | 老→新引用记录表 | 分代 GC 里记录"哪些老对象指向年轻代"，避免 minor GC 全量扫老年代 |
| card table | 脏卡表 | remembered set 的一种实现：把对象按 256-512 字节切卡，写入时标脏 |
| slot | 指针槽位 | 对象中存放 `GcHeader*` 的字段 |
| pin / handle | 固定/句柄 | 对象可能移动时，保障外部 C 指针安全的两种机制 |
| forwarding | 转发信息 | 对象搬家后旧地址→新地址的映射 |
| safepoint | 安全暂停点 | mutator 可被安全挂起的位置（字节码边界） |
| STW | 全暂停 | Stop-The-World，mutator 全部暂停，GC 独占执行 |
| mutator | 业务执行方 | 跑业务代码、创建和修改对象的线程/协程 |
| collector | GC 执行方 | 负责扫描、移动、回收对象的那部分逻辑 |

### 0.4 阅读路径

| 你的目标 | 阅读顺序 |
|---------|---------|
| 快速理解项目 | §0 → §1 → §3（五层架构图） → §19（算法速查表） |
| 接入 xgc 到 VM | §0 → §3 → §4对象模型 → §5描述符 → §6根模型 → `include/xgc/gc.h` → `examples/` |
| 实现新算法 | §0 → §3 → §8屏障 → §10插件模型 → §11运行时 → §12-18算法百科 → `src/algo/INTERFACE.md` |
| 改 substrate | §0 → §3 → 直接看 `src/core/` 源码 + 对应 `tests/test_*_substrate.c` |

---

## 1. 设计目标

### 1.1 总目标

构建一个适用于纯 C 运行时的、**可插拔、多算法、可分阶段演进**的 GC 框架：

- 对 VM 类型系统无硬编码依赖
- 对单线程、多协程、多线程运行时都能适配
- 同时支持非移动与移动型 GC
- 同时支持精确 tracing 与 reference counting 家族算法
- 从简单 STW 实现演进到分代、增量、并发实现

### 1.2 成功标准

一个"真正通用"的 GC 框架，至少应满足：

1. **对象头不绑定单一算法元数据** — 不能把 `rc`、`gc_ref`、`purple` 固化为所有算法的前提。
2. **移动与非移动对象都能表达** — 只支持"地址永不变"天然排斥 copying / compacting。
3. **写屏障 / 读屏障是框架能力，不是某算法私货** — 框架为不同算法提供统一挂接点。
4. **根扫描必须精确** — 支持对象移动的 GC 必须能**更新 slot**，而不只是"看到对象"。
5. **堆布局可多态** — 至少能表达 malloc/arena 非移动堆、semi-space copying、region-based heap、generational heap。

### 1.3 非目标

- 不要求 v1 就落地工业级 ZGC/Shenandoah 级别实现
- 不让任意 C 原始指针在 moving GC 下完全无约束工作
- 不在没有精确根信息的前提下安全支持 compacting
- 不用单一对象头同时最优支持所有算法

---

## 2. 适用场景与部署模式

xgc 通过编译时配置（`GC_MULTITHREAD`）和运行时 API（`GcRuntime` / `GcThreadContext`）的组合，覆盖四种典型部署模式。

### 2.1 单线程单协程

```
┌──────────────────────┐
│  Mutator（唯一）       │
│  GcThreadContext (可选)│
│  GcRuntime            │
└──────────────────────┘
```

- `GC_MULTITHREAD` 未定义 → `_Atomic` 退化为普通类型，零开销
- `gc_collect_cycles` 内联同步执行，无需 Safepoint
- `GcThreadContext` 可选（单线程不需要 TLS 隔离）
- 分配路径无 Safepoint 检查
- **典型场景**：嵌入式脚本引擎、CLI 工具、教学 VM

### 2.2 单线程多协程

```
┌──────────────────────────────────────┐
│  OS 线程                              │
│  ┌──────────┐ ┌──────────┐           │
│  │ 协程 A    │ │ 协程 B    │  ...      │
│  │ Ctx A    │ │ Ctx B    │           │
│  └──────────┘ └──────────┘           │
│  GcRuntime（共享）                     │
└──────────────────────────────────────┘
```

- 一个 OS 线程上运行多个协程（如 Lua coroutine / Go goroutine 的协作式调度变体）
- 每个协程持有独立的 `GcThreadContext`（独立 purple buffer + TLAB）
- 所有协程共享一个 `GcRuntime`
- **挂起的协程栈必须通过 `hooks.scan_roots` 可枚举**，否则 tracing GC 会漏标
- 回收时需扫描当前协程 + 所有挂起协程的根
- **典型场景**：游戏脚本、网络服务中的协程调度器

### 2.3 多线程

```
┌─────────────┐  ┌─────────────┐  ┌──────────────────┐
│ Mutator A   │  │ Mutator B   │  │ Background GC    │
│ Ctx A       │  │ Ctx B       │  │ Thread           │
│ purple_buf  │  │ purple_buf  │  │ Epoch N→N+1→N+2  │
└──────┬──────┘  └──────┬──────┘  └────────┬─────────┘
       │                │                  │
       └────────┬───────┘                  │
                ▼                          ▼
         GcRuntime（共享）         全局 Safepoint 协调
```

- `GC_MULTITHREAD` 定义 → C11 `_Atomic` 启用
- 每个线程独立 TLAB + purple buffer（减少 CAS 竞争）
- 写屏障中 RC 操作变为原子 `fetch_add` / `fetch_sub`
- Background GC Thread + Epoch 机制处理并发环检测
- Safepoint 采用**协作式轮询**（在分配入口 + 循环回跳指令处插入 `GC_SAFEPOINT_POLL` 宏）
- STW 通过 CAS 竞争协调员 + 全局 Safepoint 实现
- **典型场景**：多核服务器、并行编译器、浏览器 JS 引擎

### 2.4 场景对比

| | 单线程单协程 | 单线程多协程 | 多线程 |
|---|---|---|---|
| `GC_MULTITHREAD` | 未定义 | 未定义 | 定义 |
| 原子操作开销 | 无 | 无 | 有 |
| Safepoint | 无 | 无 | 协作式轮询 |
| 回收触发 | 内联同步 | 内联同步 + 挂起协程根扫描 | 后台 GC + Epoch |
| 线程上下文 | 可选 | 每协程一个 | 每线程一个 |
| 紫色缓冲区 | 全局一个 | 每协程独立 | 每线程独立 |
| 典型场景 | CLI/嵌入式 | 游戏脚本 | 服务器/浏览器 |

---

## 3. 框架架构

### 3.1 五层模型

```text
┌──────────────────────────────────────────────┐
│ ① VM 接入层                                   │
│   roots / descriptors / handles / finalizers  │
├──────────────────────────────────────────────┤
│ ② GC 协调层                                   │
│   safepoint / 调度 / 触发 / 统计              │
├──────────────────────────────────────────────┤
│ ③ 堆基础层                                    │
│   arenas / regions / pages / generations      │
├──────────────────────────────────────────────┤
│ ④ 屏障与元数据层                               │
│   card table / mark bits / forwarding / pin   │
├──────────────────────────────────────────────┤
│ ⑤ 算法插件层                                   │
│   marksweep / generational / rc / compacting  │
└──────────────────────────────────────────────┘
```

前四层是框架的稳定部分；第五层是可替换的算法插件。

### 3.2 仓库目录到分层的映射

```
include/xgc/              ← ① VM 接入层（公共 API + 类型定义）
src/core/runtime.c         ← ② GC 协调层（生命周期 + 分发 + contract 验证）
src/core/heap.c            ← ③ 堆基础层（young/old 统计 + space 抽象预留）
src/core/barrier.c         ← ④ 屏障层 facade
src/core/card_table.c      ← ④ 元数据层（owner→card dirty + 遍历）
src/core/bitmap.c          ← ④ 元数据层（side metadata bitset）
src/core/worklist.c        ← ③④ 共享工具（显式遍历栈）
src/core/reclaim.c         ← ③④ 共享工具（finalize + free）
src/core/descriptor.c      ← ①③ 共享工具（range-based slot tracing）
src/algo/marksweep/        ← ⑤ 算法插件
src/algo/gen_copy_ms/      ← ⑤ 算法插件
src/algo/bacon-rajan/      ← ⑤ 算法插件（骨架）
```

### 3.3 核心类型关系

```
GcRuntime（中枢）
├── cfg: GcConfig                  阈值 / 空间大小 / region 粒度
├── algo: GcAlgorithmVTable*       15 个函数指针（编译时选定）
├── hooks: GcVmHooks               scan_roots + vm_ctx
├── heap: GcHeap                   young_base/capacity/used + old 统计
├── barriers: GcBarrierSet         card table + dirty 统计
├── stats: GcStats                 allocated / peak / collections
├── algo_state: void*              算法私有状态
├── handles: GcHandle*             pin / 外部引用链表
├── worklist_data/capacity         共享显式工作栈
└── gc_state: atomic                RUNNING / STW_REQUESTED / STW_IN_PROGRESS

GcHeader（最小对象头 — 4 字段）
├── desc: GcDescriptor*            trace_slots / trace_edges / finalize
├── size: uint32_t
├── flags: uint16_t                PINNED / FINALIZABLE / WEAK_REF
└── kind: uint16_t                 VM 层类型标签
```

### 3.4 三条动态主线

**分配**：`gc_alloc_typed` → `runtime.c` → `algo->alloc()` → 必要时调 `heap.c`/`barrier.c`

**写引用**：`gc_store_ref` → `runtime.c` 先执行真实写入 → `algo->write_barrier()` → 如需要则 `barrier.c` → `card_table.c` → `bitmap.c`

**收集**：`gc_collect_minor/full` → `runtime.c` → `algo->collect_*()` → 过程中调用 `hooks.scan_roots` / `worklist.c` / `barrier` / `bitmap` / `heap.c` / `reclaim.c`

> **runtime 负责分发，core 提供公共零件，algo 决定回收策略。**

### 3.5 代码修改指南

| 你要做什么 | 优先改哪里 |
|-----------|-----------|
| 新增/调整公共 API | `include/xgc/gc.h` + `src/core/runtime.c` |
| 增加 side metadata 抽象 | `src/core/bitmap.c` / `card_table.c` / `barrier.c` |
| 调整 GC 扫描顺序 | `src/algo/<name>/` |
| 增加 heap/space 统计 | `src/core/heap.c` |
| 增加新算法 | `src/algo/<new>/` + vtable 接入 |
| 增加 safepoint/并发调度 | `src/core/runtime.c` |
| 验证 substrate 正确性 | `tests/test_*_substrate.c` |

---

## 4. 对象模型

### 4.1 为什么公共头必须最小化

如果把 `rc`、`gc_ref`、`color`、`purple flag` 写死进公共对象头：

- Mark-Sweep 有无用字段
- Copying/Compacting 仍缺少 forwarding / pin / region metadata
- 并发算法的大量状态更适合 side metadata 而非对象头内嵌

因此：**对象描述 + 算法状态 + 堆布局信息必须分层处理。**

### 4.2 最小公共对象头

```c
typedef struct GcHeader {
    const GcDescriptor *desc;  // 类型描述符
    uint32_t            size;  // 对象总大小
    uint16_t            flags; // GC_OBJECT_FLAG_PINNED / FINALIZABLE / WEAK_REF
    uint16_t            kind;  // VM 可选类型标签
} GcHeader;
```

### 4.3 算法元数据的三种承载方式

| 方式 | 优点 | 缺点 | 适用 |
|------|------|------|------|
| 对象头扩展字段 | 访问快，局部性好 | 与某算法耦合，头部膨胀 | RC 类强绑定对象身的字段 |
| side metadata（推荐） | header 保持小，可多算法共存 | 需要额外查表 | mark bits / age / card table / forwarding |
| algorithm-private overlay | 灵活 | 无统一抽象 | RC 特有字段（如 `rc`）放入算法私有 state |

**推荐结论**：公共 header 最小化 + side metadata 为主 + 少量 algorithm-private overlay。

---

## 5. 描述符系统

### 5.1 核心思想

所有精确 GC 都离不开对象布局信息。每个对象类型有一份 `GcDescriptor`：

```c
typedef struct GcDescriptor {
    const char    *name;
    uint32_t       fixed_size;
    uint32_t       flags;            // CONTAINS_REFS / HAS_FINALIZER / MOVABLE
    uint16_t       kind;
    GcTraceSlotsFn      trace_slots;       // 枚举所有引用 slot → 用于 moving GC
    GcTraceSlotsRangeFn trace_slots_range; // 枚举指定字节范围引用 slot → 用于 card 精细扫描
    GcTraceEdgesFn      trace_edges;       // 枚举所有子对象 → 用于 mark/tracing
    GcFinalizeFn        finalize;          // 对象释放时通知 VM 清理资源
} GcDescriptor;
```

### 5.2 `trace_slots` vs `trace_edges`

- `trace_edges`：只读遍历，适合 mark、统计
- `trace_slots`：返回 `GcHeader**`，GC 可以**改写** slot 内容——这是 moving GC 的前提
- `trace_slots_range(byte_begin, byte_end)`：card 精细扫描的关键——只扫描脏 card 覆盖的 slot 范围

---

## 6. 根模型

### 6.1 根的种类

- 线程栈局部变量
- 寄存器 / VM frame 局部变量
- 全局变量 / 模块表 / package 表
- 挂起协程 / continuation 保存栈
- C API handle / pinned handle
- 弱引用表 / finalizer queue

### 6.2 根扫描接口必须暴露 slot，不只是对象

Non-moving collector 看到 `GcHeader*` 就够了。Moving collector 必须能**更新** root：

```c
typedef void (*GcScanRootsFn)(void *vm_ctx,
                              void (*visit_root_slot)(GcHeader **slot, void *ctx),
                              void *ctx);
```

### 6.3 多协程场景

一个 OS 线程上的多个协程不是唯一 root container。必须扫描：
- 当前协程栈
- 所有挂起协程保存栈
- continuation / scheduler 保存的根

---

## 7. 堆模型

### 7.1 空间类型

| 空间 | 用途 | 移动性 |
|------|------|--------|
| Non-moving space | Mark-Sweep 老年代 / 外部 C 指针友好 | 否 |
| Copying semispace | nursery / young generation bump-pointer | 是 |
| Region space | G1 风格分区 | 可选 |
| Large object space | 大对象单独管理 | 否 |

### 7.2 推荐策略

> **年轻代可移动、老年代默认不移动。**

这是工程上最稳的折中：既获得 copying nursery 的分配/回收效率，又不强迫整个 VM 全面接受地址变化。

---

## 8. 屏障模型

### 8.1 写屏障为什么必须框架化

不同算法需要写屏障，但目的截然不同：

| 算法家族 | 屏障目的 |
|---------|---------|
| Generational GC | 记录 old → young 引用 |
| Incremental tri-color | 维护三色不变式 |
| SATB (G1) | 记录被覆盖的旧值 |
| RC | 统一赋值入口做 retain/release |

因此**屏障是框架共性设施**，算法只在其中挂接自己的行为。

### 8.2 统一写屏障入口

```c
void gc_store_ref(GcRuntime *rt, GcThreadContext *thread,
                  GcHeader *owner, GcHeader **slot, GcHeader *new_value);
```

四层语义：① 真实写入 slot → ② 调用当前算法 `write_barrier` → ③ 若有 card table 则脏化 → ④ 若算法需要 RC 则 retain/release

### 8.3 读屏障

仅在需要时启用（并发 compaction、染色指针、forwarding during evacuation），可选 capability。

---

## 9. 移动对象支持

要支持 copying / compacting，必须解决"地址变了之后所有引用都要更新"：

- **精确 slot 更新**：descriptor 的 `trace_slots` 暴露 `GcHeader**`
- **handle indirection**：外部 C 代码通过 `GcHandle` 间接持有对象
- **pinning**：`pin/unpin` API 临时固定对象
- **forwarding**：旧地址 → 新地址映射（`gen-copy-ms` 用 `GcGenForward` 链表）

推荐路线：**v1 年轻代可移动 + 老年代默认不移动，外部 C 持有裸指针的对象用 pin/handle 保护。**

---

## 10. 算法插件模型

### 10.1 Capability-based 设计

每个算法必须声明自己的能力：

```c
typedef struct GcAlgorithmCaps {
    uint8_t supports_moving;              // 是否移动对象
    uint8_t supports_compaction;          // 是否 compact
    uint8_t supports_generations;         // 是否有分代
    uint8_t requires_write_barrier;       // 是否需要写屏障
    uint8_t requires_read_barrier;        // 是否需要读屏障
    uint8_t supports_concurrent_mark;     // 是否支持并发标记
    uint8_t supports_concurrent_relocate; // 是否支持并发重定位
    uint8_t provides_deterministic_release; // 是否提供即时释放语义
} GcAlgorithmCaps;
```

### 10.2 核心 vtable

```c
typedef struct GcAlgorithmVTable {
    const char       *name;
    GcAlgorithmCaps   caps;

    // 生命周期
    void (*global_init)(GcRuntime *rt, const GcConfig *cfg);
    void (*global_destroy)(GcRuntime *rt);
    void (*thread_init)(GcRuntime *rt, GcThreadContext *thread);
    void (*thread_destroy)(GcRuntime *rt, GcThreadContext *thread);

    // 分配
    void *(*alloc)(GcRuntime *rt, GcThreadContext *thread,
                   const GcDescriptor *desc, size_t size, uint32_t alloc_flags);
    void (*post_alloc)(GcRuntime *rt, GcHeader *obj);

    // 屏障
    void (*write_barrier)(GcRuntime *rt, GcThreadContext *thread,
                          GcHeader *owner, GcHeader **slot,
                          GcHeader *old_value, GcHeader *new_value);
    GcHeader *(*read_barrier)(GcRuntime *rt, GcThreadContext *thread, GcHeader **slot);

    // 收集
    void (*collect_minor)(GcRuntime *rt);
    void (*collect_major)(GcRuntime *rt);
    void (*collect_full)(GcRuntime *rt);

    // pin / unpin
    void (*pin)(GcRuntime *rt, GcHeader *obj);
    void (*unpin)(GcRuntime *rt, GcHeader *obj);
} GcAlgorithmVTable;
```

### 10.3 Caps ↔ vtable 一致性（启动验证）

框架在 `gc_runtime_create` 中验证算法声明的能力与实际函数指针一致：

| 如果 caps 声明了 | 那么 vtable 中 |
|-----------------|---------------|
| `requires_write_barrier` | `write_barrier != NULL` |
| `requires_read_barrier` | `read_barrier != NULL` |
| `supports_moving` | `pin != NULL && unpin != NULL` |
| `supports_generations` | `collect_minor != NULL` |

任何不匹配在启动时被拒绝（返回 NULL），而非运行中崩溃。

### 10.4 添加新算法的步骤

1. 创建 `src/algo/<name>/` 目录
2. 实现至少 `alloc` + `collect_full`（最简单），其余可选
3. 声明 `GcAlgorithmCaps` 并填写 vtable
4. 在 `gc_algorithm.h` 中注册 `xgc_algorithm_<name>()` 工厂函数
5. CMake 中 `cmake -DGC_ALGORITHM=<name>` 切换

详细契约见 `src/algo/INTERFACE.md`。

---

## 11. 运行时核心

### 11.1 GcRuntime

```c
typedef struct GcRuntime {
    GcConfig                cfg;
    const GcAlgorithmVTable *algo;
    GcVmHooks               hooks;
    GcHeap                  heap;
    GcBarrierSet            barriers;
    GcStats                 stats;
    void                   *algo_state;     // 算法私有状态
    GcHandle               *handles;        // 句柄链表
    GcHeader              **worklist_data;  // 共享显式工作栈
    int                     worklist_capacity;
    gc_atomic_int_t         gc_state;       // RUNNING / STW_REQUESTED / STW_IN_PROGRESS
} GcRuntime;
```

### 11.2 三层 Contract 验证

| 层 | 约束方式 | 时机 |
|----|---------|------|
| **Algorithm contract** | caps ↔ vtable 签名一致性 | `gc_runtime_create` 启动时 |
| **Substrate contract** | `gc_*_check_invariants` 系列函数 | 测试 + 安全点断言 |
| **VM contract** | `hooks` 类型 + `GcDescriptor` 字段完整性（当前为文档约束） | VM 开发者的 demo 参考 |

---

## 12. GC 基础概念

### 12.1 可达性与三色抽象

GC 判断对象存活的标准：**从根集出发能否到达**。不可达 = 垃圾，即使对象之间互相引用形成环。

所有 tracing GC 可用三色抽象理解：

| 颜色 | 含义 | 状态 |
|------|------|------|
| 白色 | 未被扫描 | 可能是垃圾（标记完成后仍为白色 = 垃圾） |
| 灰色 | 已标记存活，子节点待扫描 | 在 worklist 中排队 |
| 黑色 | 已标记存活，子节点已扫描 | 确认存活 |

**三色不变式**：黑色对象不能直接指向白色对象。写屏障的作用就是在 mutator 修改引用关系时维护这个不变式。

### 12.2 精确 vs 保守

| | 精确 GC | 保守 GC |
|---|---|---|
| 指针识别 | VM 提供对象布局元数据 | 扫描内存，猜测哪些值像指针 |
| 根扫描 | 编译器/解释器明确告知 | 扫描整个栈和寄存器 |
| 移动对象 | ✅ | ❌（可能把整数误更新） |
| xgc 选择 | **精确 GC** | — |

### 12.3 分代假设

> 大多数对象创建后很快死亡（婴儿死亡率高），长活对象倾向继续长活。

这是分代 GC 的理论基础。young gen 频繁收集、old gen 低频收集。card table 解决"old→young 引用必须被 minor GC 看到但又不想全扫 old"的问题。

### 12.4 写屏障的四种类型

| 类型 | 目的 | 触发时机 | 使用者 |
|------|------|---------|--------|
| Remembered-set barrier | 记录 old→young 引用 | `gc_store_ref(owner(old), slot, new(young))` | 分代 GC |
| SATB barrier | 记录被覆盖的旧值 | 写入前保存旧值 | G1 |
| Incremental update barrier | 把新引用标灰 | 写入后标记新值 | CMS |
| RC barrier | retain/release | 每次赋值 | RC |

xgc 统一入口 `gc_store_ref`，由算法内部决定 barrier 行为。

### 12.5 Safepoint

xgc 采用**协作式轮询**而非信号抢占。在分配入口和循环回跳指令处插入 `GC_SAFEPOINT_POLL` 宏。线程到达 safepoint 时栈帧完整，GC 可精确扫描根。选择协作式的原因是它与延迟 RC 兼容——信号抢占可能在任意机器指令处中断，此时寄存器中可能持有"刚载入、未落盘"的对象指针。

---

## 13. Mark-Sweep 详解

### 13.1 算法

```
Phase 1 — Mark:
    所有对象初始白色
    根直接可达 → 染灰 → 入 worklist
    while worklist 非空:
        obj = pop
        for each child_slot in obj (via descriptor->trace_slots):
            if child 白色: 染灰 → push
        obj 染黑

Phase 2 — Sweep:
    for each object in 全堆对象列表:
        if 已标记: 清除标记
        else: finalize + free
```

### 13.2 实现要点

- **全对象链表**跟踪所有已分配对象
- 用 `GcWorklist` 迭代替代 C 递归防止栈溢出
- `descriptor->trace_slots` 驱动遍历，GC 核不硬编码对象类型
- sweep 前先调 `finalize` 释放非 GC 资源
- Mark 位可放对象头或 side metadata。xgc 的 `marksweep-stw` 放在算法私有 `GcMsNode.marked` 中

### 13.3 在 xgc 中的位置

- 代码：`src/algo/marksweep/ms.c`（309 行）
- Caps：`moving=0, generations=0, write_barrier=0`
- 测试：`tests/test_marksweep_smoke.c`

### 13.4 优缺点

| 优点 | 缺点 |
|------|------|
| 实现最简单，适合验证框架 | 非移动，长期运行碎片化 |
| 对象头最小（只需 mark bit） | 每次 GC 遍历所有存活对象 |
| 天然处理环 | 无分代时短命对象吞吐低 |

---

## 14. Copying GC 详解

### 14.1 Cheney 半空间复制

堆分两半：from-space（分配）和 to-space（空闲）。GC 时交换角色，存活对象从 from 复制到 to。

```
swap(from, to)
scan = free = to-start
for each root_slot: *root_slot = copy_to_to(*root_slot)
while scan < free:
    obj = *scan
    for each child_slot in obj: *child_slot = copy_to_to(*child_slot)
    scan += obj->size

copy_to_to(obj):
    if obj 已复制 (有 forwarding): return forwarding[obj]
    new = *free; memcpy(new, obj, obj->size)
    forwarding[obj] = new; free += obj->size
    return new
```

scan 和 free 指针使 to-space 本身就是 worklist——不额外需要数据结构。

### 14.2 在 xgc 中的对应

`gen-copy-ms` 的 nursery 使用 bump-pointer copying。GC 时存活对象 promote 到老年代。`GcGenForward` 链表记录 forwarding 信息。

### 14.3 优缺点

| 优点 | 缺点 |
|------|------|
| 分配 = bump pointer（极快） | 一半堆在非 GC 期间空闲 |
| GC 后零碎片 | 大对象复制成本高 |
| Cheney 不需要显式 worklist | 必须精确知道所有指针位置 |

---

## 15. Mark-Compact 详解

### 15.1 三种策略

**Two-Finger**：两端指针相向扫描，高端存活对象搬到低端空闲。简单但打乱对象顺序。

**LISP2**：先计算 forwarding 地址 → 更新所有引用 → 再 memmove。保持原有顺序，需额外 forwarding 表。

**Table-Based**（HotSpot 使用）：堆分块，块维"存活偏移表"，可并行更新和移动。

### 15.2 xgc 已具备的基础设施

`GcBitmap` 可存 mark/forwarding bits，`GcHandle` + `pin/unpin` 处理不可移动对象，`trace_slots` 是 updating references 的前提。

---

## 16. Generational GC 详解

### 16.1 Minor GC 完整流程（以 xgc gen-copy-ms 为例）

```
minor_collect:
    1. 扫 handles 中的 nursery 对象 → promote_to_old
    2. 扫 roots 中的 nursery 对象 → promote_to_old
    3. 遍历 dirty cards → 每张 card 覆盖范围内 tracing nursery 引用 → promote
    4. drain worklist → 递归处理 promoted 对象的子引用
    5. finalize dead nursery objects
    6. 清空 nursery + 清除 dirty cards
```

### 16.2 写屏障的角色

```
gc_store_ref(owner, slot, new):
    实际写入
    if owner in old && new in young:
        card_table_mark_dirty(owner, slot)
```

### 16.3 在 xgc 中的完整实现路径

```
分配 → gc_alloc_typed → runtime.c → gen_copy_ms::alloc
  ├── 小对象 → nursery bump
  └── 大/pinned → calloc + gc_barrier_register_owner

写引用 → gc_store_ref → runtime.c → gen_copy_ms::write_barrier
  └── old→young → gc_barrier_mark_slot_dirty → barrier.c → card_table.c → bitmap.c

minor GC → gc_collect_minor → runtime.c → gen_copy_ms::collect_minor
  ├── scan handles → gc_gen_evacuate_slot → gc_gen_promote
  ├── scan roots → 同上
  ├── gc_barrier_visit_dirty_cards → gc_trace_object_slots_in_range
  ├── gc_gen_minor_drain
  └── finalize_dead_nursery + gc_heap_reset_young
```

### 16.4 晋升策略

| 策略 | 描述 |
|------|------|
| Age-based | 存活 N 轮 minor GC → 晋升 |
| Size-based | 超过 nursery 一半的大对象直接进 old |
| Pinned | pinned 对象不移动，直接进 old |

---

## 17. Reference Counting 详解

### 17.1 核心原理

```
retain(obj): obj->rc++
release(obj): if --obj->rc == 0 → release children → free(obj)
```

**根本问题**：环内对象互相引用，RC 永不归零。需要额外的 **Cycle Collector**。

### 17.2 Bacon-Rajan Trial Deletion

核心洞察：环内引用都是内部引用。从环内对象的 RC 中减去内部引用，剩余 = 外部引用。外部引用 = 0 则整环是垃圾。

四个阶段：
1. 复制 RC → gc_ref，嫌疑人入紫色缓冲区
2. 深度遍历子图，对内部边执行 gc_ref--
3. gc_ref > 0 → 存活（有外部引用）；gc_ref == 0 → 垃圾
4. 释放白色对象

### 17.3 Deferred RC

栈/寄存器在正常执行期间不增减 RC。Collection 时扫描栈补偿。这消除了热路径上的 RC 操作——代价只在 GC 时支付。

### 17.4 在 xgc 中的位置

`src/algo/bacon-rajan/` 是骨架实现，`src/core/purple.c` 预留给 RC cycle collector 的嫌疑对象缓冲区。

---

## 18. 并发与增量 GC 基础

### 18.1 并发标记的根本挑战

GC 线程标记时 mutator 同时在修改对象图，可能漏标。解决方案是写屏障：

**SATB**：写入前保存旧值 → "快照时刻存在的对象都是活的"
**Incremental Update**：写入后若黑色对象指向白色 → 黑色重新染灰 → 重新扫描

### 18.2 xgc 的预留

`GcAlgorithmCaps` 已声明 `supports_concurrent_mark` / `supports_concurrent_relocate`，`GcScheduler` 预留调度结构，`core/epoch.c` 预留后台线程信号。

---

## 19. 算法选择与演进路线

### 19.1 算法速查

| 需求 | 算法 | 状态 |
|------|------|------|
| 先把 GC 跑通 | marksweep-stw | ✅ |
| 大量临时对象、高吞吐 | gen-copy-ms | ✅ |
| 资源即时释放 | rc-cycle | ⚠️ 骨架 |
| 长期运行防碎片 | gen-copy-compact | 🔜 |
| 大堆低暂停 | regional-concurrent | 🔜 远期 |

### 19.2 演进路线

```
marksweep-stw → gen-copy-ms → gen-copy-compact → rc-cycle → regional-concurrent
```

每一步新增的能力是下一步的基础：
- `marksweep-stw`：验证 descriptor + roots + worklist + finalizer 闭环
- `gen-copy-ms`：验证 moving + barrier + remembered set
- `gen-copy-compact`：验证 forwarding + slot update + pinning
- `rc-cycle`：验证 RC 插件 + deterministic release
- `regional-concurrent`：验证并发标记 + region heap + 低暂停

### 19.3 算法对 Substrate 的依赖

| Substrate | marksweep | gen-copy-ms | gen-copy-compact | rc-cycle | concurrent |
|-----------|:---------:|:-----------:|:----------------:|:--------:|:----------:|
| worklist | ✅ | ✅ | ✅ | ✅ | ✅ |
| bitmap | - | - | ✅ | - | ✅ |
| card_table | - | ✅ | ✅ | - | ✅ |
| barrier facade | - | ✅ | ✅ | ✅ | ✅ |
| heap accounting | - | ✅ | ✅ | - | ✅ |
| pin/handle | ✅(简) | ✅ | ✅ | - | ✅ |
| safepoint | - | - | - | - | ✅ |
| purple buffer | - | - | - | ✅ | - |

### 19.4 决策树

```
是否需要对象地址稳定（C 代码大量持有裸指针）？
  ├── 是 → 是否需要即时析构？
  │         ├── 是 → rc-cycle
  │         └── 否 → marksweep-stw
  └── 否 → 大量临时短命对象？
            ├── 是 → gen-copy-ms
            └── 否 → 需要低暂停？
                      ├── 是 → regional-concurrent
                      └── 否 → gen-copy-compact
```

### 19.5 为什么不从 RC 开始

1. RC 对框架的要求并不低（需统一写屏障 + precise slot tracing + 多线程原子 RC + 环检测）
2. 先做 tracing 能更快暴露框架问题
3. Tracing substrate 是未来所有复杂算法的基础

---

## 20. 工业参考

| 运行时 | 算法 | 可借鉴 | 不宜照搬 |
|--------|------|--------|---------|
| CPython | RC + cycle collector | 即时释放 + 环检测 | 强绑定 Python 对象模型与 GIL |
| Swift/ObjC ARC | 编译器辅助 RC | 资源语义清晰 | 依赖编译期插桩 |
| Lua 5.4 | 增量/分代 tracing | 小型 VM 的工程权衡 | 与 Lua 对象布局深耦合 |
| HotSpot JVM | 分代/分区/并发多算法共存 | **最成熟的"多算法框架化"案例** | 体量巨大 |
| V8 | 分代 + 增量 + compaction | 动态语言对象模型与屏障体系 | JIT 深耦合 |
| Go GC | 并发 tri-color | assist/pacer/scheduler 设计 | goroutine 栈特殊 |
| .NET GC | 分代 + compacting + background | 代际与 server/workstation 模式 | CLR 深度集成 |
| OCaml | minor copying + major non-moving/compaction | 年轻代移动、老年代保守迁移的折中 | 语言运行时约束 |

核心借鉴结论：
1. 工业级系统几乎都把"框架层"和"算法层"分开
2. 绝大多数高性能 VM 至少都有分代思想
3. 移动 GC 的前提永远是精确 roots + 可更新 slot
4. 并发 GC 的前提是完善的 barrier 和调度基础设施

---

## 21. 架构决策摘要

1. **公共对象头最小化** — 不把算法私有字段写死进 GcHeader
2. **descriptor + precise slot tracing 作为框架核心** — RC、Tracing、Copying 的共同前提
3. **heap substrate 抽象为 spaces/generations/regions** — 不固定成"全局链表 + malloc"
4. **barrier 是框架服务，不是算法实现细节** — 写屏障统一、读屏障按 capability 可选
5. **算法通过 capability-based vtable 注册** — 不用 `#ifdef` 撕裂整个工程
6. **先建 tracing 主线，再引入 RC 插件** — marksweep → gen-copy-ms → compacting → rc-cycle → concurrent

---

## 22. 演进路线图

```
Phase 0 — substrate 建立        ← 当前阶段
Phase 1 — marksweep-stw 完善    ← bitmap 接入、heap substrate 进化
Phase 2 — gen-copy-ms 优化      ← card-table 查找优化、大对象 support
Phase 3 — gen-copy-compact      ← forwarding table 正规化、old gen 压缩
Phase 4 — rc-cycle 插件化       ← 原子 RC 路径、Bacon-Rajan 环检测
Phase 5 — concurrent/regional   ← SATB barrier、并发 mark、region evacuation
```

当前最优先的工作：**把 card table 从 O(n) 线性查找优化 + bitmap 接入 marksweep + arena/page allocator 基础化 heap substrate。**

---

*文档版本: v2.1 — 2026-06-08*
