#ifndef XGC_GC_TYPES_H
#define XGC_GC_TYPES_H

#include "xgc/gc_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gc_runtime        gc_runtime;
typedef struct gc_thread_context gc_thread_context;
typedef struct gc_handle         gc_handle;
typedef struct gc_descriptor     gc_descriptor;
typedef struct gc_header         gc_header;

typedef void (*GcVisitObjectFn)(gc_header* obj, void* ctx);
typedef void (*gc_visit_slot_fn)(gc_header** slot, void* ctx);

typedef void (*gc_trace_slots_fn)(gc_header* obj, gc_visit_slot_fn visit_slot, void* ctx);
typedef void (*gc_trace_slots_range_fn)(gc_header* obj, size_t byte_begin, size_t byte_end, gc_visit_slot_fn visit_slot,
                                        void* ctx);
typedef void (*gc_trace_edges_fn)(gc_header* obj, GcVisitObjectFn visit_obj, void* ctx);
typedef void (*gc_finalize_fn)(gc_header* obj);
typedef void (*gc_scan_roots_fn)(void* vm_ctx, gc_visit_slot_fn visit_root_slot, void* ctx);

enum {
	GC_DESC_FLAG_CONTAINS_REFS = 1u << 0,
	GC_DESC_FLAG_HAS_FINALIZER = 1u << 1,
	GC_DESC_FLAG_MOVABLE       = 1u << 2,
};

enum {
	GC_OBJECT_FLAG_PINNED      = 1u << 0,
	GC_OBJECT_FLAG_FINALIZABLE = 1u << 1,
	GC_OBJECT_FLAG_WEAK_REF    = 1u << 2,
};

enum {
	GC_ALLOC_DEFAULT = 0u,
	GC_ALLOC_PINNED  = 1u << 0,
	GC_ALLOC_ATOMIC  = 1u << 1,
};

enum {
	GC_HANDLE_DEFAULT = 0u,
	GC_HANDLE_PINNED  = 1u << 0,
};

struct gc_header {
	const gc_descriptor* desc;
	uint32_t             size;
	uint16_t             flags;
	uint16_t             kind;
};

struct gc_descriptor {
	const char*             name;
	uint32_t                fixed_size;
	uint32_t                flags;
	uint16_t                kind;
	uint16_t                reserved;
	gc_trace_slots_fn       trace_slots;
	gc_trace_slots_range_fn trace_slots_range;
	gc_trace_edges_fn       trace_edges;
	gc_finalize_fn          finalize;
};

typedef struct gc_config {
	size_t   gc_threshold_soft;
	size_t   gc_threshold_hard;
	size_t   gc_young_space_size;
	size_t   gc_region_size;
	size_t   gc_large_object_threshold;
	uint32_t gc_flags;
	int      gc_debug_enable;
} gc_config;

typedef struct gc_vm_hooks {
	gc_scan_roots_fn scan_roots;
	void*            vm_ctx;
} gc_vm_hooks;

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_TYPES_H */
