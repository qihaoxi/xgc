#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	gc_header  header;
	gc_header* first;
	gc_header* second;
	uint64_t   payload;
} DescriptorTestObject;

typedef struct {
	gc_header* slots[8];
	size_t     count;
} SlotCapture;

static void capture_slot(gc_header** slot, void* ctx) {
	SlotCapture* capture = (SlotCapture*)ctx;
	assert(capture != NULL);
	assert(capture->count < (sizeof(capture->slots) / sizeof(capture->slots[0])));
	capture->slots[capture->count++] = (slot != NULL) ? *slot : NULL;
}

static void descriptor_trace_slots(gc_header* obj, gc_visit_slot_fn visit_slot, void* ctx) {
	DescriptorTestObject* test_obj = (DescriptorTestObject*)obj;
	if (visit_slot == NULL) {
		return;
	}
	visit_slot(&test_obj->first, ctx);
	visit_slot(&test_obj->second, ctx);
}

static void descriptor_trace_slots_range(gc_header* obj, size_t byte_begin, size_t byte_end,
                                         gc_visit_slot_fn visit_slot, void* ctx) {
	DescriptorTestObject* test_obj      = (DescriptorTestObject*)obj;
	size_t                first_offset  = offsetof(DescriptorTestObject, first);
	size_t                second_offset = offsetof(DescriptorTestObject, second);

	if (visit_slot == NULL) {
		return;
	}
	if (byte_begin <= first_offset && first_offset < byte_end) {
		visit_slot(&test_obj->first, ctx);
	}
	if (byte_begin <= second_offset && second_offset < byte_end) {
		visit_slot(&test_obj->second, ctx);
	}
}

int main(void) {
	DescriptorTestObject obj;
	gc_descriptor        range_desc;
	gc_descriptor        fallback_desc;
	gc_header            child_a;
	gc_header            child_b;
	SlotCapture          capture;

	memset(&obj, 0, sizeof(obj));
	memset(&range_desc, 0, sizeof(range_desc));
	memset(&fallback_desc, 0, sizeof(fallback_desc));
	memset(&child_a, 0, sizeof(child_a));
	memset(&child_b, 0, sizeof(child_b));

	range_desc.fixed_size        = sizeof(obj);
	range_desc.trace_slots       = descriptor_trace_slots;
	range_desc.trace_slots_range = descriptor_trace_slots_range;
	obj.header.desc              = &range_desc;
	obj.header.size              = (uint32_t)sizeof(obj);
	obj.first                    = &child_a;
	obj.second                   = &child_b;

	memset(&capture, 0, sizeof(capture));
	gc_trace_object_slots_in_range(&obj.header, offsetof(DescriptorTestObject, first),
	                               offsetof(DescriptorTestObject, first) + sizeof(obj.first), capture_slot, &capture);
	assert(capture.count == 1u);
	assert(capture.slots[0] == &child_a);

	memset(&capture, 0, sizeof(capture));
	gc_trace_object_slots_in_range(&obj.header, offsetof(DescriptorTestObject, second),
	                               offsetof(DescriptorTestObject, second) + sizeof(obj.second), capture_slot, &capture);
	assert(capture.count == 1u);
	assert(capture.slots[0] == &child_b);

	memset(&capture, 0, sizeof(capture));
	gc_trace_object_slots_in_range(&obj.header, 0u, sizeof(obj), capture_slot, &capture);
	assert(capture.count == 2u);
	assert(capture.slots[0] == &child_a);
	assert(capture.slots[1] == &child_b);

	memset(&capture, 0, sizeof(capture));
	gc_trace_object_slots_in_range(&obj.header, sizeof(obj), sizeof(obj) + 8u, capture_slot, &capture);
	assert(capture.count == 0u);

	fallback_desc.fixed_size  = sizeof(obj);
	fallback_desc.trace_slots = descriptor_trace_slots;
	obj.header.desc           = &fallback_desc;

	memset(&capture, 0, sizeof(capture));
	gc_trace_object_slots_in_range(&obj.header, offsetof(DescriptorTestObject, second),
	                               offsetof(DescriptorTestObject, second) + sizeof(obj.second), capture_slot, &capture);
	assert(capture.count == 2u);
	assert(capture.slots[0] == &child_a);
	assert(capture.slots[1] == &child_b);

	memset(&capture, 0, sizeof(capture));
	gc_trace_object_slots_in_range(NULL, 0u, 16u, capture_slot, &capture);
	assert(capture.count == 0u);

	return EXIT_SUCCESS;
}
