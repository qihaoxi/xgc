#include "gc_internal.h"

static void gc_bacon_rajan_noop_root_slot(GcHeader** slot, void* ctx) {
	(void)slot;
	(void)ctx;
}

void gc_bacon_rajan_collect_minor(GcRuntime* rt) {
	(void)rt;
}

void gc_bacon_rajan_collect_major(GcRuntime* rt) {
	(void)rt;
}

void gc_bacon_rajan_collect_full(GcRuntime* rt) {
	if (rt == NULL || rt->hooks.scan_roots == NULL) {
		return;
	}

	rt->hooks.scan_roots(rt->hooks.vm_ctx, gc_bacon_rajan_noop_root_slot, NULL);
}
