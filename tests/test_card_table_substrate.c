#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const GcDescriptor TEST_DESC = {
	.name        = "CardTableTestObject",
	.fixed_size  = sizeof(GcHeader),
	.flags       = 0u,
	.kind        = 100u,
	.trace_slots = NULL,
	.trace_edges = NULL,
	.finalize    = NULL,
};

typedef struct {
	GcHeader* owners[8];
	size_t    card_indices[8];
	size_t    count;
} DirtyCardCapture;

static void capture_dirty_card(GcHeader* owner, size_t card_index, void* ctx) {
	DirtyCardCapture* capture = (DirtyCardCapture*)ctx;
	assert(capture != NULL);
	assert(capture->count < (sizeof(capture->owners) / sizeof(capture->owners[0])));
	capture->owners[capture->count]       = owner;
	capture->card_indices[capture->count] = card_index;
	capture->count++;
}

int main(void) {
	GcConfig         cfg;
	GcVmHooks        hooks;
	GcRuntime*       rt;
	GcHeader*        owner_a;
	GcHeader*        owner_b;
	DirtyCardCapture capture;
	const size_t     owner_a_size = 96u;
	const size_t     owner_b_size = 48u;

	gc_config_init_default(&cfg);
	cfg.gc_region_size = 32u;
	memset(&hooks, 0, sizeof(hooks));

	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	assert(rt->barriers.old_to_young.card_granularity == 32u);
	assert(rt->barriers.old_to_young.count == 0u);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	owner_a = (GcHeader*)calloc(1, owner_a_size);
	owner_b = (GcHeader*)calloc(1, owner_b_size);
	assert(owner_a != NULL);
	assert(owner_b != NULL);
	owner_a->desc = &TEST_DESC;
	owner_a->size = (uint32_t)owner_a_size;
	owner_b->desc = &TEST_DESC;
	owner_b->size = (uint32_t)owner_b_size;

	assert(gc_barrier_register_owner(rt, owner_a, owner_a_size) == 1);
	assert(gc_barrier_register_owner(rt, owner_b, owner_b_size) == 1);
	assert(rt->barriers.old_to_young.count == 2u);
	assert(gc_barrier_is_owner_dirty(rt, owner_a) == 0);
	assert(gc_barrier_is_owner_dirty(rt, owner_b) == 0);

	gc_barrier_mark_slot_dirty(rt, owner_a, ((uint8_t*)owner_a) + 8u);
	gc_barrier_mark_slot_dirty(rt, owner_a, ((uint8_t*)owner_a) + 40u);
	gc_barrier_mark_slot_dirty(rt, owner_a, ((uint8_t*)owner_a) + 80u);
	gc_barrier_mark_slot_dirty(rt, owner_a, ((uint8_t*)owner_a) + 41u);
	assert(gc_barrier_is_owner_dirty(rt, owner_a) == 1);
	assert(rt->barriers.dirty_old_objects == 1u);
	assert(rt->barriers.dirty_cards == 3u);

	memset(&capture, 0, sizeof(capture));
	gc_barrier_visit_dirty_cards(rt, capture_dirty_card, &capture);
	assert(capture.count == 3u);
	assert(capture.owners[0] == owner_a && capture.card_indices[0] == 0u);
	assert(capture.owners[1] == owner_a && capture.card_indices[1] == 1u);
	assert(capture.owners[2] == owner_a && capture.card_indices[2] == 2u);

	gc_barrier_clear_owner_dirty(rt, owner_a);
	assert(gc_barrier_is_owner_dirty(rt, owner_a) == 0);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	gc_barrier_mark_owner_dirty(rt, owner_b);
	assert(gc_barrier_is_owner_dirty(rt, owner_b) == 1);
	assert(rt->barriers.dirty_old_objects == 1u);
	assert(rt->barriers.dirty_cards == 2u);

	memset(&capture, 0, sizeof(capture));
	gc_barrier_visit_dirty_cards(rt, capture_dirty_card, &capture);
	assert(capture.count == 2u);
	assert(capture.owners[0] == owner_b && capture.card_indices[0] == 0u);
	assert(capture.owners[1] == owner_b && capture.card_indices[1] == 1u);

	gc_barrier_unregister_owner(rt, owner_b);
	assert(rt->barriers.old_to_young.count == 1u);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	gc_barrier_unregister_owner(rt, owner_a);
	assert(rt->barriers.old_to_young.count == 0u);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	free(owner_a);
	free(owner_b);
	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}
