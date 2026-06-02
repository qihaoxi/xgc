#include "gc_internal.h"

#include <assert.h>
#include <stdlib.h>

int main(void) {
	GcBitmap bitmap;

	gc_bitmap_init(&bitmap, 10u);
	assert(bitmap.bit_count == 10u);
	assert(bitmap.byte_count == 2u);
	assert(bitmap.bits != NULL);
	assert(gc_bitmap_test(&bitmap, 0u) == 0);
	assert(gc_bitmap_test(&bitmap, 9u) == 0);

	gc_bitmap_set(&bitmap, 0u);
	gc_bitmap_set(&bitmap, 3u);
	gc_bitmap_set(&bitmap, 9u);
	assert(gc_bitmap_test(&bitmap, 0u) == 1);
	assert(gc_bitmap_test(&bitmap, 3u) == 1);
	assert(gc_bitmap_test(&bitmap, 9u) == 1);
	assert(gc_bitmap_test(&bitmap, 5u) == 0);

	gc_bitmap_clear(&bitmap, 3u);
	assert(gc_bitmap_test(&bitmap, 3u) == 0);
	assert(gc_bitmap_test(&bitmap, 0u) == 1);
	assert(gc_bitmap_test(&bitmap, 9u) == 1);

	assert(gc_bitmap_resize(&bitmap, 20u) == 1);
	assert(bitmap.bit_count == 20u);
	assert(bitmap.byte_count == 3u);
	assert(gc_bitmap_test(&bitmap, 0u) == 1);
	assert(gc_bitmap_test(&bitmap, 9u) == 1);
	assert(gc_bitmap_test(&bitmap, 19u) == 0);

	gc_bitmap_set(&bitmap, 19u);
	assert(gc_bitmap_test(&bitmap, 19u) == 1);

	gc_bitmap_clear_all(&bitmap);
	assert(gc_bitmap_test(&bitmap, 0u) == 0);
	assert(gc_bitmap_test(&bitmap, 9u) == 0);
	assert(gc_bitmap_test(&bitmap, 19u) == 0);

	assert(gc_bitmap_resize(&bitmap, 0u) == 1);
	assert(bitmap.bit_count == 0u);
	assert(bitmap.byte_count == 0u);
	assert(bitmap.bits == NULL);
	assert(gc_bitmap_test(&bitmap, 0u) == 0);

	gc_bitmap_destroy(&bitmap);
	assert(bitmap.bit_count == 0u);
	assert(bitmap.byte_count == 0u);
	assert(bitmap.bits == NULL);
	return EXIT_SUCCESS;
}

