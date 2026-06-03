#ifndef XGC_GC_H
#define XGC_GC_H

/* ============================================================================
 * xgc — 通用可插拔 GC 框架
 *
 * 用户只需 #include "xgc/gc.h" 即可访问：
 *   1. 公共对象/描述符类型
 *   2. 算法 capability 与 vtable
 *   3. 运行时创建/销毁 API
 *   4. 分配、写屏障、收集、handle API
 * ============================================================================
 */

#include "xgc/gc_algorithm.h"
#include "xgc/gc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void gc_config_init_default(gc_config* cfg);

gc_runtime* gc_runtime_create(const gc_config* cfg, const gc_algorithm_vtable* algo, const gc_vm_hooks* hooks);
void        gc_runtime_destroy(gc_runtime* rt);

gc_thread_context* gc_thread_attach(gc_runtime* rt, void* vm_thread_ctx);
void               gc_thread_detach(gc_thread_context* thread);

void* gc_alloc_typed(gc_runtime* rt, gc_thread_context* thread, const gc_descriptor* desc, size_t size,
                     uint32_t alloc_flags);

void gc_store_ref(gc_runtime* rt, gc_thread_context* thread, gc_header* owner, gc_header** slot, gc_header* new_value);

gc_header* gc_load_ref(gc_runtime* rt, gc_thread_context* thread, gc_header** slot);

void gc_collect_minor(gc_runtime* rt);
void gc_collect_major(gc_runtime* rt);
void gc_collect_full(gc_runtime* rt);

gc_handle* gc_handle_acquire(gc_runtime* rt, gc_header* obj, uint32_t flags);
gc_header* gc_handle_get(const gc_handle* handle);
void       gc_handle_release(gc_handle* handle);

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_H */
