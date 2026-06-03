#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const gc_descriptor TEST_DESC = {
	.name              = "InvariantOwner",
	.fixed_size        = sizeof(gc_header),
	.flags             = 0u,
	.kind              = 88u,
	.trace_slots       = NULL,
	.trace_slots_range = NULL,
	.trace_edges       = NULL,
	.finalize          = NULL,
};

static void init_header(gc_header* h, uint16_t kind) {
	memset(h, 0, sizeof(*h));
	h->desc = &TEST_DESC;
	h->size = (uint32_t)sizeof(*h);
	h->kind = kind;
}

/* ── bitmap invariant ── */

static void test_bitmap_invariants(void) {
	gc_bitmap bitmap;

	memset(&bitmap, 0, sizeof(bitmap));
	gc_bitmap_init(&bitmap, 9u);
	assert(gc_bitmap_check_invariants(&bitmap) == 1);

	bitmap.byte_count = 1u;
	assert(gc_bitmap_check_invariants(&bitmap) == 0);

	bitmap.byte_count = 2u;
	gc_bitmap_destroy(&bitmap);
	assert(gc_bitmap_check_invariants(&bitmap) == 1);
}

/* ── card-table invariant ── */

static void test_card_table_invariants(void) {
	gc_card_table table;

	memset(&table, 0, sizeof(table));
	gc_card_table_init(&table, 32u);
	assert(gc_card_table_check_invariants(&table) == 1);
	gc_card_table_destroy(&table);
}

/* ── runtime creation → default algo → passes ── */

static void test_runtime_default_passes(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);
	gc_runtime_destroy(rt);
}

/* ── runtime without algorithm → fails ── */

static void test_runtime_missing_algo_fails(void) {
	gc_runtime rt_mem;
	memset(&rt_mem, 0, sizeof(rt_mem));
	rt_mem.algo = NULL;
	assert(gc_runtime_check_invariants(&rt_mem) == 0);
}

/* ── runtime with algorithm but NULL name → fails ── */

static void test_runtime_null_algo_name_fails(void) {
	gc_runtime            rt_mem;
	gc_algorithm_vtable   fake_algo;
	memset(&rt_mem, 0, sizeof(rt_mem));
	memset(&fake_algo, 0, sizeof(fake_algo));
	rt_mem.algo = &fake_algo;
	assert(gc_runtime_check_invariants(&rt_mem) == 0);
}

/* ── runtime gc_state invalid → fails ── */

static void test_runtime_invalid_gc_state_fails(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);

	GC_ATOMIC_STORE(&rt->gc_state, 99 /* illegal */);
	assert(gc_runtime_check_invariants(rt) == 0);

	GC_ATOMIC_STORE(&rt->gc_state, GC_STATE_RUNNING);
	assert(gc_runtime_check_invariants(rt) == 1);

	gc_runtime_destroy(rt);
}

/* ── runtime with dirty-card counter mismatch → fails ── */

static void test_runtime_dirty_card_counter_mismatch(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;
	gc_header*  owner;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);

	owner = (gc_header*)calloc(1, 96u);
	assert(owner != NULL);
	init_header(owner, 90u);
	owner->size = 96u;

	assert(gc_barrier_register_owner(rt, owner, owner->size) == 1);
	gc_barrier_mark_slot_dirty(rt, owner, ((uint8_t*)owner) + 8u);
	gc_barrier_mark_slot_dirty(rt, owner, ((uint8_t*)owner) + 48u);
	assert(gc_runtime_check_invariants(rt) == 1);

	rt->barriers.dirty_cards++;
	assert(gc_runtime_check_invariants(rt) == 0);
	rt->barriers.dirty_cards--;
	assert(gc_runtime_check_invariants(rt) == 1);

	gc_barrier_unregister_owner(rt, owner);
	free(owner);
	gc_runtime_destroy(rt);
}

/* ── runtime with young-space overflow → fails ── */

static void test_runtime_young_space_overflow_fails(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;
	gc_header*  owner;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);

	owner = (gc_header*)calloc(1, 64u);
	assert(owner != NULL);
	init_header(owner, 91u);
	owner->size = 64u;

	rt->heap.young_base     = (uint8_t*)owner;
	rt->heap.young_capacity = 16u;
	rt->heap.young_used     = 32u;
	assert(gc_runtime_check_invariants(rt) == 0);

	rt->heap.young_base     = NULL;
	rt->heap.young_capacity = 0u;
	rt->heap.young_used     = 0u;
	assert(gc_runtime_check_invariants(rt) == 1);

	free(owner);
	gc_runtime_destroy(rt);
}

/* ── runtime peak < total → fails ── */

