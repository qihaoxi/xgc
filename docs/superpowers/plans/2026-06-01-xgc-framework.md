# xgc 程序框架搭建 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立 xgc 纯 C 高性能 GC 库的完整程序骨架，包括 CMake 构建体系、核心头文件、源文件占位、代码风格配置，确保 CMake 配置与构建通过。

**Architecture:** 完全镜像 xcompiler 的 CMake 模块化构建模式。`include/xgc/` 仅导出两个公共头文件（伞式头 + 配置），`src/` 扁平一层承载所有内部实现，CMake 模块按关注点分离（编译器选项、调试、sanitizer、测试、示例、安装、格式化）。

**Tech Stack:** C11, CMake 3.10+, clang-format

---

### Task 1: 创建目录结构

**Files:**
- Create: `src/`、`include/xgc/`、`tests/`、`examples/`、`cmake/`

- [ ] **Step 1: 创建所有目录**

```bash
mkdir -p /home/clouder/workspace/xgc/{src,include/xgc,tests,examples,cmake,docs}
```

Expected: 所有目录创建成功

- [ ] **Step 2: 验证目录结构**

```bash
ls -d /home/clouder/workspace/xgc/{src,include/xgc,tests,examples,cmake,docs}
```

Expected: 六个目录路径输出

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "chore: create xgc directory skeleton"
```

---

### Task 2: .gitignore 和 .clang-format

**Files:**
- Create: `.gitignore`
- Create: `.clang-format`

- [ ] **Step 1: 创建 .gitignore（镜像 xcompiler）**

```bash
cat > /home/clouder/workspace/xgc/.gitignore << 'GITIGNORE_EOF'
### C template
# Prerequisites
*.d

# Object files
*.o
*.ko
*.obj
*.elf

# Linker output
*.ilk
*.map
*.exp

# Precompiled Headers
*.gch
*.pch

# Libraries
*.lib
*.a
*.la
*.lo

# Shared objects (inc. Windows DLLs)
*.dll
*.so
*.so.*
*.dylib

# Executables
*.exe
*.out
*.app
*.i*86
*.x86_64
*.hex

# Debug files
*.dSYM/
*.su
*.idb
*.pdb

# Kernel Module Compile Results
*.mod*
*.cmd
.tmp_versions/
modules.order
Module.symvers
Mkfile.old
dkms.conf

/cmake-build-debug/
/cmake-build-c-debug/
.idea
GITIGNORE_EOF
```

- [ ] **Step 2: 创建 .clang-format（继承 xcompiler 配置）**

```bash
cat > /home/clouder/workspace/xgc/.clang-format << 'CLANGFORMAT_EOF'
# xgc formatting style
# Inherited from xcompiler: 4-space logical indentation, tabs for indentation,
# conservative line wrapping.
BasedOnStyle: LLVM
IndentWidth: 4
TabWidth: 4
UseTab: ForIndentation
ColumnLimit: 120
BreakBeforeBraces: Attach
AllowShortIfStatementsOnASingleLine: Never
AllowShortFunctionsOnASingleLine: Empty
AllowShortLoopsOnASingleLine: false
IndentCaseLabels: true
SortIncludes: false
DerivePointerAlignment: false
PointerAlignment: Left
SpaceBeforeParens: ControlStatements
SeparateDefinitionBlocks: Always

AlignConsecutiveAssignments: true
AlignConsecutiveDeclarations: true
Cpp11BracedListStyle: false
AlignArrayOfStructures: Left
CLANGFORMAT_EOF
```

- [ ] **Step 3: Commit**

```bash
git add .gitignore .clang-format
git commit -m "chore: add .gitignore and .clang-format (inherit xcompiler style)"
```

---

### Task 3: CMake 顶层构建文件

**Files:**
- Create: `CMakeLists.txt`

- [ ] **Step 1: 创建 CMakeLists.txt（镜像 xcompiler 模式）**

```cmake
cmake_minimum_required(VERSION 3.10)

if (DEFINED XGC_PROJECT_ROOT)
    get_filename_component(XGC_PROJECT_ROOT "${XGC_PROJECT_ROOT}" ABSOLUTE)
else ()
    set(XGC_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}")
endif ()

project(xgc VERSION 0.1.0 LANGUAGES C)

set(XGC_SOURCE_ROOT "${XGC_PROJECT_ROOT}/src")
set(XGC_INCLUDE_ROOT "${XGC_PROJECT_ROOT}/include")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_SHARED "Build shared xgc library" ON)
option(ENABLE_EXAMPLES "Build xgc examples" ON)
option(ENABLE_TESTS "Build xgc tests" ON)
option(GC_MULTITHREAD "Enable multi-threading support (atomic RC + background GC)" OFF)

