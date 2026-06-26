# 21 — JIT/逃逸分析协作契约

> **本文件定义 xgc 为支持未来 JIT 编译器和逃逸分析而必须预留的接口契约。**
>
> 重要边界:xgc **不实现** JIT,也**不实现**逃逸分析 —— 那是 VM/编译器的职责。但 xgc 必须**现在就把接口设计对**,否则未来接 JIT 时会被迫推翻 `gc_header` 布局、safepoint 模型、根扫描 SPI(违反文档 16 的 ABI 冻结)。本文件是"防止把自己堵死"的前瞻设计指南。

---

## 1. 为什么这关乎"超高速"

工业级 VM(V8/HotSpot/.NET)的极致性能,很大比例来自 JIT 编译代码中的 GC 协作:

| 优化 | 收益 | 对 GC 框架的要求 |
|------|------|----------------|
| 写屏障消除 | 编译期证明无需屏障的写入直接跳过 | 屏障语义可被静态分析、快路径可内联 |
| 栈图精确根 | 编译代码在 safepoint 提供精确根位图 | 根扫描 SPI 支持"位置→根集"查询 |
| safepoint 内联 | 编译代码轮询全局标志,非函数调用 | safepoint 状态是可被内联读取的全局/TLS |
| 标量替换/栈分配 | 不逃逸对象不进堆,零 GC 压力 | GC 不假设所有对象都在堆上 |
| derived pointer | 编译器生成指向对象内部的指针 | GC 能识别并更新内部指针(moving 时) |

如果接口设计错了,这些优化未来都做不了,框架就达不到"超高速"上限。

---

## 2. 必须预留的接口契约

### 2.1 契约 A:根扫描必须支持"按位置"的精确栈图

**解释器**:`scan_roots` 遍历整个值栈,运行时判断每个 slot 是否对象。

**JIT 编译代码**:编译器在每个 safepoint 知道**哪些寄存器/栈槽此刻持有对象指针**(stack map / GC map)。它不需要 GC 遍历,而是直接报告精确根集。

**xgc 必须预留**:`gc_scan_roots_fn` 的契约允许 VM 提供"精确根流",而不强制 GC 自己判断。当前签名已满足:

```c
typedef void (*gc_scan_roots_fn)(void* vm_ctx, gc_visit_slot_fn visit, void* ctx);
```

VM(无论解释器还是 JIT)负责枚举,GC 只接收 `gc_header**`。**这个设计已经正确,不要改。** JIT 版的 `scan_roots` 内部会:

```c
/* JIT 版 scan_roots（VM 侧未来实现，此处仅说明契约） */
void jit_scan_roots(void* vm_ctx, gc_visit_slot_fn visit, void* ctx) {
    for (each compiled frame on stack) {
        gc_stack_map* map = lookup_stack_map(frame->return_address);  /* 编译期生成 */
        for (each live slot in map) {
            visit((gc_header**)(frame->sp + slot->offset), ctx);
        }
    }
}
```

> **xgc 端零改动** —— 只要 `scan_roots` 始终传 `gc_header**`(slot 地址而非对象),JIT 就能接入。这验证了文档 6 §6.2 的设计决策是正确的。

### 2.2 契约 B:safepoint 状态必须可被编译代码廉价轮询

**解释器**:`GC_SAFEPOINT_POLL` 宏在字节码循环检查 `gc_state`。

**JIT 编译代码**:不能调用函数检查 —— 太慢。需要内联读取一个内存位置(或线程本地标志),分支预测为"不暂停"。

**xgc 必须预留**:safepoint 标志是一个**地址稳定、可被外部读取**的字段:

```c
/* gc_runtime 中,safepoint 标志的地址必须对 JIT 暴露 */
gc_atomic_int_t* gc_safepoint_flag_address(gc_runtime* rt);
/* JIT 在编译代码中内联生成:
 *   if (*safepoint_flag != RUNNING) call gc_safepoint_slow();
 * 这是一条 load + 条件分支,~1 个周期（分支预测命中时） */
```

> 更高级:per-thread safepoint(每线程一个标志页,STW 时 mprotect 该页触发 SIGSEGV)。但这是 P3 优化,当前只需保证标志地址可取。

### 2.3 契约 C:写屏障快路径必须可内联且可消除

**解释器**:每次 `gc_store_ref` 调函数,走 vtable 间接调用。

**JIT**:需要把写屏障的**快路径**内联进编译代码,并在能静态证明时**完全消除**屏障。

**xgc 必须预留**:写屏障语义必须满足"快路径可表达为几条内联指令 + 慢路径调函数":

```c
/* 分代写屏障的可内联形式（JIT 未来内联生成这段，而非调 gc_store_ref） */
static inline void gc_write_barrier_fast(gc_runtime* rt, gc_header* owner,
                                         gc_header** slot, gc_header* new_value) {
    *slot = new_value;                          /* 实际写入 */
    /* 快路径 reject:owner 在 young 或 new 不在 young → 无需记录 */
    if (GC_LIKELY(!gc_needs_barrier(rt, owner, new_value)))
        return;
    gc_write_barrier_slow(rt, owner, slot, new_value);  /* 慢路径调函数 */
}
```

**屏障消除的契约**:JIT 可以在以下情况安全跳过屏障(GC 必须保证这些情况下跳过是正确的):
- 写入的是新分配对象(还在 nursery,不可能是 old→young)
- 写入 NULL(清空字段,无新引用)
- owner 和 new_value 编译期可证明同代

