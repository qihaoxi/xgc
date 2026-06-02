#include "gc_internal.h"

static GcCardEntry* gc_barrier_find_entry(GcCardTable* table, const GcHeader* owner) {
	size_t    i;
	uintptr_t addr;

	if (table == NULL || owner == NULL) {
		return NULL;
	}

	addr = (uintptr_t)owner;
	for (i = 0; i < table->count; ++i) {
		GcCardEntry* entry = &table->entries[i];
		if (entry->owner_base == addr) {
			return entry;
		}
	}

	return NULL;
}

void gc_barrier_set_init(GcBarrierSet* barriers, size_t card_granularity) {
	if (barriers == NULL) {
		return;
	}

	memset(barriers, 0, sizeof(*barriers));
	barriers->old_to_young.card_granularity = (card_granularity != 0u) ? card_granularity : 256u;
}

void gc_barrier_set_destroy(GcBarrierSet* barriers) {
	if (barriers == NULL) {
		return;
	}

	free(barriers->old_to_young.entries);
	memset(barriers, 0, sizeof(*barriers));
}

int gc_barrier_register_owner(GcRuntime* rt, const GcHeader* owner, size_t owner_size) {
	GcCardTable* table;
	GcCardEntry* new_entries;
	GcCardEntry* entry;
	size_t       new_capacity;

	if (rt == NULL || owner == NULL || owner_size == 0u) {
		return 0;
	}

	table = &rt->barriers.old_to_young;
	entry = gc_barrier_find_entry(table, owner);
	if (entry != NULL) {
		entry->owner_size = owner_size;
		return 1;
	}

	if (table->count == table->capacity) {
		new_capacity = (table->capacity > 0u) ? (table->capacity * 2u) : 32u;
		new_entries  = (GcCardEntry*)realloc(table->entries, new_capacity * sizeof(GcCardEntry));
		if (new_entries == NULL) {
			return 0;
		}
		table->entries  = new_entries;
		table->capacity = new_capacity;
	}

	entry             = &table->entries[table->count++];
	entry->owner_base = (uintptr_t)owner;
	entry->owner_size = owner_size;
	entry->dirty      = 0u;
	return 1;
}

void gc_barrier_unregister_owner(GcRuntime* rt, const GcHeader* owner) {
	GcCardTable* table;
	size_t       i;

	if (rt == NULL || owner == NULL) {
		return;
	}

	table = &rt->barriers.old_to_young;
	for (i = 0; i < table->count; ++i) {
		if (table->entries[i].owner_base == (uintptr_t)owner) {
			if (table->entries[i].dirty != 0u && rt->barriers.dirty_old_objects > 0u) {
				rt->barriers.dirty_old_objects--;
			}
			table->entries[i] = table->entries[table->count - 1u];
			table->count--;
			return;
		}
	}
}

void gc_barrier_mark_owner_dirty(GcRuntime* rt, const GcHeader* owner) {
	GcCardEntry* entry;

	if (rt == NULL || owner == NULL) {
		return;
	}

	entry = gc_barrier_find_entry(&rt->barriers.old_to_young, owner);
	if (entry == NULL) {
		return;
	}
	if (entry->dirty == 0u) {
		entry->dirty = 1u;
		rt->barriers.dirty_old_objects++;
	}
}

int gc_barrier_is_owner_dirty(const GcRuntime* rt, const GcHeader* owner) {
	GcCardTable* table;
	size_t       i;

	if (rt == NULL || owner == NULL) {
		return 0;
	}

	table = (GcCardTable*)&rt->barriers.old_to_young;
	for (i = 0; i < table->count; ++i) {
		if (table->entries[i].owner_base == (uintptr_t)owner) {
			return table->entries[i].dirty != 0u;
		}
	}

	return 0;
}

void gc_barrier_clear_owner_dirty(GcRuntime* rt, const GcHeader* owner) {
	GcCardEntry* entry;

	if (rt == NULL || owner == NULL) {
		return;
	}

	entry = gc_barrier_find_entry(&rt->barriers.old_to_young, owner);
	if (entry != NULL && entry->dirty != 0u) {
		entry->dirty = 0u;
		if (rt->barriers.dirty_old_objects > 0u) {
			rt->barriers.dirty_old_objects--;
		}
	}
}
