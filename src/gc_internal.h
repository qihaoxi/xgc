#ifndef XGC_GC_INTERNAL_H
#define XGC_GC_INTERNAL_H

#include "xgc/gc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * 内部头文件 — 库内部使用，用户不可见
 *
 * 包含: 原子类型封装、GcHeader 完整定义、GcGlobalContext / GcThreadContext
 *        完整定义、显式工作栈、Epoch 状态（多线程）、内部辅助宏
 * ============================================================================
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. 原子类型封装
 *
 * GC_MULTITHREAD 定义时使用 C11 _Atomic，否则退化为普通类型（零开销）
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(GC_MULTITHREAD) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
typedef _Atomic uint8_t gc_atomic_u8_t;
typedef _Atomic int     gc_atomic_int_t;

#define GC_ATOMIC_LOAD(p) atomic_load(p)
#define GC_ATOMIC_STORE(p, v) atomic_store(p, v)
#define GC_ATOMIC_INC(p) atomic_fetch_add(p, 1)
#define GC_ATOMIC_DEC(p) atomic_fetch_sub(p, 1)
#define GC_ATOMIC_XCHG(p, v) atomic_exchange(p, v)
#define GC_CAS(p, exp, des) atomic_compare_exchange_weak(p, exp, des)
#else
typedef uint8_t gc_atomic_u8_t;
typedef int     gc_atomic_int_t;

#define GC_ATOMIC_LOAD(p) (*(p))
#define GC_ATOMIC_STORE(p, v) (*(p) = (v))
#define GC_ATOMIC_INC(p) ((*(p))++)
#define GC_ATOMIC_DEC(p) ((*(p))--)
#define GC_ATOMIC_XCHG(p, v)                                                                                           \
	__extension__({                                                                                                    \
		__typeof__(v) _xgc_old = *(p);                                                                                 \
		*(p)                   = (v);                                                                                  \
		_xgc_old;                                                                                                      \
	})
#define GC_CAS(p, exp, des) (*(p) == *(exp) ? (*(p) = (des), 1) : (*(exp) = *(p), 0))
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. 显式工作栈（防 C 递归栈溢出）
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
	GcHeader** data;
	int        top;
	int        capacity;
} GcWorklist;

typedef struct {
	uint8_t* bits;
	size_t   bit_count;
	size_t   byte_count;
} GcBitmap;

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. 运行时与线程内部结构
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
	GC_STATE_RUNNING         = 0,
	GC_STATE_STW_REQUESTED   = 1,
	GC_STATE_STW_IN_PROGRESS = 2,
} GcRuntimeState;

typedef struct {
	uintptr_t owner_base;
	size_t    owner_size;
	size_t    card_count;
	size_t    dirty_card_count;
	GcBitmap  dirty_map;
} GcCardEntry;

typedef struct {
	GcCardEntry* entries;
	size_t       count;
	size_t       capacity;
	size_t       card_granularity;
} GcCardTable;

typedef struct {
	uint8_t* young_base;
	size_t   young_capacity;
	size_t   young_used;
	size_t   large_object_threshold;
	size_t   old_object_count;
	size_t   old_object_bytes;
} GcHeap;

typedef struct {
	GcCardTable old_to_young;
	size_t      dirty_old_objects;
	size_t      dirty_cards;
} GcBarrierSet;

typedef struct {
	size_t total_allocated;
	size_t peak_allocated;
	size_t minor_collections;
	size_t major_collections;
	size_t full_collections;
} GcStats;

typedef struct {
	void* placeholder;
} GcScheduler;

struct GcRuntime {
	GcConfig                 cfg;
	const GcAlgorithmVTable* algo;
	GcVmHooks                hooks;
	GcHeap                   heap;
	GcScheduler              sched;
	GcBarrierSet             barriers;
	GcStats                  stats;
	void*                    algo_state;
	GcHandle*                handles;
	GcHeader**               worklist_data;
	int                      worklist_capacity;
	gc_atomic_int_t          gc_state;
};

struct GcThreadContext {
	GcRuntime* runtime;
	void*      vm_thread_ctx;
	void*      algo_state;
};

