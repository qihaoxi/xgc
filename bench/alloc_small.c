#include "gc_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
	gc_header header;
	uint64_t  words[4];
} BenchSmallObject;

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

static const gc_descriptor BENCH_SMALL_DESC = {
	.name              = "BenchSmallObject",
	.fixed_size        = sizeof(BenchSmallObject),
	.flags             = 0u,
	.kind              = 210u,
	.trace_slots       = NULL,
	.trace_slots_range = NULL,
	.trace_edges       = NULL,
	.finalize          = NULL,
};

int main(int argc, char** argv) {
	gc_config                 cfg;
	gc_vm_hooks               hooks;
	gc_runtime*               rt;
	gc_thread_context*        thread;
	const gc_algorithm_vtable* algo;
	size_t                    iterations;
	size_t                    collect_every;
	size_t                    i;
	double                    start_ms;
	double                    elapsed_ms;
	size_t                    object_size = sizeof(BenchSmallObject);

	iterations    = (argc > 1) ? parse_size_arg(argv[1], 500000u) : 500000u;
	collect_every = (argc > 2) ? parse_size_arg(argv[2], 0u) : 0u;

	gc_config_init_default(&cfg);
	cfg.gc_young_space_size = 4u * 1024u * 1024u;
	algo                    = xgc_default_algorithm();
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

	start_ms = bench_now_ms();
	for (i = 0; i < iterations; ++i) {
		BenchSmallObject* obj = (BenchSmallObject*)gc_alloc_typed(rt, thread, &BENCH_SMALL_DESC, object_size, GC_ALLOC_DEFAULT);
		if (obj == NULL) {
			fprintf(stderr, "allocation failed at iteration %zu\n", i);
			gc_thread_detach(thread);
			gc_runtime_destroy(rt);
			return EXIT_FAILURE;
		}
		obj->words[0] = (uint64_t)i;
		obj->words[1] = (uint64_t)(i + 1u);
		obj->words[2] = (uint64_t)(i ^ 0x55aa55aau);
		obj->words[3] = (uint64_t)(i * 3u);
		if (collect_every != 0u && ((i + 1u) % collect_every) == 0u) {
			gc_collect_minor(rt);
		}
	}
	elapsed_ms = bench_now_ms() - start_ms;

	printf("benchmark=%s\n", "alloc_small");
	printf("algorithm=%s\n", (algo != NULL && algo->name != NULL) ? algo->name : "unknown");
	printf("iterations=%zu\n", iterations);
	printf("collect_every=%zu\n", collect_every);
	printf("object_size=%zu\n", object_size);
	printf("elapsed_ms=%.3f\n", elapsed_ms);
	printf("ns_per_alloc=%.2f\n", (elapsed_ms * 1000000.0) / (double)iterations);
	printf("allocs_per_sec=%.2f\n", ((double)iterations * 1000.0) / elapsed_ms);
	printf("bytes_per_sec=%.2f\n", ((double)(iterations * object_size) * 1000.0) / elapsed_ms);
	printf("minor_collections=%zu\n", rt->stats.minor_collections);
	printf("young_used_bytes=%zu\n", rt->heap.young_used);
	printf("old_object_count=%zu\n", rt->heap.old_object_count);
	printf("old_object_bytes=%zu\n", rt->heap.old_object_bytes);

	gc_collect_full(rt);
	gc_thread_detach(thread);
	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}

