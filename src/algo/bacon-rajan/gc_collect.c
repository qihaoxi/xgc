#include "gc_internal.h"

void
gc_collect_cycles(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: 实现 Trial Deletion 四阶段:
     *   Phase 1: 复制 refcount → gc_ref
     *   Phase 2: 减去内部引用（深度遍历整张子图）
     *   Phase 3: 分离存活与垃圾（CAS 防护 TOCTOU）
     *   Phase 4: 释放垃圾
     */
    (void)global;
    (void)thread;
}

void
gc_scan_stack_and_protect(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: 栈根扫描 + 递归回滚保护 */
    (void)global;
    (void)thread;
}
