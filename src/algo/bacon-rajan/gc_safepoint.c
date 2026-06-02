#include "gc_internal.h"

void gc_enter_safepoint_and_park(GcRuntime* rt, GcThreadContext* thread) {
	/* TODO: Safepoint 挂起协议（标记到达 → 保存栈根 → 自旋等待 STW 完成） */
	(void)rt;
	(void)thread;
}
