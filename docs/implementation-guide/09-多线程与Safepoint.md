# 09 — 多线程与 Safepoint 实现

> 多线程 GC 的核心难点不是算法，而是**mutator 与 collector 如何安全协调**。本文件给出 xgc 的多线程实现细节。

---

## 1. 线程模型

### 1.1 线程注册与生命周期

```c
// VM 线程必须通过 attach/detach 注册到 GcRuntime
gc_thread_context* gc_thread_attach(gc_runtime* rt, void* vm_thread_ctx);
void               gc_thread_detach(gc_thread_context* thread);
```

**GcRuntime 中的线程列表**（需新增）：

```c
struct gc_runtime {
    ...
    gc_thread_context** threads;     // 已注册线程数组
    size_t              thread_count;
    size_t              thread_capacity;
    pthread_mutex_t     thread_list_lock;  // 保护线程列表
};
```

### 1.2 线程本地状态

```c
struct gc_thread_context {
    gc_runtime*     runtime;
    void*           vm_thread_ctx;
    void*           algo_state;         // 算法私有 per-thread 状态
    // ── 多线程新增 ──
    uint8_t*        tlab_base;          // Thread-Local Allocation Buffer
    size_t          tlab_used;
    size_t          tlab_capacity;
    int             at_safepoint;       // 是否在 safepoint 挂起
    gc_store_buffer store_buf;          // 延迟写屏障 buffer（可选）
};
```

### 1.3 协程 / 绿色线程的根扫描

> 这对应部署场景"**单线程多协程**"和"多线程 + 每线程多协程"。协程（coroutine / fiber / 绿色线程）有**自己的栈**，挂起时其栈上的局部变量仍持有活引用。GC 必须把**每个挂起协程的栈**也作为根来源枚举，否则协程恢复后会访问到被回收的对象。

**核心难点**：挂起的协程不在任何 OS 线程的当前栈上，`scan_roots` 默认只扫到"正在运行的那个协程"的栈。必须让 VM 显式登记所有协程。

```c
// 协程在 GC 看来是一个"根来源"，由 VM 通过 hook 提供枚举
typedef struct gc_coroutine {
    void*       stack_lo;       // 栈内存区间 [lo, hi)
    void*       stack_hi;
    void*       saved_sp;       // 挂起时保存的栈指针（仅扫 [saved_sp, stack_hi)）
    void*       vm_coro_ctx;    // VM 的协程对象，供精确栈图回调使用
    int         state;          // RUNNING / SUSPENDED / DEAD
} gc_coroutine;
```

**扫描时机与规则**：

```c
void gc_scan_all_coroutine_roots(gc_runtime* rt, gc_visit_slot_fn visit, void* ctx) {
    for (each registered coroutine co) {
        if (co->state == DEAD) continue;
        if (co->state == RUNNING) continue;   // 正在跑的那个由普通线程栈扫描覆盖
        // 挂起的协程：只扫已使用的栈区间 [saved_sp, stack_hi)
        vm_scan_coroutine_stack(co->vm_coro_ctx, co->saved_sp, co->stack_hi, visit, ctx);
    }
}
```

要点：
- **只扫 `[saved_sp, stack_hi)`**：协程栈上 `saved_sp` 以下是已弹出的垃圾帧，扫了会引入虚假根（保守式）或越界（精确式）。
- **精确 vs 保守**：与普通栈一致（见文档 11/19）。精确式要求 VM 为协程的每个挂起点提供栈图；做不到精确时，协程栈按保守扫描——**但保守扫描的对象不能被 moving GC 移动**（无法安全更新栈内疑似指针），必须 pin。这是"单线程多协程 + moving 算法"组合的关键约束。
- **safepoint 关系**：协程切换点（yield）天然是 GC 安全点——协程在 yield 处保存了完整状态。单线程多协程场景下，GC 只能在"当前没有协程处于半保存状态"时触发，即在调度器的协程边界，而非任意指令。把 `gc_safepoint_poll()` 放在协程调度循环里（每次 resume 前）即可，无需抢占式信号。
- **单线程多协程的简化**：无并发，无需 STW 协调；GC 触发点就是分配失败或调度边界，串行扫描所有协程栈即可。复杂度远低于真多线程。

VM 通过 hook 注册/注销协程：

```c
void gc_coroutine_register(gc_runtime* rt, gc_coroutine* co);
void gc_coroutine_unregister(gc_runtime* rt, gc_coroutine* co);
// 挂起/恢复时更新 saved_sp 与 state（VM 在 yield/resume 路径调用）
void gc_coroutine_suspend(gc_coroutine* co, void* sp);
void gc_coroutine_resume(gc_coroutine* co);
```

---

## 2. Safepoint 机制

### 2.1 协作式轮询

