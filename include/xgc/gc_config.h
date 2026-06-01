#ifndef XGC_GC_CONFIG_H
#define XGC_GC_CONFIG_H

/* ============================================================================
 * xgc 编译时配置选项
 *
 * 用户可在 CMake 或直接在包含此头文件前定义以下宏来控制 GC 行为。
 * ============================================================================
 */

/* GC_MULTITHREAD
 *
 * 定义后启用多线程支持:
 *   - RC 使用 C11 原子类型 (_Atomic)
 *   - 各线程独立 purple buffer + TLAB
 *   - Background GC 线程 + Epoch + Safepoint 同步
 *
 * 未定义时:
 *   - 原子类型退化为普通类型（零开销）
 *   - 单线程/单协程内联 gc_collect_cycles
 *
 * 方式 1: CMake option(GC_MULTITHREAD) 自动传入
 * 方式 2: #define GC_MULTITHREAD 放在 #include "xgc/gc_config.h" 之前
 */

/* ── 紫色缓冲区自适应范围 ── */

#ifndef GC_PURPLE_CAPACITY_MIN
#define GC_PURPLE_CAPACITY_MIN  64
#endif

#ifndef GC_PURPLE_CAPACITY_MAX
#define GC_PURPLE_CAPACITY_MAX  4096
#endif

#ifndef GC_PURPLE_CAPACITY_INIT
#define GC_PURPLE_CAPACITY_INIT 256
#endif

/* ── TLAB 批量冲刷阈值 ── */

#ifndef GC_TLAB_THRESHOLD
#define GC_TLAB_THRESHOLD  128
#endif

/* ── Ring Buffer 临时对象大小上限 ── */

#ifndef GC_RING_SLOT_MAX_SIZE
#define GC_RING_SLOT_MAX_SIZE  256
#endif

/* ── 双水位线默认值 (字节) ── */

#ifndef GC_THRESHOLD_SOFT_DEFAULT
#define GC_THRESHOLD_SOFT_DEFAULT  (64 * 1024 * 1024)   /* 64 MB */
#endif

#ifndef GC_THRESHOLD_HARD_DEFAULT
#define GC_THRESHOLD_HARD_DEFAULT  (128 * 1024 * 1024)  /* 128 MB */
#endif

#endif /* XGC_GC_CONFIG_H */
