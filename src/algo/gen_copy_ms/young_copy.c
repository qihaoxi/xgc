#include "gc_internal.h"

typedef struct GcGenOldNode {
	GcHeader*            obj;
	uint8_t              marked;
	struct GcGenOldNode* next;
} GcGenOldNode;

typedef struct GcGenForward {
	GcHeader*            from;
	GcHeader*            to;
	struct GcGenForward* next;
} GcGenForward;

typedef struct {
	uint8_t*      nursery_space;
	size_t        nursery_capacity;
	size_t        nursery_used;
	GcGenOldNode* old_objects;
	GcGenForward* forwarded;
} GcGenCopyMsState;

typedef struct {
	GcRuntime*        rt;
	GcGenCopyMsState* state;
	GcWorklist*       wl;
} GcGenMinorCtx;

typedef struct {
	GcRuntime*        rt;
	GcGenCopyMsState* state;
	GcWorklist*       wl;
} GcGenMarkCtx;

static GcGenCopyMsState* gc_gen_state(GcRuntime* rt) {
	return (GcGenCopyMsState*)((rt != NULL) ? rt->algo_state : NULL);
}

static int gc_gen_in_nursery(GcGenCopyMsState* state, GcHeader* obj) {
	uintptr_t p;
	uintptr_t start;
	uintptr_t end;
	if (state == NULL || obj == NULL || state->nursery_space == NULL) {
		return 0;
	}
	p     = (uintptr_t)obj;
	start = (uintptr_t)state->nursery_space;
	end   = start + state->nursery_capacity;
	return p >= start && p < end;
}

static GcGenForward* gc_gen_find_forward(GcGenCopyMsState* state, GcHeader* obj) {
	GcGenForward* it;
	for (it = (state != NULL) ? state->forwarded : NULL; it != NULL; it = it->next) {
		if (it->from == obj) {
			return it;
		}
	}
	return NULL;
}

static GcGenOldNode* gc_gen_find_old_node(GcGenCopyMsState* state, GcHeader* obj) {
	GcGenOldNode* it;
	for (it = (state != NULL) ? state->old_objects : NULL; it != NULL; it = it->next) {
		if (it->obj == obj) {
			return it;
		}
	}
	return NULL;
}

static int gc_gen_add_old_node(GcRuntime* rt, GcGenCopyMsState* state, GcHeader* obj) {
	GcGenOldNode* node;
	if (rt == NULL || state == NULL || obj == NULL) {
		return 0;
	}
	node = (GcGenOldNode*)calloc(1, sizeof(*node));
	if (node == NULL) {
		return 0;
	}
	node->obj          = obj;
	node->next         = state->old_objects;
	state->old_objects = node;
	if (!gc_barrier_register_owner(rt, obj, obj->size)) {
		state->old_objects = node->next;
		free(node);
		return 0;
	}
	gc_heap_record_old_alloc(rt, obj->size);
	return 1;
}

static void gc_gen_remove_old_node_accounting(GcRuntime* rt, GcHeader* obj) {
	if (rt == NULL || obj == NULL) {
		return;
	}
	gc_barrier_unregister_owner(rt, obj);
	gc_heap_record_old_free(rt, obj->size);
}

static void gc_gen_clear_forwarding(GcGenCopyMsState* state) {
	GcGenForward* it;
	GcGenForward* next;
	if (state == NULL) {
		return;
	}
	for (it = state->forwarded; it != NULL; it = next) {
		next = it->next;
		free(it);
	}
	state->forwarded = NULL;
}

static GcHeader* gc_gen_promote(GcGenMinorCtx* ctx, GcHeader* obj) {
	GcGenForward* fwd;
	GcHeader*     copy;
	GcGenForward* node;
	if (ctx == NULL || obj == NULL || !gc_gen_in_nursery(ctx->state, obj)) {
		return obj;
	}
	fwd = gc_gen_find_forward(ctx->state, obj);
	if (fwd != NULL) {
		return fwd->to;
	}
	copy = (GcHeader*)malloc(obj->size);
	if (copy == NULL) {
		return obj;
	}
	memcpy(copy, obj, obj->size);
	node = (GcGenForward*)calloc(1, sizeof(*node));
	if (node == NULL) {
		free(copy);
		return obj;
	}
	if (!gc_gen_add_old_node(ctx->rt, ctx->state, copy)) {
		free(node);
		free(copy);
		return obj;
	}
	node->from            = obj;
	node->to              = copy;
	node->next            = ctx->state->forwarded;
	ctx->state->forwarded = node;
	if (ctx->rt != NULL) {
		ctx->rt->stats.total_allocated += copy->size;
		if (ctx->rt->stats.total_allocated > ctx->rt->stats.peak_allocated) {
			ctx->rt->stats.peak_allocated = ctx->rt->stats.total_allocated;
		}
	}
	gc_barrier_mark_owner_dirty(ctx->rt, copy);
	gc_worklist_push(ctx->wl, copy);
	return copy;
}

