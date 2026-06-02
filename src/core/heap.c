#include "gc_internal.h"

void gc_heap_init(GcHeap* heap, size_t large_object_threshold) {
	if (heap == NULL) {
		return;
	}

	memset(heap, 0, sizeof(*heap));
	heap->large_object_threshold = large_object_threshold;
}

void gc_heap_set_young_space(GcRuntime* rt, uint8_t* base, size_t capacity) {
	if (rt == NULL) {
		return;
	}

	rt->heap.young_base     = base;
	rt->heap.young_capacity = capacity;
	if (base == NULL) {
		rt->heap.young_used = 0u;
	}
}

void gc_heap_record_young_alloc(GcRuntime* rt, size_t bytes) {
	if (rt == NULL) {
		return;
	}

	rt->heap.young_used += bytes;
}

void gc_heap_reset_young(GcRuntime* rt) {
	if (rt == NULL) {
		return;
	}

	rt->heap.young_used = 0u;
}

void gc_heap_record_old_alloc(GcRuntime* rt, size_t bytes) {
	if (rt == NULL) {
		return;
	}

	rt->heap.old_object_count++;
	rt->heap.old_object_bytes += bytes;
}

void gc_heap_record_old_free(GcRuntime* rt, size_t bytes) {
	if (rt == NULL) {
		return;
	}

	if (rt->heap.old_object_count > 0u) {
		rt->heap.old_object_count--;
	}
	if (rt->heap.old_object_bytes >= bytes) {
		rt->heap.old_object_bytes -= bytes;
	} else {
		rt->heap.old_object_bytes = 0u;
	}
}
