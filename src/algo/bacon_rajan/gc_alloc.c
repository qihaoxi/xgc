#include "gc_internal.h"

void
gc_global_context_init(GcGlobalContext *global,
                       GcTraceFn         on_trace_children,
                       GcFinalizeFn       on_finalize,
                       GcPressureFn       on_pressure,
                       GcPurpleFullFn     on_purple_buffer_full)
{
    /* TODO: 初始化全局上下文 */
    (void)global;
    (void)on_trace_children;
    (void)on_finalize;
    (void)on_pressure;
    (void)on_purple_buffer_full;
}

void
gc_thread_context_init(GcThreadContext *thread,
                       GcScanRootsFn     on_scan_roots,
                       void             *vm_thread_ctx)
{
    /* TODO: 初始化线程上下文 */
    (void)thread;
    (void)on_scan_roots;
    (void)vm_thread_ctx;
}

void *
gc_alloc(GcGlobalContext *global, GcThreadContext *thread,
         size_t total_size, uint16_t obj_type)
{
    /* TODO: 实现分配逻辑（Ring Buffer 快速路径 → TLAB → 水位线检查） */
    (void)global;
    (void)thread;
    (void)total_size;
    (void)obj_type;
    return NULL;
}

void
gc_flush_tlab(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: TLAB 批量冲刷到全局链表 */
    (void)global;
    (void)thread;
}

void
gc_check_pressure(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: 双水位线 + 背压控制 */
    (void)global;
    (void)thread;
}
