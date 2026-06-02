# xgc 算法可插拔重构 — 设计规格

## 概述

将 xgc 框架重构为编译时可切换 GC 算法的架构。当前唯一算法为 Bacon-Rajan (RC + Trial Deletion)。公共 API 保持不变，算法实现在 `src/algo/<name>/` 目录下独立组织。

## 设计决策

- **编译时切换**：CMake `GC_ALGORITHM` 选项选择算法，只编译选中算法的源文件
- **零运行时开销**：直接函数调用，无虚表、无函数指针间接
- **共享基础设施**：worklist、ring buffer、reclaim、epoch、atomic 所有算法共用

## 目录结构

```
src/
├── algo/bacon_rajan/             # 算法: RC + Trial Deletion
│   ├── gc_alloc.c                # 分配 + TLAB + 上下文初始化
│   ├── gc_assign.c               # 统一写屏障
│   ├── gc_collect.c              # Trial Deletion + 栈根保护
│   └── gc_safepoint.c            # Safepoint 协作轮询
├── gc_internal.h                 # 共享内部头（含紫色缓冲区 push 函数）
├── gc_purple.c                   # 共享：自适应紫色缓冲区
├── gc_worklist.c                 # 共享：显式工作栈
├── gc_ring.c                     # 共享：Ring Buffer
├── gc_reclaim.c                  # 共享：递归释放
├── gc_epoch.c                    # 共享：Epoch 状态机
└── gc_atomic.c                   # 共享：原子 fallback
```

## CMake 变化

- 新增 `GC_ALGORITHM` cache string，默认 `bacon-rajan`
- `file(GLOB_RECURSE)` 从 `src/algo/${GC_ALGORITHM}/*.c` 收集算法源文件
- 共享工具源文件单独 `file(GLOB)` 收集
- 删除 `src/` 根目录下移入算法目录的旧文件

## 公共 API

`gc.h` 完全不变。函数实现由 `src/algo/<selected>/` 提供。

## 实现步骤

1. 创建 `src/algo/bacon_rajan/` 目录
2. 将 `gc_alloc.c`、`gc_assign.c`、`gc_collect.c`、`gc_safepoint.c` 从 `src/` 移入 `src/algo/bacon_rajan/`
3. 保留 `gc_worklist.c`、`gc_ring.c`、`gc_reclaim.c`、`gc_epoch.c`、`gc_atomic.c`、`gc_purple.c` 在 `src/` 作为共享工具
4. 更新 `CMakeLists.txt`：添加 `GC_ALGORITHM` 选项，区分算法源和共享源
5. 更新 `gc_internal.h` 中的 `#include` 路径和内部函数声明
6. 验证 Debug 和 GC_MULTITHREAD 构建通过
