#ifndef XGC_GC_H
#define XGC_GC_H

/* ============================================================================
 * xgc — 通用高性能混合 GC 库
 *
 * 用户只需 #include "xgc/gc.h" 即可使用全部公开 API。
 *
 * 使用步骤:
 *   1. 实现 SPI 回调: on_trace_children / on_scan_roots / on_finalize
 *   2. 初始化 GcGlobalContext + GcThreadContext
 *   3. 通过 gc_alloc 分配对象，通过 gc_assign_field 修改堆引用
 *   4. 栈/寄存器引用不增减 RC（延迟计数语义）
 *   5. 触发 gc_collect_cycles 回收循环垃圾
 * ============================================================================
 */

#include "xgc/gc_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 前向声明（完整类型定义在 src/gc_internal.h，用户不可见） ── */

typedef struct GcHeader       GcHeader;
typedef struct GcGlobalContext GcGlobalContext;
typedef struct GcThreadContext GcThreadContext;

/* ── GC 颜色状态 ── */

typedef enum {
    GC_COLOR_BLACK  = 0,  /* 存活（外部引用存在）                          */
    GC_COLOR_GRAY   = 1,  /* 正在进行拓扑减分扫描（trial deletion 内部状态） */
    GC_COLOR_WHITE  = 2,  /* 确认为垃圾（仅环内引用，无外部引用）            */
    GC_COLOR_PURPLE = 3   /* 循环引用嫌疑（RC 减少但未归零）                 */
} GcColor;

/* ── SPI 回调类型 ── */

/* 遍历子节点: GC 核心需要知道 parent 下挂载了哪些 child */
typedef void (*GcTraceFn)(GcHeader *parent,
                          void (*visit)(GcHeader *child, void *ctx),
                          void *ctx);

/* 扫描根节点: GC 核心需要 VM 提供当前活跃栈/寄存器中的所有对象引用 */
typedef void (*GcScanRootsFn)(void *vm_thread_ctx,
                              void (*mark_alive)(GcHeader *root, void *ctx),
                              void *ctx);

/* 终结器: 对象被 GC 释放时通知 VM 清理内部资源 */
typedef void (*GcFinalizeFn)(GcHeader *obj);

/* 内存压力通知 */
typedef void (*GcPressureFn)(void *vm_ctx, int level);

/* 紫色缓冲区满触发 */
typedef void (*GcPurpleFullFn)(GcGlobalContext *global, GcThreadContext *thread);

/* ── 公开 API ── */

/* 初始化全局上下文 */
void gc_global_context_init(GcGlobalContext *global,
                            GcTraceFn         on_trace_children,
                            GcFinalizeFn       on_finalize,
                            GcPressureFn       on_pressure,
                            GcPurpleFullFn     on_purple_buffer_full);

/* 初始化线程上下文 */
void gc_thread_context_init(GcThreadContext *thread,
                            GcScanRootsFn     on_scan_roots,
                            void             *vm_thread_ctx);

/* 通用分配接口 */
void *gc_alloc(GcGlobalContext *global, GcThreadContext *thread,
               size_t total_size, uint16_t obj_type);

/* 统一写屏障 — 所有堆属性赋值必须经过此函数 */
void gc_assign_field(GcGlobalContext *global, GcThreadContext *thread,
                     GcHeader **field, GcHeader *new_child);

/* 循环检测 — Trial Deletion 核心 */
void gc_collect_cycles(GcGlobalContext *global, GcThreadContext *thread);

/* 栈根扫描并保护 — collection 期间补偿延迟 RC */
void gc_scan_stack_and_protect(GcGlobalContext *global, GcThreadContext *thread);

/* TLAB 批量冲刷到全局链表 */
void gc_flush_tlab(GcGlobalContext *global, GcThreadContext *thread);

/* 水位线检查 + 背压控制 */
void gc_check_pressure(GcGlobalContext *global, GcThreadContext *thread);

/* 自适应紫色缓冲区推入 */
void gc_push_purple_adaptive(GcGlobalContext *global, GcThreadContext *thread,
                             GcHeader *obj);

/* Safepoint 协作式轮询宏（嵌入字节码解释器循环） */
#define GC_SAFEPOINT_POLL(global, thread)                                      \
    do {                                                                       \
        if ((global)->gc_state == 2 /* GC_STATE_STW_REQUESTED */)             \
            gc_enter_safepoint_and_park((global), (thread));                   \
    } while (0)

void gc_enter_safepoint_and_park(GcGlobalContext *global, GcThreadContext *thread);

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_H */
