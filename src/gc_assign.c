#include "gc_internal.h"

void
gc_assign_field(GcGlobalContext *global, GcThreadContext *thread,
                GcHeader **field, GcHeader *new_child)
{
    /* TODO: 实现统一写屏障（原子交换 + RC 增减 + 紫色缓冲区推入） */
    (void)global;
    (void)thread;
    (void)field;
    (void)new_child;
}