include(${XGC_PROJECT_ROOT}/cmake/sanitizer.cmake)
include(${XGC_PROJECT_ROOT}/cmake/debug.cmake)
include(${XGC_PROJECT_ROOT}/cmake/compiler-options.cmake)

# ---- Format target ----
add_custom_target(format
    COMMAND ${CMAKE_COMMAND}
    -DXGC_PROJECT_ROOT=${XGC_PROJECT_ROOT}
    -P ${XGC_PROJECT_ROOT}/cmake/format.cmake
    WORKING_DIRECTORY ${XGC_PROJECT_ROOT}
    COMMENT "Running clang-format over xgc sources"
)

# ---- Library sources ----
file(GLOB_RECURSE XGC_LIBRARY_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/*.c
)
file(GLOB_RECURSE XGC_LIBRARY_HEADERS CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/*.h
)

add_library(xgc STATIC ${XGC_LIBRARY_SOURCES} ${XGC_LIBRARY_HEADERS})
add_library(xgc::xgc ALIAS xgc)
target_include_directories(xgc
    PUBLIC
    $<BUILD_INTERFACE:${XGC_INCLUDE_ROOT}>
    $<BUILD_INTERFACE:${XGC_SOURCE_ROOT}>
    $<INSTALL_INTERFACE:include>
)
xgc_apply_target_defaults(xgc)
xgc_apply_debug_options(xgc)

if (GC_MULTITHREAD)
    target_compile_definitions(xgc PUBLIC GC_MULTITHREAD)
endif ()

if (ENABLE_SHARED)
    add_library(xgc_shared SHARED ${XGC_LIBRARY_SOURCES} ${XGC_LIBRARY_HEADERS})
    set_target_properties(xgc_shared PROPERTIES OUTPUT_NAME xgc)
    target_include_directories(xgc_shared
        PUBLIC
        $<BUILD_INTERFACE:${XGC_INCLUDE_ROOT}>
        $<BUILD_INTERFACE:${XGC_SOURCE_ROOT}>
        $<INSTALL_INTERFACE:include>
    )
    xgc_apply_target_defaults(xgc_shared)
    xgc_apply_debug_options(xgc_shared)
    if (GC_MULTITHREAD)
        target_compile_definitions(xgc_shared PUBLIC GC_MULTITHREAD)
    endif ()
endif ()

# ---- CLI (diagnostic tool stub) ----
# add_executable(xgc-cli ${XGC_SOURCE_ROOT}/main.c)
# target_link_libraries(xgc-cli PRIVATE xgc)
# xgc_apply_target_defaults(xgc-cli)
# xgc_apply_debug_options(xgc-cli)

if (ENABLE_EXAMPLES)
    include(${XGC_PROJECT_ROOT}/cmake/examples.cmake)
endif ()

if (ENABLE_TESTS)
    include(CTest)
    include(${XGC_PROJECT_ROOT}/cmake/tests.cmake)
endif ()

include(${XGC_PROJECT_ROOT}/cmake/install.cmake)

message(STATUS "xgc version: ${PROJECT_VERSION}")
message(STATUS "xgc source root: ${XGC_SOURCE_ROOT}")
message(STATUS "GC_MULTITHREAD: ${GC_MULTITHREAD}")
```

- [ ] **Step 2: 验证 CMake 语法（此时会因缺少 cmake 模块而失败，但能检测语法错误）**

```bash
cd /home/clouder/workspace/xgc && cmake -B /tmp/xgc-test -DXGC_PROJECT_ROOT=/home/clouder/workspace/xgc 2>&1 || true
```

Expected: 报错缺少 cmake 模块（正常），但无 CMake 语法错误

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add top-level CMakeLists.txt (mirrors xcompiler pattern)"
```

---

### Task 4: CMake 模块 — compiler-options.cmake

**Files:**
- Create: `cmake/compiler-options.cmake`

- [ ] **Step 1: 创建 compiler-options.cmake**

```cmake
include_guard(GLOBAL)

option(ENABLE_STRICT_CHECKS "Enable stricter compiler diagnostics" ON)

set(XGC_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(XGC_LIBRARY_OUTPUT_DIR "${CMAKE_BINARY_DIR}/lib")
set(XGC_ARCHIVE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/lib")

function(xgc_apply_target_defaults target_name)
    target_compile_features(${target_name} PUBLIC c_std_11)

    if (MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )

        if (ENABLE_STRICT_CHECKS)
            target_compile_options(${target_name} PRIVATE /sdl)
        endif ()
    else ()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wwrite-strings
            -Wcast-align
            -Wuninitialized
            -ffile-prefix-map=${XGC_PROJECT_ROOT}/=
            -fmacro-prefix-map=${XGC_PROJECT_ROOT}/=
            -fPIC
        )

        if (ENABLE_STRICT_CHECKS)
            target_compile_options(${target_name} PRIVATE
                -Werror
                -Wstrict-prototypes
                -Wmissing-declarations
                -Wno-sign-compare
                -Wno-unused-parameter
                -Wno-unused-function
            )

            if (CMAKE_C_COMPILER_ID STREQUAL "GNU" AND UNIX)
                target_compile_options(${target_name} PRIVATE
                    -fstack-protector
                    -fstack-protector-strong
                    -fstack-clash-protection
                )
            elseif (CMAKE_C_COMPILER_ID MATCHES "Clang" AND UNIX)
                target_compile_options(${target_name} PRIVATE
                    -fstack-protector-strong
                )
            endif ()
        endif ()
    endif ()

    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XGC_RUNTIME_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${XGC_LIBRARY_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${XGC_ARCHIVE_OUTPUT_DIR}"
    )
endfunction()
```

- [ ] **Step 2: Commit**

```bash
git add cmake/compiler-options.cmake
git commit -m "build: add cmake/compiler-options.cmake (mirrors xcompiler)"
```

---

### Task 5: CMake 模块 — debug.cmake

**Files:**
- Create: `cmake/debug.cmake`

- [ ] **Step 1: 创建 debug.cmake**

```cmake
include_guard(GLOBAL)

function(xgc_apply_debug_options target_name)
    target_compile_definitions(${target_name} PRIVATE "$<$<CONFIG:Debug>:DEBUG>")

    if (MSVC)
        target_compile_options(${target_name} PRIVATE
            "$<$<CONFIG:Debug>:/Zi>"
            "$<$<CONFIG:Debug>:/Od>"
            "$<$<CONFIG:Debug>:/RTC1>"
            "$<$<CONFIG:Debug>:/Oy->"
        )
    else ()
        target_compile_options(${target_name} PRIVATE
            "$<$<CONFIG:Debug>:-g>"
            "$<$<CONFIG:Debug>:-ggdb3>"
            "$<$<CONFIG:Debug>:-O0>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        )

        if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE
                "$<$<CONFIG:Debug>:-mno-omit-leaf-frame-pointer>"
            )
        endif ()
    endif ()
endfunction()
```

- [ ] **Step 2: Commit**

```bash
git add cmake/debug.cmake
git commit -m "build: add cmake/debug.cmake (mirrors xcompiler)"
```

---

### Task 6: CMake 模块 — sanitizer.cmake

**Files:**
- Create: `cmake/sanitizer.cmake`

- [ ] **Step 1: 创建 sanitizer.cmake**

```cmake
option(SAN_ADDRESS "Enable Address Sanitizer (Linux only)" OFF)
option(SAN_THREAD "Enable Thread Sanitizer (Linux/macOS)" OFF)
option(SAN_UB "Enable Undefined Behavior Sanitizer" OFF)

set(SANITIZER_ACTIVE OFF)

if (SAN_ADDRESS AND SAN_THREAD)
    message(WARNING "ASan and TSan are mutually exclusive. Disabling TSan.")
    set(SAN_THREAD OFF)
endif ()

if (SAN_THREAD AND SAN_UB)
    message(WARNING "TSan and UBSan may interact; disabling UBSan for TSan run.")
    set(SAN_UB OFF)
endif ()

if (SAN_ADDRESS)
    if (WIN32)
        message(WARNING "ASan disabled on Windows. SAN_ADDRESS forced OFF.")
        set(SAN_ADDRESS OFF)
    elseif (APPLE)
        message(WARNING "ASan disabled on macOS. SAN_ADDRESS forced OFF.")
        set(SAN_ADDRESS OFF)
    else ()
        message(STATUS "=== ASan enabled (Linux) ===")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize-address-use-after-scope")
        add_link_options("-fsanitize=address" "-fsanitize-address-use-after-scope")
        if (SAN_UB)
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined -fno-sanitize-recover=undefined")
            add_link_options("-fsanitize=undefined")
            message(STATUS "  UBSan: combined with ASan")
            set(SAN_UB OFF)
        endif ()
        set(SANITIZER_ACTIVE ON)
    endif ()
endif ()

if (SAN_THREAD)
    if (WIN32)
        message(WARNING "TSan disabled on Windows. SAN_THREAD forced OFF.")
        set(SAN_THREAD OFF)
    else ()
        message(STATUS "=== TSan enabled ===")
        add_compile_options("-fsanitize=thread")
        add_link_options("-fsanitize=thread")
        set(SANITIZER_ACTIVE ON)
    endif ()
endif ()

if (SAN_UB)
    message(STATUS "=== UBSan enabled ===")
    add_compile_options("-fsanitize=undefined" "-fno-sanitize-recover=undefined")
    add_link_options("-fsanitize=undefined")
    set(SANITIZER_ACTIVE ON)
endif ()

if (SANITIZER_ACTIVE)
    add_compile_definitions(SANITIZER_ENABLED=1)
endif ()
```

- [ ] **Step 2: Commit**

```bash
git add cmake/sanitizer.cmake
git commit -m "build: add cmake/sanitizer.cmake (mirrors xcompiler)"
```

---

### Task 7: CMake 模块 — tests.cmake, examples.cmake, install.cmake, format.cmake

**Files:**
- Create: `cmake/tests.cmake`
- Create: `cmake/examples.cmake`
- Create: `cmake/install.cmake`
- Create: `cmake/format.cmake`

- [ ] **Step 1: 创建 tests.cmake**

```cmake
set(XGC_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests")
file(MAKE_DIRECTORY "${XGC_TEST_OUTPUT_DIR}")

enable_testing()

function(add_xgc_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE xgc)
    xgc_apply_target_defaults(${target_name})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XGC_TEST_OUTPUT_DIR}"
    )
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# Tests will be registered here as source files are created
# add_xgc_test(test_worklist ${XGC_PROJECT_ROOT}/tests/test_worklist.c)
# add_xgc_test(test_purple   ${XGC_PROJECT_ROOT}/tests/test_purple.c)
# ...
```

- [ ] **Step 2: 创建 examples.cmake**

```cmake
if (NOT ENABLE_EXAMPLES)
    return()
endif ()

set(XGC_EXAMPLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/examples")
file(MAKE_DIRECTORY "${XGC_EXAMPLE_OUTPUT_DIR}")

function(add_xgc_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE xgc)
    xgc_apply_target_defaults(${target_name})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XGC_EXAMPLE_OUTPUT_DIR}"
    )
endfunction()

# Examples will be registered here as source files are created
# add_xgc_example(xgc_basic ${XGC_PROJECT_ROOT}/examples/basic.c)
# add_xgc_example(xgc_custom_spi ${XGC_PROJECT_ROOT}/examples/custom_spi.c)

message(STATUS "xgc examples enabled")
```

- [ ] **Step 3: 创建 install.cmake**

```cmake
install(TARGETS xgc
    EXPORT xgcTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if (TARGET xgc_shared)
    install(TARGETS xgc_shared
        EXPORT xgcTargets
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
endif ()

install(FILES
    ${XGC_INCLUDE_ROOT}/xgc/gc.h
    ${XGC_INCLUDE_ROOT}/xgc/gc_config.h
    DESTINATION include/xgc
)

install(EXPORT xgcTargets
    FILE xgcTargets.cmake
    NAMESPACE xgc::
    DESTINATION lib/cmake/xgc
)
```

- [ ] **Step 4: 创建 format.cmake**

```cmake
if (NOT DEFINED XGC_PROJECT_ROOT)
    message(FATAL_ERROR "XGC_PROJECT_ROOT is required")
endif ()

if (NOT DEFINED CLANG_FORMAT_EXECUTABLE)
    find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format-18 clang-format-17 clang-format-16)
endif ()

if (NOT CLANG_FORMAT_EXECUTABLE)
    message(FATAL_ERROR "clang-format not found. Please install clang-format to use the format target.")
endif ()

set(_xgc_format_roots
    "${XGC_PROJECT_ROOT}/include"
    "${XGC_PROJECT_ROOT}/src"
    "${XGC_PROJECT_ROOT}/tests"
    "${XGC_PROJECT_ROOT}/examples"
)

set(_xgc_format_files)
foreach(_root IN LISTS _xgc_format_roots)
    file(GLOB_RECURSE _files
        "${_root}/*.c"
        "${_root}/*.h"
    )
    list(APPEND _xgc_format_files ${_files})
endforeach()

list(REMOVE_DUPLICATES _xgc_format_files)
list(SORT _xgc_format_files)

if (_xgc_format_files)
    execute_process(
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${_xgc_format_files}
        RESULT_VARIABLE _format_result
    )
    if (NOT _format_result EQUAL 0)
        message(FATAL_ERROR "clang-format failed with exit code ${_format_result}")
    endif ()
    foreach(_file IN LISTS _xgc_format_files)
        message(STATUS "Formatted ${_file}")
    endforeach()
else ()
    message(STATUS "No C/C header files found to format")
endif ()
```

- [ ] **Step 5: Commit**

```bash
git add cmake/tests.cmake cmake/examples.cmake cmake/install.cmake cmake/format.cmake
git commit -m "build: add cmake modules (tests, examples, install, format)"
```

---

### Task 8: 对外导出头文件 — gc_config.h 和 gc.h

**Files:**
- Create: `include/xgc/gc_config.h`
- Create: `include/xgc/gc.h`

- [ ] **Step 1: 创建 gc_config.h — 用户可调整的编译开关**

```c
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
```

- [ ] **Step 2: 创建 gc.h — 伞式头文件**

```c
#ifndef XGC_GC_H
#define XGC_GC_H

/* ============================================================================
 * xgc — 通用高性能混合 GC 库
 *
 * 用户只需 #include "xgc/gc.h" 即可使用全部公开 API。
 *
 * 使用步骤:
 *   1. 实现 SPI 回调: on_trace_children / on_scan_roots / on_finalize
 *   2. 初始化 GcGlobalContext + GcThreadContext
 *   3. 通过 gc_alloc 分配对象，通过 gc_assign_field 修改堆引用
 *   4. 栈/寄存器引用不增减 RC（延迟计数语义）
 *   5. 触发 gc_collect_cycles 回收循环垃圾
 * ============================================================================
 */

#include "xgc/gc_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 前向声明（完整类型定义在 src/gc_internal.h，用户不可见） ── */

typedef struct GcHeader       GcHeader;
typedef struct GcGlobalContext GcGlobalContext;
typedef struct GcThreadContext GcThreadContext;

/* ── GC 颜色状态 ── */

typedef enum {
    GC_COLOR_BLACK  = 0,  /* 存活（外部引用存在）                          */
    GC_COLOR_GRAY   = 1,  /* 正在进行拓扑减分扫描（trial deletion 内部状态） */
    GC_COLOR_WHITE  = 2,  /* 确认为垃圾（仅环内引用，无外部引用）            */
    GC_COLOR_PURPLE = 3   /* 循环引用嫌疑（RC 减少但未归零）                 */
} GcColor;

/* ── SPI 回调类型 ── */

/* 遍历子节点: GC 核心需要知道 parent 下挂载了哪些 child */
typedef void (*GcTraceFn)(GcHeader *parent,
                          void (*visit)(GcHeader *child, void *ctx),
                          void *ctx);

/* 扫描根节点: GC 核心需要 VM 提供当前活跃栈/寄存器中的所有对象引用 */
typedef void (*GcScanRootsFn)(void *vm_thread_ctx,
                              void (*mark_alive)(GcHeader *root, void *ctx),
                              void *ctx);

/* 终结器: 对象被 GC 释放时通知 VM 清理内部资源 */
typedef void (*GcFinalizeFn)(GcHeader *obj);

/* 内存压力通知 */
typedef void (*GcPressureFn)(void *vm_ctx, int level);

/* 紫色缓冲区满触发 */
typedef void (*GcPurpleFullFn)(GcGlobalContext *global, GcThreadContext *thread);

/* ── 公开 API ── */

/* 初始化全局上下文 */
void gc_global_context_init(GcGlobalContext *global,
                            GcTraceFn         on_trace_children,
                            GcFinalizeFn       on_finalize,
                            GcPressureFn       on_pressure,
                            GcPurpleFullFn     on_purple_buffer_full);

/* 初始化线程上下文 */
void gc_thread_context_init(GcThreadContext *thread,
                            GcScanRootsFn     on_scan_roots,
                            void             *vm_thread_ctx);

/* 通用分配接口 */
void *gc_alloc(GcGlobalContext *global, GcThreadContext *thread,
               size_t total_size, uint16_t obj_type);

/* 统一写屏障 — 所有堆属性赋值必须经过此函数 */
void gc_assign_field(GcGlobalContext *global, GcThreadContext *thread,
                     GcHeader **field, GcHeader *new_child);

/* 循环检测 — Trial Deletion 核心 */
void gc_collect_cycles(GcGlobalContext *global, GcThreadContext *thread);

/* 栈根扫描并保护 — collection 期间补偿延迟 RC */
void gc_scan_stack_and_protect(GcGlobalContext *global, GcThreadContext *thread);

/* TLAB 批量冲刷到全局链表 */
void gc_flush_tlab(GcGlobalContext *global, GcThreadContext *thread);

/* 水位线检查 + 背压控制 */
void gc_check_pressure(GcGlobalContext *global, GcThreadContext *thread);

/* 自适应紫色缓冲区推入 */
void gc_push_purple_adaptive(GcGlobalContext *global, GcThreadContext *thread,
                             GcHeader *obj);

/* Safepoint 协作式轮询宏（嵌入字节码解释器循环） */
#define GC_SAFEPOINT_POLL(global, thread)                                      \
    do {                                                                       \
        if ((global)->gc_state == 2 /* GC_STATE_STW_REQUESTED */)             \
            gc_enter_safepoint_and_park((global), (thread));                   \
    } while (0)

void gc_enter_safepoint_and_park(GcGlobalContext *global, GcThreadContext *thread);

#ifdef __cplusplus
}
#endif

