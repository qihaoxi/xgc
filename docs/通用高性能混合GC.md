# 通用高性能混合 GC 核心算法 — 正式设计文档

## 〇、设计目标与适用范围

本 GC 核心是一个**独立于任何具体 VM 的纯算法底座**。它不绑定特定类型系统、不假设特定并发模型、不依赖特定字节码解释器结构。适用范围：

| 场景 | 配置 |
|------|------|
| 单协程 / 单线程 | `GC_MULTITHREAD` 未定义，原子操作退化为普通类型 |
| 多协程共享解释器 | 各协程独立 `GcThreadContext`，共享 `GcGlobalContext` |
| 多线程并行 | 定义 `GC_MULTITHREAD`，原子 RC + 各线程独立 purple buffer + safepoint 同步 |
| 栈式虚拟机 | `on_scan_roots` 扫描操作数栈 |
| 寄存器式虚拟机 | `on_scan_roots` 扫描寄存器数组 |

核心原则：**不可变算法核心 + 可插拔策略层**。收集算法本身不随场景变化；触发时机、缓冲区大小、线程同步策略通过 SPI 回调和配置参数由上层控制。

---

## 一、核心数据结构

### 1.1 侵入式通用对象头

```c
typedef enum {
    GC_COLOR_BLACK  = 0,  // 存活（外部引用存在）
    GC_COLOR_GRAY   = 1,  // 正在进行拓扑减分扫描（trial deletion 内部状态）
    GC_COLOR_WHITE  = 2,  // 确认为垃圾（仅环内引用，无外部引用）
    GC_COLOR_PURPLE = 3,  // 循环引用嫌疑（RC 减少但未归零）
} GcColor;

// 编译时条件：多线程使用 C11 原子类型，单线程退化为普通类型
#if defined(GC_MULTITHREAD) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    #include <stdatomic.h>
    typedef _Atomic uint32_t atomic_rc_t;
    typedef _Atomic uint8_t  atomic_color_t;
#else
    typedef uint32_t atomic_rc_t;
    typedef uint8_t  atomic_color_t;
#endif

typedef struct GcHeader {
    atomic_rc_t    rc;             // 仅记录"堆引用"的计数（栈/寄存器引用不在此计数内）
    uint32_t       gc_ref;         // trial deletion 工作计数（非原子，仅 GC 线程访问）
    atomic_color_t color;          // GC 颜色状态
    uint8_t        in_purple_buf;  // 防止重复加入嫌疑缓冲区
    uint16_t       obj_type;       // 弱类型标签（由 VM 层定义，如 TYPE_DICT=1）
    struct GcHeader *next;         // 全局对象链表节点
} GcHeader;
```

**死命令**：`GcHeader` 必须是 VM 对象结构体的**第一个成员**。C 语言保证第一个成员的地址与结构体地址相同——`GcHeader*` 与 `VmObject*` 之间的转换是零成本的。

```c
typedef struct VmDict {
    GcHeader gc;         // 必须在此
    int      capacity;
    int      count;
    // ...
} VmDict;
```

### 1.2 全局上下文与线程本地上下文

```c
// ── SPI 回调接口 ──

// 遍历子节点：GC 核心需要知道 parent 下挂载了哪些 child
typedef void (*GcTraceFn)(GcHeader *parent,
                          void (*visit)(GcHeader *child, void *ctx),
                          void *ctx);

// 扫描根节点：GC 核心需要 VM 提供当前活跃栈/寄存器中的所有对象引用
typedef void (*GcScanRootsFn)(void *vm_thread_ctx,
                              void (*mark_alive)(GcHeader *root, void *ctx),
                              void *ctx);

// 终结器：对象被 GC 释放时通知 VM 清理内部资源
typedef void (*GcFinalizeFn)(GcHeader *obj);

// 内存压力通知：GC 核心在内存逼近水位线时通知 VM
typedef void (*GcPressureFn)(void *vm_ctx, int level);
    // level 0: 正常
    // level 1: 超过 Soft Limit — 建议 VM 限速或触发轻量回收
    // level 2: 超过 Hard Limit — 建议 VM 熔断或抛出 OOM

// 紫色缓冲区满触发：由上层策略层决定如何执行收集
//   单线程: 注册为同步内联的 gc_collect_cycles
//   多线程: 注册为信号通知 Background GC 线程
typedef void (*GcPurpleFullFn)(GcGlobalContext *global, GcThreadContext *thread);

// ── 上下文结构 ──

typedef struct {
    GcHeader     *global_head;               // 全局对象无锁单链表（通过 CAS 并发挂载）
    GcTraceFn      on_trace_children;         // VM 注册：遍历子节点
    GcFinalizeFn   on_finalize;               // VM 注册：析构对象
    GcPressureFn   on_pressure;               // VM 注册：内存压力通知
    GcPurpleFullFn on_purple_buffer_full;     // 策略层注册：紫色缓冲区满时的行为

    size_t        total_allocated;           // 全局已分配字节数（近似值）
    size_t        gc_threshold_soft;         // 低水位线（如 64MB）
    size_t        gc_threshold_hard;         // 高水位线（如 128MB）

    GcHeader     *worklist_data;             // 显式工作栈（防 C 递归栈溢出）
    int           worklist_top;
    int           worklist_capacity;
} GcGlobalContext;

typedef struct {
    GcHeader      **purple_buffer;           // 本地嫌疑人环形缓冲区
    int             purple_count;
    int             purple_capacity;         // 自适应动态调整

    GcHeader       *local_head;              // TLAB 本地分配链表头
    int             local_alloc_count;
    int             tlab_threshold;          // 批量冲刷阈值（如 128）

    GcScanRootsFn   on_scan_roots;           // VM 注册：扫描本线程栈/寄存器根
    void           *vm_thread_ctx;           // 外部 VM 线程/协程上下文句柄

    // Ring Buffer 瞬时对象区
    GcHeader       *zct_ring;               // 零计数表环形缓冲区
    int             zct_head;
    int             zct_capacity;
} GcThreadContext;
```

---

## 二、核心算法

### 2.1 统一写屏障（堆引用的唯一入口）

所有堆属性的赋值（`dict[key] = value`、`list[i] = obj`）必须经过此函数。这是保证 RC 正确性的**唯一通道**。

