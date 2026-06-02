#include "gc_internal.h"

void gc_bitmap_init(GcBitmap* bitmap, size_t bit_count) {
	if (bitmap == NULL) {
		return;
	}

	memset(bitmap, 0, sizeof(*bitmap));
	if (bit_count != 0u) {
		(void)gc_bitmap_resize(bitmap, bit_count);
	}
}

void gc_bitmap_destroy(GcBitmap* bitmap) {
	if (bitmap == NULL) {
		return;
	}

	free(bitmap->bits);
	memset(bitmap, 0, sizeof(*bitmap));
}

int gc_bitmap_resize(GcBitmap* bitmap, size_t bit_count) {
	size_t   byte_count;
	uint8_t* new_bits;

	if (bitmap == NULL) {
		return 0;
	}

	byte_count = (bit_count + 7u) / 8u;
	if (byte_count == 0u) {
		free(bitmap->bits);
		bitmap->bits = NULL;
		bitmap->bit_count = 0u;
		bitmap->byte_count = 0u;
		return 1;
	}

	new_bits = (uint8_t*)realloc(bitmap->bits, byte_count);
	if (new_bits == NULL) {
		return 0;
	}

	if (byte_count > bitmap->byte_count) {
		memset(new_bits + bitmap->byte_count, 0, byte_count - bitmap->byte_count);
	}

	bitmap->bits = new_bits;
	bitmap->bit_count = bit_count;
	bitmap->byte_count = byte_count;
	return 1;
}

void gc_bitmap_clear_all(GcBitmap* bitmap) {
	if (bitmap == NULL || bitmap->bits == NULL) {
		return;
	}

	memset(bitmap->bits, 0, bitmap->byte_count);
}

void gc_bitmap_set(GcBitmap* bitmap, size_t index) {
	if (bitmap == NULL || bitmap->bits == NULL || index >= bitmap->bit_count) {
		return;
	}

	bitmap->bits[index / 8u] = (uint8_t)(bitmap->bits[index / 8u] | (uint8_t)(1u << (index % 8u)));
}

void gc_bitmap_clear(GcBitmap* bitmap, size_t index) {
	if (bitmap == NULL || bitmap->bits == NULL || index >= bitmap->bit_count) {
		return;
	}

	bitmap->bits[index / 8u] = (uint8_t)(bitmap->bits[index / 8u] & (uint8_t)~(1u << (index % 8u)));
}

int gc_bitmap_test(const GcBitmap* bitmap, size_t index) {
	if (bitmap == NULL || bitmap->bits == NULL || index >= bitmap->bit_count) {
		return 0;
	}

	return (bitmap->bits[index / 8u] & (uint8_t)(1u << (index % 8u))) != 0u;
}

