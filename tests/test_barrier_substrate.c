#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>

static const gc_descriptor TEST_DESC = {
	.name        = "BarrierTestObject",
	.fixed_size  = sizeof(gc_header),
	.flags       = 0u,
	.kind        = 99u,
	.trace_slots = NULL,
	.trace_edges = NULL,
	.finalize    = NULL,
};

int main(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;
	gc_header*  owner_a;
	gc_header*  owner_b;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));

	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->barriers.old_to_young.count == 0u);
	assert(rt->barriers.dirty_old_objects == 0u);

	owner_a = (gc_header*)calloc(1, sizeof(*owner_a));
	owner_b = (gc_header*)calloc(1, sizeof(*owner_b));
	assert(owner_a != NULL);
	assert(owner_b != NULL);
	owner_a->desc = &TEST_DESC;
	owner_a->size = sizeof(*owner_a);
	owner_b->desc = &TEST_DESC;
	owner_b->size = sizeof(*owner_b);

	assert(gc_barrier_register_owner(rt, owner_a, owner_a->size) == 1);
	assert(gc_barrier_register_owner(rt, owner_b, owner_b->size) == 1);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->barriers.old_to_young.count == 2u);
	assert(gc_barrier_is_owner_dirty(rt, owner_a) == 0);
	assert(gc_barrier_is_owner_dirty(rt, owner_b) == 0);

	gc_barrier_mark_owner_dirty(rt, owner_a);
	assert(gc_barrier_is_owner_dirty(rt, owner_a) == 1);
	assert(gc_barrier_is_owner_dirty(rt, owner_b) == 0);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->barriers.dirty_old_objects == 1u);

	gc_barrier_mark_owner_dirty(rt, owner_a);
	assert(rt->barriers.dirty_old_objects == 1u);

	gc_barrier_mark_owner_dirty(rt, owner_b);
	assert(gc_barrier_is_owner_dirty(rt, owner_b) == 1);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->barriers.dirty_old_objects == 2u);

	gc_barrier_clear_owner_dirty(rt, owner_a);
	assert(gc_barrier_is_owner_dirty(rt, owner_a) == 0);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->barriers.dirty_old_objects == 1u);

	gc_barrier_unregister_owner(rt, owner_b);
	assert(rt->barriers.old_to_young.count == 1u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(gc_barrier_is_owner_dirty(rt, owner_b) == 0);

	gc_barrier_unregister_owner(rt, owner_a);
	assert(rt->barriers.old_to_young.count == 0u);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(rt->barriers.dirty_old_objects == 0u);

	free(owner_a);
	free(owner_b);
	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}