#endif /* XGC_GC_H */
```

- [ ] **Step 3: Commit**

```bash
git add include/xgc/gc_config.h include/xgc/gc.h
git commit -m "feat: add public headers (gc.h umbrella + gc_config.h)"
```

---

### Task 9: 内部头文件 — gc_internal.h

**Files:**
- Create: `src/gc_internal.h`

- [ ] **Step 1: 创建 gc_internal.h（所有不对外暴露的结构体定义）**

```c
#ifndef XGC_GC_INTERNAL_H
#define XGC_GC_INTERNAL_H

#include "xgc/gc.h"

#include <stddef.h>
#include <stdint.h>
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
    typedef _Atomic uint32_t gc_atomic_rc_t;
    typedef _Atomic uint8_t  gc_atomic_color_t;
    typedef _Atomic int      gc_atomic_int_t;

    #define GC_ATOMIC_LOAD(p)     atomic_load(p)
    #define GC_ATOMIC_STORE(p,v)  atomic_store(p, v)
    #define GC_ATOMIC_INC(p)      atomic_fetch_add(p, 1)
    #define GC_ATOMIC_DEC(p)      atomic_fetch_sub(p, 1)
    #define GC_ATOMIC_XCHG(p,v)   atomic_exchange(p, v)
    #define GC_CAS(p,exp,des)     atomic_compare_exchange_weak(p, exp, des)
