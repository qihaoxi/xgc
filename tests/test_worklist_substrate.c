#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void init_header(gc_header* header, uint16_t kind) {
	memset(header, 0, sizeof(*header));
	header->size = (uint32_t)sizeof(*header);
	header->kind = kind;
}

/* ── basic push/pop ── */

static void test_basic_push_pop(void) {
	gc_worklist wl;
	gc_header   a;
	gc_header   b;
	gc_header   c;

	memset(&wl, 0, sizeof(wl));
	init_header(&a, 1u);
	init_header(&b, 2u);
	init_header(&c, 3u);

	assert(gc_worklist_pop(&wl) == NULL);

	gc_worklist_push(&wl, &a);
	gc_worklist_push(&wl, &b);
	gc_worklist_push(&wl, &c);
	assert(wl.top == 3);
	assert(wl.capacity >= 3);

	assert(gc_worklist_pop(&wl) == &c);
	assert(gc_worklist_pop(&wl) == &b);
	assert(gc_worklist_pop(&wl) == &a);
	assert(gc_worklist_pop(&wl) == NULL);
	assert(wl.top == 0);

	free(wl.data);
}

/* ── dynamic expansion ── */

static void test_dynamic_expansion(void) {
	gc_worklist wl;
	gc_header   extra[20];
	int         i;

	memset(&wl, 0, sizeof(wl));
	for (i = 0; i < (int)(sizeof(extra) / sizeof(extra[0])); ++i) {
		init_header(&extra[i], (uint16_t)(10 + i));
	}

	for (i = 0; i < (int)(sizeof(extra) / sizeof(extra[0])); ++i) {
		gc_worklist_push(&wl, &extra[i]);
	}
	assert(wl.top == (int)(sizeof(extra) / sizeof(extra[0])));
	assert(wl.capacity >= wl.top);

	for (i = (int)(sizeof(extra) / sizeof(extra[0])) - 1; i >= 0; --i) {
		assert(gc_worklist_pop(&wl) == &extra[i]);
	}
	assert(gc_worklist_pop(&wl) == NULL);
	assert(wl.top == 0);

	free(wl.data);
}

/* ── invariant: NULL fails ── */

static void test_invariant_null_fails(void) {
	assert(gc_worklist_check_invariants(NULL) == 0);
}

/* ── invariant: empty worklist passes ── */

static void test_invariant_empty_passes(void) {
	gc_worklist wl;
	memset(&wl, 0, sizeof(wl));
	assert(gc_worklist_check_invariants(&wl) == 1);
}

/* ── invariant: top out of [0, capacity] fails ── */

static void test_invariant_top_out_of_range(void) {
	gc_worklist wl;
	gc_header*  data[2];

	memset(&wl, 0, sizeof(wl));
	wl.data     = data;
	wl.capacity = 2;

	wl.top = 3;
	assert(gc_worklist_check_invariants(&wl) == 0);

	wl.top = -1;
	assert(gc_worklist_check_invariants(&wl) == 0);

	wl.top = 0;
	assert(gc_worklist_check_invariants(&wl) == 1);
}

/* ── invariant: capacity==0 implies data==NULL and top==0 ── */

static void test_invariant_zero_capacity_rules(void) {
	gc_worklist  wl;
	gc_header*   ptr;

	/* clean zero state */
	memset(&wl, 0, sizeof(wl));
	assert(gc_worklist_check_invariants(&wl) == 1);

	/* capacity 0 but data non-NULL */
	ptr     = NULL;
	wl.data = &ptr;
	assert(gc_worklist_check_invariants(&wl) == 0);

	/* capacity 0, data NULL, but top non-zero */
	memset(&wl, 0, sizeof(wl));
	wl.top = 1;
	assert(gc_worklist_check_invariants(&wl) == 0);
}

/* ── invariant: positive capacity with NULL data fails ── */

static void test_invariant_capacity_no_data(void) {
	gc_worklist wl;

	memset(&wl, 0, sizeof(wl));
	wl.capacity = 4;
	assert(gc_worklist_check_invariants(&wl) == 0);
}

/* ── invariant: passes at full capacity ── */

static void test_invariant_passes_at_full(void) {
	gc_worklist wl;
	gc_header   a;
	gc_header   b;

	init_header(&a, 100u);
	init_header(&b, 101u);

	memset(&wl, 0, sizeof(wl));
	gc_worklist_push(&wl, &a);
	gc_worklist_push(&wl, &b);
	assert(wl.top == 2);
	assert(gc_worklist_check_invariants(&wl) == 1);

	free(wl.data);
}

int main(void) {
	test_basic_push_pop();
	test_dynamic_expansion();

	test_invariant_null_fails();
	test_invariant_empty_passes();
	test_invariant_top_out_of_range();
	test_invariant_zero_capacity_rules();
	test_invariant_capacity_no_data();
	test_invariant_passes_at_full();

	return EXIT_SUCCESS;
}