> **xgc 端要求**:写屏障的"是否需要"判断(`gc_needs_barrier`)必须是**纯函数式、无副作用**的,这样 JIT 才能静态推导。当前 gen_copy_ms 的屏障(`owner 不在 young && new 在 young`)满足此条件。

### 2.4 契约 D:GC 不假设所有对象都在堆上(逃逸分析前提)

逃逸分析让 VM 把"不逃逸"的对象分配在**栈上**而非 GC 堆。这要求:

- GC 的 `trace_slots` 遇到指向栈对象的引用时**不能崩溃、不能尝试回收**
- 栈对象的 `gc_header` 可以存在,但不在 GC 的全局对象链表里

**xgc 必须预留**:提供"这个指针是否是 GC 管理的堆对象"的判断:

```c
int gc_is_heap_object(gc_runtime* rt, const gc_header* obj);
/* trace/mark 时:if (!gc_is_heap_object(rt, child)) skip;  // 栈对象,不管理 */
```

> 这也用于保守扫描(文档未来)和外部 C 对象的安全处理。**当前实现可以先 stub 为"总是返回 1"**,但接口要留好。

### 2.5 契约 E:derived pointer / interior pointer 处理

JIT 可能生成指向**对象内部**的指针(如指向数组第 N 个元素,而非对象头)。moving GC 移动对象时必须同时更新这些 derived pointer。

**xgc 必须预留**:descriptor 或根扫描能区分"base pointer"和"derived pointer + 偏移":

```c
/* 根扫描时,derived pointer 报告为 (base_slot, derived_slot) 对 */
typedef void (*gc_visit_derived_fn)(gc_header** base_slot,    /* 指向对象头 */
                                    void**      derived_slot, /* 指向内部 */
                                    void* ctx);
/* moving 时:先更新 base,再用 (new_base - old_base) 修正 derived */
```

> **当前不需要实现** —— 解释器不产生 derived pointer。但 moving 算法(gen_copy/compact)的根扫描接口设计时要意识到这个未来需求,不要把"根一定指向对象头"硬编码进算法。

---

## 3. 逃逸分析:xgc 的角色(几乎为零)

逃逸分析完全是 **VM/编译器职责**。xgc 只需做到:

| 逃逸分析层面 | xgc 的责任 |
|-------------|-----------|
| 判断对象是否逃逸 | 无(编译器分析) |
| 栈分配不逃逸对象 | 无(VM 在栈上构造) |
| 标量替换(对象拆成寄存器) | 无(VM 优化) |
| 不逃逸对象不进 GC 堆 | **仅需:GC 不假设对象都在堆(契约 D)** |

**结论**:逃逸分析对 xgc 的唯一要求就是契约 D。其余全在 VM 侧。

---

## 4. 当前应该做什么 vs 推迟

| 契约 | 当前(解释器阶段) | 接 JIT 时 |
|------|------------------|----------|
| A 精确栈图 | ✅ 已满足(scan_roots 传 slot) | VM 实现 JIT 版 scan_roots |
| B safepoint 标志地址 | 🔨 加 `gc_safepoint_flag_address()` 接口 | JIT 内联轮询 |
| C 屏障可内联 | 🔨 把 `gc_needs_barrier` 抽成纯函数 | JIT 内联快路径 + 消除 |
| D 非堆对象判断 | 🔨 加 `gc_is_heap_object()` 接口(可先 stub) | 逃逸分析栈对象 |
| E derived pointer | ⏸ 推迟,但 moving 算法根扫描别硬编码"根=对象头" | moving + JIT 时实现 |

**现在要做的(都是小接口,防止 ABI 锁死)**:
1. `gc_safepoint_flag_address(rt)` —— 返回标志地址
2. `gc_is_heap_object(rt, obj)` —— 当前 stub 返回 1
3. 把分代写屏障的"是否需要"判断抽成独立纯函数 `gc_needs_barrier(rt, owner, new_value)`

这三个加起来 < 30 行,但保证未来接 JIT 不用改 ABI。

---

## 5. 验收

```c
/* tests/test_jit_contract_substrate.c */
int main(void) {
    gc_runtime* rt = setup();
    /* 契约 B:标志地址稳定 */
    gc_atomic_int_t* f1 = gc_safepoint_flag_address(rt);
    gc_atomic_int_t* f2 = gc_safepoint_flag_address(rt);
    assert(f1 == f2 && f1 != NULL);

    /* 契约 D:堆对象判断 */
    gc_header* obj = gc_alloc_typed(rt, NULL, &desc, sizeof(gc_header), 0);
    assert(gc_is_heap_object(rt, obj) == 1);
    gc_header stack_obj = { .desc = &desc, .size = sizeof(gc_header) };
    /* 栈对象当前 stub 也返回 1,真实实现后应返回 0 */

    /* 契约 C:纯函数式屏障判断（无副作用,可重复调用结果一致） */
    int r1 = gc_needs_barrier(rt, obj, obj);
    int r2 = gc_needs_barrier(rt, obj, obj);
    assert(r1 == r2);

    teardown(rt);
    return EXIT_SUCCESS;
}
```

---

## 6. 一句话总结

> **xgc 不实现 JIT 和逃逸分析,但必须把 5 个接口契约设计对,使未来的 JIT 能内联 safepoint、内联/消除写屏障、提供精确栈图、处理栈对象和 derived pointer —— 现在只需加 3 个小接口(< 30 行)就能锁定正确的 ABI,避免未来推翻重来。**