#else
    typedef uint32_t gc_atomic_rc_t;
    typedef uint8_t  gc_atomic_color_t;
    typedef int      gc_atomic_int_t;

    #define GC_ATOMIC_LOAD(p)     (*(p))
    #define GC_ATOMIC_STORE(p,v)  (*(p) = (v))
    #define GC_ATOMIC_INC(p)      ((*(p))++)
    #define GC_ATOMIC_DEC(p)      ((*(p))--)
    #define GC_ATOMIC_XCHG(p,v)   ({ typeof(v) _old = *(p); *(p) = (v); _old; })
    #define GC_CAS(p,exp,des)     (*(p) == *(exp) ? (*(p) = (des), 1) : (*(exp) = *(p), 0))
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. 侵入式通用对象头
 * ═══════════════════════════════════════════════════════════════════════════ */

struct GcHeader {
    gc_atomic_rc_t    rc;             /* 仅记录"堆引用"的计数（栈/寄存器引用不在此计数内） */
    uint32_t          gc_ref;         /* trial deletion 工作计数（非原子，仅 GC 线程访问） */
    gc_atomic_color_t color;          /* GC 颜色状态                               */
    uint8_t           in_purple_buf;  /* 防止重复加入嫌疑缓冲区                       */
    uint16_t          obj_type;       /* 弱类型标签（由 VM 层定义）                   */
    GcHeader         *next;           /* 全局对象链表节点                            */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. 显式工作栈（防 C 递归栈溢出）
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    GcHeader **data;
    int        top;
    int        capacity;
} GcWorklist;

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. 全局上下文
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    GC_STATE_RUNNING         = 0,  /* 正常运行                                   */
    GC_STATE_STW_REQUESTED   = 2,  /* 有人发起了 STW 请求，等待所有线程到达 Safepoint */
    GC_STATE_STW_IN_PROGRESS = 3,  /* STW 回收进行中（所有 Mutator 已挂起）        */
} GcGlobalState;

