#include "gc_internal.h"

void gc_bacon_rajan_write_barrier(GcRuntime* rt, GcThreadContext* thread, GcHeader* owner, GcHeader** slot,
                                  GcHeader* old_value, GcHeader* new_value);
void gc_bacon_rajan_collect_minor(GcRuntime* rt);
void gc_bacon_rajan_collect_major(GcRuntime* rt);
void gc_bacon_rajan_collect_full(GcRuntime* rt);

static void gc_bacon_rajan_global_init(GcRuntime* rt, const GcConfig* cfg) {
	(void)cfg;
	if (rt == NULL) {
		return;
	}
	if (rt->worklist_data == NULL && rt->worklist_capacity > 0) {
		rt->worklist_data = (GcHeader**)calloc((size_t)rt->worklist_capacity, sizeof(GcHeader*));
	}
}

static void gc_bacon_rajan_global_destroy(GcRuntime* rt) {
	(void)rt;
}

static void gc_bacon_rajan_thread_init(GcRuntime* rt, GcThreadContext* thread) {
	(void)rt;
	(void)thread;
}

static void gc_bacon_rajan_thread_destroy(GcRuntime* rt, GcThreadContext* thread) {
	(void)rt;
	(void)thread;
}

static void* gc_bacon_rajan_alloc(GcRuntime* rt, GcThreadContext* thread, const GcDescriptor* desc, size_t total_size,
                                  uint32_t alloc_flags) {
	GcHeader* obj;

	(void)thread;
	obj = (GcHeader*)calloc(1, total_size);
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

static void gc_bacon_rajan_post_alloc(GcRuntime* rt, GcHeader* obj) {
	(void)rt;
	(void)obj;
}

static GcHeader* gc_bacon_rajan_read_barrier(GcRuntime* rt, GcThreadContext* thread, GcHeader** slot) {
	(void)rt;
	(void)thread;
	return (slot != NULL) ? *slot : NULL;
}

static void gc_bacon_rajan_pin(GcRuntime* rt, GcHeader* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags | GC_OBJECT_FLAG_PINNED);
	}
}

static void gc_bacon_rajan_unpin(GcRuntime* rt, GcHeader* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags & ~GC_OBJECT_FLAG_PINNED);
	}
}

static const GcAlgorithmVTable GC_BACON_RAJAN_VTABLE = {
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

const GcAlgorithmVTable* xgc_algorithm_bacon_rajan(void) {
	return &GC_BACON_RAJAN_VTABLE;
}

const GcAlgorithmVTable* xgc_default_algorithm(void) {
	return xgc_algorithm_bacon_rajan();
}
