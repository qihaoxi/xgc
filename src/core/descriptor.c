#include "gc_internal.h"

void gc_trace_object_slots_in_range(gc_header* obj, size_t byte_begin, size_t byte_end, gc_visit_slot_fn visit_slot,
                                    void* ctx) {
	const gc_descriptor* desc;
	size_t               object_size;

	if (obj == NULL || visit_slot == NULL) {
		return;
	}

	desc = obj->desc;
	if (desc == NULL) {
		return;
	}

	object_size = (obj->size != 0u) ? (size_t)obj->size : (size_t)desc->fixed_size;
	if (object_size == 0u) {
		return;
	}

	if (byte_begin >= object_size) {
		return;
	}
	if (byte_end > object_size) {
		byte_end = object_size;
	}
	if (byte_begin >= byte_end) {
		return;
	}

	if (desc->trace_slots_range != NULL) {
		desc->trace_slots_range(obj, byte_begin, byte_end, visit_slot, ctx);
		return;
	}

	if (desc->trace_slots != NULL) {
		desc->trace_slots(obj, visit_slot, ctx);
	}
}
