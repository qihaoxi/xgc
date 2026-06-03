#include "gc_internal.h"

static void gc_noop_visit_root_slot(gc_header** slot, void* ctx) {
	(void)slot;
	(void)ctx;
}

void gc_runtime_apply_default_config(gc_config* cfg) {
	if (cfg == NULL) {
		return;
	}

	if (cfg->gc_threshold_soft == 0) {
		cfg->gc_threshold_soft = GC_THRESHOLD_SOFT_DEFAULT;
	}
	if (cfg->gc_threshold_hard == 0) {
		cfg->gc_threshold_hard = GC_THRESHOLD_HARD_DEFAULT;
	}
	if (cfg->gc_young_space_size == 0) {
		cfg->gc_young_space_size = 4u * 1024u * 1024u;
	}
	if (cfg->gc_region_size == 0) {
		cfg->gc_region_size = 256u * 1024u;
	}
	if (cfg->gc_large_object_threshold == 0) {
		cfg->gc_large_object_threshold = 32u * 1024u;
	}
}

void gc_config_init_default(gc_config* cfg) {
	if (cfg == NULL) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	gc_runtime_apply_default_config(cfg);
}

gc_runtime* gc_runtime_create(const gc_config* cfg, const gc_algorithm_vtable* algo, const gc_vm_hooks* hooks) {
	gc_runtime* rt = (gc_runtime*)calloc(1, sizeof(*rt));
	if (rt == NULL) {
		return NULL;
	}

	if (cfg != NULL) {
		rt->cfg = *cfg;
	}
	gc_runtime_apply_default_config(&rt->cfg);

	if (hooks != NULL) {
		rt->hooks = *hooks;
	}

	rt->algo = (algo != NULL) ? algo : xgc_default_algorithm();
	gc_heap_init(&rt->heap, rt->cfg.gc_large_object_threshold);
	rt->worklist_capacity = 256;
	rt->worklist_data     = (gc_header**)calloc((size_t)rt->worklist_capacity, sizeof(gc_header*));
	if (rt->worklist_data == NULL) {
		free(rt);
		return NULL;
	}

	gc_barrier_set_init(&rt->barriers, rt->cfg.gc_region_size);

	GC_ATOMIC_STORE(&rt->gc_state, GC_STATE_RUNNING);

	if (rt->algo != NULL && rt->algo->global_init != NULL) {
		rt->algo->global_init(rt, &rt->cfg);
	}

	return rt;
}

void gc_runtime_destroy(gc_runtime* rt) {
	gc_handle* handle;
	gc_handle* next;

	if (rt == NULL) {
		return;
	}

	handle = rt->handles;
	while (handle != NULL) {
		next = handle->next;
		free(handle);
		handle = next;
	}

	if (rt->algo != NULL && rt->algo->global_destroy != NULL) {
		rt->algo->global_destroy(rt);
	}

	gc_barrier_set_destroy(&rt->barriers);
	free(rt->worklist_data);
	free(rt);
}

gc_thread_context* gc_thread_attach(gc_runtime* rt, void* vm_thread_ctx) {
	gc_thread_context* thread;

	if (rt == NULL) {
		return NULL;
	}

	thread = (gc_thread_context*)calloc(1, sizeof(*thread));
	if (thread == NULL) {
		return NULL;
	}

	thread->runtime       = rt;
	thread->vm_thread_ctx = vm_thread_ctx;

	if (rt->algo != NULL && rt->algo->thread_init != NULL) {
		rt->algo->thread_init(rt, thread);
	}

	return thread;
}

void gc_thread_detach(gc_thread_context* thread) {
	gc_runtime* rt;

	if (thread == NULL) {
		return;
	}

	rt = thread->runtime;
	if (rt != NULL && rt->algo != NULL && rt->algo->thread_destroy != NULL) {
		rt->algo->thread_destroy(rt, thread);
	}

	free(thread);
}

