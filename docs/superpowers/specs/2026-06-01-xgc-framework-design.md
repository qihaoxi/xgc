# xgc 程序框架设计规格

## 概述

建立纯 C 高性能混合 GC 算法库 `xgc` 的程序框架。镜像 `xcompiler` 的 CMake 构建体系和目录组织方式，实现一个独立于任何 VM 的纯算法底座。

## 目录结构

```
xgc/
├── CMakeLists.txt                  # 顶层构建
├── cmake/                          # CMake 模块
│   ├── compiler-options.cmake      # 编译选项（-Wall -Wextra -Wpedantic ...）
│   ├── debug.cmake                 # Debug 配置（-g -O0 ...）
│   ├── sanitizer.cmake             # ASan/TSan/UBSan 支持
│   ├── tests.cmake                 # 测试注册
│   ├── examples.cmake              # 示例注册
│   ├── install.cmake               # 安装规则
│   └── format.cmake                # clang-format 目标
│
├── include/xgc/                    # 对外导出 API（深度=2）
│   ├── gc.h                        # 伞式头文件
│   └── gc_config.h                 # 编译时配置选项
│
├── src/                            # 库源码（扁平一层）
│   ├── gc_internal.h               # 内部类型定义
│   ├── gc_alloc.c                  # 分配 + TLAB
│   ├── gc_assign.c                 # 统一写屏障
│   ├── gc_collect.c                # Trial Deletion 循环检测
│   ├── gc_worklist.c               # 显式工作栈
│   ├── gc_purple.c                 # 自适应紫色缓冲区
│   ├── gc_ring.c                   # Ring Buffer
│   ├── gc_reclaim.c                # 递归释放
│   ├── gc_safepoint.c              # Safepoint 协作轮询
│   ├── gc_epoch.c                  # Epoch 状态机（多线程）
│   └── gc_atomic.c                 # 原子操作封装
│
├── tests/                          # 测试（扁平一层）
│   ├── test_worklist.c
│   ├── test_purple.c
│   ├── test_assign.c
│   ├── test_alloc.c
│   ├── test_cycle.c
│   ├── test_ring.c
│   └── test_integration.c
│
├── examples/                       # 使用示例
│   ├── basic.c
│   └── custom_spi.c
│
├── docs/
├── .clang-format
└── .gitignore
```

## 构建系统

完全镜像 xcompiler 的 CMake 模式：

- C11 标准，禁用扩展
- 静态库 `xgc` + 共享库 `xgc_shared`（别名 `xgc::xgc`）
- 编译选项 `-Wall -Wextra -Wpedantic -Werror`
- ASan/TSan/UBSan opt-in
- CTest 测试注册
- `make format` clang-format 目标
- .clang-format 继承 xcompiler 配置

## 头文件职责

| 文件 | 使用者 | 内容 |
|------|--------|------|
| `gc.h` | 用户 | 公开 API |
| `gc_config.h` | 用户 | 编译开关宏 |
| `gc_internal.h` | 库内部 | 所有结构体、SPI 签名、内部宏 |

## 设计文档参考

算法设计详见 `docs/通用高性能混合GC.md`。本框架是该设计文档的程序骨架实现。

## 第一步实现范围

1. 完整目录结构和 CMake 构建体系（镜像 xcompiler）
2. `gc_internal.h` — 所有核心类型定义（GcHeader, GcColor, GcGlobalContext, GcThreadContext, SPI 回调签名, 原子类型封装）
3. `gc.h` / `gc_config.h` — 对外导出头文件
4. 各 `.c` 源文件 — 函数签名骨架 + TODO 标记
5. `.clang-format` + `.gitignore`
6. 验证 CMake 构建通过
