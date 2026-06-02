# xgc v1.2 可落地实现蓝图
> 面向当前仓库：`/home/clouder/workspace/xgc`
---
## 0. 蓝图目标
本蓝图不讨论理想最终态，只定义：
- 当前仓库下一步该改哪些文件
- 每个文件应该承担什么职责
- 先实现什么，后实现什么
- 用什么测试证明 v1.2 可用
结论先行：
> **先做单线程 / 单 OS 线程多协程的正确实现，再推进多线程 STW。**
---
## 1. 仓库现状判断
当前仓库已经具备良好骨架：
- 公共头：`include/xgc/gc.h`
- 内部头：`src/gc_internal.h`
- 算法目录：`src/algo/bacon-rajan/`
- 共享工具：`src/gc_worklist.c`、`src/gc_reclaim.c`、`src/gc_purple.c` 等
但还存在三个阻塞实现的关键问题：
1. **公开 API 与不透明 context 类型不匹配**
2. **缺少 `trace_slots`，reclaim 无法安全断边**
3. **`gc_collect_cycles()` 当前只有注释，没有可执行状态机**
---
## 2. v1.2 必须落地的设计决策
### 2.1 决策一：把框架范围收敛为 RC-family
当前 API / header 已高度绑定：
- `rc`
- `gc_ref`
- `color`
- purple buffer
所以 v1.2 不再追求“任意 GC 算法通用底座”，而是明确为：
> **RC-family 混合 GC 框架**
支持的算法形态：
- naive RC
- deferred RC
- Bacon-Rajan / Trial Deletion
- future generational RC
### 2.2 决策二：补 `GcTraceSlotsFn`
要正确 reclaim，一个对象的出边必须能按**字段槽位**遍历。
建议在公开头新增：
```c
typedef void (*GcTraceSlotsFn)(GcHeader *parent,
                               void (*visit_slot)(GcHeader **slot, void *ctx),
                               void *ctx);
```
### 2.3 决策三：对象头增加 `flags` 和 `alloc_size`
推荐对象头：
```c
typedef struct GcHeader {
    gc_atomic_rc_t    rc;
    uint32_t          gc_ref;
    gc_atomic_u8_t    color;
    gc_atomic_u8_t    flags;
    uint16_t          obj_type;
    uint32_t          alloc_size;
    struct GcHeader  *next;
} GcHeader;
```
### 2.4 决策四：`worklist_data` 改为 `GcHeader **`
当前 `src/gc_internal.h` 中工作栈元素类型应为指针数组，而不是单个 `GcHeader *` 缓冲区。
---
## 3. 文件级改造清单
## 3.1 `include/xgc/gc.h`
### 目标
把公开 API 调整到“用户真的能接入”的状态。
### 必改项
1. 明确 `GcTraceChildrenFn` 和 `GcTraceSlotsFn`
2. 修正 `GcPressureFn` 的 `vm_ctx` 来源
3. 决定 context 是公开结构体还是 create/destroy
4. 若保留 `GC_SAFEPOINT_POLL`，状态枚举要与内部定义一致
### 建议
v1.2 直接公开 `GcGlobalContext` / `GcThreadContext` 结构体定义；实现优先，ABI 封装留到后面。
---
## 3.2 `src/gc_internal.h`
### 目标
统一内部元数据布局和原子宏。
### 必改项
1. 修正 `worklist_data` 类型为 `GcHeader **`
2. 把 `in_purple_buf` 替换为 `flags`
3. 增加 `alloc_size`
4. 把多线程相关状态降级为“接口预留，不参与 v1.2 正确性路径”
### 建议新增辅助宏
```c
#define GC_FLAG_SET(h, bit)   ...
#define GC_FLAG_CLEAR(h, bit) ...
#define GC_FLAG_TEST(h, bit)  ...
```
---
## 3.3 `src/gc_worklist.c`
### 目标
成为所有深图算法的基础设施。
### 应实现
- `gc_worklist_push()`：满了就 `realloc`
- `gc_worklist_pop()`：LIFO 弹栈
- 所有算法都通过它避免 C 递归
### 验收测试
- 构造 10000 深链不栈溢出
---
## 3.4 `src/algo/bacon-rajan/gc_alloc.c`
### v1.2 职责
1. `gc_global_context_init()`
2. `gc_thread_context_init()`
3. `gc_alloc()`
4. `gc_flush_tlab()`
5. `gc_check_pressure()`
### 推荐实现顺序
#### 第一步：先别做 TLAB 优化
`gc_alloc()` 直接：
```c
obj = malloc(total_size);
obj->rc = 0;
obj->gc_ref = 0;
obj->color = GC_COLOR_BLACK;
obj->flags = 0;
obj->obj_type = obj_type;
obj->alloc_size = total_size;
obj->next = NULL;
```
> 初生对象默认放在 mutator 可见区，颜色直接用 `BLACK` 比 `WHITE` 更稳。
#### 第二步：更新内存统计
- `global->total_allocated += total_size`
- 超 `soft`：调用 `gc_check_pressure()`
- 超 `hard`：同步回收一次，再通知 `on_pressure(level=2)`
#### 第三步：TLAB 只做轻量本地链表
`local_head` / `local_alloc_count` 可先保留，但不依赖其正确性。
---
## 3.5 `src/algo/bacon-rajan/gc_assign.c`
### v1.2 职责
实现统一写屏障。
### 正确流程
```text
old = *field
*field = new_child
if (new_child) new_child->rc++
if (old) {
    old->rc--
    if (old->rc == 0)
        gc_reclaim_object(global, old)
    else
        gc_push_purple_adaptive(global, thread, old)
}
```
### 细节要求
- 避免重复把同一对象放入 purple buffer
- v1.2 默认不处理“collection 正在并发进行时的颜色竞争”
- 多线程真正共享可变堆留到 v2
---
## 3.6 `src/gc_purple.c`
### v1.2 职责
- `gc_push_purple_adaptive()`
- 满时调用 `global->on_purple_buffer_full`
- collection 后按出货率调整容量
### 建议规则
- 初始 256
- 下限 64
- 上限 4096
- 若回收率 > 50%，容量减半
- 否则容量翻倍
---
## 3.7 `src/algo/bacon-rajan/gc_collect.c`
这是 v1.2 的核心文件。
### 应拆成的内部辅助函数
1. `gc_discover_gray_subgraph()`
2. `gc_subtract_gray_edges()`
3. `gc_scan_black_iterative()`
4. `gc_partition_gray_objects()`
5. `gc_collect_white_iterative()`
6. `gc_collect_cycles()`
7. `gc_scan_stack_and_protect()`
### 推荐状态机
#### Phase A：发现灰子图
从 `thread->purple_buffer` 出发：
- 首次见到对象：
  - `gc_ref = rc`
  - `color = GRAY`
  - 放入 `gray_vec`
