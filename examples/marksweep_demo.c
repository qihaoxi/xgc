#include "xgc/gc.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct demo_node {
	gc_header         gc;
	struct demo_node* next;
	const char*       name;
} demo_node;

typedef struct {
	demo_node* root;
} DemoVm;

static void demo_trace_slots(gc_header* obj, gc_visit_slot_fn visit_slot, void* ctx) {
	demo_node* node = (demo_node*)obj;
	if (node->next != NULL) {
		visit_slot((gc_header**)&node->next, ctx);
	}
}

static void demo_trace_edges(gc_header* obj, GcVisitObjectFn visit_obj, void* ctx) {
	demo_node* node = (demo_node*)obj;
	if (node->next != NULL) {
		visit_obj((gc_header*)node->next, ctx);
	}
}

static void demo_finalize(gc_header* obj) {
	demo_node* node = (demo_node*)obj;
	printf("finalize: %s\n", node->name);
}

static void demo_scan_roots(void* vm_ctx, gc_visit_slot_fn visit_root_slot, void* ctx) {
	DemoVm* vm = (DemoVm*)vm_ctx;
	visit_root_slot((gc_header**)&vm->root, ctx);
}

static const gc_descriptor DEMO_NODE_DESC = {
	.name        = "demo_node",
	.fixed_size  = sizeof(demo_node),
	.flags       = GC_DESC_FLAG_CONTAINS_REFS | GC_DESC_FLAG_HAS_FINALIZER,
	.kind        = 7,
	.trace_slots = demo_trace_slots,
	.trace_edges = demo_trace_edges,
	.finalize    = demo_finalize,
};

static demo_node* demo_new_node(gc_runtime* rt, gc_thread_context* thread, const char* name) {
	demo_node* node = (demo_node*)gc_alloc_typed(rt, thread, &DEMO_NODE_DESC, sizeof(demo_node), GC_ALLOC_DEFAULT);
	if (node == NULL) {
		return NULL;
	}
	node->next = NULL;
	node->name = name;
	return node;
}

int main(void) {
	gc_config          cfg;
	gc_vm_hooks        hooks;
	gc_runtime*        rt;
	gc_thread_context* thread;
	DemoVm             vm = { 0 };
	demo_node*         a;
	demo_node*         b;
	demo_node*         c;

	gc_config_init_default(&cfg);
	hooks.scan_roots = demo_scan_roots;
	hooks.vm_ctx     = &vm;

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

	gc_store_ref(rt, thread, (gc_header*)a, (gc_header**)&a->next, (gc_header*)b);
	gc_store_ref(rt, thread, (gc_header*)b, (gc_header**)&b->next, (gc_header*)c);
	vm.root = a;

	puts("first full collection: all nodes are reachable");
	gc_collect_full(rt);

	puts("drop root and collect again:");
	vm.root = NULL;
	gc_collect_full(rt);

	gc_thread_detach(thread);
	gc_runtime_destroy(rt);
	return EXIT_SUCCESS;
}