static void test_runtime_peak_below_total_fails(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);

	rt->stats.total_allocated = 128u;
	rt->stats.peak_allocated  = 64u;
	assert(gc_runtime_check_invariants(rt) == 0);

	rt->stats.peak_allocated = 128u;
	assert(gc_runtime_check_invariants(rt) == 1);

	gc_runtime_destroy(rt);
}

/* ── runtime worklist capacity/data mismatch → fails ── */

static void test_runtime_worklist_consistency(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;
	gc_header** saved_data;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);

	/* pass by default */
	assert(gc_runtime_check_invariants(rt) == 1);

	/* data == NULL but capacity > 0 — must fail */
	saved_data           = rt->worklist_data;
	rt->worklist_data    = NULL;
	assert(gc_runtime_check_invariants(rt) == 0);

	/* restore the real pointer so gc_runtime_destroy can free it */
	rt->worklist_data = saved_data;
	assert(gc_runtime_check_invariants(rt) == 1);

	gc_runtime_destroy(rt);
}

/* ── runtime handle list: every handle must pass its own invariants
 *   and must back-point to this runtime ── */

static void test_runtime_handle_consistency(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	gc_runtime* rt;
	gc_header*  obj;
	gc_handle*  h1;
	gc_handle*  h2;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);

	/* Use a plain calloc object — avoid coupling the invariant test
	 * to any particular algorithm's allocator behaviour. */
	obj = (gc_header*)calloc(1, sizeof(*obj));
	assert(obj != NULL);
	init_header(obj, 200u);

	h1 = gc_handle_acquire(rt, obj, GC_HANDLE_DEFAULT);
	assert(h1 != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);

	h2 = gc_handle_acquire(rt, obj, GC_HANDLE_PINNED);
	assert(h2 != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);

	/* break back-pointer — runtime invariant should catch it */
	h1->runtime = (gc_runtime*)0x1;
	assert(gc_runtime_check_invariants(rt) == 0);
	h1->runtime = rt;
	assert(gc_runtime_check_invariants(rt) == 1);

	/* break target desc — handle invariant should catch it */
	obj->desc = NULL;
	assert(gc_runtime_check_invariants(rt) == 0);
	obj->desc = &TEST_DESC;
	assert(gc_runtime_check_invariants(rt) == 1);

	/* release one handle — the other must still pass */
	gc_handle_release(h2);
	assert(gc_runtime_check_invariants(rt) == 1);

	/* release last handle — list becomes empty */
	gc_handle_release(h1);
	assert(gc_runtime_check_invariants(rt) == 1);

	free(obj);
	gc_runtime_destroy(rt);
}

/* ── worklist invariants ── */

static void test_worklist_null_fails(void) {
	assert(gc_worklist_check_invariants(NULL) == 0);
}

static void test_worklist_empty_passes(void) {
	gc_worklist wl;
	memset(&wl, 0, sizeof(wl));
	assert(gc_worklist_check_invariants(&wl) == 1);
}

static void test_worklist_top_out_of_range_fails(void) {
	gc_worklist wl;
	gc_header*  data[2];

	memset(&wl, 0, sizeof(wl));
	wl.data     = data;
	wl.capacity = 2;
	wl.top      = 3;
	assert(gc_worklist_check_invariants(&wl) == 0);

	wl.top = -1;
	assert(gc_worklist_check_invariants(&wl) == 0);

	wl.top = 0;
	assert(gc_worklist_check_invariants(&wl) == 1);
}

static void test_worklist_zero_capacity_no_data(void) {
	gc_worklist wl;
	gc_header*  dummy;

	memset(&wl, 0, sizeof(wl));

	/* clean zero state */
	assert(gc_worklist_check_invariants(&wl) == 1);

	/* zero capacity but non-NULL data → fail */
	wl.data = &dummy;
	assert(gc_worklist_check_invariants(&wl) == 0);
}

static void test_worklist_positive_capacity_null_data_fails(void) {
	gc_worklist wl;

	memset(&wl, 0, sizeof(wl));
	wl.capacity = 4;
	assert(gc_worklist_check_invariants(&wl) == 0);
}

int main(void) {
	test_bitmap_invariants();
	test_card_table_invariants();
	test_runtime_default_passes();
	test_runtime_missing_algo_fails();
	test_runtime_null_algo_name_fails();
	test_runtime_invalid_gc_state_fails();
	test_runtime_dirty_card_counter_mismatch();
	test_runtime_young_space_overflow_fails();
	test_runtime_peak_below_total_fails();
	test_runtime_worklist_consistency();
	test_runtime_handle_consistency();

	test_worklist_null_fails();
	test_worklist_empty_passes();
	test_worklist_top_out_of_range_fails();
	test_worklist_zero_capacity_no_data();
	test_worklist_positive_capacity_null_data_fails();

	return EXIT_SUCCESS;
}
