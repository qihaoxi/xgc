# xgc 算法可插拔重构 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 xgc 框架为编译时可切换 GC 算法架构，当前保留 Bacon-Rajan 为唯一算法实现。

**Architecture:** 算法相关源文件（alloc/assign/collect/safepoint）移入 `src/algo/bacon_rajan/`，共享工具（worklist/ring/reclaim/epoch/atomic/purple）保留在 `src/`。CMake 通过 `GC_ALGORITHM` 选项选择编译哪个算法目录。

**Tech Stack:** C11, CMake 3.12+

---

### Task 1: 创建算法目录并迁移文件

**Files:**
- Create: `src/algo/bacon_rajan/`
- Move: `src/gc_alloc.c` -> `src/algo/bacon_rajan/gc_alloc.c`
- Move: `src/gc_assign.c` -> `src/algo/bacon_rajan/gc_assign.c`
- Move: `src/gc_collect.c` -> `src/algo/bacon_rajan/gc_collect.c`
- Move: `src/gc_safepoint.c` -> `src/algo/bacon_rajan/gc_safepoint.c`

- [ ] **Step 1: 创建算法目录**

```bash
mkdir -p /home/clouder/workspace/xgc/src/algo/bacon_rajan
```

- [ ] **Step 2: 用 git mv 迁移 4 个算法源文件**

```bash
cd /home/clouder/workspace/xgc
git mv src/gc_alloc.c    src/algo/bacon_rajan/gc_alloc.c
git mv src/gc_assign.c   src/algo/bacon_rajan/gc_assign.c
git mv src/gc_collect.c  src/algo/bacon_rajan/gc_collect.c
git mv src/gc_safepoint.c src/algo/bacon_rajan/gc_safepoint.c
```

- [ ] **Step 3: 确认 src/ 下只剩共享文件**

```bash
ls /home/clouder/workspace/xgc/src/*.c
```

Expected: `gc_atomic.c  gc_epoch.c  gc_purple.c  gc_reclaim.c  gc_ring.c  gc_worklist.c`

- [ ] **Step 4: Commit**

```bash
git commit -m "refactor: move algo-specific sources to src/algo/bacon_rajan/"
```

---

### Task 2: 更新 CMakeLists.txt 支持算法选择

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 替换源文件收集逻辑**

将现有的:
```cmake
file(GLOB_RECURSE XGC_LIBRARY_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/*.c
)
```

替换为:
```cmake
# ---- GC Algorithm selection ----
set(GC_ALGORITHM "bacon-rajan" CACHE STRING "GC algorithm implementation (bacon-rajan)")
message(STATUS "GC Algorithm: ${GC_ALGORITHM}")

# ---- Algorithm sources ----
file(GLOB_RECURSE XGC_ALGO_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/algo/${GC_ALGORITHM}/*.c
)

# ---- Shared utility sources ----
file(GLOB XGC_SHARED_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/*.c
)

set(XGC_LIBRARY_SOURCES
    ${XGC_ALGO_SOURCES}
    ${XGC_SHARED_SOURCES}
)
```

- [ ] **Step 2: 完整 CMakeLists.txt 验证**

```bash
cat /home/clouder/workspace/xgc/CMakeLists.txt
```

确认 `XGC_LIBRARY_SOURCES` 由 `XGC_ALGO_SOURCES` + `XGC_SHARED_SOURCES` 组成。

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add GC_ALGORITHM option for compile-time algo selection"
```

---

### Task 3: 验证构建通过

- [ ] **Step 1: 清理旧构建，重新配置**

```bash
cd /home/clouder/workspace/xgc
rm -rf cmake-build-debug cmake-build-mt
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
```

Expected: `-- GC Algorithm: bacon-rajan` 出现在输出中，`-- Configuring done`

- [ ] **Step 2: 构建**

```bash
cmake --build cmake-build-debug
```

Expected: 所有源文件编译通过，`libxgc.a` + `libxgc.so` 生成成功，零警告。

- [ ] **Step 3: GC_MULTITHREAD 构建**

```bash
cmake -B cmake-build-mt -DCMAKE_BUILD_TYPE=Debug -DGC_MULTITHREAD=ON
cmake --build cmake-build-mt --target xgc
```

Expected: 编译通过，零警告。

- [ ] **Step 4: Commit (如有修复) 或确认通过**
