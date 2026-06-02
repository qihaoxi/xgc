#include "gc_internal.h"

void gc_barrier_set_init(GcBarrierSet* barriers, size_t card_granularity) {
	if (barriers == NULL) {
		return;
	}

	memset(barriers, 0, sizeof(*barriers));
	gc_card_table_init(&barriers->old_to_young, card_granularity);
}

void gc_barrier_set_destroy(GcBarrierSet* barriers) {
	if (barriers == NULL) {
		return;
	}

	gc_card_table_destroy(&barriers->old_to_young);
	memset(barriers, 0, sizeof(*barriers));
}

int gc_barrier_register_owner(GcRuntime* rt, const GcHeader* owner, size_t owner_size) {
	if (rt == NULL) {
		return 0;
	}

	return gc_card_table_register_owner(rt, &rt->barriers.old_to_young, owner, owner_size);
}

void gc_barrier_unregister_owner(GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_unregister_owner(rt, &rt->barriers.old_to_young, owner);
}

void gc_barrier_mark_slot_dirty(GcRuntime* rt, const GcHeader* owner, const void* slot_addr) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_mark_slot(rt, &rt->barriers.old_to_young, owner, slot_addr);
}

void gc_barrier_mark_owner_dirty(GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_mark_owner(rt, &rt->barriers.old_to_young, owner);
}

int gc_barrier_is_owner_dirty(const GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return 0;
	}

	return gc_card_table_owner_is_dirty(&rt->barriers.old_to_young, owner);
}

void gc_barrier_clear_owner_dirty(GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_clear_owner(rt, &rt->barriers.old_to_young, owner);
}

void gc_barrier_visit_dirty_cards(const GcRuntime* rt, GcVisitDirtyCardFn visit, void* ctx) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_visit_dirty(&rt->barriers.old_to_young, visit, ctx);
}
