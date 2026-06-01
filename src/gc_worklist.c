#include "gc_internal.h"

void
gc_worklist_push(GcWorklist *wl, GcHeader *obj)
{
    /* TODO: 动态扩容 + 压栈 */
    (void)wl;
    (void)obj;
}

GcHeader *
gc_worklist_pop(GcWorklist *wl)
{
    /* TODO: 弹栈 */
    (void)wl;
    return NULL;
}
