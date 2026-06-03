#include "gc_internal.h"

void* gc_try_ring_buffer_alloc(gc_thread_context* thread, size_t size) {
	/* TODO: Ring Buffer 快速路径分配（只分配不复用） */
	(void)thread;
	(void)size;
	return NULL;
}

void gc_reclaim_ring_buffer_slots(gc_runtime* rt, gc_thread_context* thread) {
	/* TODO: Collection 后安全复用 Ring Buffer 槽位 */
	(void)rt;
	(void)thread;
}
