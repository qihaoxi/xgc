#include "gc_internal.h"

#include <assert.h>
#include <stdint.h>
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

static void test_trace_slots_range(GcHeader* obj, size_t byte_begin, size_t byte_end, GcVisitSlotFn visit_slot,
                                   void* ctx) {
	TestNode* node = (TestNode*)obj;
	size_t    left_offset;
	size_t    right_offset;

	if (visit_slot == NULL) {
		return;
	}

	left_offset  = offsetof(TestNode, left);
	right_offset = offsetof(TestNode, right);
	if (byte_begin <= left_offset && left_offset < byte_end && node->left != NULL) {
		visit_slot((GcHeader**)&node->left, ctx);
	}
	if (byte_begin <= right_offset && right_offset < byte_end && node->right != NULL) {
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

static void counted_finalize(GcHeader* obj) {
	(void)obj;
	if (g_test_vm != NULL) {
		g_test_vm->finalized_count++;
	}
}

static void test_scan_roots(void* vm_ctx, GcVisitSlotFn visit_root_slot, void* ctx) {
	TestVm* vm = (TestVm*)vm_ctx;
	int     i;

	for (i = 0; i < vm->root_count; ++i) {
		visit_root_slot((GcHeader**)&vm->roots[i], ctx);
	}
}

static const GcDescriptor TEST_NODE_DESC = {
	.name              = "GenNode",
	.fixed_size        = sizeof(TestNode),
	.flags             = GC_DESC_FLAG_CONTAINS_REFS | GC_DESC_FLAG_HAS_FINALIZER,
	.kind              = 2,
	.trace_slots       = test_trace_slots,
	.trace_slots_range = test_trace_slots_range,
	.trace_edges       = test_trace_edges,
	.finalize          = counted_finalize,
};

static TestNode* make_node(GcRuntime* rt, GcThreadContext* thread, int id) {
	TestNode* node = (TestNode*)gc_alloc_typed(rt, thread, &TEST_NODE_DESC, sizeof(TestNode), GC_ALLOC_DEFAULT);
	assert(node != NULL);
	node->left  = NULL;
	node->right = NULL;
	node->id    = id;
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
	TestNode*        d;
	uintptr_t        old_a;
	uintptr_t        old_b;
	uintptr_t        old_c;
	uintptr_t        old_d;

	gc_config_init_default(&cfg);
	cfg.gc_young_space_size = 256;
	g_test_vm               = &vm;

	hooks.scan_roots = test_scan_roots;
	hooks.vm_ctx     = &vm;

	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	thread = gc_thread_attach(rt, &vm);
	assert(thread != NULL);

	a = make_node(rt, thread, 1);
	b = make_node(rt, thread, 2);
	c = make_node(rt, thread, 3);
	gc_store_ref(rt, thread, (GcHeader*)a, (GcHeader**)&a->left, (GcHeader*)b);
	gc_store_ref(rt, thread, (GcHeader*)b, (GcHeader**)&b->left, (GcHeader*)c);
	vm.roots[0]   = a;
	vm.root_count = 1;
	handle        = gc_handle_acquire(rt, (GcHeader*)c, GC_HANDLE_PINNED);
	assert(handle != NULL);

	old_a = (uintptr_t)a;
	old_b = (uintptr_t)b;
	old_c = (uintptr_t)c;

	gc_collect_minor(rt);
	assert((uintptr_t)vm.roots[0] != old_a);
	assert((uintptr_t)vm.roots[0]->left != old_b);
	assert((uintptr_t)((TestNode*)gc_handle_get(handle)) != old_c);
	assert(vm.roots[0]->left->left == (TestNode*)gc_handle_get(handle));
	assert(vm.finalized_count == 0);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	d     = make_node(rt, thread, 6);
	old_d = (uintptr_t)d;
	gc_store_ref(rt, thread, (GcHeader*)vm.roots[0], (GcHeader**)&vm.roots[0]->right, (GcHeader*)d);
	assert(rt->barriers.dirty_old_objects == 1u);
	assert(rt->barriers.dirty_cards == 1u);
	gc_collect_minor(rt);
	assert(vm.roots[0]->right != NULL);
	assert((uintptr_t)vm.roots[0]->right != old_d);
	assert(vm.roots[0]->right->id == 6);
	assert(vm.finalized_count == 0);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	vm.roots[0] = NULL;
	gc_collect_full(rt);
	assert(vm.finalized_count == 3);
	assert(gc_handle_get(handle) != NULL);

	gc_handle_release(handle);
	gc_collect_full(rt);
	assert(vm.finalized_count == 4);

	x = make_node(rt, thread, 4);
	y = make_node(rt, thread, 5);
	gc_store_ref(rt, thread, (GcHeader*)x, (GcHeader**)&x->left, (GcHeader*)y);
	gc_store_ref(rt, thread, (GcHeader*)y, (GcHeader**)&y->left, (GcHeader*)x);
	gc_collect_minor(rt);
	assert(vm.finalized_count == 6);

	gc_thread_detach(thread);
	gc_runtime_destroy(rt);
	printf("gen-copy-ms smoke test passed\n");
	return EXIT_SUCCESS;
}
