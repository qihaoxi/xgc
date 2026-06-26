# 16 — 构建系统、平台移植与 ABI

> 工业级框架不只是算法。本文件覆盖构建系统细节、跨平台移植、ABI/版本稳定性、CI 配置。

---

## 1. CMake 构建系统完整说明

### 1.1 顶层选项

```cmake
# CMakeLists.txt 中的所有 option
option(ENABLE_SHARED      "构建共享库 libxgc.so"        ON)
option(ENABLE_EXAMPLES    "构建示例程序"                ON)
option(ENABLE_TESTS       "构建测试"                    ON)
option(ENABLE_BENCHMARKS  "构建 benchmark"              OFF)
option(GC_MULTITHREAD     "启用多线程（原子操作）"       OFF)
option(GC_DEBUG           "启用调试观测框架"             OFF)
option(SAN_ADDRESS        "启用 AddressSanitizer"       OFF)
option(SAN_THREAD         "启用 ThreadSanitizer"        OFF)
option(SAN_UB             "启用 UBSanitizer"            OFF)

# 算法选择（编译时单选）
set(GC_ALGORITHM "gen_copy_ms" CACHE STRING "GC 算法")
set_property(CACHE GC_ALGORITHM PROPERTY STRINGS
    marksweep gen_copy_ms bacon-rajan gen_copy_compact regional)
```

### 1.2 算法选择如何工作

```cmake
# 只编译选中算法目录下的 .c
file(GLOB_RECURSE XGC_ALGO_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/algo/${GC_ALGORITHM}/*.c)
# core/ 全部编译
file(GLOB_RECURSE XGC_CORE_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/core/*.c)
```

**约束**：每个算法目录必须实现完整 vtable + 工厂函数。算法目录名 = `GC_ALGORITHM` 取值（注意 `bacon-rajan` 用连字符，`gen_copy_ms` 用下划线，目录名必须精确匹配）。

### 1.3 新增 cmake 模块

| 模块 | 何时新建 |
|------|---------|
| `cmake/benchmarks.cmake` | Step C/优化阶段 |
| `cmake/coverage.cmake` | 需要覆盖率报告时 |
| `cmake/install.cmake` | 已存在，发布时用 |

### 1.4 benchmarks.cmake 模板

```cmake
if (NOT ENABLE_BENCHMARKS)
    return()
endif()

set(XGC_BENCH_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bench")
file(MAKE_DIRECTORY "${XGC_BENCH_OUTPUT_DIR}")

function(add_xgc_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE xgc)
    xgc_apply_target_defaults(${target_name})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XGC_BENCH_OUTPUT_DIR}")
endfunction()

add_xgc_benchmark(bench_alloc_small ${XGC_PROJECT_ROOT}/bench/alloc_small.c)
add_xgc_benchmark(bench_old_to_young ${XGC_PROJECT_ROOT}/bench/old_to_young_writes.c)
```

---

## 2. 跨平台移植

### 2.1 平台抽象点

| 能力 | Linux | macOS | Windows | fallback |
|------|-------|-------|---------|---------|
| 页分配 | `mmap` | `mmap` | `VirtualAlloc` | `malloc` |
| 线程 | `pthread` | `pthread` | Win32 thread / C11 thread | 单线程退化 |
| 原子 | C11 `_Atomic` | C11 `_Atomic` | C11 `_Atomic` / MSVC intrinsic | 普通类型 |
| 条件变量 | `pthread_cond` | `pthread_cond` | `CONDITION_VARIABLE` | — |
| 高精度时钟 | `timespec_get` | `timespec_get` | `QueryPerformanceCounter` | `clock()` |

### 2.2 平台抽象层文件

```
src/core/platform.h（新建）— 平台抽象宏/inline
src/core/platform_posix.c   — POSIX 实现
src/core/platform_win32.c   — Windows 实现
```

```c
/* platform.h */
#if defined(_WIN32)
    #define GC_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
    #define GC_PLATFORM_MACOS 1
#elif defined(__linux__)
    #define GC_PLATFORM_LINUX 1
#endif

/* 统一接口 */
void*  gc_platform_page_alloc(size_t size);
void   gc_platform_page_free(void* ptr, size_t size);
double gc_platform_now_ms(void);
```

### 2.3 编译器差异

| 特性 | GCC/Clang | MSVC |
|------|-----------|------|
| `__typeof__` | 支持 | 不支持（用 C11 `_Generic` 或模板宏） |
| statement expression `({...})` | 支持（`__extension__`） | 不支持 |
| `__builtin_prefetch` | 支持 | `_mm_prefetch` |
| `__attribute__((aligned))` | 支持 | `__declspec(align())` |

**当前 `GC_ATOMIC_XCHG` 用了 GNU 扩展** → 移植 MSVC 时需要替换为 C11 `atomic_exchange` 或 `_InterlockedExchange`。

