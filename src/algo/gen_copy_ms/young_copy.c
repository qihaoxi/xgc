#include "gc_internal.h"

typedef struct gc_gen_old_node {
	gc_header*              obj;
	uint8_t                 marked;
	struct gc_gen_old_node* next;
} gc_gen_old_node;

typedef struct gc_gen_forward {
	gc_header*             from;
	gc_header*             to;
	struct gc_gen_forward* next;
} gc_gen_forward;

typedef struct {
	uint8_t*         nursery_space;
	size_t           nursery_capacity;
	size_t           nursery_used;
	gc_gen_old_node* old_objects;
	gc_gen_forward*  forwarded;
} gc_gen_copy_ms_state;

typedef struct {
	gc_runtime*           rt;
	gc_gen_copy_ms_state* state;
	gc_worklist*          wl;
} gc_gen_minor_ctx;

typedef struct {
	gc_runtime*           rt;
	gc_gen_copy_ms_state* state;
	gc_worklist*          wl;
} gc_gen_mark_ctx;

typedef struct {
	gc_gen_minor_ctx* minor;
	gc_header**       owners_to_clear;
	size_t            owner_count;
	size_t            owner_capacity;
} gc_gen_dirty_card_visit_ctx;

static int gc_gen_dirty_visit_remember_owner(gc_gen_dirty_card_visit_ctx* visit_ctx, gc_header* owner) {
	gc_header** new_items;
	size_t      new_capacity;
	size_t      i;

	if (visit_ctx == NULL || owner == NULL) {
		return 0;
	}

	for (i = 0; i < visit_ctx->owner_count; ++i) {
		if (visit_ctx->owners_to_clear[i] == owner) {
			return 1;
		}
	}

	if (visit_ctx->owner_count == visit_ctx->owner_capacity) {
		new_capacity = (visit_ctx->owner_capacity > 0u) ? (visit_ctx->owner_capacity * 2u) : 8u;
		new_items    = (gc_header**)realloc(visit_ctx->owners_to_clear, new_capacity * sizeof(*new_items));
		if (new_items == NULL) {
			return 0;
		}

		visit_ctx->owners_to_clear = new_items;
		visit_ctx->owner_capacity  = new_capacity;
	}

	visit_ctx->owners_to_clear[visit_ctx->owner_count++] = owner;
	return 1;
}

static void gc_gen_dirty_visit_clear_owners(gc_gen_dirty_card_visit_ctx* visit_ctx) {
	size_t i;

	if (visit_ctx == NULL || visit_ctx->minor == NULL) {
		return;
	}

	for (i = 0; i < visit_ctx->owner_count; ++i) {
		gc_barrier_clear_owner_dirty(visit_ctx->minor->rt, visit_ctx->owners_to_clear[i]);
	}

	free(visit_ctx->owners_to_clear);
	visit_ctx->owners_to_clear = NULL;
	visit_ctx->owner_count     = 0u;
	visit_ctx->owner_capacity  = 0u;
}

static gc_gen_copy_ms_state* gc_gen_state(gc_runtime* rt) {
	return (gc_gen_copy_ms_state*)((rt != NULL) ? rt->algo_state : NULL);
}