xgc 选择协作式 safepoint（不使用信号抢占）。原因：
- 兼容延迟 RC 的栈根语义（safepoint 处栈帧必须清洁）
- 纯 C 可移植性
- 实现复杂度低

### 2.2 轮询点插入位置

```c
// VM 解释器循环中，以下位置插入 safepoint poll：
// 1. gc_alloc_typed 入口
// 2. 循环回跳指令（JUMP_BACKWARD / LOOP）
// 3. 函数调用/返回边界（可选）

#define GC_SAFEPOINT_POLL(rt, thread) \
    do { \
        if (GC_ATOMIC_LOAD(&(rt)->gc_state) != GC_STATE_RUNNING) \
            gc_enter_safepoint_and_park(rt, thread); \
    } while (0)
```

### 2.3 STW 完整时序

```
GC 协调线程:
    1. CAS(gc_state, RUNNING → STW_REQUESTED)
    2. 等待所有线程到达 safepoint
    3. gc_state = STW_IN_PROGRESS
    4. 执行 GC 工作
    5. gc_state = RUNNING
    6. 唤醒所有挂起线程

Mutator 线程:
    1. poll 发现 gc_state != RUNNING
    2. gc_enter_safepoint_and_park:
        a. ATOMIC_DEC(safepoint_arrive_count)  // 报到
        b. 保存当前栈帧信息
        c. thread->at_safepoint = 1
        d. 自旋/睡眠等待 gc_state == RUNNING
        e. thread->at_safepoint = 0
```

### 2.4 实现文件

```
src/core/safepoint.c（新建）
├── gc_request_stw(rt)               — 发起 STW（CAS 竞争协调员）
├── gc_wait_for_stw_complete(rt)     — 等待所有线程到达
├── gc_enter_safepoint_and_park(rt, thread) — mutator 挂起
├── gc_end_stw(rt)                   — 唤醒所有线程
├── gc_safepoint_poll_inline(rt, thread) — 快速路径内联检查
```

### 2.5 避免死锁的规则

| 规则 | 说明 |
|------|------|
| 协调员不 park | 抢到 CAS 的线程负责等其他人到齐，自己**不睡** |
| 不在持有 mutex 时进入 safepoint | 否则其他线程可能等待这个 mutex 而无法到达 safepoint → 死锁 |
| alloc 中 poll 但不在 alloc 中间 park | safepoint 在 alloc 入口，不在 alloc 半途 |

---

## 3. 并发 GC 线程

### 3.1 Background GC Thread

```c
void* gc_background_thread_main(void* arg) {
    gc_runtime* rt = (gc_runtime*)arg;
    while (rt->running) {
        gc_wait_for_signal(rt);   // 条件变量等待
        if (!rt->running) break;

        // 执行并发标记或其他后台工作
        if (rt->algo->collect_concurrent)
            rt->algo->collect_concurrent(rt);
    }
    return NULL;
}
```

### 3.2 Epoch 机制（用于 RC Cycle Detection）

```
Epoch N:   收集所有线程提交的嫌疑人
Epoch N+1: 等待引用沉淀（并发安全窗口）
Epoch N+2: 异步 Trial Deletion + 释放
```

**关键**：Epoch 之间的安全窗口确保 mutator "逻辑上已抛弃但还在寄存器里倒手"的引用已完成落盘。

---

## 4. 原子操作封装

### 4.1 条件编译策略

```c
#if defined(GC_MULTITHREAD)
    // C11 _Atomic
    #define GC_ATOMIC_LOAD(p) atomic_load(p)
    ...
#else
    // 零成本退化
    #define GC_ATOMIC_LOAD(p) (*(p))
    ...
#endif
```

### 4.2 内存序选择

| 操作 | 推荐内存序 | 理由 |
|------|-----------|------|
| gc_state 读 | `memory_order_acquire` | 看到最新状态 |
| gc_state 写 | `memory_order_release` | 对其他线程可见 |
| RC inc/dec | `memory_order_relaxed` + dec 时 `acquire` | 只需计数正确，dec 到 0 时需要看到子引用 |
| CAS 竞争协调员 | `memory_order_acq_rel` | 经典 CAS 语义 |
| card table dirty bit | 非原子即可 | 多标一次脏无害 |

---

## 5. 测试

```c
// tests/test_safepoint_substrate.c
// 场景 1: 单线程 poll → 不 park（gc_state == RUNNING）
// 场景 2: 模拟 STW_REQUESTED → 线程 park → 唤醒
// 场景 3: 多线程并发 park（使用 pthread）

// tests/test_multithread_smoke.c
// 场景 1: 2 线程并发分配 → 无崩溃
// 场景 2: 1 线程分配 + 1 线程触发 GC → 正确 STW
// 场景 3: 4 线程写屏障竞争 → invariant 通过
```
