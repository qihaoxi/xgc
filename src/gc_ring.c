#include "gc_internal.h"

void *
gc_try_ring_buffer_alloc(GcThreadContext *thread, size_t size)
{
    /* TODO: Ring Buffer 快速路径分配（只分配不复用） */
    (void)thread;
    (void)size;
    return NULL;
}

void
gc_reclaim_ring_buffer_slots(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: Collection 后安全复用 Ring Buffer 槽位 */
    (void)global;
    (void)thread;
}