void* gc_alloc_typed(gc_runtime* rt, gc_thread_context* thread, const gc_descriptor* desc, size_t size,
                     uint32_t alloc_flags) {
	gc_header* obj = NULL;

	if (rt == NULL || desc == NULL || size < sizeof(gc_header)) {
		return NULL;
	}

	if (rt->algo != NULL && rt->algo->alloc != NULL) {
		obj = (gc_header*)rt->algo->alloc(rt, thread, desc, size, alloc_flags);
	}
	if (obj == NULL) {
		obj = (gc_header*)calloc(1, size);
	}
	if (obj == NULL) {
		return NULL;
	}

	if (obj->desc == NULL) {
		obj->desc = desc;
	}
	if (obj->size == 0) {
		obj->size = (uint32_t)size;
	}
	if (obj->kind == 0) {
		obj->kind = desc->kind;
	}
	if ((alloc_flags & GC_ALLOC_PINNED) != 0u) {
		obj->flags = (uint16_t)(obj->flags | GC_OBJECT_FLAG_PINNED);
	}

	if (rt->algo != NULL && rt->algo->post_alloc != NULL) {
		rt->algo->post_alloc(rt, obj);
	}

	return obj;
}

void gc_store_ref(gc_runtime* rt, gc_thread_context* thread, gc_header* owner, gc_header** slot, gc_header* new_value) {
	gc_header* old_value;

	if (slot == NULL) {
		return;
	}

	old_value = *slot;
	*slot     = new_value;

	if (rt != NULL && rt->algo != NULL && rt->algo->write_barrier != NULL) {
		rt->algo->write_barrier(rt, thread, owner, slot, old_value, new_value);
	}
}

gc_header* gc_load_ref(gc_runtime* rt, gc_thread_context* thread, gc_header** slot) {
	if (slot == NULL) {
		return NULL;
	}
	if (rt != NULL && rt->algo != NULL && rt->algo->read_barrier != NULL) {
		return rt->algo->read_barrier(rt, thread, slot);
	}
	return *slot;
}

void gc_collect_minor(gc_runtime* rt) {
	if (rt == NULL) {
		return;
	}
	if (rt->algo != NULL && rt->algo->collect_minor != NULL) {
		rt->algo->collect_minor(rt);
	}
	rt->stats.minor_collections++;
}

void gc_collect_major(gc_runtime* rt) {
	if (rt == NULL) {
		return;
	}
	if (rt->algo != NULL && rt->algo->collect_major != NULL) {
		rt->algo->collect_major(rt);
	} else if (rt->algo != NULL && rt->algo->collect_full != NULL) {
		rt->algo->collect_full(rt);
	}
	rt->stats.major_collections++;
}

void gc_collect_full(gc_runtime* rt) {
	if (rt == NULL) {
		return;
	}
	if (rt->algo != NULL && rt->algo->collect_full != NULL) {
		rt->algo->collect_full(rt);
	} else if (rt->hooks.scan_roots != NULL) {
		rt->hooks.scan_roots(rt->hooks.vm_ctx, gc_noop_visit_root_slot, NULL);
	}
	rt->stats.full_collections++;
}

gc_handle* gc_handle_acquire(gc_runtime* rt, gc_header* obj, uint32_t flags) {
	gc_handle* handle;

	if (rt == NULL || obj == NULL) {
		return NULL;
	}

	handle = (gc_handle*)calloc(1, sizeof(*handle));
	if (handle == NULL) {
		return NULL;
	}

	handle->runtime = rt;
	handle->target  = obj;
	handle->flags   = flags;
	handle->next    = rt->handles;
	rt->handles     = handle;

	if (rt->algo != NULL && rt->algo->pin != NULL) {
		rt->algo->pin(rt, obj);
	}

	return handle;
}

gc_header* gc_handle_get(const gc_handle* handle) {
	return (handle != NULL) ? handle->target : NULL;
}

void gc_handle_release(gc_handle* handle) {
	gc_handle** link;
	gc_runtime* rt;

	if (handle == NULL) {
		return;
	}

	rt = handle->runtime;
	if (rt != NULL) {
		link = &rt->handles;
		while (*link != NULL && *link != handle) {
			link = &(*link)->next;
		}
		if (*link == handle) {
			*link = handle->next;
		}
	}

	if (handle->runtime != NULL && handle->runtime->algo != NULL && handle->runtime->algo->unpin != NULL &&
	    handle->target != NULL) {
		handle->runtime->algo->unpin(handle->runtime, handle->target);
	}

	free(handle);
}

int gc_handle_check_invariants(const gc_handle* handle) {
	if (handle == NULL) {
		return 0;
	}
	if (handle->target != NULL) {
		if (handle->target->size == 0u) {
			return 0;
		}
		if (handle->target->desc == NULL) {
			return 0;
		}
	}
	return 1;
}
