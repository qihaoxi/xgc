#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>

int main(void) {
	gc_heap     heap;
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;
	uint8_t*    young;

	gc_heap_init(&heap, 4096u);
	assert(heap.large_object_threshold == 4096u);
	assert(heap.young_base == NULL);
	assert(heap.young_capacity == 0u);
	assert(heap.young_used == 0u);
	assert(heap.old_object_count == 0u);
	assert(heap.old_object_bytes == 0u);

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);

	young = (uint8_t*)malloc(1024u);
	assert(young != NULL);
	gc_heap_set_young_space(rt, young, 1024u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->heap.young_base == young);
	assert(rt->heap.young_capacity == 1024u);
	assert(rt->heap.young_used == 0u);

	gc_heap_record_young_alloc(rt, 128u);
	gc_heap_record_young_alloc(rt, 64u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->heap.young_used == 192u);

	gc_heap_reset_young(rt);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->heap.young_used == 0u);

	gc_heap_record_old_alloc(rt, 256u);
	gc_heap_record_old_alloc(rt, 512u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->heap.old_object_count == 2u);
	assert(rt->heap.old_object_bytes == 768u);

	gc_heap_record_old_free(rt, 256u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->heap.old_object_count == 1u);
	assert(rt->heap.old_object_bytes == 512u);

	gc_heap_record_old_free(rt, 1024u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->heap.old_object_count == 0u);
	assert(rt->heap.old_object_bytes == 0u);

	gc_heap_set_young_space(rt, NULL, 0u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->heap.young_base == NULL);
	assert(rt->heap.young_capacity == 0u);
	assert(rt->heap.young_used == 0u);

	free(young);
	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}
