#include "gc_internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_SLOT_COUNT 4096u

typedef struct {
	gc_header  header;
	gc_header* slots[BENCH_SLOT_COUNT];
} BenchOwner;

typedef struct {
	gc_header header;
	uint64_t payload[2];
} BenchLeaf;

static void bench_owner_trace_slots(gc_header* obj, gc_visit_slot_fn visit_slot, void* ctx) {
	BenchOwner* owner;
	size_t      i;

	if (obj == NULL || visit_slot == NULL) {
		return;
	}

	owner = (BenchOwner*)obj;
	for (i = 0; i < BENCH_SLOT_COUNT; ++i) {
		visit_slot(&owner->slots[i], ctx);
	}
}

static void bench_owner_trace_slots_range(gc_header* obj, size_t byte_begin, size_t byte_end, gc_visit_slot_fn visit_slot, void* ctx) {
	BenchOwner* owner;
	size_t      slots_begin;
	size_t      slots_end;
	size_t      first_index;
	size_t      last_index;
	size_t      i;

	if (obj == NULL || visit_slot == NULL) {
		return;
	}

	owner       = (BenchOwner*)obj;
	slots_begin = offsetof(BenchOwner, slots);
	slots_end   = slots_begin + sizeof(owner->slots);
	if (byte_end <= slots_begin || byte_begin >= slots_end) {
		return;
	}
	if (byte_begin < slots_begin) {
		byte_begin = slots_begin;
	}
	if (byte_end > slots_end) {
		byte_end = slots_end;
	}

	first_index = (byte_begin - slots_begin) / sizeof(owner->slots[0]);
	last_index  = (byte_end - slots_begin + sizeof(owner->slots[0]) - 1u) / sizeof(owner->slots[0]);
	if (last_index > BENCH_SLOT_COUNT) {
		last_index = BENCH_SLOT_COUNT;
	}

	for (i = first_index; i < last_index; ++i) {
		visit_slot(&owner->slots[i], ctx);
	}
}

static double bench_now_ms(void) {
	struct timespec ts;
	(void)timespec_get(&ts, TIME_UTC);
	return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
}

static size_t parse_size_arg(const char* arg, size_t fallback) {
	char*              end = NULL;
	unsigned long long v;

	if (arg == NULL || *arg == '\0') {
		return fallback;
	}

	v = strtoull(arg, &end, 10);
	if (end == arg || *end != '\0' || v == 0ull) {
		return fallback;
	}
	return (size_t)v;
}

static const gc_descriptor BENCH_OWNER_DESC = {
	.name        = "BenchOwner",
	.fixed_size  = sizeof(BenchOwner),
	.flags       = GC_DESC_FLAG_CONTAINS_REFS,
	.kind        = 201u,
	.trace_slots = bench_owner_trace_slots,
	.trace_slots_range = bench_owner_trace_slots_range,
	.trace_edges = NULL,
	.finalize    = NULL,
};

static const gc_descriptor BENCH_LEAF_DESC = {
	.name        = "BenchLeaf",
	.fixed_size  = sizeof(BenchLeaf),
	.flags       = 0u,
	.kind        = 202u,
	.trace_slots = NULL,
	.trace_edges = NULL,
	.finalize    = NULL,
};

