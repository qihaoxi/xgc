#include "gc_internal.h"

/* 原子操作通过 gc_internal.h 中的宏实现（GC_MULTITHREAD 条件编译）。
 * 此文件为未来可能需要的平台特定原子 fallback 预留。
 * 例如: 在不支持 C11 _Atomic 的老旧编译器上通过 CAS 循环模拟。 */
