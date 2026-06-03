#include "gc_internal.h"

void gc_worklist_push(gc_worklist* wl, gc_header* obj) {
	gc_header** new_data;
	int         new_capacity;

	if (wl == NULL || obj == NULL) {
		return;
	}
	if (wl->top < wl->capacity) {
		wl->data[wl->top++] = obj;
		return;
	}

	new_capacity = (wl->capacity > 0) ? (wl->capacity * 2) : 16;
	new_data     = (gc_header**)realloc(wl->data, (size_t)new_capacity * sizeof(gc_header*));
	if (new_data == NULL) {
		return;
	}

	wl->data            = new_data;
	wl->capacity        = new_capacity;
	wl->data[wl->top++] = obj;
}

gc_header* gc_worklist_pop(gc_worklist* wl) {
	if (wl == NULL || wl->top <= 0) {
		return NULL;
	}
	return wl->data[--wl->top];
}
