#include "gc_internal.h"

static void gc_noop_visit_root_slot(GcHeader** slot, void* ctx) {
	(void)slot;
	(void)ctx;
}

void gc_runtime_apply_default_config(GcConfig* cfg) {
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

void gc_config_init_default(GcConfig* cfg) {
	if (cfg == NULL) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	gc_runtime_apply_default_config(cfg);
}

GcRuntime* gc_runtime_create(const GcConfig* cfg, const GcAlgorithmVTable* algo, const GcVmHooks* hooks) {
	GcRuntime* rt = (GcRuntime*)calloc(1, sizeof(*rt));
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
	rt->worklist_data     = (GcHeader**)calloc((size_t)rt->worklist_capacity, sizeof(GcHeader*));
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

void gc_runtime_destroy(GcRuntime* rt) {
	GcHandle* handle;
	GcHandle* next;

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

GcThreadContext* gc_thread_attach(GcRuntime* rt, void* vm_thread_ctx) {
	GcThreadContext* thread;

	if (rt == NULL) {
		return NULL;
	}

	thread = (GcThreadContext*)calloc(1, sizeof(*thread));
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

void gc_thread_detach(GcThreadContext* thread) {
	GcRuntime* rt;

	if (thread == NULL) {
		return;
	}

	rt = thread->runtime;
	if (rt != NULL && rt->algo != NULL && rt->algo->thread_destroy != NULL) {
		rt->algo->thread_destroy(rt, thread);
	}

	free(thread);
}

void* gc_alloc_typed(GcRuntime* rt, GcThreadContext* thread, const GcDescriptor* desc, size_t size,
                     uint32_t alloc_flags) {
	GcHeader* obj = NULL;

	if (rt == NULL || desc == NULL || size < sizeof(GcHeader)) {
		return NULL;
	}

	if (rt->algo != NULL && rt->algo->alloc != NULL) {
		obj = (GcHeader*)rt->algo->alloc(rt, thread, desc, size, alloc_flags);
	}
	if (obj == NULL) {
		obj = (GcHeader*)calloc(1, size);
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

void gc_store_ref(GcRuntime* rt, GcThreadContext* thread, GcHeader* owner, GcHeader** slot, GcHeader* new_value) {
	GcHeader* old_value;

	if (slot == NULL) {
		return;
	}

	old_value = *slot;
	*slot     = new_value;

	if (rt != NULL && rt->algo != NULL && rt->algo->write_barrier != NULL) {
		rt->algo->write_barrier(rt, thread, owner, slot, old_value, new_value);
	}
}

GcHeader* gc_load_ref(GcRuntime* rt, GcThreadContext* thread, GcHeader** slot) {
	if (slot == NULL) {
		return NULL;
	}
	if (rt != NULL && rt->algo != NULL && rt->algo->read_barrier != NULL) {
		return rt->algo->read_barrier(rt, thread, slot);
	}
	return *slot;
}

void gc_collect_minor(GcRuntime* rt) {
	if (rt == NULL) {
		return;
	}
	if (rt->algo != NULL && rt->algo->collect_minor != NULL) {
		rt->algo->collect_minor(rt);
	}
	rt->stats.minor_collections++;
}

void gc_collect_major(GcRuntime* rt) {
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

void gc_collect_full(GcRuntime* rt) {
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

GcHandle* gc_handle_acquire(GcRuntime* rt, GcHeader* obj, uint32_t flags) {
	GcHandle* handle;

	if (rt == NULL || obj == NULL) {
		return NULL;
	}

	handle = (GcHandle*)calloc(1, sizeof(*handle));
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

GcHeader* gc_handle_get(const GcHandle* handle) {
	return (handle != NULL) ? handle->target : NULL;
}

void gc_handle_release(GcHandle* handle) {
	GcHandle** link;
	GcRuntime* rt;

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