---

## 3. ABI 与版本稳定性

### 3.1 版本号规则（语义化版本）

```c
/* include/xgc/gc_version.h（新建） */
#define XGC_VERSION_MAJOR 0
#define XGC_VERSION_MINOR 1
#define XGC_VERSION_PATCH 0
#define XGC_VERSION_STRING "0.1.0"

const char* gc_version_string(void);
int gc_version_major(void);
```

- MAJOR：不兼容的 API/ABI 变更（如改 `gc_header` 字段）
- MINOR：向后兼容的新功能
- PATCH：bug 修复

### 3.2 ABI 稳定性承诺

| 元素 | 稳定性 | 说明 |
|------|--------|------|
| `gc_header` 布局 | **冻结** | 改动 = MAJOR 版本 |
| `include/xgc/*.h` 中的公共函数签名 | 稳定 | 新增 OK，改签名 = MAJOR |
| `gc_descriptor` 布局 | 半稳定 | 末尾追加字段 OK（旧代码零初始化） |
| `gc_algorithm_vtable` 布局 | 半稳定 | 末尾追加函数指针 OK |
| `src/gc_internal.h` | **不稳定** | 内部头，随时可改 |
| `src/core/` `src/algo/` 内部函数 | **不稳定** | 实现细节 |

### 3.3 结构体扩展的兼容性规则

```c
/* 给 gc_descriptor 加字段时，加在末尾： */
struct gc_descriptor {
    /* ... 现有字段 ... */
    gc_finalize_fn finalize;
    /* 新增字段加这里 ↓ 旧的静态初始化器会零填充新字段 */
    gc_trace_weak_slots_fn trace_weak_slots;  /* 新增 */
};
/* 旧代码: static gc_descriptor d = {.name=..., .finalize=...};
   trace_weak_slots 自动为 NULL → 框架检查 NULL 后跳过 → 兼容 */
```

---

## 4. CI 配置

### 4.1 CI 矩阵

```yaml
# .github/workflows/ci.yml（示意）
matrix:
  algorithm: [marksweep, gen_copy_ms, bacon-rajan]
  build_type: [Debug, Release]
  sanitizer: [none, address, undefined]
  multithread: [OFF, ON]
```

### 4.2 CI 必跑步骤

```bash
# 1. 格式检查
cmake --build build --target format-check  # clang-format --dry-run -Werror

# 2. 每个算法 × 每个 sanitizer 构建 + 测试
for algo in marksweep gen_copy_ms bacon-rajan; do
  for san in "" "-DSAN_ADDRESS=ON" "-DSAN_UB=ON"; do
    cmake -B b -DGC_ALGORITHM=$algo $san -DCMAKE_BUILD_TYPE=Debug
    cmake --build b
    ctest --test-dir b --output-on-failure
  done
done

# 3. 多线程构建 + TSan
cmake -B bmt -DGC_MULTITHREAD=ON -DSAN_THREAD=ON
cmake --build bmt && ctest --test-dir bmt

# 4. Release 构建（确保无 Debug-only 代码泄漏）
cmake -B brel -DCMAKE_BUILD_TYPE=Release
cmake --build brel && ctest --test-dir brel
```

### 4.3 CI 通过标准

- 所有矩阵组合构建 0 警告
- 所有测试 Passed
- 所有 sanitizer 0 报错
- clang-format 无 diff

---

## 5. 发布与安装

### 5.1 安装产物

```
install/
├── include/xgc/        # 公共头文件
│   ├── gc.h
│   ├── gc_types.h
│   ├── gc_algorithm.h
│   ├── gc_config.h
│   └── gc_version.h
├── lib/
│   ├── libxgc.a        # 静态库
│   └── libxgc.so       # 共享库
└── lib/cmake/xgc/      # CMake config（供 find_package(xgc)）
    └── xgcConfig.cmake
```

### 5.2 下游集成方式

```cmake
# 下游项目
find_package(xgc REQUIRED)
target_link_libraries(my_vm PRIVATE xgc::xgc)
```

---

## 6. 编译期配置矩阵总结

| 配置项 | 取值 | 影响 |
|--------|------|------|
| `GC_ALGORITHM` | marksweep/gen_copy_ms/bacon-rajan/... | 编译哪个算法 |
| `GC_MULTITHREAD` | ON/OFF | 原子操作 vs 普通操作 |
| `GC_DEBUG` | ON/OFF | 调试观测框架 |
| `CMAKE_BUILD_TYPE` | Debug/Release | 优化级别 + 断言 |
| `SAN_ADDRESS/THREAD/UB` | ON/OFF | sanitizer |

**正交性要求**：以上配置项必须能任意组合，每种组合都能构建通过。
