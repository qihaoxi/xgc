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

void gc_config_init_default(GcConfig* cfg);

GcRuntime* gc_runtime_create(const GcConfig* cfg, const GcAlgorithmVTable* algo, const GcVmHooks* hooks);
void       gc_runtime_destroy(GcRuntime* rt);

GcThreadContext* gc_thread_attach(GcRuntime* rt, void* vm_thread_ctx);
void             gc_thread_detach(GcThreadContext* thread);

void* gc_alloc_typed(GcRuntime* rt, GcThreadContext* thread, const GcDescriptor* desc, size_t size,
                     uint32_t alloc_flags);

void gc_store_ref(GcRuntime* rt, GcThreadContext* thread, GcHeader* owner, GcHeader** slot, GcHeader* new_value);

GcHeader* gc_load_ref(GcRuntime* rt, GcThreadContext* thread, GcHeader** slot);

void gc_collect_minor(GcRuntime* rt);
void gc_collect_major(GcRuntime* rt);
void gc_collect_full(GcRuntime* rt);

GcHandle* gc_handle_acquire(GcRuntime* rt, GcHeader* obj, uint32_t flags);
GcHeader* gc_handle_get(const GcHandle* handle);
void      gc_handle_release(GcHandle* handle);

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_H */
