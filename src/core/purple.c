#include "gc_internal.h"

void gc_push_purple_adaptive(gc_runtime* rt, gc_thread_context* thread, gc_header* obj) {
	/* TODO: 未来可由 rc-cycle 算法插件复用的自适应候选集逻辑 */
	(void)rt;
	(void)thread;
	(void)obj;
}
