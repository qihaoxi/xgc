#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	gc_header header;
	int       payload;
} ReclaimTestObject;

static int g_finalize_calls;
static int g_finalize_payload_sum;

static void reclaim_test_finalize(gc_header* obj) {
	ReclaimTestObject* test_obj = (ReclaimTestObject*)obj;
	g_finalize_calls++;
	g_finalize_payload_sum += test_obj->payload;
}

static const gc_descriptor FINALIZABLE_DESC = {
	.name              = "ReclaimFinalizable",
	.fixed_size        = sizeof(ReclaimTestObject),
	.flags             = GC_DESC_FLAG_HAS_FINALIZER,
	.kind              = 77u,
	.trace_slots       = NULL,
	.trace_slots_range = NULL,
	.trace_edges       = NULL,
	.finalize          = reclaim_test_finalize,
};

static const gc_descriptor PLAIN_DESC = {
	.name              = "ReclaimPlain",
	.fixed_size        = sizeof(ReclaimTestObject),
	.flags             = 0u,
	.kind              = 78u,
	.trace_slots       = NULL,
	.trace_slots_range = NULL,
	.trace_edges       = NULL,
	.finalize          = NULL,
};

int main(void) {
	gc_config          cfg;
	gc_vm_hooks        hooks;
	gc_runtime*        rt;
	ReclaimTestObject* obj;
	ReclaimTestObject* plain;

	gc_config_init_default(&cfg);
	memset(&hooks, 0, sizeof(hooks));
	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);

	g_finalize_calls       = 0;
	g_finalize_payload_sum = 0;

	obj = (ReclaimTestObject*)calloc(1, sizeof(*obj));
	assert(obj != NULL);
	obj->header.desc          = &FINALIZABLE_DESC;
	obj->header.size          = (uint32_t)sizeof(*obj);
	obj->header.kind          = FINALIZABLE_DESC.kind;
	obj->payload              = 17;
	rt->stats.total_allocated = sizeof(*obj) + 9u;
	rt->stats.peak_allocated  = rt->stats.total_allocated;
	gc_reclaim_object(rt, &obj->header);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(g_finalize_calls == 1);
	assert(g_finalize_payload_sum == 17);
	assert(rt->stats.total_allocated == 9u);

	plain = (ReclaimTestObject*)calloc(1, sizeof(*plain));
	assert(plain != NULL);
	plain->header.desc        = &PLAIN_DESC;
	plain->header.size        = (uint32_t)sizeof(*plain);
	plain->header.kind        = PLAIN_DESC.kind;
	plain->payload            = 23;
	rt->stats.total_allocated = 4u;
	rt->stats.peak_allocated  = rt->stats.total_allocated;
	gc_reclaim_object(rt, &plain->header);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(g_finalize_calls == 1);
	assert(g_finalize_payload_sum == 17);
	assert(rt->stats.total_allocated == 0u);

	gc_reclaim_object(rt, NULL);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(g_finalize_calls == 1);
	assert(g_finalize_payload_sum == 17);

	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}