struct GcGlobalContext {
    /* ── 全局对象链表 ── */
    GcHeader       *global_head;

    /* ── SPI 回调 ── */
    GcTraceFn        on_trace_children;
    GcFinalizeFn     on_finalize;
    GcPressureFn     on_pressure;
    GcPurpleFullFn   on_purple_buffer_full;

    /* ── 内存统计 ── */
    size_t           total_allocated;
    size_t           gc_threshold_soft;
    size_t           gc_threshold_hard;

    /* ── 显式工作栈（共享） ── */
    GcHeader        *worklist_data;
    int              worklist_top;
    int              worklist_capacity;

    /* ── 多线程状态（GC_MULTITHREAD 时有效） ── */
    gc_atomic_int_t  gc_state;
    gc_atomic_int_t  safepoint_arrive_count;
    int              running;        /* Background GC 线程运行标志 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. 线程本地上下文
 * ═══════════════════════════════════════════════════════════════════════════ */

struct GcThreadContext {
    /* ── 紫色缓冲区 ── */
    GcHeader       **purple_buffer;
    int              purple_count;
    int              purple_capacity;

    /* ── TLAB 本地分配链表 ── */
    GcHeader        *local_head;
    int              local_alloc_count;
    int              tlab_threshold;

    /* ── SPI 回调 ── */
    GcScanRootsFn    on_scan_roots;
    void            *vm_thread_ctx;