struct GcHandle {
	GcRuntime* runtime;
	GcHeader*  target;
	uint32_t   flags;
	GcHandle*  next;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

void gc_runtime_apply_default_config(GcConfig* cfg);

/* heap substrate */
void gc_heap_init(GcHeap* heap, size_t large_object_threshold);
void gc_heap_set_young_space(GcRuntime* rt, uint8_t* base, size_t capacity);
void gc_heap_record_young_alloc(GcRuntime* rt, size_t bytes);
void gc_heap_reset_young(GcRuntime* rt);
void gc_heap_record_old_alloc(GcRuntime* rt, size_t bytes);
void gc_heap_record_old_free(GcRuntime* rt, size_t bytes);

/* bitmap substrate */
void gc_bitmap_init(GcBitmap* bitmap, size_t bit_count);
void gc_bitmap_destroy(GcBitmap* bitmap);
int  gc_bitmap_resize(GcBitmap* bitmap, size_t bit_count);
void gc_bitmap_clear_all(GcBitmap* bitmap);
void gc_bitmap_set(GcBitmap* bitmap, size_t index);
void gc_bitmap_clear(GcBitmap* bitmap, size_t index);
int  gc_bitmap_test(const GcBitmap* bitmap, size_t index);

typedef void (*GcVisitDirtyCardFn)(GcHeader* owner, size_t card_index, void* ctx);

/* card-table substrate */
void gc_card_table_init(GcCardTable* table, size_t card_granularity);
void gc_card_table_destroy(GcCardTable* table);
int  gc_card_table_register_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner, size_t owner_size);
void gc_card_table_unregister_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner);
void gc_card_table_mark_slot(GcRuntime* rt, GcCardTable* table, const GcHeader* owner, const void* slot_addr);
void gc_card_table_mark_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner);
int  gc_card_table_owner_is_dirty(const GcCardTable* table, const GcHeader* owner);
void gc_card_table_clear_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner);
void gc_card_table_visit_dirty(const GcCardTable* table, GcVisitDirtyCardFn visit, void* ctx);

/* barrier/card-table substrate */
void gc_barrier_set_init(GcBarrierSet* barriers, size_t card_granularity);
void gc_barrier_set_destroy(GcBarrierSet* barriers);
int  gc_barrier_register_owner(GcRuntime* rt, const GcHeader* owner, size_t owner_size);
void gc_barrier_unregister_owner(GcRuntime* rt, const GcHeader* owner);
void gc_barrier_mark_slot_dirty(GcRuntime* rt, const GcHeader* owner, const void* slot_addr);
void gc_barrier_mark_owner_dirty(GcRuntime* rt, const GcHeader* owner);
int  gc_barrier_is_owner_dirty(const GcRuntime* rt, const GcHeader* owner);
void gc_barrier_clear_owner_dirty(GcRuntime* rt, const GcHeader* owner);
void gc_barrier_visit_dirty_cards(const GcRuntime* rt, GcVisitDirtyCardFn visit, void* ctx);

/* gc_worklist.c */
void      gc_worklist_push(GcWorklist* wl, GcHeader* obj);
GcHeader* gc_worklist_pop(GcWorklist* wl);

/* gc_ring.c */
void* gc_try_ring_buffer_alloc(GcThreadContext* thread, size_t size);
void  gc_reclaim_ring_buffer_slots(GcRuntime* rt, GcThreadContext* thread);

/* gc_reclaim.c */
void gc_reclaim_object(GcRuntime* rt, GcHeader* obj);

/* gc_purple.c */
void gc_push_purple_adaptive(GcRuntime* rt, GcThreadContext* thread, GcHeader* obj);

/* gc_epoch.c (GC_MULTITHREAD only) */
void gc_signal_background_thread(GcRuntime* rt);
void gc_wait_for_signal(GcRuntime* rt);

/* bacon-rajan algorithm hooks */
void gc_bacon_rajan_write_barrier(GcRuntime* rt, GcThreadContext* thread, GcHeader* owner, GcHeader** slot,
                                  GcHeader* old_value, GcHeader* new_value);
void gc_bacon_rajan_collect_minor(GcRuntime* rt);
void gc_bacon_rajan_collect_major(GcRuntime* rt);
void gc_bacon_rajan_collect_full(GcRuntime* rt);
void gc_enter_safepoint_and_park(GcRuntime* rt, GcThreadContext* thread);

#endif /* XGC_GC_INTERNAL_H */
