#ifndef XGC_GC_ALGORITHM_H
#define XGC_GC_ALGORITHM_H

#include "xgc/gc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcAlgorithmCaps {
	uint8_t supports_moving;
	uint8_t supports_compaction;
	uint8_t supports_generations;
	uint8_t requires_write_barrier;
	uint8_t requires_read_barrier;
	uint8_t supports_concurrent_mark;
	uint8_t supports_concurrent_relocate;
	uint8_t provides_deterministic_release;
} GcAlgorithmCaps;

typedef struct GcAlgorithmVTable {
	const char*     name;
	GcAlgorithmCaps caps;

	void (*global_init)(GcRuntime* rt, const GcConfig* cfg);
	void (*global_destroy)(GcRuntime* rt);

	void (*thread_init)(GcRuntime* rt, GcThreadContext* thread);
	void (*thread_destroy)(GcRuntime* rt, GcThreadContext* thread);

	void* (*alloc)(GcRuntime* rt, GcThreadContext* thread, const GcDescriptor* desc, size_t size, uint32_t alloc_flags);

	void (*post_alloc)(GcRuntime* rt, GcHeader* obj);

	void (*write_barrier)(GcRuntime* rt, GcThreadContext* thread, GcHeader* owner, GcHeader** slot, GcHeader* old_value,
	                      GcHeader* new_value);

	GcHeader* (*read_barrier)(GcRuntime* rt, GcThreadContext* thread, GcHeader** slot);

	void (*collect_minor)(GcRuntime* rt);
	void (*collect_major)(GcRuntime* rt);
	void (*collect_full)(GcRuntime* rt);

	void (*pin)(GcRuntime* rt, GcHeader* obj);
	void (*unpin)(GcRuntime* rt, GcHeader* obj);
} GcAlgorithmVTable;

const GcAlgorithmVTable* xgc_default_algorithm(void);
const GcAlgorithmVTable* xgc_algorithm_bacon_rajan(void);
const GcAlgorithmVTable* xgc_algorithm_marksweep_stw(void);
const GcAlgorithmVTable* xgc_algorithm_gen_copy_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_ALGORITHM_H */
