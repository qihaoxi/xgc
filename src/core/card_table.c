#include "gc_internal.h"

static const size_t GC_DEFAULT_CARD_GRANULARITY = 256u;

static const GcCardEntry* gc_card_table_find_entry_const(const GcCardTable* table, const GcHeader* owner) {
	size_t    i;
	uintptr_t owner_base;

	if (table == NULL || owner == NULL) {
		return NULL;
	}

	owner_base = (uintptr_t)owner;
	for (i = 0; i < table->count; ++i) {
		const GcCardEntry* entry = &table->entries[i];
		if (entry->owner_base == owner_base) {
			return entry;
		}
	}

	return NULL;
}

static GcCardEntry* gc_card_table_find_entry(GcCardTable* table, const GcHeader* owner) {
	return (GcCardEntry*)gc_card_table_find_entry_const(table, owner);
}

static size_t gc_card_table_effective_granularity(const GcCardTable* table) {
	if (table == NULL || table->card_granularity == 0u) {
		return GC_DEFAULT_CARD_GRANULARITY;
	}

	return table->card_granularity;
}

static size_t gc_card_table_card_index_for_slot(const GcCardTable* table, const GcCardEntry* entry,
                                                const void* slot_addr) {
	size_t slot_offset;
	size_t card_index;
	size_t granularity;

	if (table == NULL || entry == NULL || slot_addr == NULL || entry->card_count == 0u) {
		return 0u;
	}

	if ((uintptr_t)slot_addr <= entry->owner_base) {
		return 0u;
	}

	granularity = gc_card_table_effective_granularity(table);
	slot_offset = (size_t)((uintptr_t)slot_addr - entry->owner_base);
	card_index  = slot_offset / granularity;
	if (card_index >= entry->card_count) {
		card_index = entry->card_count - 1u;
	}

	return card_index;
}

static void gc_card_table_account_dirty_card(GcRuntime* rt, GcCardEntry* entry) {
	if (entry == NULL) {
		return;
	}

	if (entry->dirty_card_count == 0u && rt != NULL) {
		rt->barriers.dirty_old_objects++;
	}

	entry->dirty_card_count++;
	if (rt != NULL) {
		rt->barriers.dirty_cards++;
	}
}

static void gc_card_table_unaccount_dirty_owner(GcRuntime* rt, size_t dirty_card_count) {
	if (rt == NULL || dirty_card_count == 0u) {
		return;
	}

	if (rt->barriers.dirty_old_objects > 0u) {
		rt->barriers.dirty_old_objects--;
	}

	if (dirty_card_count >= rt->barriers.dirty_cards) {
		rt->barriers.dirty_cards = 0u;
	} else {
		rt->barriers.dirty_cards -= dirty_card_count;
	}
}

typedef struct {
	GcHeader* owner;
	size_t    card_index;
} GcDirtyCardSnapshot;

void gc_card_table_init(GcCardTable* table, size_t card_granularity) {
	if (table == NULL) {
		return;
	}

	memset(table, 0, sizeof(*table));
	table->card_granularity = (card_granularity != 0u) ? card_granularity : GC_DEFAULT_CARD_GRANULARITY;
}

void gc_card_table_destroy(GcCardTable* table) {
	size_t i;

	if (table == NULL) {
		return;
	}

	for (i = 0; i < table->count; ++i) {
		gc_bitmap_destroy(&table->entries[i].dirty_map);
	}

	free(table->entries);
	memset(table, 0, sizeof(*table));
}

int gc_card_table_register_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner, size_t owner_size) {
	GcCardEntry* new_entries;
	GcCardEntry* entry;
	size_t       card_count;
	size_t       granularity;
	size_t       new_capacity;

	(void)rt;
	if (table == NULL || owner == NULL || owner_size == 0u) {
		return 0;
	}

	if (gc_card_table_find_entry(table, owner) != NULL) {
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

	granularity = gc_card_table_effective_granularity(table);
	card_count  = (owner_size + granularity - 1u) / granularity;
	if (card_count == 0u) {
		card_count = 1u;
	}

	entry = &table->entries[table->count++];
	memset(entry, 0, sizeof(*entry));
	entry->owner_base = (uintptr_t)owner;
	entry->owner_size = owner_size;
	entry->card_count = card_count;
	gc_bitmap_init(&entry->dirty_map, card_count);
	if (entry->dirty_map.bit_count != card_count) {
		table->count--;
		return 0;
	}

	return 1;
}

void gc_card_table_unregister_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner) {
	size_t i;

	if (table == NULL || owner == NULL) {
		return;
	}

	for (i = 0; i < table->count; ++i) {
		GcCardEntry* entry = &table->entries[i];
		if (entry->owner_base != (uintptr_t)owner) {
			continue;
		}

		gc_card_table_unaccount_dirty_owner(rt, entry->dirty_card_count);
		gc_bitmap_destroy(&entry->dirty_map);
		*entry = table->entries[table->count - 1u];
		table->count--;
		return;
	}
}

