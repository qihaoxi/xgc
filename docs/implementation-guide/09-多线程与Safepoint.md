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
