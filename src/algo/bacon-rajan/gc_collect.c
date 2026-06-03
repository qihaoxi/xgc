#include "gc_internal.h"

static void gc_bacon_rajan_noop_root_slot(gc_header** slot, void* ctx) {
	(void)slot;
	(void)ctx;
}

void gc_bacon_rajan_collect_minor(gc_runtime* rt) {
	(void)rt;
}

void gc_bacon_rajan_collect_major(gc_runtime* rt) {
	(void)rt;
}

void gc_bacon_rajan_collect_full(gc_runtime* rt) {
	if (rt == NULL || rt->hooks.scan_roots == NULL) {
		return;
	}

	rt->hooks.scan_roots(rt->hooks.vm_ctx, gc_bacon_rajan_noop_root_slot, NULL);
}
