#include "gc_internal.h"

void gc_bacon_rajan_write_barrier(gc_runtime* rt, gc_thread_context* thread, gc_header* owner, gc_header** slot,
                                  gc_header* old_value, gc_header* new_value) {
	(void)rt;
	(void)thread;
	(void)owner;
	(void)slot;
	(void)old_value;
	(void)new_value;
}