```c
void gc_assign_field(GcGlobalContext *global, GcThreadContext *thread,
                     GcHeader **field, GcHeader *new_child)
{
    // Step 1: 原子交换 — 同时写入新指针、取回旧指针（1 次原子操作）
    GcHeader *old_child = ATOMIC_XCHG(field, new_child);

    // Step 2: 对新孩子增加引用计数（第 2 次原子操作）
    if (new_child) {
        ATOMIC_INC(new_child->rc);
        if (new_child->color == GC_COLOR_WHITE)
            new_child->color = GC_COLOR_BLACK;
    }

    // Step 3: 对旧孩子减少引用计数
    if (old_child) {
        uint32_t current_rc = ATOMIC_DEC(old_child->rc);
        if (current_rc == 0) {
            // RC 归零: 无外部引用，直接标记为白色
            ATOMIC_STORE(&old_child->color, GC_COLOR_WHITE);
        } else {
            uint8_t c = ATOMIC_LOAD(&old_child->color);
            if (c == GC_COLOR_GRAY || c == GC_COLOR_PURPLE) {
                // ★ 关键: old_child 正处于 GC 线程的嫌疑子图中（GRAY）
                //    或在某线程的紫色缓冲区中等待扫描（PURPLE）。
                //    写屏障切断了指向它的一个引用，但它仍有 RC>0。
                //    无论它是 GRAY 还是 PURPLE，只要断连后还活着，
                //    一律强制拉回 BLACK 安全区——退出当前异步嫌疑集，
                //    交由下一轮 Epoch 重新评估。
                ATOMIC_STORE(&old_child->color, GC_COLOR_BLACK);
                old_child->in_purple_buf = 0;
            } else if (c != GC_COLOR_PURPLE && !old_child->in_purple_buf) {
                // RC 减少但未归零且不在任何嫌疑集中 → 送入紫色缓冲区
                ATOMIC_STORE(&old_child->color, GC_COLOR_PURPLE);
                old_child->in_purple_buf = 1;
                gc_push_purple_adaptive(global, thread, old_child);
            }
        }
    }
}
```

**为什么 Step 1 先交换再增减是安全的**：调用方在传递 `new_child` 之前已持有它的引用（RC ≥ 1）。交换完成后 `field` 指向 `new_child`（RC ≥ 1），旧值被取出但尚未释放——这个瞬间不存在"RC=0 但被引用"的窗口。

### 2.2 自适应紫色缓冲区

```c
void gc_push_purple_adaptive(GcGlobalContext *global, GcThreadContext *thread,
                             GcHeader *obj)
{
    thread->purple_buffer[thread->purple_count++] = obj;

    if (thread->purple_count >= thread->purple_capacity) {
        // ★ 核心不自己解环 — 而是调用由上层策略注册的 SPI 回调。
        //    单线程模式: 注册为同步内联的 gc_collect_cycles
        //    多线程模式: 注册为信号通知 Background GC 线程
        //    核心自身不关心上层如何实现收集 — 这是"可插拔策略层"的关键。
        global->on_purple_buffer_full(global, thread);
    }
}

// ── 单线程/单协程模式下的默认 SPI 实现 ──
void gc_on_purple_full_singlethread(GcGlobalContext *global, GcThreadContext *thread)
{
    int pre_count = thread->purple_count;

    gc_collect_cycles(global, thread);

    // 根据出货率自适应调整缓冲区大小
    int freed = pre_count - thread->purple_count;
    if (freed > pre_count / 2)
        thread->purple_capacity = thread->purple_capacity / 2;
    else
        thread->purple_capacity = thread->purple_capacity * 2;

    if (thread->purple_capacity < 64)   thread->purple_capacity = 64;
    if (thread->purple_capacity > 4096) thread->purple_capacity = 4096;
}
```

初始 `purple_capacity` 建议设为 256，让系统自适应调整。

**多线程模式下的 SPI 实现见第八节（多线程并发架构）。**

### 2.3 循环检测核心：Bacon-Rajan Trial Deletion

这是整个通用 GC 的"心脏"。不扫描全局 roots，只对紫色缓冲区内对象做拓扑分析。

```c
void gc_collect_cycles(GcGlobalContext *global, GcThreadContext *thread)
{
    int n = thread->purple_count;
    GcHeader **buf = thread->purple_buffer;

    // ── Phase 1: 复制 refcount → gc_ref ──
    // ★ 关键: obj->rc 可能正在被 Mutator 并发的写屏障修改。
    //    必须用 atomic_load 确保读取到一致的 RC 值。
    //    gc_ref 本身仅由 GC 线程单方面修改，不需要原子操作。
    for (int i = 0; i < n; i++) {
        GcHeader *obj = buf[i];
        obj->gc_ref        = ATOMIC_LOAD(&obj->rc);
        ATOMIC_STORE(&obj->color, GC_COLOR_GRAY);   // ★ 必须原子: color 是 atomic_color_t
        obj->in_purple_buf = 0;
    }

    // ── Phase 2: 减去内部引用（深度遍历整张子图） ──
    // ★ 关键: 不能只遍历 buf 内对象的直接子节点。循环引用环可能很
    //    深（A→B→C→D→A），而 buf 中可能只有 A。必须使用显式工作
    //    栈从 buf 出发，深度优先遍历能触达的整张 GRAY 子图，
    //    对其中每条内部边都执行 gc_ref--。
    //    效果: 消除嫌疑子图内部的所有相互引用，只保留外部引用在 gc_ref 中。
    GcWorklist wl = { .data = global->worklist_data, .top = 0,
                     .capacity = global->worklist_capacity };

    // 将所有嫌疑人压入工作栈作为遍历起点
    for (int i = 0; i < n; i++)
        gc_worklist_push(&wl, buf[i]);

    // 深度优先遍历: 对每个 GRAY 节点, 遍历其子节点。
    // 若子节点也是 GRAY → gc_ref-- (消除一条内部边) → 将子节点也压栈继续深入
    while (wl.top > 0) {
        GcHeader *obj = gc_worklist_pop(&wl);
        global->on_trace_children(obj, gc_phase2_subtract_and_push, &wl);
    }
    // gc_phase2_subtract_and_push 回调:
    //   if (child->color == GC_COLOR_GRAY) {
    //       child->gc_ref--;
    //       gc_worklist_push(wl, child);  // 继续向深处探索
    //   }

    // ── Phase 3: 分离存活与垃圾 ──
    // gc_ref > 0                            → 嫌疑集外部有引用 → 存活
    // color == GC_COLOR_BLACK               → Mutator 在 Phase 2 期间通过写屏障
    //                                         将其染黑（挂载了新的外部引用）→ 存活
    // gc_ref == 0 && color != BLACK         → 仅嫌疑集内部引用 → 垃圾
    // gc_ref < 0                            → 内部引用超外部引用 → 垃圾
    //
    // ★ TOCTOU 防护: GC 线程在判定为垃圾并打 WHITE 标签时，必须使用 CAS。
    //   在 ATOMIC_LOAD(color) 与 ATOMIC_STORE(WHITE) 之间的纳秒窗口中，
    //   Mutator 可能已通过写屏障将颜色改为 BLACK。CAS 原子检测这种并发修改。
    int alive_count = 0;
    for (int i = 0; i < n; i++) {
        GcHeader *obj = buf[i];
        uint8_t c = ATOMIC_LOAD(&obj->color);
        if (obj->gc_ref > 0 || c == GC_COLOR_BLACK) {
            ATOMIC_STORE(&obj->color, GC_COLOR_BLACK);
            buf[alive_count++] = obj;
        } else {
            // 尝试将 GRAY 原子切换为 WHITE
            uint8_t expected = GC_COLOR_GRAY;
            if (CAS(&obj->color, &expected, GC_COLOR_WHITE)) {
                // CAS 成功: 确认垃圾, 保持 WHITE
            } else {
                // CAS 失败: Mutator 在此期间将其染成了 BLACK → 救回!
                ATOMIC_STORE(&obj->color, GC_COLOR_BLACK);
                buf[alive_count++] = obj;
            }
        }
    }

    // ── Phase 4: 释放垃圾 ──
    for (int i = 0; i < n; i++) {
        if (buf[i]->color == GC_COLOR_WHITE) {
            gc_reclaim_object(global, buf[i]);   // 递归释放子节点
        }
    }

    // 将存活对象压缩到缓冲区前端（后续可重用这些槽位）
    thread->purple_count = alive_count;
}
```

