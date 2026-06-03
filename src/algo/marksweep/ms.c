#include "gc_internal.h"

typedef struct gc_ms_node {
	gc_header*         obj;
	uint8_t            marked;
	struct gc_ms_node* next;
} gc_ms_node;

typedef struct {
	gc_ms_node* objects;
} gc_marksweep_state_state;

typedef struct {
	gc_runtime*               rt;
	gc_marksweep_state_state* state;
	gc_worklist*              wl;
} gc_marksweep_mark_ctx_ctx;

static gc_marksweep_state_state* gc_marksweep_state(gc_runtime* rt) {
	return (gc_marksweep_state_state*)((rt != NULL) ? rt->algo_state : NULL);
}

static gc_ms_node* gc_marksweep_find_node(gc_marksweep_state_state* state, gc_header* obj) {
	gc_ms_node* node = (state != NULL) ? state->objects : NULL;
	while (node != NULL) {
		if (node->obj == obj) {
			return node;
		}
		node = node->next;
	}
	return NULL;
}

static void gc_marksweep_mark_object(gc_marksweep_mark_ctx_ctx* ctx, gc_header* obj) {
	gc_ms_node* node;

	if (ctx == NULL || obj == NULL) {
		return;
	}

	node = gc_marksweep_find_node(ctx->state, obj);
	if (node == NULL || node->marked) {
		return;
	}

	node->marked = 1u;
	gc_worklist_push(ctx->wl, obj);
}

static void gc_marksweep_mark_root_slot(gc_header** slot, void* ctx) {
	gc_marksweep_mark_ctx_ctx* mark_ctx = (gc_marksweep_mark_ctx_ctx*)ctx;
	if (slot == NULL) {
		return;
	}
	gc_marksweep_mark_object(mark_ctx, *slot);
}

static void gc_marksweep_mark_child_slot(gc_header** slot, void* ctx) {
	gc_marksweep_mark_ctx_ctx* mark_ctx = (gc_marksweep_mark_ctx_ctx*)ctx;
	if (slot == NULL) {
		return;
	}
	gc_marksweep_mark_object(mark_ctx, *slot);
}

static void gc_marksweep_mark_handle_roots(gc_marksweep_mark_ctx_ctx* ctx) {
	gc_handle* handle;

	if (ctx == NULL || ctx->rt == NULL) {
		return;
	}

	handle = ctx->rt->handles;
	while (handle != NULL) {
		gc_marksweep_mark_object(ctx, handle->target);
		handle = handle->next;
	}
}

static void gc_marksweep_drain_worklist(gc_marksweep_mark_ctx_ctx* ctx) {
	gc_header* obj;

	while ((obj = gc_worklist_pop(ctx->wl)) != NULL) {
		if (obj->desc != NULL && obj->desc->trace_slots != NULL) {
			obj->desc->trace_slots(obj, gc_marksweep_mark_child_slot, ctx);
		}
	}
}

static void gc_marksweep_sweep(gc_runtime* rt, gc_marksweep_state_state* state) {
	gc_ms_node** link = &state->objects;

	while (*link != NULL) {
		gc_ms_node* node = *link;
		if (node->marked) {
			node->marked = 0u;
			link         = &node->next;
			continue;
		}

		*link = node->next;
		gc_reclaim_object(rt, node->obj);
		free(node);
	}
}

static void gc_marksweep_global_init(gc_runtime* rt, const gc_config* cfg) {
	gc_marksweep_state_state* state;
	(void)cfg;

	if (rt == NULL) {
		return;
	}

	state = (gc_marksweep_state_state*)calloc(1, sizeof(*state));
	if (state != NULL) {
		rt->algo_state = state;
	}
}

static void gc_marksweep_global_destroy(gc_runtime* rt) {
	gc_marksweep_state_state* state = gc_marksweep_state(rt);
	gc_ms_node*               node;
	gc_ms_node*               next;

	if (state == NULL) {
		return;
	}

	node = state->objects;
	while (node != NULL) {
		next = node->next;
		gc_reclaim_object(rt, node->obj);
		free(node);
		node = next;
	}

	free(state);
	if (rt != NULL) {
		rt->algo_state = NULL;
	}
}

static void gc_marksweep_thread_init(gc_runtime* rt, gc_thread_context* thread) {
	(void)rt;
	(void)thread;
}

static void gc_marksweep_thread_destroy(gc_runtime* rt, gc_thread_context* thread) {
	(void)rt;
	(void)thread;
}

