#include "gc_internal.h"

void
gc_push_purple_adaptive(GcGlobalContext *global, GcThreadContext *thread,
                        GcHeader *obj)
{
    /* TODO: 自适应紫色缓冲区推入 + 满时回调 on_purple_buffer_full */
    (void)global;
    (void)thread;
    (void)obj;
}