**为什么 gc_ref < 0 也要判定为垃圾**：当嫌疑对象 A 包含嫌疑对象 B 的多个引用时（如 list 中有重复元素），Phase 2 对 B 的 gc_ref 减去多次——可能导致 gc_ref 为负。负值 = 外部引用数 < 内部引用消除数，等价于没有足够的外部引用，应判定为垃圾。CPython `gcmodule.c` 采用相同逻辑。

### 2.4 栈根保护：延迟 RC 下的递归回滚

栈/寄存器在正常执行期间不增减 RC。Collection 期间必须扫描栈，将被栈引用的对象及其**所有子孙节点**从 GRAY 状态"救回"——否则 Phase 2 中已被扣减 gc_ref 的子孙节点会被 Phase 3 误判为垃圾。

```c
void gc_scan_stack_and_protect(GcGlobalContext *global, GcThreadContext *thread)
{
    // Phase 1: VM 注册的回调扫描栈/寄存器，把所有被栈引用的对象
    //          放入显式工作栈作为"救回起点"
    GcWorklist wl = { .data = global->worklist_data, .top = 0,
                     .capacity = global->worklist_capacity };

    thread->on_scan_roots(thread->vm_thread_ctx, gc_push_root_to_worklist, &wl);

    // Phase 2: 显式工作栈驱动的递归回滚 — 从栈根出发遍历整张子图
    //          对每个处在 GRAY 状态的子孙：
    //            gc_ref++   （撤销 Phase 2 中试探性扣减的内部引用）
    //            color=BLACK（标记为存活）
    //            → 继续向子节点深入
    while (wl.top > 0) {
        GcHeader *obj = gc_worklist_pop(&wl);
        if (obj->color == GC_COLOR_GRAY) {
            obj->gc_ref++;           // 补偿: 撤销试探性扣减
            obj->color = GC_COLOR_BLACK;
            // 继续深入子节点 — 确保整条引用链上所有对象都被救回
            global->on_trace_children(obj, gc_push_gray_child_to_worklist, &wl);
        }
    }
}

// 辅助回调:
static void gc_push_root_to_worklist(GcHeader *root, void *ctx) {
    GcWorklist *wl = (GcWorklist *)ctx;
    if (root->color == GC_COLOR_GRAY) gc_worklist_push(wl, root);
}

static void gc_push_gray_child_to_worklist(GcHeader *child, void *ctx) {
    GcWorklist *wl = (GcWorklist *)ctx;
    if (child->color == GC_COLOR_GRAY) gc_worklist_push(wl, child);
}
```

**为什么必须递归回滚**：Phase 2 的深度遍历把嫌疑人集能触达的整张子图全部染成 GRAY 并扣减了 gc_ref。如果栈上只持有根节点 A，A 的子孙 B、C、D 也处于 GRAY 状态且 gc_ref 已被扣减。仅把 A 救回 BLACK 是不够的——B、C、D 会在 Phase 3 中被判定为垃圾。必须沿 A 的引用链递归恢复所有子孙。

### 2.5 递归释放（断环）

```c
void gc_reclaim_object(GcGlobalContext *global, GcHeader *obj)
{
    // 防止重入（环可能导致递归回到已释放对象）
    if (obj->color == GC_COLOR_BLACK) return;
    obj->color = GC_COLOR_BLACK;  // 标记为"处理中"

    // 递归遍历子节点: 断开所有引用（触发子节点的 gc_assign_field(old=NULL)）
    // 子节点的 RC 可能因此归零 → 递归释放
    global->on_trace_children(obj, gc_reclaim_child, global);

    // 通知 VM 清理内部资源
    global->on_finalize(obj);

    // 归还内存
    free(obj);
}

static void gc_reclaim_child(GcHeader *child, void *ctx) {
    GcGlobalContext *global = (GcGlobalContext *)ctx;
    uint32_t current_rc = ATOMIC_DEC(child->rc);
    if (current_rc == 0) {
        gc_reclaim_object(global, child);
    }
}
```

---

## 三、内存分配与 TLAB

### 3.1 通用分配接口

```c
void *gc_alloc(GcGlobalContext *global, GcThreadContext *thread,
               size_t total_size, uint16_t obj_type)
{
    // 优先尝试 Ring Buffer 原地复用（临时对象快速路径）
    void *fast = gc_try_ring_buffer_alloc(thread, total_size);
    if (fast) return fast;

    // 常规路径
    GcHeader *obj = (GcHeader *)malloc(total_size);
    obj->rc           = 0;         // 延迟计数：初生对象栈引用不计入 RC
    obj->color        = GC_COLOR_WHITE;
    obj->in_purple_buf = 0;
    obj->obj_type     = obj_type;

    // TLAB 批量挂载: 先挂本地链表，达到阈值再一次 CAS 挂全局
    obj->next = thread->local_head;
    thread->local_head = obj;
    thread->local_alloc_count++;

    if (thread->local_alloc_count >= thread->tlab_threshold) {
        gc_flush_tlab(global, thread);
    }

    global->total_allocated += total_size;

    // 水位线检查
    if (global->total_allocated > global->gc_threshold_hard)
        global->on_pressure(global, 2);  // Hard Limit: 通知 VM
    else if (global->total_allocated > global->gc_threshold_soft)
        global->on_pressure(global, 1);  // Soft Limit: 建议 VM 限速

    return (void *)obj;
}
```

### 3.2 TLAB 批量冲刷

```c
void gc_flush_tlab(GcGlobalContext *global, GcThreadContext *thread)
{
    if (thread->local_head == NULL) return;

    // 找到本地链表尾节点（单线程访问 local_head，无需原子操作）
    GcHeader *tail = thread->local_head;
    while (tail->next != NULL) tail = tail->next;

    // 一次 CAS 把整条 TLAB 链挂到全局链表头
    do {
        tail->next = global->global_head;
    } while (!CAS(&global->global_head, tail->next, thread->local_head));

    thread->local_head      = NULL;
    thread->local_alloc_count = 0;
}
```