static void gc_gen_evacuate_slot(GcHeader** slot, void* ctx) {
	GcGenMinorCtx* minor = (GcGenMinorCtx*)ctx;
	if (slot == NULL || *slot == NULL) {
		return;
	}
	*slot = gc_gen_promote(minor, *slot);
}

static void gc_gen_minor_scan_handles(GcGenMinorCtx* ctx) {
	GcHandle* handle;
	if (ctx == NULL || ctx->rt == NULL) {
		return;
	}
	for (handle = ctx->rt->handles; handle != NULL; handle = handle->next) {
		handle->target = gc_gen_promote(ctx, handle->target);
	}
}

static void gc_gen_minor_scan_old_objects(GcGenMinorCtx* ctx) {
	GcGenOldNode* node;
	if (ctx == NULL || ctx->state == NULL) {
		return;
	}
	for (node = ctx->state->old_objects; node != NULL; node = node->next) {
		if (!gc_barrier_is_owner_dirty(ctx->rt, node->obj)) {
			continue;
		}
		if (node->obj != NULL && node->obj->desc != NULL && node->obj->desc->trace_slots != NULL) {
			node->obj->desc->trace_slots(node->obj, gc_gen_evacuate_slot, ctx);
		}
		gc_barrier_clear_owner_dirty(ctx->rt, node->obj);
	}
}

static void gc_gen_minor_drain(GcGenMinorCtx* ctx) {
	GcHeader* obj;
	while ((obj = gc_worklist_pop(ctx->wl)) != NULL) {
		if (obj->desc != NULL && obj->desc->trace_slots != NULL) {
			obj->desc->trace_slots(obj, gc_gen_evacuate_slot, ctx);
		}
	}
}

static void gc_gen_finalize_dead_nursery(GcRuntime* rt, GcGenCopyMsState* state, size_t used_before) {
	uint8_t* cursor;
	uint8_t* end;
	if (rt == NULL || state == NULL || state->nursery_space == NULL) {
		return;
	}
	cursor = state->nursery_space;
	end    = state->nursery_space + used_before;
	while (cursor < end) {
		GcHeader* obj = (GcHeader*)cursor;
		if (gc_gen_find_forward(state, obj) == NULL && obj->desc != NULL && obj->desc->finalize != NULL) {
			obj->desc->finalize(obj);
		}
		cursor += obj->size;
	}
	if (rt->stats.total_allocated >= used_before) {
		rt->stats.total_allocated -= used_before;
	} else {
		rt->stats.total_allocated = 0u;
	}
}

static void gc_gen_mark_old(GcGenMarkCtx* ctx, GcHeader* obj) {
	GcGenOldNode* node;
	if (ctx == NULL || obj == NULL) {
		return;
	}
	node = gc_gen_find_old_node(ctx->state, obj);
	if (node == NULL || node->marked) {
		return;
	}
	node->marked = 1u;
	gc_worklist_push(ctx->wl, obj);
}

static void gc_gen_mark_root_slot(GcHeader** slot, void* ctx) {
	GcGenMarkCtx* mark = (GcGenMarkCtx*)ctx;
	if (slot == NULL) {
		return;
	}
	gc_gen_mark_old(mark, *slot);
}

static void gc_gen_mark_child_slot(GcHeader** slot, void* ctx) {
	GcGenMarkCtx* mark = (GcGenMarkCtx*)ctx;
	if (slot == NULL) {
		return;
	}
	gc_gen_mark_old(mark, *slot);
}

static void gc_gen_mark_handles(GcRuntime* rt, GcGenMarkCtx* ctx) {
	GcHandle* handle;
	if (rt == NULL || ctx == NULL) {
		return;
	}
	for (handle = rt->handles; handle != NULL; handle = handle->next) {
		gc_gen_mark_old(ctx, handle->target);
	}
}

static void gc_gen_mark_drain(GcGenMarkCtx* ctx) {
	GcHeader* obj;
	while ((obj = gc_worklist_pop(ctx->wl)) != NULL) {
		if (obj->desc != NULL && obj->desc->trace_slots != NULL) {
			obj->desc->trace_slots(obj, gc_gen_mark_child_slot, ctx);
		}
	}
}

static void gc_gen_sweep_old(GcRuntime* rt, GcGenCopyMsState* state) {
	GcGenOldNode** link = &state->old_objects;
	while (*link != NULL) {
		GcGenOldNode* node = *link;
		if (node->marked) {
			node->marked = 0u;
			link         = &node->next;
			continue;
		}
		*link = node->next;
		gc_gen_remove_old_node_accounting(rt, node->obj);
		gc_reclaim_object(rt, node->obj);
		free(node);
	}
}

