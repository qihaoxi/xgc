# xgc 实施指南（总索引）

> 本指南的目标：**让一个没有本项目上下文的 AI 或开发者，完全依照本系列文档，就能把 xgc 框架从当前骨架推进到可用状态。**

---

## 阅读顺序

**新接手的 AI 必须先读 [00-关键实现路径.md](00-关键实现路径.md)** —— 它规定了严格的 Step 执行顺序和验收标准。其余文档是按需查阅的参考。

## 文档结构

### 总控（最先读）
| 文件 | 内容 |
|------|------|
| [00-关键实现路径.md](00-关键实现路径.md) | **总控文档**：Step 依赖图、执行顺序、验收标准、失败处理协议、文档导航 |

### 参考文档（按需查阅）
| 文件 | 内容 |
|------|------|
| [01-文件清单与接口规范.md](01-文件清单与接口规范.md) | 当前每个文件的职责、公开接口、内部数据结构、依赖关系 |
| [02-路线图与实现计划.md](02-路线图与实现计划.md) | 6 个 Phase 的详细任务拆解，每个任务精确到文件、函数、验证命令 |
| [03-编码约束与模式.md](03-编码约束与模式.md) | 代码风格、命名规则、Contract 规则、禁止行为清单 |
| [04-测试与示例编写规范.md](04-测试与示例编写规范.md) | 单元测试模板、集成测试模板、示例程序结构、benchmark 规范 |
| [05-算法实现详解.md](05-算法实现详解.md) | 6 种算法的完整伪代码、数据结构、关键陷阱、步骤拆解 |
| [06-调试观测框架.md](06-调试观测框架.md) | 三层调试机制（invariant/event log/panic dump）、插桩点、使用方法 |
| [07-高速GC优化路线.md](07-高速GC优化路线.md) | 分配/屏障/收集优化手段、atomic vs 无锁策略、性能度量标准 |
| [08-弱引用与终结器.md](08-弱引用与终结器.md) | 弱引用语义、两阶段终结模型、复活处理、交互矩阵 |
| [09-多线程与Safepoint.md](09-多线程与Safepoint.md) | 线程注册、协作式轮询、STW 时序、Epoch 机制、死锁规避 |
| [10-内存分配器与堆布局.md](10-内存分配器与堆布局.md) | Arena/Page/Size-Class/Region 四层分配器、Space 抽象 |
| [11-VM集成指南.md](11-VM集成指南.md) | Descriptor/Roots/写屏障/Handle 完整接入手册 + Checklist |
| [12-统计度量与调优.md](12-统计度量与调优.md) | 扩展 gc_stats、背压机制、自适应触发、Benchmark 规范 |
| [13-对象元数据映射实现.md](13-对象元数据映射实现.md) | **伪代码→可编译 C**：rc_node/find_node/forwarding 的真实实现，三种映射方案 |
| [14-错误处理与边界条件.md](14-错误处理与边界条件.md) | NULL/OOM/溢出/重入的确定性行为规约、边界条件清单 |
| [15-术语表与FAQ.md](15-术语表与FAQ.md) | 术语表、设计 FAQ、故障排查表、完成度自检清单 |
| [16-构建平台与ABI.md](16-构建平台与ABI.md) | CMake 细节、跨平台移植、ABI/版本稳定性、CI 矩阵 |
| [17-算法插件注册与切换.md](17-算法插件注册与切换.md) | 编译时/运行时切换、算法注册表、能力协商、算法选择决策树 |
| [18-并发正确性与内存模型.md](18-并发正确性与内存模型.md) | happens-before、C11 内存序速查、三色不变式、RC 原子、TSan 门禁 |
| [19-正确性验证与压力测试.md](19-正确性验证与压力测试.md) | 差分测试(影子预言机)、随机图生成、压力/模糊测试、回归语料库 |
| [20-性能基线与硬件优化.md](20-性能基线与硬件优化.md) | 性能基线、CI 回归门禁、指针压缩/tagged/NUMA/SIMD/预取 |
| [21-JIT逃逸分析协作契约.md](21-JIT逃逸分析协作契约.md) | 前瞻接口契约：safepoint 内联/屏障消除/精确栈图/derived pointer |

### 分类
-   **00 关键路径(总控)**
-   **01-04 现状/路线/约束/测试**
-   **05-07 算法/调试/优化**
-   **08-12 弱引用/多线程/分配/集成/度量**
-   **13-16 元数据映射/错误/术语/构建ABI**
-   **17-21 插件切换/内存模型/验证/性能/JIT契约**

---

## 全局约束（必须在开始任何实现之前阅读）

### C 标准
- C11，`-Wall -Wextra -Wpedantic -Werror`
- 禁用 GNU 扩展（`CMAKE_C_EXTENSIONS OFF`），仅 `__extension__` 包裹的 `GC_ATOMIC_XCHG` 例外
- 使用 tab 缩进，4 空格宽度，120 列限制

### 架构原则
1. **公共对象头 `gc_header` 不允许增加字段** — 算法私有状态放 `rt->algo_state` 或 side metadata
2. **所有堆字段写入必须经过 `gc_store_ref`** — 不允许绕过写屏障
3. **新的共享能力优先沉淀到 `src/core/`** — `src/algo/` 只放算法差异实现
4. **每个 substrate 必须有独立单元测试** — `tests/test_*_substrate.c`
5. **每个算法必须实现 `GcAlgorithmVTable` 中至少 `alloc` + `collect_full`**
6. **caps 声明必须与实际函数指针一致** — 启动时 `gc_runtime_validate_contracts` 验证
7. **不要在 `include/xgc/` 头文件中暴露 `src/gc_internal.h` 中的类型**

### 构建与验证
```bash
# 默认构建（gen_copy_ms 算法）
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build && ctest --test-dir build

# 切换算法
cmake -B build-ms -DGC_ALGORITHM=marksweep
cmake -B build-br -DGC_ALGORITHM=bacon-rajan

# ASan 检查
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSAN_ADDRESS=ON
cmake --build build-asan && ctest --test-dir build-asan

# 每次提交前必须：全算法通过 + ASan 零报错
```

### 提交规范
- 遵循 Conventional Commits：`feat:` / `fix:` / `refactor:` / `test:` / `docs:` / `build:`
- 每个子任务一个 commit
- commit message 描述 why，不描述 what