**为什么找尾节点不需要锁**：`local_head` 链表仅在当前线程/协程内访问。其他线程看不到这条链——直到 CAS 将它挂到 `global_head` 之后。

### 3.3 Ring Buffer 临时对象快速路径

Ring Buffer 的复用受严格的**安全窗口**限制。在延迟 RC 语义下，一个纯粹被栈引用的临时对象，其 `rc == 0 && color == WHITE` —— 与"已被释放可复用"的槽位在数值上完全相同。**因此在 Mutator 运行期间绝不能盲目复用 Ring Buffer 槽位**——当前操作数栈可能仍持有指向该槽位的指针。

```c
// 安全的 Ring Buffer 复用: 仅在 collection 期间、栈已扫描确认无引用后调用
void gc_reclaim_ring_buffer_slots(GcGlobalContext *global, GcThreadContext *thread)
{
    GcHeader *slot = &thread->zct_ring[thread->zct_head];

    // 安全条件: RC=0 且栈根扫描已确认该对象不在当前活跃栈/寄存器中
    // (此函数只在 gc_collect_cycles 的栈根扫描完成后调)
    if (slot->rc == 0 && slot->color == GC_COLOR_WHITE) {
        // 此时已通过栈扫描确认: 没有任何栈/寄存器引用指向此 slot
        gc_reset_object_in_place(slot, size);   // 安全: 可原地复用
        thread->zct_head = (thread->zct_head + 1) % thread->zct_capacity;
    }
}

// 热路径分配: Ring Buffer 不盲目复用，只从空闲槽位分配
void *gc_try_ring_buffer_alloc(GcThreadContext *thread, size_t size)
{
    if (size > 256) return NULL;

    // 只从未使用过的空白槽位分配，永不覆盖可能存活的旧槽位。
    // 旧槽位的复用统一在 collection 后的 gc_reclaim_ring_buffer_slots 中批量进行。
    int next = (thread->zct_head + thread->purple_count + 1) % thread->zct_capacity;
    // 若 Ring Buffer 已满 → 走常规 malloc
    if (next == thread->zct_head) return NULL;

    GcHeader *slot = &thread->zct_ring[thread->zct_head];
    thread->zct_head = next;
    slot->rc = 0;
    slot->color = GC_COLOR_WHITE;
    return slot;
}
```

**设计原则**：Mutator 热路径上的 Ring Buffer **只分配、不复用**。旧槽位的安全复用推迟到 collection 阶段——此时栈已被扫描、活对象已被回滚为 BLACK，剩余的 WHITE 对象确认为无引用，可以安全回收。用 Ring Buffer 的空间换 Mutator 热路径上零 malloc 的速度——用 collection 时的批量回收处理空间回收。

---

## 四、显式工作栈：防 C 递归深度溢出

在 Trial Deletion 的 Phase 2 中，遍历子节点可能遇到极深的引用链。如果用 C 函数递归，会在深度 ≥ 10000 时栈溢出。必须用显式工作栈。

```c
typedef struct {
    GcHeader **data;
    int        top;
    int        capacity;
} GcWorklist;

void gc_worklist_push(GcWorklist *wl, GcHeader *obj) {
    if (wl->top >= wl->capacity) {
        wl->capacity *= 2;
        wl->data = realloc(wl->data, wl->capacity * sizeof(GcHeader *));
    }
    wl->data[wl->top++] = obj;
}

GcHeader *gc_worklist_pop(GcWorklist *wl) {
    return (wl->top > 0) ? wl->data[--wl->top] : NULL;
}

// 非递归的 gray 标记
void gc_mark_gray_iterative(GcGlobalContext *global, GcHeader *root) {
    GcWorklist *wl = &(GcWorklist){
        .data     = global->worklist_data,
        .top      = 0,
        .capacity = global->worklist_capacity
    };
    gc_worklist_push(wl, root);

    while (wl->top > 0) {
        GcHeader *obj = gc_worklist_pop(wl);
        if (obj->color != GC_COLOR_GRAY) {
            obj->color = GC_COLOR_GRAY;
            global->on_trace_children(obj, gc_push_to_worklist, wl);
        }
    }
}
```

---

## 五、双水位线与背压控制

不能让 VM 在内存耗尽时才被动响应。"双水位线 + 背压"机制在内存逼近危险区域时就主动干预。

| 状态 | 条件 | GC 核心行为 | VM 建议行为 |
|------|------|-----------|-----------|
| 正常 | < Soft Limit | 仅 RC 快路径释放 | 正常执行 |
| 预警 | ≥ Soft Limit | 触发增量 ZCT 扫描 + 轻量 cycle collect | 限制脚本分配速率（背压） |
| 危急 | ≥ Hard Limit | 强制 Stop-The-World 全量回收 | 回收后内存仍超限 → 抛出 OOM 异常 |

```c
void gc_check_pressure(GcGlobalContext *global, GcThreadContext *thread)
{
    if (global->total_allocated >= global->gc_threshold_hard) {
        // 危急: 强制全局回收
        gc_collect_full_stw(global, thread);
        // 回收后仍超限: 通知 VM 熔断
        if (global->total_allocated >= global->gc_threshold_hard)
            global->on_pressure(global, 2);   // level 2: 建议 OOM
    } else if (global->total_allocated >= global->gc_threshold_soft) {
        // 预警: 轻量回收 + 建议限速
        gc_collect_cycles(global, thread);
        global->on_pressure(global, 1);        // level 1: 建议背压
    }
}
```

---

## 六、VM 集成接口（SPI）

### 6.1 VM 必须实现的回调

```c
// ① 遍历子节点 — GC 核心需要知道对象持有哪些引用
void vm_trace_children(GcHeader *parent,
                       void (*visit)(GcHeader *child, void *ctx),
                       void *ctx)
{
    switch (parent->obj_type) {
    case TYPE_DICT:
        VmDict *d = (VmDict *)parent;
        for (int i = 0; i < d->count; i++)
            if (d->pairs[i].value.type == TYPE_OBJECT)
                visit((GcHeader *)d->pairs[i].value.as_obj, ctx);
        break;
    case TYPE_LIST:
        VmList *l = (VmList *)parent;
        for (VmItem *item = l->head; item; item = item->next)
            if (item->value.type == TYPE_OBJECT)
                visit((GcHeader *)item->value.as_obj, ctx);
        break;
    case TYPE_CLOSURE:
        VmClosure *c = (VmClosure *)parent;
        if (c->outer_dict) visit((GcHeader *)c->outer_dict, ctx);
        if (c->self_dict)  visit((GcHeader *)c->self_dict, ctx);
        break;
    }
}

// ② 扫描栈/寄存器根 — collection 期间补偿延迟 RC 的栈引用
void vm_scan_stack_roots(void *vm_thread_ctx,
                         void (*mark_alive)(GcHeader *root, void *ctx),
                         void *ctx)
{
    VmThread *thread = (VmThread *)vm_thread_ctx;
    // 扫描操作数栈
    for (Value *p = thread->stack_base; p < thread->stack_top; p++) {
        if (IS_OBJECT(p)) {
            mark_alive((GcHeader *)p->as_obj, ctx);
        }
    }
    // 扫描帧局部变量（如果是寄存器式虚拟机，改为扫描寄存器数组）
    for (int f = 0; f < thread->frame_count; f++) {
        VmFrame *frame = thread->frames[f];
        for (int r = 0; r < frame->reg_count; r++) {
            if (IS_OBJECT(&frame->regs[r])) {
                mark_alive((GcHeader *)frame->regs[r].as_obj, ctx);
            }
        }
    }
}

// ③ 终结器 — 释放对象内部资源
void vm_finalize(GcHeader *obj) {
    switch (obj->obj_type) {
    case TYPE_DICT:  free(((VmDict *)obj)->pairs); break;
    case TYPE_STRING: free(((VmString *)obj)->data); break;
    }
}
```