static void gc_gen_collect_minor_impl(GcRuntime* rt) {
	GcGenCopyMsState* state = gc_gen_state(rt);
	GcWorklist        wl;
	GcGenMinorCtx     ctx;
	size_t            used_before;
	if (rt == NULL || state == NULL || state->nursery_space == NULL) {
		return;
	}
	used_before = state->nursery_used;
	if (used_before == 0u) {
		return;
	}
	gc_gen_clear_forwarding(state);
	wl.data     = rt->worklist_data;
	wl.top      = 0;
	wl.capacity = rt->worklist_capacity;
	ctx.rt      = rt;
	ctx.state   = state;
	ctx.wl      = &wl;
	gc_gen_minor_scan_handles(&ctx);
	if (rt->hooks.scan_roots != NULL) {
		rt->hooks.scan_roots(rt->hooks.vm_ctx, gc_gen_evacuate_slot, &ctx);
	}
	gc_gen_minor_scan_old_objects(&ctx);
	gc_gen_minor_drain(&ctx);
	rt->worklist_data     = wl.data;
	rt->worklist_capacity = wl.capacity;
	gc_gen_finalize_dead_nursery(rt, state, used_before);
	state->nursery_used = 0u;
	gc_heap_reset_young(rt);
	gc_gen_clear_forwarding(state);
}

static void gc_gen_collect_full_impl(GcRuntime* rt) {
	GcGenCopyMsState* state = gc_gen_state(rt);
	GcWorklist        wl;
	GcGenMarkCtx      ctx;
	if (rt == NULL || state == NULL) {
		return;
	}
	gc_gen_collect_minor_impl(rt);
	wl.data     = rt->worklist_data;
	wl.top      = 0;
	wl.capacity = rt->worklist_capacity;
	ctx.rt      = rt;
	ctx.state   = state;
	ctx.wl      = &wl;
	gc_gen_mark_handles(rt, &ctx);
	if (rt->hooks.scan_roots != NULL) {
		rt->hooks.scan_roots(rt->hooks.vm_ctx, gc_gen_mark_root_slot, &ctx);
	}
	gc_gen_mark_drain(&ctx);
	rt->worklist_data     = wl.data;
	rt->worklist_capacity = wl.capacity;
	gc_gen_sweep_old(rt, state);
}

static void gc_gen_global_init(GcRuntime* rt, const GcConfig* cfg) {
	GcGenCopyMsState* state;
	size_t            nursery_size =
        (cfg != NULL && cfg->gc_young_space_size != 0u) ? cfg->gc_young_space_size : (4u * 1024u * 1024u);
	if (rt == NULL) {
		return;
	}
	state = (GcGenCopyMsState*)calloc(1, sizeof(*state));
	if (state == NULL) {
		return;
	}
	state->nursery_space = (uint8_t*)malloc(nursery_size);
	if (state->nursery_space == NULL) {
		free(state);
		return;
	}
	state->nursery_capacity = nursery_size;
	gc_heap_set_young_space(rt, state->nursery_space, nursery_size);
	rt->algo_state = state;
}

static void gc_gen_global_destroy(GcRuntime* rt) {
	GcGenCopyMsState* state = gc_gen_state(rt);
	GcGenOldNode*     node;
	GcGenOldNode*     next;
	if (state == NULL) {
		return;
	}
	for (node = state->old_objects; node != NULL; node = next) {
		next = node->next;
		gc_gen_remove_old_node_accounting(rt, node->obj);
		gc_reclaim_object(rt, node->obj);
		free(node);
	}
	gc_gen_clear_forwarding(state);
	free(state->nursery_space);
	gc_heap_set_young_space(rt, NULL, 0u);
	free(state);
	rt->algo_state = NULL;
}

static void gc_gen_thread_init(GcRuntime* rt, GcThreadContext* thread) {
	(void)rt;
	(void)thread;
}

static void gc_gen_thread_destroy(GcRuntime* rt, GcThreadContext* thread) {
	(void)rt;
	(void)thread;
}

