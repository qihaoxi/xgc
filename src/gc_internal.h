#ifndef XGC_GC_INTERNAL_H
#define XGC_GC_INTERNAL_H

#include "xgc/gc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ============================================================================
 * 内部头文件 — 库内部使用，用户不可见
 *
 * 包含: 原子类型封装、GcHeader 完整定义、GcGlobalContext / GcThreadContext
 *        完整定义、显式工作栈、Epoch 状态（多线程）、内部辅助宏
 * ============================================================================
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. 原子类型封装
 *
 * GC_MULTITHREAD 定义时使用 C11 _Atomic，否则退化为普通类型（零开销）
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(GC_MULTITHREAD) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    #include <stdatomic.h>
    typedef _Atomic uint32_t gc_atomic_rc_t;
    typedef _Atomic uint8_t  gc_atomic_color_t;
    typedef _Atomic int      gc_atomic_int_t;

    #define GC_ATOMIC_LOAD(p)     atomic_load(p)
    #define GC_ATOMIC_STORE(p,v)  atomic_store(p, v)
    #define GC_ATOMIC_INC(p)      atomic_fetch_add(p, 1)
    #define GC_ATOMIC_DEC(p)      atomic_fetch_sub(p, 1)
    #define GC_ATOMIC_XCHG(p,v)   atomic_exchange(p, v)
    #define GC_CAS(p,exp,des)     atomic_compare_exchange_weak(p, exp, des)
#else
    typedef uint32_t gc_atomic_rc_t;
    typedef uint8_t  gc_atomic_color_t;
    typedef int      gc_atomic_int_t;

    #define GC_ATOMIC_LOAD(p)     (*(p))
    #define GC_ATOMIC_STORE(p,v)  (*(p) = (v))
    #define GC_ATOMIC_INC(p)      ((*(p))++)
    #define GC_ATOMIC_DEC(p)      ((*(p))--)
    #define GC_ATOMIC_XCHG(p,v)   ({ typeof(v) _old = *(p); *(p) = (v); _old; })
    #define GC_CAS(p,exp,des)     (*(p) == *(exp) ? (*(p) = (des), 1) : (*(exp) = *(p), 0))
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. 侵入式通用对象头
 * ═══════════════════════════════════════════════════════════════════════════ */

struct GcHeader {
    gc_atomic_rc_t    rc;             /* 仅记录"堆引用"的计数（栈/寄存器引用不在此计数内） */
    uint32_t          gc_ref;         /* trial deletion 工作计数（非原子，仅 GC 线程访问） */
    gc_atomic_color_t color;          /* GC 颜色状态                               */
    uint8_t           in_purple_buf;  /* 防止重复加入嫌疑缓冲区                       */
    uint16_t          obj_type;       /* 弱类型标签（由 VM 层定义）                   */
    GcHeader         *next;           /* 全局对象链表节点                            */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. 显式工作栈（防 C 递归栈溢出）
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    GcHeader **data;
    int        top;
    int        capacity;
} GcWorklist;

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. 全局上下文
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    GC_STATE_RUNNING         = 0,  /* 正常运行                                   */
    GC_STATE_STW_REQUESTED   = 2,  /* 有人发起了 STW 请求，等待所有线程到达 Safepoint */
    GC_STATE_STW_IN_PROGRESS = 3,  /* STW 回收进行中（所有 Mutator 已挂起）        */
} GcGlobalState;

struct GcGlobalContext {
    /* ── 全局对象链表 ── */
    GcHeader       *global_head;

    /* ── SPI 回调 ── */
    GcTraceFn        on_trace_children;
    GcFinalizeFn     on_finalize;
    GcPressureFn     on_pressure;
    GcPurpleFullFn   on_purple_buffer_full;

    /* ── 内存统计 ── */
    size_t           total_allocated;
    size_t           gc_threshold_soft;
    size_t           gc_threshold_hard;

    /* ── 显式工作栈（共享） ── */
    GcHeader        *worklist_data;
    int              worklist_top;
    int              worklist_capacity;

    /* ── 多线程状态（GC_MULTITHREAD 时有效） ── */
    gc_atomic_int_t  gc_state;
    gc_atomic_int_t  safepoint_arrive_count;
    int              running;        /* Background GC 线程运行标志 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. 线程本地上下文
 * ═══════════════════════════════════════════════════════════════════════════ */

struct GcThreadContext {
    /* ── 紫色缓冲区 ── */
    GcHeader       **purple_buffer;
    int              purple_count;
    int              purple_capacity;

    /* ── TLAB 本地分配链表 ── */
    GcHeader        *local_head;
    int              local_alloc_count;
    int              tlab_threshold;

    /* ── SPI 回调 ── */
    GcScanRootsFn    on_scan_roots;
    void            *vm_thread_ctx;

    /* ── Ring Buffer 临时对象区 ── */
    GcHeader        *zct_ring;
    int              zct_head;
    int              zct_capacity;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 6. 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* gc_worklist.c */
void      gc_worklist_push(GcWorklist *wl, GcHeader *obj);
GcHeader *gc_worklist_pop(GcWorklist *wl);

/* gc_ring.c */
void *gc_try_ring_buffer_alloc(GcThreadContext *thread, size_t size);
void  gc_reclaim_ring_buffer_slots(GcGlobalContext *global, GcThreadContext *thread);

/* gc_reclaim.c */
void gc_reclaim_object(GcGlobalContext *global, GcHeader *obj);

/* gc_epoch.c (GC_MULTITHREAD only) */
void gc_signal_background_thread(GcGlobalContext *global);
void gc_wait_for_signal(GcGlobalContext *global);

#endif /* XGC_GC_INTERNAL_H */