### 6.2 VM 分配与写屏障对接

```c
// 虚拟机创建对象 — 调用 GC 核心的通用分配
VmDict *vm_create_dict(GcGlobalContext *gc, GcThreadContext *thread) {
    VmDict *d = (VmDict *)gc_alloc(gc, thread, sizeof(VmDict), TYPE_DICT);
    d->capacity = 16;
    d->count    = 0;
    d->pairs    = malloc(sizeof(VmPair) * 16);
    return d;
}

// 虚拟机修改堆属性 — 必须走 GC 核心的统一写屏障
void vm_dict_set(VmDict *d, const char *key, VmObject *value) {
    VmPair *slot = vm_dict_find_slot(d, key);
    gc_assign_field(gc_global, gc_thread,
                    (GcHeader **)&slot->value.as_obj,  // field
                    (GcHeader *)value);                  // new_child
}

// 虚拟机栈操作 — 不触发 RC
void vm_stack_push(VmThread *thread, VmObject *obj) {
    *thread->stack_top = obj;   // 纯指针复制，不增减 refcount
    thread->stack_top++;
}
VmObject *vm_stack_pop(VmThread *thread) {
    thread->stack_top--;
    return *thread->stack_top;  // 纯指针复制
}
```

---

## 七、Rhino 集成路线

### Phase 1: 修基座 — 统一写屏障 + 修正 RC（2-3 周）

**目标**：消除所有散落的 `copy_tv`/`clear_tv` 中直接的 refcount 操作。

1. 定义 `gc_header_t`（嵌入 `dict_t`, `list_t`, `partial_t`, `userfunc_t` 等前缀）
2. 实现 `gc_assign_field` 统一写屏障
3. 找到所有直接操作 `dv_refcount++`/`lv_refcount++` 的位置，替换为 `gc_assign_field`
4. 修复 interp 销毁时的 9 处 Force free（改为正确的写屏障空赋值）
5. 恢复 func_hashtab/package_hashtab 清理

**验证**：valgrind zero definitely lost。`Test.ose` 1000 次循环 — 无 use-after-free（即使不回收，也不崩溃）。

### Phase 2: 激活性 — 延迟 RC + Trial Deletion（2-3 周）

**目标**：操作数栈不再计数，GC 真正能回收环。

1. 实现 `gc_collect_cycles`（Trial Deletion 核心）
2. 实现栈根扫描回调（`vm_scan_stack_roots`）
3. 全面取消栈 push/pop 中的 `copy_tv`/`clear_tv` 调用
4. 实现紫色缓冲区 + 自适应策略
5. 触发策略：每 10000 条指令或 1MB 分配

**验证**：`Test.ose` 1000 次循环 — RSS 不增长。

### Phase 3: 高性能 — Ring Buffer + TLAB + 分代（2-3 周）

**目标**：消除分配瓶颈。

1. Ring Buffer 临时对象原地复用
2. TLAB 批量挂载全局链表
3. 简单分代：generation 0（新对象，频繁收集），generation 1（晋升对象，低频收集）
4. 双水位线 + 背压

**验证**：编译器 500 行源文件编译 — 内存颠簸降低 5x。

### Phase 4: 多线程（按需）

**目标**：`GC_MULTITHREAD` 宏开启，多 interp 并行。

1. 原子 RC 启用
2. 各线程独立 TLAB + purple buffer
3. Safepoint 同步机制

---

## 八、多线程并发架构：Background GC + Epoch

在共享堆的多线程模型下，任何一个 Mutator 线程**不能**独自内联执行 `gc_collect_cycles`。因为它正在遍历/修改的紫色子图，可能被另一个线程并发的写屏障操作修改——导致 gc_ref 被污染、活对象被误判为垃圾。

工业级的解法来自 Bacon-Rajan 2001 年经典论文的下半篇：**Background GC Thread + Epoch 机制**。

### 8.1 架构总览

```
┌──────────────────────────────────────────────────┐
│  Mutator Thread A        Mutator Thread B        │
│  (执行脚本字节码)        (执行脚本字节码)         │
│  ┌─────────────────┐    ┌─────────────────┐      │
│  │ purple_buffer   │    │ purple_buffer   │      │
│  │ 满了 → 发信号   │    │ 满了 → 发信号   │      │
│  └────────┬────────┘    └────────┬────────┘      │
│           │                      │               │
│           ▼                      ▼               │
│  ┌──────────────────────────────────────────┐    │
│  │         Background GC Thread             │    │
│  │  Epoch N:   收集所有线程的嫌疑人          │    │
│  │  Epoch N+1: 等待引用沉淀（并发安全窗口）  │    │
│  │  Epoch N+2: 异步 Trial Deletion + 释放   │    │
│  └──────────────────────────────────────────┘    │
└──────────────────────────────────────────────────┘
```

### 8.2 SPI 策略切换

```c
// ── 单线程模式: 注册同步回调 ──
global->on_purple_buffer_full = gc_on_purple_full_singlethread;

// ── 多线程模式: 注册异步通知回调 ──
global->on_purple_buffer_full = gc_on_purple_full_multithread;

void gc_on_purple_full_multithread(GcGlobalContext *global, GcThreadContext *thread)
{
    // 1. 将本线程的嫌疑人批量提交到全局并发队列
    gc_flush_purple_to_global(global, thread);

    // 2. 发信号唤醒 Background GC 线程（非阻塞）
    gc_signal_background_thread(global);

    // 3. [Mark Assist 背压] 如果跨过 Soft Limit，本线程被迫"纳税"
    //    从全局队列抢一些嫌疑人，现场帮后台 GC 做一部分标记工作。
    //    高频分配的线程天然被限速 — 代价是它必须在继续分配前
    //    "偿还内存债"。
    if (global->total_allocated >= global->gc_threshold_soft) {
        GcHeader *jobs = gc_steal_suspects_from_global(global, 16);
        if (jobs) gc_mutator_assist_scan(global, thread, jobs);
    }

    // 4. 本线程返回 — 继续执行脚本
}
```