static void* gc_marksweep_alloc(gc_runtime* rt, gc_thread_context* thread, const gc_descriptor* desc, size_t size,
                                uint32_t alloc_flags) {
	gc_marksweep_state_state* state = gc_marksweep_state(rt);
	gc_ms_node*               node;
	gc_header*                obj;

	(void)thread;
	(void)alloc_flags;

	if (state == NULL) {
		return NULL;
	}

	obj = (gc_header*)calloc(1, size);
	if (obj == NULL) {
		return NULL;
	}

	obj->desc = desc;
	obj->size = (uint32_t)size;
	obj->kind = (desc != NULL) ? desc->kind : 0u;
	obj->flags =
	    (uint16_t)(((desc != NULL && (desc->flags & GC_DESC_FLAG_HAS_FINALIZER) != 0u) ? GC_OBJECT_FLAG_FINALIZABLE
	                                                                                   : 0u) |
	               ((alloc_flags & GC_ALLOC_PINNED) != 0u ? GC_OBJECT_FLAG_PINNED : 0u));

	node = (gc_ms_node*)calloc(1, sizeof(*node));
	if (node == NULL) {
		free(obj);
		return NULL;
	}

	node->obj      = obj;
	node->next     = state->objects;
	state->objects = node;

	if (rt != NULL) {
		rt->stats.total_allocated += size;
		if (rt->stats.total_allocated > rt->stats.peak_allocated) {
			rt->stats.peak_allocated = rt->stats.total_allocated;
		}
	}

	return obj;
}

static void gc_marksweep_post_alloc(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	(void)obj;
}

static void gc_marksweep_write_barrier(gc_runtime* rt, gc_thread_context* thread, gc_header* owner, gc_header** slot,
                                       gc_header* old_value, gc_header* new_value) {
	(void)rt;
	(void)thread;
	(void)owner;
	(void)slot;
	(void)old_value;
	(void)new_value;
}

static gc_header* gc_marksweep_read_barrier(gc_runtime* rt, gc_thread_context* thread, gc_header** slot) {
	(void)rt;
	(void)thread;
	return (slot != NULL) ? *slot : NULL;
}

static void gc_marksweep_collect_full_impl(gc_runtime* rt) {
	gc_marksweep_state_state* state = gc_marksweep_state(rt);
	gc_worklist               wl;
	gc_marksweep_mark_ctx_ctx ctx;

	if (rt == NULL || state == NULL) {
		return;
	}

	wl.data     = rt->worklist_data;
	wl.top      = 0;
	wl.capacity = rt->worklist_capacity;

	ctx.rt    = rt;
	ctx.state = state;
	ctx.wl    = &wl;

	gc_marksweep_mark_handle_roots(&ctx);
	if (rt->hooks.scan_roots != NULL) {
		rt->hooks.scan_roots(rt->hooks.vm_ctx, gc_marksweep_mark_root_slot, &ctx);
	}
	gc_marksweep_drain_worklist(&ctx);

	rt->worklist_data     = wl.data;
	rt->worklist_capacity = wl.capacity;
	gc_marksweep_sweep(rt, state);
}

static void gc_marksweep_collect_minor(gc_runtime* rt) {
	gc_marksweep_collect_full_impl(rt);
}

static void gc_marksweep_collect_major(gc_runtime* rt) {
	gc_marksweep_collect_full_impl(rt);
}

static void gc_marksweep_collect_full(gc_runtime* rt) {
	gc_marksweep_collect_full_impl(rt);
}

static void gc_marksweep_pin(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags | GC_OBJECT_FLAG_PINNED);
	}
}

static void gc_marksweep_unpin(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags & ~GC_OBJECT_FLAG_PINNED);
	}
}

static const gc_algorithm_vtable GC_MARKSWEEP_STW_VTABLE = {
	.name = "marksweep-stw",
	.caps = {
		.supports_moving = 0,
		.supports_compaction = 0,
		.supports_generations = 0,
		.requires_write_barrier = 0,
		.requires_read_barrier = 0,
		.supports_concurrent_mark = 0,
		.supports_concurrent_relocate = 0,
		.provides_deterministic_release = 0,
	},
	.global_init = gc_marksweep_global_init,
	.global_destroy = gc_marksweep_global_destroy,
	.thread_init = gc_marksweep_thread_init,
	.thread_destroy = gc_marksweep_thread_destroy,
	.alloc = gc_marksweep_alloc,
	.post_alloc = gc_marksweep_post_alloc,
	.write_barrier = gc_marksweep_write_barrier,
	.read_barrier = gc_marksweep_read_barrier,
	.collect_minor = gc_marksweep_collect_minor,
	.collect_major = gc_marksweep_collect_major,
	.collect_full = gc_marksweep_collect_full,
	.pin = gc_marksweep_pin,
	.unpin = gc_marksweep_unpin,
};

const gc_algorithm_vtable* xgc_algorithm_marksweep_stw(void) {
	return &GC_MARKSWEEP_STW_VTABLE;
}

const gc_algorithm_vtable* xgc_default_algorithm(void) {
	return xgc_algorithm_marksweep_stw();
}
