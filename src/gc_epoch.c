#include "gc_internal.h"

#ifdef GC_MULTITHREAD

void
gc_signal_background_thread(GcGlobalContext *global)
{
    /* TODO: 发信号唤醒 Background GC 线程 */
    (void)global;
}

void
gc_wait_for_signal(GcGlobalContext *global)
{
    /* TODO: Background GC 线程等待信号 */
    (void)global;
}

#endif /* GC_MULTITHREAD */
