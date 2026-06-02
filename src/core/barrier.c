#include "gc_internal.h"

static GcCardTable* gc_barrier_old_to_young(GcRuntime* rt) {
	return (rt != NULL) ? &rt->barriers.old_to_young : NULL;
}

static const GcCardTable* gc_barrier_old_to_young_const(const GcRuntime* rt) {
	return (rt != NULL) ? &rt->barriers.old_to_young : NULL;
}

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

	return gc_card_table_register_owner(rt, gc_barrier_old_to_young(rt), owner, owner_size);
}

void gc_barrier_unregister_owner(GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_unregister_owner(rt, gc_barrier_old_to_young(rt), owner);
}

void gc_barrier_mark_slot_dirty(GcRuntime* rt, const GcHeader* owner, const void* slot_addr) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_mark_slot(rt, gc_barrier_old_to_young(rt), owner, slot_addr);
}

void gc_barrier_mark_owner_dirty(GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_mark_owner(rt, gc_barrier_old_to_young(rt), owner);
}

int gc_barrier_is_owner_dirty(const GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return 0;
	}

	return gc_card_table_owner_is_dirty(gc_barrier_old_to_young_const(rt), owner);
}

void gc_barrier_clear_owner_dirty(GcRuntime* rt, const GcHeader* owner) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_clear_owner(rt, gc_barrier_old_to_young(rt), owner);
}

void gc_barrier_visit_dirty_cards(const GcRuntime* rt, GcVisitDirtyCardFn visit, void* ctx) {
	if (rt == NULL) {
		return;
	}

	gc_card_table_visit_dirty(gc_barrier_old_to_young_const(rt), visit, ctx);
}