    /* ── Ring Buffer 临时对象区 ── */
    GcHeader        *zct_ring;
    int              zct_head;
    int              zct_capacity;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 6. 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* gc_worklist.c */
void      gc_worklist_push(GcWorklist *wl, GcHeader *obj);
GcHeader *gc_worklist_pop(GcWorklist *wl);

/* gc_ring.c */
void *gc_try_ring_buffer_alloc(GcThreadContext *thread, size_t size);
void  gc_reclaim_ring_buffer_slots(GcGlobalContext *global, GcThreadContext *thread);

/* gc_reclaim.c */
void gc_reclaim_object(GcGlobalContext *global, GcHeader *obj);

/* gc_epoch.c (GC_MULTITHREAD only) */
void gc_signal_background_thread(GcGlobalContext *global);
void gc_wait_for_signal(GcGlobalContext *global);

#endif /* XGC_GC_INTERNAL_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/gc_internal.h
git commit -m "feat: add internal header with all core type definitions"
```

---

### Task 10: 源文件骨架（10 个 .c 文件）

**Files:**
- Create: `src/gc_alloc.c`, `src/gc_assign.c`, `src/gc_collect.c`, `src/gc_worklist.c`, `src/gc_purple.c`, `src/gc_ring.c`, `src/gc_reclaim.c`, `src/gc_safepoint.c`, `src/gc_epoch.c`, `src/gc_atomic.c`

- [ ] **Step 1: 创建 src/gc_alloc.c**

```c
#include "gc_internal.h"

void
gc_global_context_init(GcGlobalContext *global,
                       GcTraceFn         on_trace_children,
                       GcFinalizeFn       on_finalize,
                       GcPressureFn       on_pressure,
                       GcPurpleFullFn     on_purple_buffer_full)
{
    /* TODO: 初始化全局上下文 */
    (void)global;
    (void)on_trace_children;
    (void)on_finalize;
    (void)on_pressure;
    (void)on_purple_buffer_full;
}

void
gc_thread_context_init(GcThreadContext *thread,
                       GcScanRootsFn     on_scan_roots,
                       void             *vm_thread_ctx)
{
    /* TODO: 初始化线程上下文 */
    (void)thread;
    (void)on_scan_roots;
    (void)vm_thread_ctx;
}

void *
gc_alloc(GcGlobalContext *global, GcThreadContext *thread,
         size_t total_size, uint16_t obj_type)
{
    /* TODO: 实现分配逻辑（Ring Buffer 快速路径 → TLAB → 水位线检查） */
    (void)global;
    (void)thread;
    (void)total_size;
    (void)obj_type;
    return NULL;
}

void
gc_flush_tlab(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: TLAB 批量冲刷到全局链表 */
    (void)global;
    (void)thread;
}

void
gc_check_pressure(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: 双水位线 + 背压控制 */
    (void)global;
    (void)thread;
}
```

- [ ] **Step 2: 创建 src/gc_assign.c**

```c
#include "gc_internal.h"

void
gc_assign_field(GcGlobalContext *global, GcThreadContext *thread,
                GcHeader **field, GcHeader *new_child)
{
    /* TODO: 实现统一写屏障（原子交换 + RC 增减 + 紫色缓冲区推入） */
    (void)global;
    (void)thread;
    (void)field;
    (void)new_child;
}
```

- [ ] **Step 3: 创建 src/gc_collect.c**

```c
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
```

- [ ] **Step 4: 创建 src/gc_worklist.c**

```c
#include "gc_internal.h"

void
gc_worklist_push(GcWorklist *wl, GcHeader *obj)
{
    /* TODO: 动态扩容 + 压栈 */
    (void)wl;
    (void)obj;
}

GcHeader *
gc_worklist_pop(GcWorklist *wl)
{
    /* TODO: 弹栈 */
    (void)wl;
    return NULL;
}
```

- [ ] **Step 5: 创建 src/gc_purple.c**

```c
#include "gc_internal.h"

void
gc_push_purple_adaptive(GcGlobalContext *global, GcThreadContext *thread,
                        GcHeader *obj)
{
    /* TODO: 自适应紫色缓冲区推入 + 满时回调 on_purple_buffer_full */
    (void)global;
    (void)thread;
    (void)obj;
}
```

- [ ] **Step 6: 创建 src/gc_ring.c**

```c
#include "gc_internal.h"

void *
gc_try_ring_buffer_alloc(GcThreadContext *thread, size_t size)
{
    /* TODO: Ring Buffer 快速路径分配（只分配不复用） */
    (void)thread;
    (void)size;
    return NULL;
}

void
gc_reclaim_ring_buffer_slots(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: Collection 后安全复用 Ring Buffer 槽位 */
    (void)global;
    (void)thread;
}
```

- [ ] **Step 7: 创建 src/gc_reclaim.c**

```c
#include "gc_internal.h"

void
gc_reclaim_object(GcGlobalContext *global, GcHeader *obj)
{
    /* TODO: 递归释放子节点 + 终结器回调 + free */
    (void)global;
    (void)obj;
}
```

- [ ] **Step 8: 创建 src/gc_safepoint.c**

```c
#include "gc_internal.h"

void
gc_enter_safepoint_and_park(GcGlobalContext *global, GcThreadContext *thread)
{
    /* TODO: Safepoint 挂起协议（标记到达 → 保存栈根 → 自旋等待 STW 完成） */
    (void)global;
    (void)thread;
}
```

- [ ] **Step 9: 创建 src/gc_epoch.c**

```c
#include "gc_internal.h"

#ifdef GC_MULTITHREAD

void
gc_signal_background_thread(GcGlobalContext *global)
{
    /* TODO: 发信号唤醒 Background GC 线程 */
    (void)global;
}

void
gc_wait_for_signal(GcGlobalContext *global)
{
    /* TODO: Background GC 线程等待信号 */
    (void)global;
}

#endif /* GC_MULTITHREAD */
```

- [ ] **Step 10: 创建 src/gc_atomic.c（空占位，原子操作在 gc_internal.h 中通过宏实现）**

```c
#include "gc_internal.h"

/* 原子操作通过 gc_internal.h 中的宏实现（GC_MULTITHREAD 条件编译）。
 * 此文件为未来可能需要的平台特定原子 fallback 预留。
 * 例如: 在不支持 C11 _Atomic 的老旧编译器上通过 CAS 循环模拟。 */
```

- [ ] **Step 11: Commit**

```bash
git add src/gc_alloc.c src/gc_assign.c src/gc_collect.c src/gc_worklist.c \
        src/gc_purple.c src/gc_ring.c src/gc_reclaim.c src/gc_safepoint.c \
        src/gc_epoch.c src/gc_atomic.c
git commit -m "feat: add source file skeletons with TODO markers"
```

---

### Task 11: 验证 CMake 配置与构建

- [ ] **Step 1: CMake 配置**

```bash
cd /home/clouder/workspace/xgc && cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug 2>&1
```

Expected: `-- Configuring done` `-- Generating done`，无错误。

- [ ] **Step 2: 构建静态库**

```bash
cmake --build cmake-build-debug --target xgc 2>&1
```

Expected: 所有 .c 文件编译通过（有 unused-parameter warning 但不影响），`libxgc.a` 生成成功。

- [ ] **Step 3: 构建共享库**

```bash
cmake --build cmake-build-debug --target xgc_shared 2>&1
```

Expected: `libxgc.so` 生成成功。

- [ ] **Step 4: 验证产物**

```bash
ls -la cmake-build-debug/lib/libxgc.a cmake-build-debug/lib/libxgc.so
```

Expected: 两个文件均存在。

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "build: verify cmake configure and build pass"
```

---

## 验证清单

构建完成后执行:

```bash
cd /home/clouder/workspace/xgc

# 1. 目录结构
find . -not -path './.git/*' -not -path './cmake-build-debug/*' -not -name '*.md' | sort

# 2. Debug 构建
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug

# 3. Release 构建
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release

# 4. 多线程构建
cmake -B cmake-build-mt -DCMAKE_BUILD_TYPE=Debug -DGC_MULTITHREAD=ON
cmake --build cmake-build-mt
```
