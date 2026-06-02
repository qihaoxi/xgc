#include "gc_internal.h"

void gc_bacon_rajan_write_barrier(GcRuntime* rt, GcThreadContext* thread, GcHeader* owner, GcHeader** slot,
                                  GcHeader* old_value, GcHeader* new_value) {
	(void)rt;
	(void)thread;
	(void)owner;
	(void)slot;
	(void)old_value;
	(void)new_value;
}
