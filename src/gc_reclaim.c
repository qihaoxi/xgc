#include "gc_internal.h"

void
gc_reclaim_object(GcGlobalContext *global, GcHeader *obj)
{
    /* TODO: 递归释放子节点 + 终结器回调 + free */
    (void)global;
    (void)obj;
}
