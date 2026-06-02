#include "gc_internal.h"

void gc_assign_field(GcGlobalContext* global, GcThreadContext* thread, GcHeader** field, GcHeader* new_child) {
	// 1. atomic exchange field with new_child, get old_child
	GcHeader* old_child = GC_ATOMIC_XCHG(field, new_child);

	// 2. 对新孩子增加引用计数（第 2 次原子操作）
	if (new_child) {
		GC_ATOMIC_INC(&new_child->rc);
		if (new_child->color == GC_COLOR_WHITE) {
			new_child->color = GC_COLOR_BLACK;
		}
	}

	// 3. dec ref of old child
	if (old_child) {
		uint32_t current_rc = GC_ATOMIC_DEC(&old_child->rc);
		if (current_rc == 0) {
			// RC 归零: 无外部引用，直接标记为白色
			GC_ATOMIC_STORE(&old_child->color, GC_COLOR_WHITE);
		} else {
			uint8_t c = GC_ATOMIC_LOAD(&old_child->color);
			if (c == GC_COLOR_GRAY) {
				// ★ 关键: old_child 正处于 GC 线程的嫌疑子图中（GRAY）。
				//    写屏障切断了指向它的一个引用，但它仍有 RC>0（可能被栈
				//    或其他堆对象引用）。此时必须将其强制染黑——
				//    防止 GC 线程在 Phase 3 中仅凭 gc_ref==0 将其误判为垃圾。
				GC_ATOMIC_STORE(&old_child->color, GC_COLOR_BLACK);
			} else if (c != GC_COLOR_PURPLE && !old_child->in_purple_buf) {
				// RC 减少但未归零且不在嫌疑集中 → 送入紫色缓冲区
				GC_ATOMIC_STORE(&old_child->color, GC_COLOR_PURPLE);
				old_child->in_purple_buf = 1;
				gc_push_purple_adaptive(global, thread, old_child);
			}
		}
	}
}