static void* gc_gen_alloc(GcRuntime* rt, GcThreadContext* thread, const GcDescriptor* desc, size_t size,
                          uint32_t alloc_flags) {
	GcGenCopyMsState* state = gc_gen_state(rt);
	GcHeader*         obj;
	size_t            aligned_size;
	(void)thread;
	if (rt == NULL || state == NULL || desc == NULL || size < sizeof(GcHeader)) {
		return NULL;
	}
	aligned_size = (size + (sizeof(void*) - 1u)) & ~(sizeof(void*) - 1u);
	if ((alloc_flags & GC_ALLOC_PINNED) == 0u && aligned_size <= state->nursery_capacity / 2u) {
		if (state->nursery_used + aligned_size > state->nursery_capacity) {
			gc_gen_collect_minor_impl(rt);
		}
		if (state->nursery_used + aligned_size <= state->nursery_capacity) {
			obj = (GcHeader*)(state->nursery_space + state->nursery_used);
			memset(obj, 0, aligned_size);
			state->nursery_used += aligned_size;
			gc_heap_record_young_alloc(rt, aligned_size);
			obj->desc = desc;
			obj->size = (uint32_t)aligned_size;
			obj->kind = desc->kind;
			obj->flags =
			    (uint16_t)(((desc->flags & GC_DESC_FLAG_HAS_FINALIZER) != 0u) ? GC_OBJECT_FLAG_FINALIZABLE : 0u);
			rt->stats.total_allocated += aligned_size;
			if (rt->stats.total_allocated > rt->stats.peak_allocated) {
				rt->stats.peak_allocated = rt->stats.total_allocated;
			}
			return obj;
		}
	}
	obj = (GcHeader*)calloc(1, aligned_size);
	if (obj == NULL) {
		return NULL;
	}
	obj->desc  = desc;
	obj->size  = (uint32_t)aligned_size;
	obj->kind  = desc->kind;
	obj->flags = (uint16_t)((((desc->flags & GC_DESC_FLAG_HAS_FINALIZER) != 0u) ? GC_OBJECT_FLAG_FINALIZABLE : 0u) |
	                        (((alloc_flags & GC_ALLOC_PINNED) != 0u) ? GC_OBJECT_FLAG_PINNED : 0u));
	if (!gc_gen_add_old_node(rt, state, obj)) {
		free(obj);
		return NULL;
	}
	rt->stats.total_allocated += aligned_size;
	if (rt->stats.total_allocated > rt->stats.peak_allocated) {
		rt->stats.peak_allocated = rt->stats.total_allocated;
	}
	return obj;
}

static void gc_gen_post_alloc(GcRuntime* rt, GcHeader* obj) {
	(void)rt;
	(void)obj;
}

static void gc_gen_write_barrier(GcRuntime* rt, GcThreadContext* thread, GcHeader* owner, GcHeader** slot,
                                 GcHeader* old_value, GcHeader* new_value) {
	GcGenCopyMsState* state;
	(void)thread;
	(void)slot;
	(void)old_value;
	if (rt == NULL || owner == NULL || new_value == NULL) {
		return;
	}
	state = gc_gen_state(rt);
	if (state == NULL) {
		return;
	}
	if (gc_gen_in_nursery(state, owner)) {
		return;
	}
	if (!gc_gen_in_nursery(state, new_value)) {
		return;
	}
	gc_barrier_mark_owner_dirty(rt, owner);
}

static GcHeader* gc_gen_read_barrier(GcRuntime* rt, GcThreadContext* thread, GcHeader** slot) {
	(void)rt;
	(void)thread;
	return (slot != NULL) ? *slot : NULL;
}

static void gc_gen_collect_minor(GcRuntime* rt) {
	gc_gen_collect_minor_impl(rt);
}

static void gc_gen_collect_major(GcRuntime* rt) {
	gc_gen_collect_full_impl(rt);
}

static void gc_gen_collect_full(GcRuntime* rt) {
	gc_gen_collect_full_impl(rt);
}

static void gc_gen_pin(GcRuntime* rt, GcHeader* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags | GC_OBJECT_FLAG_PINNED);
	}
}

static void gc_gen_unpin(GcRuntime* rt, GcHeader* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags & ~GC_OBJECT_FLAG_PINNED);
	}
}

static const GcAlgorithmVTable GC_GEN_COPY_MS_VTABLE = {
.name = "gen-copy-ms",
.caps = {
.supports_moving = 1,
.supports_compaction = 0,
.supports_generations = 1,
.requires_write_barrier = 1,
.requires_read_barrier = 0,
.supports_concurrent_mark = 0,
.supports_concurrent_relocate = 0,
.provides_deterministic_release = 0,
},
.global_init = gc_gen_global_init,
.global_destroy = gc_gen_global_destroy,
.thread_init = gc_gen_thread_init,
.thread_destroy = gc_gen_thread_destroy,
.alloc = gc_gen_alloc,
.post_alloc = gc_gen_post_alloc,
.write_barrier = gc_gen_write_barrier,
.read_barrier = gc_gen_read_barrier,
.collect_minor = gc_gen_collect_minor,
.collect_major = gc_gen_collect_major,
.collect_full = gc_gen_collect_full,
.pin = gc_gen_pin,
.unpin = gc_gen_unpin,
};

const GcAlgorithmVTable* xgc_algorithm_gen_copy_ms(void) {
	return &GC_GEN_COPY_MS_VTABLE;
}

const GcAlgorithmVTable* xgc_default_algorithm(void) {
	return xgc_algorithm_gen_copy_ms();
}