### 8.3 Epoch 三阶段

```c
typedef struct {
    GcHeader  *pending_purple_head;   // 当前 Epoch 收集的嫌疑人链表
    int        epoch;                 // 当前 Epoch 编号 (递增)
    atomic_int phase;                 // 0=收集, 1=沉淀等待, 2=异步解环
} GcEpochState;

void gc_background_thread_main(GcGlobalContext *global)
{
    while (global->running) {
        // ── Epoch N: 收集嫌疑人 ──
        // 从全局队列取走所有线程提交的嫌疑人，打包为一个集合
        GcHeader *suspects = gc_drain_global_purple_queue(global);
        if (!suspects) { gc_wait_for_signal(global); continue; }

        // ── Epoch N+1: 并发安全等待 ──
        // 等待一个短暂的安全窗口，确保所有 Mutator 在上个 Epoch 期间
        // "逻辑上已抛弃但还在寄存器/栈里倒手"的引用已完成落盘。
        // 同时请求所有 Mutator 进入轻量 safepoint（仅同步栈根，不暂停执行）。
        gc_request_safepoint_scan(global);   // 各线程下次 safepoint 时扫描栈
        gc_wait_for_safepoint_ack(global);   // 等待所有线程确认完成栈扫描

        // ── Epoch N+2: 异步拓扑计算 ──
        // 此时嫌疑集的拓扑状态已稳定。
        // 即使 Mutator 此时向该集合内的对象挂载新引用，
        // 写屏障会强制将新挂载对象染成 BLACK —— Trial Deletion
        // 在 Phase 3 检查 gc_ref 时会安全将其判定为存活。
        gc_collect_cycles_async(global, suspects);

        // 自适应出货率反馈
        gc_update_purple_capacity(global);
    }
}
```

### 8.4 并发写屏障的安全性保证

多线程 Mutator 在 Background GC 线程执行 Trial Deletion 期间仍可并发运行。安全性由写屏障的一个关键不变量保证：

```c
void gc_assign_field(GcGlobalContext *global, GcThreadContext *thread,
                     GcHeader **field, GcHeader *new_child)
{
    GcHeader *old_child = ATOMIC_XCHG(field, new_child);

    if (new_child) {
        ATOMIC_INC(new_child->rc);
        // ★ 关键不变量: 任何被字段赋值新引用的对象, 其颜色被强制提升为 BLACK。
        //    这确保了 Background GC 线程在 Epoch N+2 做 Trial Deletion 时，
        //    即使它正在处理 new_child 所在的子图, new_child 也不会被误判——
        //    因为 gc_ref 中包含了这个新加的外部引用。
        if (new_child->color != GC_COLOR_BLACK)
            new_child->color = GC_COLOR_BLACK;
    }

    // ... 对 old_child 的处理不变 ...
}
```

**这个不变量的效果**：如果 Background GC 线程正在对嫌疑集 S 做 Trial Deletion，而 Mutator 在外部通过写屏障往 S 中的对象 A 挂载了新引用 B，B 被染成 BLACK → B 在 Phase 3 中因为 `gc_ref > 0`(外部引用) 或 `color == BLACK` 而被判定为存活。S 的拓扑一致性得以保持。

### 8.5 工业级多线程危急水位处理

在多线程并发堆模型下，**不允许 Mutator 线程独自内联执行全量回收**。当一个 Mutator 触及 Hard Limit 时，必须通过 CAS 协调 + 全局 Safepoint + STW 全量回收的标准工业时序。

#### 8.5.1 全局状态机

```c
typedef enum {
    GC_STATE_RUNNING         = 0,  // 正常运行
    GC_STATE_STW_REQUESTED   = 1,  // 有人发起了 STW 请求，等待所有线程到达 Safepoint
    GC_STATE_STW_IN_PROGRESS = 2,  // STW 回收进行中（所有 Mutator 已挂起）
} GcGlobalState;
```

#### 8.5.2 分配路径上的 Safepoint 检查

```c
void *gc_alloc_industrial(GcGlobalContext *global, GcThreadContext *thread,
                          size_t total_size, uint16_t obj_type)
{
    // ★ 每次分配前检查全局状态：是否有人已发起 STW？
    //    若是，自己先去 Safepoint 报到挂起，不要继续执行可能污染堆状态的分配。
    if (global->gc_state == GC_STATE_STW_REQUESTED)
        gc_enter_safepoint_and_park(global, thread);

    // 常规 TLAB 分配 ...
    GcHeader *obj = gc_alloc_tlab_fast(global, thread, total_size, obj_type);
    if (obj) return obj;

    // 检查水位线
    if (global->total_allocated >= global->gc_threshold_hard) {

        // ── Step 1: CAS 竞争"GC 协调员"身份 ──
        //    抢到 CAS 的线程成为协调员。协调员负责摇人、等其他人到齐、亲手 STW、
        //    然后唤醒所有人。协调员**绝对不能**调用 gc_enter_safepoint_and_park
        //    去睡觉——否则所有人一起死锁。
        bool is_coord = false;
        GcGlobalState expected = GC_STATE_RUNNING;
        if (CAS(&global->gc_state, &expected, GC_STATE_STW_REQUESTED)) {
            is_coord = true;
            gc_request_global_safepoint(global);       // 通知所有线程
        }

        if (is_coord) {
            // ── 协调员路径: 不睡觉, 等其他人到齐 ──
            while (ATOMIC_LOAD(&global->safepoint_arrive_count) > 1) {
                gc_spin_hint();  // 短暂自旋, 等最后一个线程报到
            }

            // 所有普通线程已安全挂起 — 协调员亲手执行全量 STW
            gc_collect_full_stw(global);

            // 重置状态, 唤醒所有挂起的 Mutator
            global->gc_state = GC_STATE_RUNNING;
            gc_wakeup_all_mutators(global);
        } else {
            // ── 普通线程路径: 没抢到协调员, 乖乖去 Safepoint 挂起 ──
            gc_enter_safepoint_and_park(global, thread);
        }
    }

    return (void *)obj;
}

// 协调员 STW 全量回收内部: 回收后做最终 OOM 裁决
static void gc_collect_full_stw(GcGlobalContext *global)
{
    // 合并所有线程紫色缓冲区 + 扫描所有栈根 + 全量 Trial Deletion + 释放
    // ... (实现细节略)

    // 回收后仍超限 → 通知 VM 熔断
    if (global->total_allocated >= global->gc_threshold_hard)
        global->on_pressure(global, 2);  // 通知 VM 抛出 OOM
}
```

#### 8.5.3 Safepoint 挂起协议

