#include "gc_internal.h"

#include <assert.h>
#include <stdint.h>
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

static void test_trace_slots_range(gc_header* obj, size_t byte_begin, size_t byte_end, gc_visit_slot_fn visit_slot,
                                   void* ctx) {
	test_node* node = (test_node*)obj;
	size_t     left_offset;
	size_t     right_offset;

	if (visit_slot == NULL) {
		return;
	}

	left_offset  = offsetof(test_node, left);
	right_offset = offsetof(test_node, right);
	if (byte_begin <= left_offset && left_offset < byte_end && node->left != NULL) {
		visit_slot((gc_header**)&node->left, ctx);
	}
	if (byte_begin <= right_offset && right_offset < byte_end && node->right != NULL) {
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

static void counted_finalize(gc_header* obj) {
	(void)obj;
	if (g_test_vm != NULL) {
		g_test_vm->finalized_count++;
	}
}

static void test_scan_roots(void* vm_ctx, gc_visit_slot_fn visit_root_slot, void* ctx) {
	test_vm* vm = (test_vm*)vm_ctx;
	int      i;

	for (i = 0; i < vm->root_count; ++i) {
		visit_root_slot((gc_header**)&vm->roots[i], ctx);
	}
}

static const gc_descriptor TEST_NODE_DESC = {
	.name              = "GenNode",
	.fixed_size        = sizeof(test_node),
	.flags             = GC_DESC_FLAG_CONTAINS_REFS | GC_DESC_FLAG_HAS_FINALIZER,
	.kind              = 2,
	.trace_slots       = test_trace_slots,
	.trace_slots_range = test_trace_slots_range,
	.trace_edges       = test_trace_edges,
	.finalize          = counted_finalize,
};

static test_node* make_node(gc_runtime* rt, gc_thread_context* thread, int id) {
	test_node* node = (test_node*)gc_alloc_typed(rt, thread, &TEST_NODE_DESC, sizeof(test_node), GC_ALLOC_DEFAULT);
	assert(node != NULL);
	node->left  = NULL;
	node->right = NULL;
	node->id    = id;
	return node;
}

int main(void) {
	gc_config          cfg;
	gc_vm_hooks        hooks;
	gc_runtime*        rt;
	gc_thread_context* thread;
	gc_handle*         handle;
	test_vm            vm = { 0 };
	test_node*         a;
	test_node*         b;
	test_node*         c;
	test_node*         x;
	test_node*         y;
	test_node*         d;
	uintptr_t          old_a;
	uintptr_t          old_b;
	uintptr_t          old_c;
	uintptr_t          old_d;

	gc_config_init_default(&cfg);
	cfg.gc_young_space_size = 256;
	g_test_vm               = &vm;

	hooks.scan_roots = test_scan_roots;
	hooks.vm_ctx     = &vm;

	rt = gc_runtime_create(&cfg, xgc_default_algorithm(), &hooks);
	assert(rt != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);
	thread = gc_thread_attach(rt, &vm);
	assert(thread != NULL);

	a = make_node(rt, thread, 1);
	b = make_node(rt, thread, 2);
	c = make_node(rt, thread, 3);
	gc_store_ref(rt, thread, (gc_header*)a, (gc_header**)&a->left, (gc_header*)b);
	gc_store_ref(rt, thread, (gc_header*)b, (gc_header**)&b->left, (gc_header*)c);
	vm.roots[0]   = a;
	vm.root_count = 1;
	handle        = gc_handle_acquire(rt, (gc_header*)c, GC_HANDLE_PINNED);
	assert(handle != NULL);
	assert(gc_runtime_check_invariants(rt) == 1);

	old_a = (uintptr_t)a;
	old_b = (uintptr_t)b;
	old_c = (uintptr_t)c;

	gc_collect_minor(rt);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert((uintptr_t)vm.roots[0] != old_a);
	assert((uintptr_t)vm.roots[0]->left != old_b);
	assert((uintptr_t)((test_node*)gc_handle_get(handle)) != old_c);
	assert(vm.roots[0]->left->left == (test_node*)gc_handle_get(handle));
	assert(vm.finalized_count == 0);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	d     = make_node(rt, thread, 6);
	old_d = (uintptr_t)d;
	gc_store_ref(rt, thread, (gc_header*)vm.roots[0], (gc_header**)&vm.roots[0]->right, (gc_header*)d);
	assert(rt->barriers.dirty_old_objects == 1u);
	assert(rt->barriers.dirty_cards == 1u);
	assert(gc_runtime_check_invariants(rt) == 1);
	gc_collect_minor(rt);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(vm.roots[0]->right != NULL);
	assert((uintptr_t)vm.roots[0]->right != old_d);
	assert(vm.roots[0]->right->id == 6);
	assert(vm.finalized_count == 0);
	assert(rt->barriers.dirty_old_objects == 0u);
	assert(rt->barriers.dirty_cards == 0u);

	vm.roots[0] = NULL;
	gc_collect_full(rt);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(vm.finalized_count == 3);
	assert(gc_handle_get(handle) != NULL);

	gc_handle_release(handle);
	gc_collect_full(rt);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(vm.finalized_count == 4);

	x = make_node(rt, thread, 4);
	y = make_node(rt, thread, 5);
	gc_store_ref(rt, thread, (gc_header*)x, (gc_header**)&x->left, (gc_header*)y);
	gc_store_ref(rt, thread, (gc_header*)y, (gc_header**)&y->left, (gc_header*)x);
	assert(gc_runtime_check_invariants(rt) == 1);
	gc_collect_minor(rt);
	assert(gc_runtime_check_invariants(rt) == 1);
	assert(vm.finalized_count == 6);

	gc_thread_detach(thread);
	gc_runtime_destroy(rt);
	printf("gen-copy-ms smoke test passed\n");
	return EXIT_SUCCESS;
}
