#ifndef XGC_GC_ALGORITHM_H
#define XGC_GC_ALGORITHM_H

#include "xgc/gc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gc_algorithm_caps {
	uint8_t supports_moving;
	uint8_t supports_compaction;
	uint8_t supports_generations;
	uint8_t requires_write_barrier;
	uint8_t requires_read_barrier;
	uint8_t supports_concurrent_mark;
	uint8_t supports_concurrent_relocate;
	uint8_t provides_deterministic_release;
} gc_algorithm_caps;

typedef struct gc_algorithm_v_table {
	const char*       name;
	gc_algorithm_caps caps;

	void (*global_init)(gc_runtime* rt, const gc_config* cfg);
	void (*global_destroy)(gc_runtime* rt);

	void (*thread_init)(gc_runtime* rt, gc_thread_context* thread);
	void (*thread_destroy)(gc_runtime* rt, gc_thread_context* thread);

	void* (*alloc)(gc_runtime* rt, gc_thread_context* thread, const gc_descriptor* desc, size_t size,
	               uint32_t alloc_flags);

	void (*post_alloc)(gc_runtime* rt, gc_header* obj);

	void (*write_barrier)(gc_runtime* rt, gc_thread_context* thread, gc_header* owner, gc_header** slot,
	                      gc_header* old_value, gc_header* new_value);

	gc_header* (*read_barrier)(gc_runtime* rt, gc_thread_context* thread, gc_header** slot);

	void (*collect_minor)(gc_runtime* rt);
	void (*collect_major)(gc_runtime* rt);
	void (*collect_full)(gc_runtime* rt);

	void (*pin)(gc_runtime* rt, gc_header* obj);
	void (*unpin)(gc_runtime* rt, gc_header* obj);
} gc_algorithm_vtable;

const gc_algorithm_vtable* xgc_default_algorithm(void);
const gc_algorithm_vtable* xgc_algorithm_bacon_rajan(void);
const gc_algorithm_vtable* xgc_algorithm_marksweep_stw(void);
const gc_algorithm_vtable* xgc_algorithm_gen_copy_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_ALGORITHM_H */