```c
// Mutator 线程进入 Safepoint 并挂起
void gc_enter_safepoint_and_park(GcGlobalContext *global, GcThreadContext *thread)
{
    // 1. 标记本线程已到达 Safepoint（原子递减到达计数器）
    ATOMIC_DEC(global->safepoint_arrive_count);

    // 2. 保存当前栈帧和寄存器中的 GC 根引用（为后续 STW 扫描做准备）
    //    这一步是延迟 RC 语义的要求 — STW 期间需要知道所有对象引用
    gc_save_stack_roots(thread);

    // 3. 在此处自旋/睡眠，等待全局状态恢复为 GC_STATE_RUNNING
    while (global->gc_state != GC_STATE_RUNNING) {
        gc_spin_or_sleep(global);
    }

    // 4. 被唤醒 — STW 回收完成，恢复正常执行
}
```

#### 8.5.4 单线程 vs 多线程的路径差异

| 场景 | Hard Limit 触发行为 |
|------|-------------------|
| **单线程/单协程** | 直接内联 `gc_collect_cycles`（无锁开销，O(1) 暂停） |
| **多线程（GC_MULTITHREAD）** | CAS 竞争协调员 → 通知所有线程 Safepoint → 全体 Park → 协调员执行全量 STW → 唤醒全员 → 超限则 OOM |
| **后台 GC 已在进行中** | Mutator 线程直接 Park 等待后台 GC 完成（不重复发起 STW） |

#### 8.5.5 工业级对比

| 机制 | JVM G1/ZGC | Go Runtime | 本设计 |
|------|-----------|-----------|--------|
| 触顶行为 | Allocation Stall（分配阻断挂起） | Mark Assist（用户线程"纳税"帮助标记） | STW + CAS 协调员全量回收 |
| 后台 GC 存在时 | Mutator 挂起等待 | Mutator 变标记工人 | Mutator Park 等待 |
| 最终防线 | 同步 Full GC → OOM | GC Pacer 强制限速 → OOM | STW Fallback → OOM |

**Mark Assist 已在 8.2 节的多线程 SPI 中固化**——当 `total_allocated >= gc_threshold_soft` 时，紫色缓冲区满的线程被强制从全局队列抢嫌疑人并现场执行扫描工作。高频分配线程天然被物理限速。

### 8.6 Safepoint 实现策略：为什么本底座选择协作式轮询而非信号抢占

工业级 VM（如 Go 1.14+、JVM ZGC）在 Safepoint 实现上分为两派：**协作式**（线程主动在安全点检查标志位并挂起）和**抢占式**（OS 信号如 SIGUSR1 强行中断线程）。

**本设计明确选择协作式轮询。** 原因不在实现复杂度——而在延迟 RC 的数学正确性。

#### 8.6.1 信号抢占与延迟 RC 的不可调和的冲突

延迟 RC 的核心前提是"栈/寄存器在正常执行期间不增减 RC，只在 collection 时临时扫描"。这个前提要求 Safepoint 处的栈必须是**完全确定、完全清洁的字节码边界**。

信号抢占式通过 OS 信号在**任意机器指令**处中断线程。此时：
- 底层 C 栈可能处于 `realloc` 动态扩容的中间状态
- 寄存器中可能持有刚载入、尚未落盘的对象指针
- 编译器优化可能将对象引用暂存在被调用者保存寄存器（callee-saved register）中，信号处理函数无法可靠枚举

在这些条件下，延迟 RC 的栈根扫描**从根本上不可信**——`on_scan_roots` 可能在 GC 认为"没有栈引用"时漏掉实际存在于寄存器中的指针，导致活对象被误释放。

#### 8.6.2 协作式 Loop Hook 轮询的实现

利用解释器架构的天然优势——字节码边界是确定的。仅在**分配路径**和**循环回跳指令**（JUMP_BACKWARD / LOOP）处插入极轻量的全局标志检查：

```c
// 宏: 嵌入字节码解释器核心循环
#define GC_SAFEPOINT_POLL(global, thread)                               \
    do {                                                                 \
        if (unlikely((global)->gc_state == GC_STATE_STW_REQUESTED))     \
            gc_enter_safepoint_and_park((global), (thread));             \
    } while (0)

// 解释器放置点:
//   1. gc_alloc 入口（自然分配点）
//   2. OP_JUMP_BACKWARD 处理器中（死循环的必经之路）
//   3. OP_CALL / OP_RETURN 处理器中（函数调用边界）
```

- **消除死循环卡死**：任何导致 GC 阻塞的"纯计算死循环"在字节码层面必然包含 `JUMP_BACKWARD`。在该指令处检查一个全局整型变量，`unlikely` 宏使分支预测器将"不挂起"路径设为默认，CPU 开销 < 0.5 时钟周期。
- **栈根完整**：挂起始终发生在字节码边界——操作数栈、寄存器数组、帧链全部清洁完整。`on_scan_roots` 可以可靠地枚举所有延迟 RC 的栈引用。
- **吞吐量损耗 < 1%**：现代 CPU 的分支预测器使得 `unlikely` 路径的实际执行几乎为零开销。

#### 8.6.3 工业级对比

| 特性 | 协作式轮询（本设计） | 信号抢占（Go 1.14+） | 全局轮询（Go < 1.14） |
|------|-------------------|---------------------|---------------------|
| 死循环响应 | ✅ 回跳指令捕获 | ✅ 信号中断 | ❌ 可能卡死 |
| 实现复杂度 | **极低** (~20 行宏) | 极高 (~2000 行汇编+信号) | 低 |
| 延迟 RC 兼容性 | ✅ 字节码边界=栈清洁 | ❌ 寄存器级不确定 | ✅ |
| 线程暂停延迟 | < 1µs（等当前字节码结束） | < 10µs（信号投递延迟） | < 1µs |
| 可移植性 | ✅ 纯 C，无 OS 依赖 | ❌ 依赖 POSIX 信号/ucontext | ✅ |

---




## 附录 A: 算法参考

| 算法 | 来源 | 用途 |
|------|------|------|
| Trial Deletion (引用减法) | CPython 3.12 `Modules/gcmodule.c` | 循环检测核心 |
| Bacon-Rajan 紫色缓冲区 | Bacon & Rajan, "Concurrent Cycle Collection in RC Systems", 2001 | 嫌疑人追踪理论 |
| TLAB 批量冲刷 | HotSpot JVM `threadLocalAllocBuffer` | 减少全局链表 CAS 竞争 |
| 自适应缓冲区 | LuaJIT GC, Go GC pacer | 收集频率自调优 |
| Deferred RC (栈不计数) | Swift/ObjC ARC 最佳实践 | 消除热路径 RC 操作 |

## 附录 B: trial deletion 的数学正确性概要

