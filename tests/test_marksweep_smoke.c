#include "xgc/gc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct TestNode {
	GcHeader         gc;
	struct TestNode* left;
	struct TestNode* right;
	int              id;
} TestNode;

typedef struct {
	TestNode* roots[8];
	int       root_count;
	int       finalized_count;
} TestVm;

static TestVm* g_test_vm;

static void test_trace_slots(GcHeader* obj, GcVisitSlotFn visit_slot, void* ctx) {
	TestNode* node = (TestNode*)obj;
	if (node->left != NULL) {
		visit_slot((GcHeader**)&node->left, ctx);
	}
	if (node->right != NULL) {
		visit_slot((GcHeader**)&node->right, ctx);
	}
}

static void test_trace_edges(GcHeader* obj, GcVisitObjectFn visit_obj, void* ctx) {
	TestNode* node = (TestNode*)obj;
	if (node->left != NULL) {
		visit_obj((GcHeader*)node->left, ctx);
	}
	if (node->right != NULL) {
		visit_obj((GcHeader*)node->right, ctx);
	}
}

static void test_finalize(GcHeader* obj) {
	TestNode* node = (TestNode*)obj;
	(void)node;
}

static void test_scan_roots(void* vm_ctx, GcVisitSlotFn visit_root_slot, void* ctx) {
	TestVm* vm = (TestVm*)vm_ctx;
	int     i;

	for (i = 0; i < vm->root_count; ++i) {
		visit_root_slot((GcHeader**)&vm->roots[i], ctx);
	}
}

static void counted_finalize(GcHeader* obj) {
	TestNode* node = (TestNode*)obj;
	(void)node;
	if (g_test_vm != NULL) {
		g_test_vm->finalized_count++;
	}
}

static const GcDescriptor TEST_NODE_DESC = {
	.name        = "TestNode",
	.fixed_size  = sizeof(TestNode),
	.flags       = GC_DESC_FLAG_CONTAINS_REFS | GC_DESC_FLAG_HAS_FINALIZER,
	.kind        = 1,
	.trace_slots = test_trace_slots,
	.trace_edges = test_trace_edges,
	.finalize    = counted_finalize,
};

static TestNode* make_node(GcRuntime* rt, GcThreadContext* thread, TestVm* vm, int id) {
	TestNode* node = (TestNode*)gc_alloc_typed(rt, thread, &TEST_NODE_DESC, sizeof(TestNode), GC_ALLOC_DEFAULT);
	assert(node != NULL);
	node->left  = NULL;
	node->right = NULL;
	node->id    = id;
	(void)vm;
	return node;
}

int main(void) {
	GcConfig         cfg;
	GcVmHooks        hooks;
	GcRuntime*       rt;
	GcThreadContext* thread;
	GcHandle*        handle;
	TestVm           vm = { 0 };
	TestNode*        a;
	TestNode*        b;
	TestNode*        c;
	TestNode*        x;
	TestNode*        y;

	gc_config_init_default(&cfg);
	g_test_vm        = &vm;
	hooks.scan_roots = test_scan_roots;
	hooks.vm_ctx     = &vm;

	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	thread = gc_thread_attach(rt, &vm);
	assert(thread != NULL);

	a = make_node(rt, thread, &vm, 1);
	b = make_node(rt, thread, &vm, 2);
	c = make_node(rt, thread, &vm, 3);

	gc_store_ref(rt, thread, (GcHeader*)a, (GcHeader**)&a->left, (GcHeader*)b);
	gc_store_ref(rt, thread, (GcHeader*)b, (GcHeader**)&b->left, (GcHeader*)c);
	vm.roots[0]   = a;
	vm.root_count = 1;

	handle = gc_handle_acquire(rt, (GcHeader*)c, GC_HANDLE_PINNED);
	assert(handle != NULL);
	assert(gc_handle_get(handle) == (GcHeader*)c);

	gc_collect_full(rt);
	assert(vm.finalized_count == 0);

	vm.roots[0] = NULL;
	gc_collect_full(rt);
	assert(vm.finalized_count == 2);
	assert(gc_handle_get(handle) == (GcHeader*)c);

	gc_handle_release(handle);
	handle = NULL;
	gc_collect_full(rt);
	assert(vm.finalized_count == 3);

	x = make_node(rt, thread, &vm, 4);
	y = make_node(rt, thread, &vm, 5);
	gc_store_ref(rt, thread, (GcHeader*)x, (GcHeader**)&x->left, (GcHeader*)y);
	gc_store_ref(rt, thread, (GcHeader*)y, (GcHeader**)&y->left, (GcHeader*)x);
	gc_collect_full(rt);
	assert(vm.finalized_count == 5);

	gc_thread_detach(thread);
	gc_runtime_destroy(rt);

	printf("marksweep smoke test passed\n");
	return EXIT_SUCCESS;
}
