#include "xgc/gc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct DemoNode {
	GcHeader         gc;
	struct DemoNode* next;
	const char*      name;
} DemoNode;

typedef struct {
	DemoNode* root;
} DemoVm;

static void demo_trace_slots(GcHeader* obj, GcVisitSlotFn visit_slot, void* ctx) {
	DemoNode* node = (DemoNode*)obj;
	if (node->next != NULL) {
		visit_slot((GcHeader**)&node->next, ctx);
	}
}

static void demo_trace_slots_range(GcHeader* obj, size_t byte_begin, size_t byte_end, GcVisitSlotFn visit_slot, void* ctx) {
	DemoNode* node = (DemoNode*)obj;
	size_t    next_offset;

	if (visit_slot == NULL) {
		return;
	}

	next_offset = offsetof(DemoNode, next);
	if (byte_begin <= next_offset && next_offset < byte_end && node->next != NULL) {
		visit_slot((GcHeader**)&node->next, ctx);
	}
}

static void demo_trace_edges(GcHeader* obj, GcVisitObjectFn visit_obj, void* ctx) {
	DemoNode* node = (DemoNode*)obj;
	if (node->next != NULL) {
		visit_obj((GcHeader*)node->next, ctx);
	}
}

static void demo_finalize(GcHeader* obj) {
	DemoNode* node = (DemoNode*)obj;
	printf("finalize: %s\n", node->name);
}

static void demo_scan_roots(void* vm_ctx, GcVisitSlotFn visit_root_slot, void* ctx) {
	DemoVm* vm = (DemoVm*)vm_ctx;
	visit_root_slot((GcHeader**)&vm->root, ctx);
}

static const GcDescriptor DEMO_NODE_DESC = {
	.name        = "GenCopyDemoNode",
	.fixed_size  = sizeof(DemoNode),
	.flags       = GC_DESC_FLAG_CONTAINS_REFS | GC_DESC_FLAG_HAS_FINALIZER,
	.kind        = 9,
	.trace_slots = demo_trace_slots,
	.trace_slots_range = demo_trace_slots_range,
	.trace_edges = demo_trace_edges,
	.finalize    = demo_finalize,
};

static DemoNode* demo_new_node(GcRuntime* rt, GcThreadContext* thread, const char* name) {
	DemoNode* node = (DemoNode*)gc_alloc_typed(rt, thread, &DEMO_NODE_DESC, sizeof(DemoNode), GC_ALLOC_DEFAULT);
	if (node == NULL) {
		return NULL;
	}
	node->next = NULL;
	node->name = name;
	return node;
}

int main(void) {
	GcConfig         cfg;
	GcVmHooks        hooks;
	GcRuntime*       rt;
	GcThreadContext* thread;
	DemoVm           vm = { 0 };
	DemoNode*        a;
	DemoNode*        b;
	DemoNode*        c;
	DemoNode*        d;
	uintptr_t        old_root;

	gc_config_init_default(&cfg);
	cfg.gc_young_space_size = 256;
	hooks.scan_roots        = demo_scan_roots;
	hooks.vm_ctx            = &vm;

	rt     = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	thread = gc_thread_attach(rt, &vm);
	if (rt == NULL || thread == NULL) {
		fprintf(stderr, "failed to create runtime\n");
		return EXIT_FAILURE;
	}

	a = demo_new_node(rt, thread, "A");
	b = demo_new_node(rt, thread, "B");
	c = demo_new_node(rt, thread, "C");
	if (a == NULL || b == NULL || c == NULL) {
		fprintf(stderr, "allocation failed\n");
		return EXIT_FAILURE;
	}

	gc_store_ref(rt, thread, (GcHeader*)a, (GcHeader**)&a->next, (GcHeader*)b);
	gc_store_ref(rt, thread, (GcHeader*)b, (GcHeader**)&b->next, (GcHeader*)c);
	vm.root  = a;
	old_root = (uintptr_t)vm.root;

	puts("minor collection before dropping root:");
	gc_collect_minor(rt);
	printf("root moved: %s\n", ((uintptr_t)vm.root != old_root) ? "yes" : "no");

	d = demo_new_node(rt, thread, "D");
	if (d == NULL) {
		fprintf(stderr, "allocation failed\n");
		return EXIT_FAILURE;
	}
	gc_store_ref(rt, thread, (GcHeader*)vm.root, (GcHeader**)&vm.root->next, (GcHeader*)d);
	puts("minor collection after old->young write barrier:");
	gc_collect_minor(rt);
	printf("root now points to: %s\n", vm.root->next != NULL ? vm.root->next->name : "<null>");

	puts("drop root and run full collection:");
	vm.root = NULL;
	gc_collect_full(rt);

	gc_thread_detach(thread);
	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}
