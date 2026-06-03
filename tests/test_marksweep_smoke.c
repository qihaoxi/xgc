#include "xgc/gc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct test_node {
	gc_header         gc;
	struct test_node* left;
	struct test_node* right;
	int               id;
} test_node;

typedef struct {
	test_node* roots[8];
	int        root_count;
	int        finalized_count;
} test_vm;

static test_vm* g_test_vm;

static void test_trace_slots(gc_header* obj, gc_visit_slot_fn visit_slot, void* ctx) {
	test_node* node = (test_node*)obj;
	if (node->left != NULL) {
		visit_slot((gc_header**)&node->left, ctx);
	}
	if (node->right != NULL) {
		visit_slot((gc_header**)&node->right, ctx);
	}
}

static void test_trace_edges(gc_header* obj, GcVisitObjectFn visit_obj, void* ctx) {
	test_node* node = (test_node*)obj;
	if (node->left != NULL) {
		visit_obj((gc_header*)node->left, ctx);
	}
	if (node->right != NULL) {
		visit_obj((gc_header*)node->right, ctx);
	}
}

static void test_finalize(gc_header* obj) {
	test_node* node = (test_node*)obj;
	(void)node;
}

static void test_scan_roots(void* vm_ctx, gc_visit_slot_fn visit_root_slot, void* ctx) {
	test_vm* vm = (test_vm*)vm_ctx;
	int      i;

	for (i = 0; i < vm->root_count; ++i) {
		visit_root_slot((gc_header**)&vm->roots[i], ctx);
	}
}

static void counted_finalize(gc_header* obj) {
	test_node* node = (test_node*)obj;
	(void)node;
	if (g_test_vm != NULL) {
		g_test_vm->finalized_count++;
	}
}

static const gc_descriptor TEST_NODE_DESC = {
	.name        = "test_node",
	.fixed_size  = sizeof(test_node),
	.flags       = GC_DESC_FLAG_CONTAINS_REFS | GC_DESC_FLAG_HAS_FINALIZER,
	.kind        = 1,
	.trace_slots = test_trace_slots,
	.trace_edges = test_trace_edges,
	.finalize    = counted_finalize,
};

static test_node* make_node(gc_runtime* rt, gc_thread_context* thread, test_vm* vm, int id) {
	test_node* node = (test_node*)gc_alloc_typed(rt, thread, &TEST_NODE_DESC, sizeof(test_node), GC_ALLOC_DEFAULT);
	assert(node != NULL);
	node->left  = NULL;
	node->right = NULL;
	node->id    = id;
	(void)vm;
	return node;
}

int main(void) {
	gc_config   cfg;
	gc_vm_hooks hooks;
	test_vm     vm = { 0 };
	test_node*  a;
	test_node*  b;
	test_node*  c;
	test_node*  x;
	test_node*  y;

	gc_config_init_default(&cfg);
	g_test_vm        = &vm;
	hooks.scan_roots = test_scan_roots;
	hooks.vm_ctx     = &vm;

	gc_runtime* rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	gc_thread_context* thread = gc_thread_attach(rt, &vm);
	assert(thread != NULL);

	a = make_node(rt, thread, &vm, 1);
	b = make_node(rt, thread, &vm, 2);
	c = make_node(rt, thread, &vm, 3);

	gc_store_ref(rt, thread, (gc_header*)a, (gc_header**)&a->left, (gc_header*)b);
	gc_store_ref(rt, thread, (gc_header*)b, (gc_header**)&b->left, (gc_header*)c);
	vm.roots[0]   = a;
	vm.root_count = 1;

	gc_handle* handle = gc_handle_acquire(rt, (gc_header*)c, GC_HANDLE_PINNED);
	assert(handle != NULL);
	assert(gc_handle_get(handle) == (gc_header*)c);

	gc_collect_full(rt);
	assert(vm.finalized_count == 0);

	vm.roots[0] = NULL;
	gc_collect_full(rt);
	assert(vm.finalized_count == 2);
	assert(gc_handle_get(handle) == (gc_header*)c);

	gc_handle_release(handle);
	handle = NULL;
	gc_collect_full(rt);
	assert(vm.finalized_count == 3);

	x = make_node(rt, thread, &vm, 4);
	y = make_node(rt, thread, &vm, 5);
	gc_store_ref(rt, thread, (gc_header*)x, (gc_header**)&x->left, (gc_header*)y);
	gc_store_ref(rt, thread, (gc_header*)y, (gc_header**)&y->left, (gc_header*)x);
	gc_collect_full(rt);
	assert(vm.finalized_count == 5);

	gc_thread_detach(thread);
	gc_runtime_destroy(rt);

	printf("marksweep smoke test passed\n");
	return EXIT_SUCCESS;
}