int main(int argc, char** argv) {
	gc_config                  cfg;
	gc_vm_hooks                 hooks;
	gc_runtime*                rt;
	gc_thread_context*          thread;
	gc_handle*                 owner_handle;
	BenchOwner*               owner;
	const gc_algorithm_vtable*  algo;
	size_t                    iterations;
	size_t                    minor_every;
	size_t                    minor_runs;
	size_t                    i;
	double                    start_ms;
	double                    elapsed_ms;
	double                    minor_per_run_ms;

	iterations = (argc > 1) ? parse_size_arg(argv[1], 200000u) : 200000u;
	minor_every = (argc > 2) ? parse_size_arg(argv[2], 256u) : 256u;
	minor_runs = 0u;

	gc_config_init_default(&cfg);
	cfg.gc_young_space_size = 2u * 1024u * 1024u;
	cfg.gc_region_size = 256u;
	algo = xgc_default_algorithm();
	memset(&hooks, 0, sizeof(hooks));

	rt = gc_runtime_create(&cfg, algo, &hooks);
	if (rt == NULL) {
		fprintf(stderr, "failed to create runtime\n");
		return EXIT_FAILURE;
	}

	thread = gc_thread_attach(rt, NULL);
	if (thread == NULL) {
		fprintf(stderr, "failed to attach thread\n");
		gc_runtime_destroy(rt);
		return EXIT_FAILURE;
	}

	owner = (BenchOwner*)gc_alloc_typed(rt, thread, &BENCH_OWNER_DESC, sizeof(*owner), GC_ALLOC_PINNED);
	if (owner == NULL) {
		fprintf(stderr, "failed to allocate benchmark owner\n");
		gc_thread_detach(thread);
		gc_runtime_destroy(rt);
		return EXIT_FAILURE;
	}

	owner_handle = gc_handle_acquire(rt, (gc_header*)owner, GC_HANDLE_PINNED);
	if (owner_handle == NULL) {
		fprintf(stderr, "failed to pin benchmark owner\n");
		gc_thread_detach(thread);
		gc_runtime_destroy(rt);
		return EXIT_FAILURE;
	}

	start_ms = bench_now_ms();
	for (i = 0; i < iterations; ++i) {
		BenchLeaf* leaf = (BenchLeaf*)gc_alloc_typed(rt, thread, &BENCH_LEAF_DESC, sizeof(BenchLeaf), GC_ALLOC_DEFAULT);
		if (leaf == NULL) {
			fprintf(stderr, "allocation failed at iteration %zu\n", i);
			gc_handle_release(owner_handle);
			gc_thread_detach(thread);
			gc_runtime_destroy(rt);
			return EXIT_FAILURE;
		}
		leaf->payload[0] = (uint64_t)i;
		leaf->payload[1] = (uint64_t)(i ^ 0x5a5a5a5au);
		gc_store_ref(rt, thread, (gc_header*)owner, &owner->slots[i % BENCH_SLOT_COUNT], (gc_header*)leaf);
		if (((i + 1u) % minor_every) == 0u) {
			gc_collect_minor(rt);
			minor_runs++;
		}
	}
	gc_collect_minor(rt);
	minor_runs++;
	elapsed_ms = bench_now_ms() - start_ms;
	minor_per_run_ms = (minor_runs != 0u) ? (elapsed_ms / (double)minor_runs) : 0.0;

	printf("benchmark=%s\n", "old_to_young_writes");
	printf("algorithm=%s\n", (algo != NULL && algo->name != NULL) ? algo->name : "unknown");
	printf("iterations=%zu\n", iterations);
	printf("minor_every=%zu\n", minor_every);
	printf("minor_runs=%zu\n", minor_runs);
	printf("slot_count=%u\n", (unsigned)BENCH_SLOT_COUNT);
	printf("elapsed_ms=%.3f\n", elapsed_ms);
	printf("ns_per_store=%.2f\n", (elapsed_ms * 1000000.0) / (double)iterations);
	printf("stores_per_sec=%.2f\n", ((double)iterations * 1000.0) / elapsed_ms);
	printf("avg_minor_ms=%.4f\n", minor_per_run_ms);
	printf("minor_collections=%zu\n", rt->stats.minor_collections);
	printf("dirty_old_objects=%zu\n", rt->barriers.dirty_old_objects);
	printf("dirty_cards=%zu\n", rt->barriers.dirty_cards);
	printf("young_used_bytes=%zu\n", rt->heap.young_used);
	printf("old_object_count=%zu\n", rt->heap.old_object_count);
	printf("old_object_bytes=%zu\n", rt->heap.old_object_bytes);
	printf("peak_allocated=%zu\n", rt->stats.peak_allocated);

	gc_handle_release(owner_handle);
	gc_collect_full(rt);
	gc_thread_detach(thread);
	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}


