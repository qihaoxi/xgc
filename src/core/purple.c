#include "gc_internal.h"

void gc_push_purple_adaptive(GcRuntime* rt, GcThreadContext* thread, GcHeader* obj) {
	/* TODO: 未来可由 rc-cycle 算法插件复用的自适应候选集逻辑 */
	(void)rt;
	(void)thread;
	(void)obj;
}
