# GC 算法接口契约

## 必须实现的接口（9 个函数）

新算法在 `src/algo/<name>/` 下创建以下 4 个 `.c` 文件，实现全部 9 个函数：

### gc_alloc.c — 分配与初始化

```c
// 初始化全局上下文：设置 SPI 回调、水位线、工作栈
void gc_global_context_init(GcGlobalContext *global,
                            GcTraceFn      on_trace_children,
                            GcFinalizeFn   on_finalize,
                            GcPressureFn   on_pressure,
                            GcPurpleFullFn on_purple_buffer_full);

// 初始化线程上下文：分配紫色缓冲区、Ring Buffer、TLAB
void gc_thread_context_init(GcThreadContext *thread,
                            GcScanRootsFn   on_scan_roots,
                            void           *vm_thread_ctx);

// 分配对象。返回 GcHeader*（嵌入 VM 对象首部）
void *gc_alloc(GcGlobalContext *global, GcThreadContext *thread,
               size_t total_size, uint16_t obj_type);

// 将 TLAB 本地链表批量冲刷到全局链表
void gc_flush_tlab(GcGlobalContext *global, GcThreadContext *thread);

// 双水位线检查：Soft Limit → 轻量回收, Hard Limit → STW
void gc_check_pressure(GcGlobalContext *global, GcThreadContext *thread);
```

### gc_assign.c — 写屏障

```c
// 统一写屏障。所有堆属性赋值（dict[key]=v, list[i]=obj）必须经过此函数
void gc_assign_field(GcGlobalContext *global, GcThreadContext *thread,
                     GcHeader **field, GcHeader *new_child);
```

### gc_collect.c — 垃圾回收核心

```c
// 循环检测 / 垃圾回收主入口。
// Bacon-Rajan: Trial Deletion 四阶段
// Mark-Sweep:  Mark → Sweep
void gc_collect_cycles(GcGlobalContext *global, GcThreadContext *thread);

// 扫描栈/寄存器根，保护被栈引用的对象免于误回收。
// 延迟 RC 语义要求 collection 期间补偿栈引用。
void gc_scan_stack_and_protect(GcGlobalContext *global, GcThreadContext *thread);
```

### gc_safepoint.c — 线程同步

```c
// Mutator 线程在 Safepoint 挂起，等待 STW 回收完成。
// 单线程可退化为空实现。
void gc_enter_safepoint_and_park(GcGlobalContext *global, GcThreadContext *thread);
```

## 可用的共享工具（可选使用）

以下函数在 `src/gc_*.c` 中实现，所有算法均可直接调用：

| 函数 | 来源 | 用途 |
|------|------|------|
| `gc_worklist_push(wl, obj)` | `gc_worklist.c` | 显式工作栈压栈（防递归溢出） |
| `gc_worklist_pop(wl)` | `gc_worklist.c` | 显式工作栈弹栈 |
| `gc_reclaim_object(global, obj)` | `gc_reclaim.c` | 递归释放子节点 + 终结器 + free |
| `gc_try_ring_buffer_alloc(thread, size)` | `gc_ring.c` | Ring Buffer 快速分配 |
| `gc_reclaim_ring_buffer_slots(global, thread)` | `gc_ring.c` | Collection 后安全复用 Ring Buffer |
| `gc_push_purple_adaptive(global, thread, obj)` | `gc_purple.c` | 紫色缓冲区推入（Bacon-Rajan 用） |
| `gc_signal_background_thread(global)` | `gc_epoch.c` | 唤醒 Background GC 线程 |
| `gc_wait_for_signal(global)` | `gc_epoch.c` | Background GC 线程等待信号 |

## GcHeader 字段使用约定

| 字段 | 读写者 | 语义 |
|------|--------|------|
| `rc` | Mutator 写（原子 INC/DEC），GC 读 | 堆引用计数 |
| `gc_ref` | 仅 GC 线程 | Trial Deletion 工作计数 |
| `color` | Mutator + GC 均可写（原子） | BLACK/GRAY/WHITE/PURPLE |
| `in_purple_buf` | Mutator 写，GC 清 | 防重复加入紫色缓冲区 |
| `obj_type` | VM 写（分配时设置） | 弱类型标签 |
| `next` | GC 写 | 全局链表 / TLAB 链节点 |

## 添加新算法的步骤

```bash
# 1. 创建算法目录
mkdir -p src/algo/<name>

# 2. 实现 4 个 .c 文件（9 个函数）
#    gc_alloc.c  gc_assign.c  gc_collect.c  gc_safepoint.c

# 3. 编译
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGC_ALGORITHM=<name>
cmake --build build
```
