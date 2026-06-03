#include "gc_internal.h"

void gc_bacon_rajan_write_barrier(gc_runtime* rt, gc_thread_context* thread, gc_header* owner, gc_header** slot,
                                  gc_header* old_value, gc_header* new_value);
void gc_bacon_rajan_collect_minor(gc_runtime* rt);
void gc_bacon_rajan_collect_major(gc_runtime* rt);
void gc_bacon_rajan_collect_full(gc_runtime* rt);

static void gc_bacon_rajan_global_init(gc_runtime* rt, const gc_config* cfg) {
	(void)cfg;
	if (rt == NULL) {
		return;
	}
	if (rt->worklist_data == NULL && rt->worklist_capacity > 0) {
		rt->worklist_data = (gc_header**)calloc((size_t)rt->worklist_capacity, sizeof(gc_header*));
	}
}

static void gc_bacon_rajan_global_destroy(gc_runtime* rt) {
	(void)rt;
}

static void gc_bacon_rajan_thread_init(gc_runtime* rt, gc_thread_context* thread) {
	(void)rt;
	(void)thread;
}

static void gc_bacon_rajan_thread_destroy(gc_runtime* rt, gc_thread_context* thread) {
	(void)rt;
	(void)thread;
}

static void* gc_bacon_rajan_alloc(gc_runtime* rt, gc_thread_context* thread, const gc_descriptor* desc,
                                  size_t total_size, uint32_t alloc_flags) {
	gc_header* obj;

	(void)thread;
	obj = (gc_header*)calloc(1, total_size);
	if (obj == NULL) {
		return NULL;
	}

	obj->desc = desc;
	obj->size = (uint32_t)total_size;
	obj->kind = (desc != NULL) ? desc->kind : 0;
	obj->flags =
	    (uint16_t)(((desc != NULL && (desc->flags & GC_DESC_FLAG_HAS_FINALIZER) != 0u) ? GC_OBJECT_FLAG_FINALIZABLE
	                                                                                   : 0u) |
	               ((alloc_flags & GC_ALLOC_PINNED) != 0u ? GC_OBJECT_FLAG_PINNED : 0u));

	if (rt != NULL) {
		rt->stats.total_allocated += total_size;
		if (rt->stats.total_allocated > rt->stats.peak_allocated) {
			rt->stats.peak_allocated = rt->stats.total_allocated;
		}
	}

	return obj;
}

static void gc_bacon_rajan_post_alloc(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	(void)obj;
}

static gc_header* gc_bacon_rajan_read_barrier(gc_runtime* rt, gc_thread_context* thread, gc_header** slot) {
	(void)rt;
	(void)thread;
	return (slot != NULL) ? *slot : NULL;
}

static void gc_bacon_rajan_pin(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags | GC_OBJECT_FLAG_PINNED);
	}
}

static void gc_bacon_rajan_unpin(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags & ~GC_OBJECT_FLAG_PINNED);
	}
}

static const gc_algorithm_vtable GC_BACON_RAJAN_VTABLE = {
	.name = "bacon-rajan",
	.caps = {
		.supports_moving = 0,
		.supports_compaction = 0,
		.supports_generations = 0,
		.requires_write_barrier = 1,
		.requires_read_barrier = 0,
		.supports_concurrent_mark = 0,
		.supports_concurrent_relocate = 0,
		.provides_deterministic_release = 0,
	},
	.global_init = gc_bacon_rajan_global_init,
	.global_destroy = gc_bacon_rajan_global_destroy,
	.thread_init = gc_bacon_rajan_thread_init,
	.thread_destroy = gc_bacon_rajan_thread_destroy,
	.alloc = gc_bacon_rajan_alloc,
	.post_alloc = gc_bacon_rajan_post_alloc,
	.write_barrier = gc_bacon_rajan_write_barrier,
	.read_barrier = gc_bacon_rajan_read_barrier,
	.collect_minor = gc_bacon_rajan_collect_minor,
	.collect_major = gc_bacon_rajan_collect_major,
	.collect_full = gc_bacon_rajan_collect_full,
	.pin = gc_bacon_rajan_pin,
	.unpin = gc_bacon_rajan_unpin,
};

const gc_algorithm_vtable* xgc_algorithm_bacon_rajan(void) {
	return &GC_BACON_RAJAN_VTABLE;
}

const gc_algorithm_vtable* xgc_default_algorithm(void) {
	return xgc_algorithm_bacon_rajan();
}