- 用 `trace_children` 继续向下发现
#### Phase B：扣减灰边
对 `gray_vec` 中每个对象：
- 遍历 children
- 若 child 也是 `GRAY`，则 `child->gc_ref--`
#### Phase C：扫描根并救回
调用 `thread->on_scan_roots()`：
- 根若是 `GRAY`，执行 `gc_scan_black_iterative(root)`
- `scan_black` 把整条可达灰链重新染为 `BLACK`
- 恢复相应 `gc_ref`
#### Phase D：分离并回收
- `BLACK`：保留
- `GRAY && gc_ref > 0`：转 `BLACK`
- `GRAY && gc_ref == 0`：转 `WHITE`
- `WHITE`：交给非递归 reclaim
### 最重要的工程约束
> **每个节点的 children 只能在“首次进入灰子图”时展开一次。**
否则会出现重复扣减内部边的问题。
---
## 3.8 `src/gc_reclaim.c`
### v1.2 职责
把递归释放改成**非递归断边 + 非递归释放**。
### 推荐流程
```text
queue <- [obj]
while queue not empty:
    cur = pop(queue)
    if cur already reclaiming: continue
    mark reclaiming
    trace_slots(cur):
        old = *slot
        *slot = NULL
        if old:
            old->rc--
            if old->rc == 0:
                push old into queue
    finalize(cur)
    total_allocated -= cur->alloc_size
    free(cur)
```
### 注意
- `trace_slots` 必须在 `finalize` 前完成
- `finalize` 只负责对象外部资源，不再负责断 GC 引用边
---
## 3.9 `src/gc_ring.c`
### v1.2 结论
先不要把它纳入正确性路径。
### 推荐实现
```c
void *gc_try_ring_buffer_alloc(...) {
    return NULL;
}
void gc_reclaim_ring_buffer_slots(...) {
    // no-op in v1.2
}
```
后续如果需要优化，再单独演进成 tiny object arena。
---
## 3.10 `src/algo/bacon-rajan/gc_safepoint.c`
### v1.2 职责
- 单线程：空实现
- 预留多线程 STW 钩子
```c
void gc_enter_safepoint_and_park(...) {
    (void)global;
    (void)thread;
}
```
---
## 3.11 `src/gc_epoch.c`
### v1.2 结论
不进入主路径。
保留空壳或最小实现即可，但不要让主算法依赖它。
---
## 4. 建议的最小测试矩阵
## 4.1 单元测试对象模型
建议在 `tests/` 下建立一个最小 VM mock：
```c
typedef struct TestNode {
    GcHeader gc;
    struct TestNode *left;
    struct TestNode *right;
} TestNode;
```
并为它实现：
- `test_trace_children()`
- `test_trace_slots()`
- `test_scan_roots()`
- `test_finalize()`
## 4.2 必测用例
### 用例 1：线性释放
`A -> B -> C`，逐步清空字段后应立即释放。
### 用例 2：自环
`A -> A`，外部引用断开后不能靠 RC 立即释放，但 `gc_collect_cycles()` 后必须释放。
### 用例 3：双环
`A <-> B`，断开外部引用后应在一轮 collection 后释放。
### 用例 4：深链
10000 深度链表，调用 collection 不得栈溢出。
### 用例 5：挂起协程根
模拟两个 coroutine：
- 当前 coroutine 无引用
- 挂起 coroutine 栈上保留 `A`
collection 后 `A` 必须存活。
### 用例 6：重复 child
`A.left = B; A.right = B; B.left = A`  
验证内部边扣减不会因重复展开导致错误释放。
---
## 5. 推荐开发顺序（两周级别）
### 第 1 天：接口收口
- 修 `gc.h`
- 修 `gc_internal.h`
- 同步 `INTERFACE.md`
### 第 2-3 天：基础设施
- `gc_worklist.c`
- `gc_purple.c`
- `gc_alloc.c`
### 第 4-6 天：写屏障 + reclaim
- `gc_assign.c`
- `gc_reclaim.c`
### 第 7-10 天：collection 核心
- `gc_collect.c`
- 自环 / 双环 / 深链测试
### 第 11-12 天：多协程根扫描测试
- mock scheduler
- 挂起协程根保护
### 第 13-14 天：收尾
- 文档同步
- 边界条件修复
- 打通 `GC_MULTITHREAD=ON` 编译
---
## 6. v2 以后再做什么
当 v1.2 稳定后，再考虑：
1. 多线程 STW safepoint
2. tiny object arena
3. generational RC
4. 后台线程辅助但非并发解环
5. 真正的 concurrent cycle collection
---
## 7. 蓝图结论
对当前仓库，最合理的落地策略是：
1. **先把 API 修成可接入的样子**
2. **先把单线程 / 单 OS 线程多协程路径做对**
3. **先把 reclaim 改成非递归**
4. **先让 Trial Deletion 有可验证状态机**
5. **多线程共享堆只保留接口，不在 v1.2 强行完成并发解环**
这样才能让 `xgc` 从“有理念的框架”变成“能工作的 GC 内核”。
