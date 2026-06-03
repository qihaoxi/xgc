#include "gc_internal.h"

int gc_bitmap_check_invariants(const gc_bitmap* bitmap) {
	size_t expected_byte_count;

	if (bitmap == NULL) {
		return 0;
	}

	expected_byte_count = (bitmap->bit_count + 7u) / 8u;
	if (bitmap->byte_count != expected_byte_count) {
		return 0;
	}
	if ((bitmap->byte_count == 0u) != (bitmap->bits == NULL)) {
		return 0;
	}

	return 1;
}

int gc_card_table_check_invariants(const gc_card_table* table) {
	size_t i;

	if (table == NULL) {
		return 0;
	}
	if (table->count > table->capacity) {
		return 0;
	}
	if ((table->capacity == 0u) != (table->entries == NULL)) {
		return 0;
	}
	if (table->card_granularity == 0u) {
		return 0;
	}

	for (i = 0; i < table->count; ++i) {
		const gc_card_entry* entry = &table->entries[i];
		size_t               dirty_count;
		size_t               card_index;

		if (entry->owner_base == 0u) {
			return 0;
		}
		if (entry->owner_size == 0u) {
			return 0;
		}
		if (entry->card_count == 0u) {
			return 0;
		}
		if (!gc_bitmap_check_invariants(&entry->dirty_map)) {
			return 0;
		}
		if (entry->dirty_map.bit_count != entry->card_count) {
			return 0;
		}

		dirty_count = 0u;
		for (card_index = 0; card_index < entry->card_count; ++card_index) {
			if (gc_bitmap_test(&entry->dirty_map, card_index)) {
				dirty_count++;
			}
		}
		if (dirty_count != entry->dirty_card_count) {
			return 0;
		}
	}

	return 1;
}

int gc_barrier_set_check_invariants(const gc_barrier_set* barriers) {
	size_t i;
	size_t dirty_old_objects;
	size_t dirty_cards;

	if (barriers == NULL) {
		return 0;
	}
	if (!gc_card_table_check_invariants(&barriers->old_to_young)) {
		return 0;
	}

	dirty_old_objects = 0u;
	dirty_cards       = 0u;
	for (i = 0; i < barriers->old_to_young.count; ++i) {
		const gc_card_entry* entry = &barriers->old_to_young.entries[i];
		if (entry->dirty_card_count != 0u) {
			dirty_old_objects++;
			dirty_cards += entry->dirty_card_count;
		}
	}

	if (dirty_old_objects != barriers->dirty_old_objects) {
		return 0;
	}
	if (dirty_cards != barriers->dirty_cards) {
		return 0;
	}
	if (barriers->dirty_old_objects > barriers->old_to_young.count) {
		return 0;
	}

	return 1;
}

int gc_heap_check_invariants(const gc_heap* heap) {
	if (heap == NULL) {
		return 0;
	}
	if (heap->young_base == NULL) {
		if (heap->young_capacity != 0u || heap->young_used != 0u) {
			return 0;
		}
	} else if (heap->young_used > heap->young_capacity) {
		return 0;
	}
	if (heap->old_object_count == 0u && heap->old_object_bytes != 0u) {
		return 0;
	}

	return 1;
}

int gc_runtime_check_invariants(const gc_runtime* rt) {
	if (rt == NULL) {
		return 0;
	}
	if (!gc_heap_check_invariants(&rt->heap)) {
		return 0;
	}
	if (!gc_barrier_set_check_invariants(&rt->barriers)) {
		return 0;
	}
	if (rt->worklist_capacity < 0) {
		return 0;
	}
	if ((rt->worklist_capacity == 0) != (rt->worklist_data == NULL)) {
		return 0;
	}
	if (rt->stats.total_allocated > rt->stats.peak_allocated) {
		return 0;
	}

	return 1;
}

int gc_handle_check_invariants(const gc_handle* handle) {
	if (handle == NULL) {
		return 0;
	}

	/* 1. A handle must belong to a runtime. */
	if (handle->runtime == NULL) {
		return 0;
	}

	/* 2. An acquired handle must have a non-NULL target. */
	if (handle->target == NULL) {
		return 0;
	}

	/* 3. The target must carry a valid descriptor. */
	if (handle->target->desc == NULL) {
		return 0;
	}

	/* 4. The target's recorded size must be non-zero. */
	if (handle->target->size == 0u) {
		return 0;
	}

	/* 5. Cross-check: the handle must be reachable from the runtime's
	 *    handle list (i.e. it has been acquired and not yet released). */
	if (handle->runtime->handles == NULL) {
		return 0;
	}
	{
		const gc_handle* cur = handle->runtime->handles;
		int              found = 0;

		while (cur != NULL) {
			if (cur == handle) {
				found = 1;
				break;
			}
			cur = cur->next;
		}
		if (!found) {
			return 0;
		}
	}

	return 1;
}