void gc_card_table_mark_slot(GcRuntime* rt, GcCardTable* table, const GcHeader* owner, const void* slot_addr) {
	GcCardEntry* entry;
	size_t       card_index;

	if (table == NULL || owner == NULL || slot_addr == NULL) {
		return;
	}

	entry = gc_card_table_find_entry(table, owner);
	if (entry == NULL || entry->card_count == 0u) {
		return;
	}

	card_index = gc_card_table_card_index_for_slot(table, entry, slot_addr);
	if (gc_bitmap_test(&entry->dirty_map, card_index)) {
		return;
	}

	gc_bitmap_set(&entry->dirty_map, card_index);
	gc_card_table_account_dirty_card(rt, entry);
}

void gc_card_table_mark_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner) {
	GcCardEntry* entry;
	size_t       i;

	if (table == NULL || owner == NULL) {
		return;
	}

	entry = gc_card_table_find_entry(table, owner);
	if (entry == NULL) {
		return;
	}

	for (i = 0; i < entry->card_count; ++i) {
		if (gc_bitmap_test(&entry->dirty_map, i)) {
			continue;
		}

		gc_bitmap_set(&entry->dirty_map, i);
		gc_card_table_account_dirty_card(rt, entry);
	}
}

int gc_card_table_owner_is_dirty(const GcCardTable* table, const GcHeader* owner) {
	const GcCardEntry* entry = gc_card_table_find_entry_const(table, owner);
	return (entry != NULL) ? (entry->dirty_card_count != 0u) : 0;
}

void gc_card_table_clear_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner) {
	GcCardEntry* entry;

	if (table == NULL || owner == NULL) {
		return;
	}

	entry = gc_card_table_find_entry(table, owner);
	if (entry == NULL || entry->dirty_card_count == 0u) {
		return;
	}

	gc_bitmap_clear_all(&entry->dirty_map);
	gc_card_table_unaccount_dirty_owner(rt, entry->dirty_card_count);
	entry->dirty_card_count = 0u;
}

void gc_card_table_visit_dirty(const GcCardTable* table, GcVisitDirtyCardFn visit, void* ctx) {
	GcDirtyCardSnapshot* snapshot;
	size_t               dirty_count;
	size_t               snapshot_index;
	size_t               i;

	if (table == NULL || visit == NULL) {
		return;
	}

	dirty_count = 0u;
	for (i = 0; i < table->count; ++i) {
		const GcCardEntry* entry = &table->entries[i];
		size_t             card_index;

		if (entry->dirty_card_count == 0u) {
			continue;
		}

		for (card_index = 0; card_index < entry->card_count; ++card_index) {
			if (gc_bitmap_test(&entry->dirty_map, card_index)) {
				dirty_count++;
			}
		}
	}

	if (dirty_count == 0u) {
		return;
	}

	snapshot = (GcDirtyCardSnapshot*)calloc(dirty_count, sizeof(*snapshot));
	if (snapshot == NULL) {
		return;
	}

	snapshot_index = 0u;
	for (i = 0; i < table->count; ++i) {
		const GcCardEntry* entry = &table->entries[i];
		size_t             card_index;

		if (entry->dirty_card_count == 0u) {
			continue;
		}

		for (card_index = 0; card_index < entry->card_count; ++card_index) {
			if (!gc_bitmap_test(&entry->dirty_map, card_index)) {
				continue;
			}

			snapshot[snapshot_index].owner      = (GcHeader*)entry->owner_base;
			snapshot[snapshot_index].card_index = card_index;
			snapshot_index++;
		}
	}

	for (i = 0; i < snapshot_index; ++i) {
		visit(snapshot[i].owner, snapshot[i].card_index, ctx);
	}

	free(snapshot);
}