对嫌疑集 S，定义：
- `E(i)`: 从 S 外部指向对象 i 的引用数（外部引用）
- `I(i)`: 从 S 内部指向对象 i 的引用数（内部引用）
- `RC(i)` = `E(i) + I(i)`：对象 i 的实际 refcount

Trial Deletion 执行：
1. `gc_ref(i) = RC(i)`：复制
2. 对每个对象 i，对其在 S 内的每个子对象 j 执行 `gc_ref(j)--`：减去 I(j)
3. 结果 `gc_ref(i) = RC(i) - I(i) = E(i) + I(i) - I(i) = E(i)`

`gc_ref(i) > 0` → `E(i) > 0` → 有 S 外部引用 → 存活。  
`gc_ref(i) = 0` → `E(i) = 0` → 无外部引用 → 垃圾。  
`gc_ref(i) < 0` → `E(i) < I(i) - RC(i)` 的可能原因：S 内重复引用导致 subtract 过多次 → 同样无足够外部引用 → 垃圾。

---

## 附录 C: 与 CPython / JVM / Lua 的对比

### C.1 整体架构对比

| 维度 | 本设计 | CPython 3.12 | HotSpot JVM (ZGC/G1) | Lua 5.4 |
|------|--------|-------------|---------------------|---------|
| **主要回收机制** | RC（堆引用）+ 延迟 RC（栈引用） | RC（全量） | 纯追踪（无 RC） | 纯追踪（增量标记） |
| **循环检测** | Bacon-Rajan Trial Deletion | Trial Deletion（CPython 变体） | 不需要（全追踪天然处理） | 不需要 |
| **并发/并行** | `GC_MULTITHREAD` 宏可选开启 | 单线程（GIL 保护） | 完全并发（ZGC: <1ms 暂停） | 单线程（增量步进） |
| **分代** | 可选（Phase 3） | 三代（gen0/1/2） | 分代（G1 Young/Old）+ 分区（ZGC region） | 无（增量标记替代） |
| **写屏障** | `gc_assign_field`（原子交换） | 无（所有操作走 RC） | SATB（G1）/ 指针染色（ZGC） | 写屏障（`luaC_barrier_`） |
| **对象头大小** | 24B（rc+gc_ref+color+flag+type+next） | 16B（ob_refcnt+ob_type） | 8-16B（Mark Word + Klass ptr） | 8B（GCObject 基础，扩展后 ~16-24B） |

### C.2 关键设计选择对比

**为什么不用 CPython 的纯 RC + 分代 GC？**

CPython 对**所有引用**（含栈引用）维护 RC。一个 `a = b` 赋值触发 `b->ob_refcnt++`。这意味着：
- 热路径每条变量赋值都做原子操作（CPython 的 GIL 使"原子"退化为普通操作，这才是它不慢的真正原因）
- GC 的 trial deletion 从 `gc.generations[0]` 链表遍历，但 CPython 有 GIL 保护——在多线程无锁场景下，遍历全局链表需要快照或暂停，开销更大

本设计选择了**延迟 RC（栈不计数）+ 单次 collection 时扫描栈补偿**。栈扫描只在 GC 触发时支付一次 O(栈深) 的代价，热路径完全不付。这在无 GIL 的多线程场景下尤为重要——否则每次栈 push/pop 都要原子操作。

**为什么不用 JVM 的纯追踪（无 RC）？**

JVM/ZGC 的纯追踪 GC 不依赖引用计数。优势是无需维护 RC，不需处理环。代价是：
- 必须扫描**所有**根（全局变量 + 所有线程栈 + 所有活跃寄存器）——每次 GC 都需要完整快照
- 需要**读屏障/写屏障**来维护并发标记不变式（SATB 或指针染色）
- 完整实现（含并发、压缩、分代）代码量 5 万行+

本设计的核心优势：**RC 负责 95% 的回收（即时释放，无需等 GC），GC 只处理 5% 的环（局部扫描，不看全局）**。代码量可以控制在 2000 行 C。对于中小型 VM 来说，投入产出比极高。

**为什么不用 Lua 5.4 的增量三色标记？**

Lua 的纯追踪 GC 使用增量标记（`luaC_step`），把 GC 工作分散到多个小步骤，每次执行少量标记工作后返回给 mutator。优势是避免长时间暂停。代价是：
- 需要写屏障（`luaC_barrier_`）来防止增量标记期间 mutator 修改黑白引用关系
- 需要处理"复活"（标记期间新创建的对象）
- 屏障开销在全追踪模式下无法避免

本设计的 Trial Deletion **只扫描紫色缓冲区内的对象**，不扫描全局。紫色缓冲区通常只有几十到几百个对象——即使 stop-the-world，暂停时间也比遍历全部活跃对象（可能是几十万个）短得多。对于绝大多数请求场景，**purple buffer 收集的暂停 < 0.1ms**。

### C.3 优势与取舍

| 特性 | 本设计优势 | 本设计取舍 |
|------|----------|----------|
| 即时释放 | ✅ RC 即时释放，延迟低 | — |
| 循环回收 | ✅ 局部扫描，低成本 | ⚠️ 紫色缓冲区满前，环对象暂时不被回收 |
| 栈操作性能 | ✅ 延迟 RC：push/pop 零开销 | ⚠️ collection 时额外扫描一次栈 |
| 多线程扩展 | ✅ 编译时原子 opt-out + TLAB | ⚠️ 原子 RC 在多线程高竞争下仍有开销 |
| 实现复杂度 | ✅ 核心 ~2000 行 C | ⚠️ 比纯 RC（~500 行）复杂 |
| 内存碎片 | ✅ Ring Buffer 复用临时小对象 | ⚠️ 不处理大对象碎片（需要额外 compact） |
| 实时性 | ✅ 双水位 + 自适应缓冲区 | ⚠️ Hard Limit 下会 STW（极端场景） |

### C.4 演进路径：何时需要切换到全追踪

本设计的核心假设是：**绝大多数对象通过 RC 快路径释放，环是少数**。如果场景发生根本变化（如语言特性使得 50%+ 的对象参与环，或图数据库/神经网络等极端环密集场景），Trial Deletion 的紫色缓冲区会频繁溢出，每次扫描范围变大，逐步退化为全量扫描。

信号（需要切换到全追踪）：
1. `purple_capacity` 自适应到上限（4096）仍频繁溢出
2. 每次 `gc_collect_cycles` 的暂停时间 > 10ms
3. 紫色缓冲区中"存活率"（误判率）持续 > 80%

切换到全追踪的路线：
- 保留延迟 RC（栈操作性能优势不变）
- 将 Trial Deletion 替换为并发标记扫描（参考 ZGC 的染色指针或 G1 的 SATB）
- 从 RC 收集对象分配统计信息，作为分代决策的输入

此时本设计的 SPI 接口（`on_trace_children`、`on_scan_roots`）不变——切换的是内部收集算法实现，不需要修改 VM 对接代码。

---

*文档版本: v1.1 — 2026-05-28*
