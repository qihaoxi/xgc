#include "gc_internal.h"

void gc_reclaim_object(gc_runtime* rt, gc_header* obj) {
	if (obj == NULL) {
		return;
	}

	if (obj->desc != NULL && obj->desc->finalize != NULL) {
		obj->desc->finalize(obj);
	}
	if (rt != NULL) {
		if (rt->stats.total_allocated >= obj->size) {
			rt->stats.total_allocated -= obj->size;
		} else {
			rt->stats.total_allocated = 0;
		}
	}

	free(obj);
}
