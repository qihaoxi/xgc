#ifndef XGC_GC_TYPES_H
#define XGC_GC_TYPES_H

#include "xgc/gc_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcRuntime       GcRuntime;
typedef struct GcThreadContext GcThreadContext;
typedef struct GcHandle        GcHandle;
typedef struct GcDescriptor    GcDescriptor;
typedef struct GcHeader        GcHeader;

typedef void (*GcVisitObjectFn)(GcHeader* obj, void* ctx);
typedef void (*GcVisitSlotFn)(GcHeader** slot, void* ctx);

typedef void (*GcTraceSlotsFn)(GcHeader* obj, GcVisitSlotFn visit_slot, void* ctx);
typedef void (*GcTraceEdgesFn)(GcHeader* obj, GcVisitObjectFn visit_obj, void* ctx);
typedef void (*GcFinalizeFn)(GcHeader* obj);
typedef void (*GcScanRootsFn)(void* vm_ctx, GcVisitSlotFn visit_root_slot, void* ctx);

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

struct GcHeader {
	const GcDescriptor* desc;
	uint32_t            size;
	uint16_t            flags;
	uint16_t            kind;
};

struct GcDescriptor {
	const char*    name;
	uint32_t       fixed_size;
	uint32_t       flags;
	uint16_t       kind;
	uint16_t       reserved;
	GcTraceSlotsFn trace_slots;
	GcTraceEdgesFn trace_edges;
	GcFinalizeFn   finalize;
};

typedef struct GcConfig {
	size_t   gc_threshold_soft;
	size_t   gc_threshold_hard;
	size_t   gc_young_space_size;
	size_t   gc_region_size;
	size_t   gc_large_object_threshold;
	uint32_t gc_flags;
	int      gc_debug_enable;
} GcConfig;

typedef struct GcVmHooks {
	GcScanRootsFn scan_roots;
	void*         vm_ctx;
} GcVmHooks;

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_TYPES_H */