static int gc_gen_in_nursery(gc_gen_copy_ms_state* state, gc_header* obj) {
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

static gc_gen_forward* gc_gen_find_forward(gc_gen_copy_ms_state* state, gc_header* obj) {
	gc_gen_forward* it;
	for (it = (state != NULL) ? state->forwarded : NULL; it != NULL; it = it->next) {
		if (it->from == obj) {
			return it;
		}
	}
	return NULL;
}

static gc_gen_old_node* gc_gen_find_old_node(gc_gen_copy_ms_state* state, gc_header* obj) {
	gc_gen_old_node* it;
	for (it = (state != NULL) ? state->old_objects : NULL; it != NULL; it = it->next) {
		if (it->obj == obj) {
			return it;
		}
	}
	return NULL;
}

static int gc_gen_add_old_node(gc_runtime* rt, gc_gen_copy_ms_state* state, gc_header* obj) {
	gc_gen_old_node* node;
	if (rt == NULL || state == NULL || obj == NULL) {
		return 0;
	}
	node = (gc_gen_old_node*)calloc(1, sizeof(*node));
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

static void gc_gen_remove_old_node_accounting(gc_runtime* rt, gc_header* obj) {
	if (rt == NULL || obj == NULL) {
		return;
	}
	gc_barrier_unregister_owner(rt, obj);
	gc_heap_record_old_free(rt, obj->size);
}

static void gc_gen_clear_forwarding(gc_gen_copy_ms_state* state) {
	gc_gen_forward* it;
	gc_gen_forward* next;
	if (state == NULL) {
		return;
	}
	for (it = state->forwarded; it != NULL; it = next) {
		next = it->next;
		free(it);
	}
	state->forwarded = NULL;
}

static gc_header* gc_gen_promote(gc_gen_minor_ctx* ctx, gc_header* obj) {
	gc_gen_forward* fwd;
	gc_header*      copy;
	gc_gen_forward* node;
	if (ctx == NULL || obj == NULL || !gc_gen_in_nursery(ctx->state, obj)) {
		return obj;
	}
	fwd = gc_gen_find_forward(ctx->state, obj);
	if (fwd != NULL) {
		return fwd->to;
	}
	copy = (gc_header*)malloc(obj->size);
	if (copy == NULL) {
		return obj;
	}
	memcpy(copy, obj, obj->size);
	node = (gc_gen_forward*)calloc(1, sizeof(*node));
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
	gc_worklist_push(ctx->wl, copy);
	return copy;
}

static void gc_gen_evacuate_slot(gc_header** slot, void* ctx) {
	gc_gen_minor_ctx* minor = (gc_gen_minor_ctx*)ctx;
	if (slot == NULL || *slot == NULL) {
		return;
	}
	*slot = gc_gen_promote(minor, *slot);
}

static void gc_gen_minor_scan_handles(gc_gen_minor_ctx* ctx) {
	gc_handle* handle;
	if (ctx == NULL || ctx->rt == NULL) {
		return;
	}
	for (handle = ctx->rt->handles; handle != NULL; handle = handle->next) {
		handle->target = gc_gen_promote(ctx, handle->target);
	}
}

static void gc_gen_minor_visit_dirty_card(gc_header* owner, size_t card_index, void* ctx) {
	gc_gen_dirty_card_visit_ctx* visit_ctx = (gc_gen_dirty_card_visit_ctx*)ctx;
	gc_gen_minor_ctx*            minor;
	size_t                       card_size;
	size_t                       byte_begin;
	size_t                       byte_end;

	if (visit_ctx == NULL || owner == NULL) {
		return;
	}

	minor = visit_ctx->minor;
	if (minor == NULL || minor->rt == NULL) {
		return;
	}

	card_size  = minor->rt->barriers.old_to_young.card_granularity;
	byte_begin = card_index * card_size;
	byte_end   = byte_begin + card_size;
	gc_trace_object_slots_in_range(owner, byte_begin, byte_end, gc_gen_evacuate_slot, minor);
	if (!gc_gen_dirty_visit_remember_owner(visit_ctx, owner)) {
		/* best-effort：即使无法记录 clear 列表，也不影响本轮可达性正确性 */
	}
}

static void gc_gen_minor_scan_dirty_cards(gc_gen_minor_ctx* ctx) {
	gc_gen_dirty_card_visit_ctx visit_ctx;

	if (ctx == NULL || ctx->rt == NULL) {
		return;
	}

	memset(&visit_ctx, 0, sizeof(visit_ctx));
	visit_ctx.minor = ctx;
	gc_barrier_visit_dirty_cards(ctx->rt, gc_gen_minor_visit_dirty_card, &visit_ctx);
	gc_gen_dirty_visit_clear_owners(&visit_ctx);
}

static void gc_gen_minor_drain(gc_gen_minor_ctx* ctx) {
	gc_header* obj;
	while ((obj = gc_worklist_pop(ctx->wl)) != NULL) {
		if (obj->desc != NULL && obj->desc->trace_slots != NULL) {
			obj->desc->trace_slots(obj, gc_gen_evacuate_slot, ctx);
		}
	}
}

static void gc_gen_finalize_dead_nursery(gc_runtime* rt, gc_gen_copy_ms_state* state, size_t used_before) {
	uint8_t* cursor;
	uint8_t* end;
	if (rt == NULL || state == NULL || state->nursery_space == NULL) {
		return;
	}
	cursor = state->nursery_space;
	end    = state->nursery_space + used_before;
	while (cursor < end) {
		gc_header* obj = (gc_header*)cursor;
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

static void gc_gen_mark_old(gc_gen_mark_ctx* ctx, gc_header* obj) {
	gc_gen_old_node* node;
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

static void gc_gen_mark_root_slot(gc_header** slot, void* ctx) {
	gc_gen_mark_ctx* mark = (gc_gen_mark_ctx*)ctx;
	if (slot == NULL) {
		return;
	}
	gc_gen_mark_old(mark, *slot);
}

static void gc_gen_mark_child_slot(gc_header** slot, void* ctx) {
	gc_gen_mark_ctx* mark = (gc_gen_mark_ctx*)ctx;
	if (slot == NULL) {
		return;
	}
	gc_gen_mark_old(mark, *slot);
}

static void gc_gen_mark_handles(gc_runtime* rt, gc_gen_mark_ctx* ctx) {
	gc_handle* handle;
	if (rt == NULL || ctx == NULL) {
		return;
	}
	for (handle = rt->handles; handle != NULL; handle = handle->next) {
		gc_gen_mark_old(ctx, handle->target);
	}
}

static void gc_gen_mark_drain(gc_gen_mark_ctx* ctx) {
	gc_header* obj;
	while ((obj = gc_worklist_pop(ctx->wl)) != NULL) {
		if (obj->desc != NULL && obj->desc->trace_slots != NULL) {
			obj->desc->trace_slots(obj, gc_gen_mark_child_slot, ctx);
		}
	}
}

static void gc_gen_sweep_old(gc_runtime* rt, gc_gen_copy_ms_state* state) {
	gc_gen_old_node** link = &state->old_objects;
	while (*link != NULL) {
		gc_gen_old_node* node = *link;
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

static void gc_gen_collect_minor_impl(gc_runtime* rt) {
	gc_gen_copy_ms_state* state = gc_gen_state(rt);
	gc_worklist           wl;
	gc_gen_minor_ctx      ctx;
	size_t                used_before;
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
	gc_gen_minor_scan_dirty_cards(&ctx);
	gc_gen_minor_drain(&ctx);
	rt->worklist_data     = wl.data;
	rt->worklist_capacity = wl.capacity;
	gc_gen_finalize_dead_nursery(rt, state, used_before);
	state->nursery_used = 0u;
	gc_heap_reset_young(rt);
	gc_gen_clear_forwarding(state);
}

static void gc_gen_collect_full_impl(gc_runtime* rt) {
	gc_gen_copy_ms_state* state = gc_gen_state(rt);
	gc_worklist           wl;
	gc_gen_mark_ctx       ctx;
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

static void gc_gen_global_init(gc_runtime* rt, const gc_config* cfg) {
	gc_gen_copy_ms_state* state;
	size_t                nursery_size =
        (cfg != NULL && cfg->gc_young_space_size != 0u) ? cfg->gc_young_space_size : (4u * 1024u * 1024u);
	if (rt == NULL) {
		return;
	}
	state = (gc_gen_copy_ms_state*)calloc(1, sizeof(*state));
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

static void gc_gen_global_destroy(gc_runtime* rt) {
	gc_gen_copy_ms_state* state = gc_gen_state(rt);
	gc_gen_old_node*      node;
	gc_gen_old_node*      next;
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

static void gc_gen_thread_init(gc_runtime* rt, gc_thread_context* thread) {
	(void)rt;
	(void)thread;
}

static void gc_gen_thread_destroy(gc_runtime* rt, gc_thread_context* thread) {
	(void)rt;
	(void)thread;
}

static void* gc_gen_alloc(gc_runtime* rt, gc_thread_context* thread, const gc_descriptor* desc, size_t size,
                          uint32_t alloc_flags) {
	gc_gen_copy_ms_state* state = gc_gen_state(rt);
	gc_header*            obj;
	size_t                aligned_size;
	(void)thread;
	if (rt == NULL || state == NULL || desc == NULL || size < sizeof(gc_header)) {
		return NULL;
	}
	aligned_size = (size + (sizeof(void*) - 1u)) & ~(sizeof(void*) - 1u);
	if ((alloc_flags & GC_ALLOC_PINNED) == 0u && aligned_size <= state->nursery_capacity / 2u) {
		if (state->nursery_used + aligned_size > state->nursery_capacity) {
			gc_gen_collect_minor_impl(rt);
		}
		if (state->nursery_used + aligned_size <= state->nursery_capacity) {
			obj = (gc_header*)(state->nursery_space + state->nursery_used);
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
	obj = (gc_header*)calloc(1, aligned_size);
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

static void gc_gen_post_alloc(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	(void)obj;
}

static void gc_gen_write_barrier(gc_runtime* rt, gc_thread_context* thread, gc_header* owner, gc_header** slot,
                                 gc_header* old_value, gc_header* new_value) {
	gc_gen_copy_ms_state* state;
	(void)thread;
	(void)old_value;
	if (rt == NULL || owner == NULL || slot == NULL || new_value == NULL) {
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
	gc_barrier_mark_slot_dirty(rt, owner, slot);
}

static gc_header* gc_gen_read_barrier(gc_runtime* rt, gc_thread_context* thread, gc_header** slot) {
	(void)rt;
	(void)thread;
	return (slot != NULL) ? *slot : NULL;
}

static void gc_gen_collect_minor(gc_runtime* rt) {
	gc_gen_collect_minor_impl(rt);
}

static void gc_gen_collect_major(gc_runtime* rt) {
	gc_gen_collect_full_impl(rt);
}

static void gc_gen_collect_full(gc_runtime* rt) {
	gc_gen_collect_full_impl(rt);
}

static void gc_gen_pin(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags | GC_OBJECT_FLAG_PINNED);
	}
}

static void gc_gen_unpin(gc_runtime* rt, gc_header* obj) {
	(void)rt;
	if (obj != NULL) {
		obj->flags = (uint16_t)(obj->flags & ~GC_OBJECT_FLAG_PINNED);
	}
}

static const gc_algorithm_vtable GC_GEN_COPY_MS_VTABLE = {
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

const gc_algorithm_vtable* xgc_algorithm_gen_copy_ms(void) {
	return &GC_GEN_COPY_MS_VTABLE;
}

const gc_algorithm_vtable* xgc_default_algorithm(void) {
	return xgc_algorithm_gen_copy_ms();
}
