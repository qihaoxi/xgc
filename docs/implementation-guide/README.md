# xgc 实施指南（总索引）

> 本指南的目标：**让一个没有本项目上下文的 AI 或开发者，完全依照本系列文档，就能把 xgc 框架从当前骨架推进到可用状态。**

---

## 文档结构

| 文件 | 内容 |
|------|------|
| [01-文件清单与接口规范.md](01-文件清单与接口规范.md) | 当前每个文件的职责、公开接口、内部数据结构、依赖关系 |
| [02-路线图与实现计划.md](02-路线图与实现计划.md) | 6 个 Phase 的详细任务拆解，每个任务精确到文件、函数、验证命令 |
| [03-编码约束与模式.md](03-编码约束与模式.md) | 代码风格、命名规则、Contract 规则、禁止行为清单 |
| [04-测试与示例编写规范.md](04-测试与示例编写规范.md) | 单元测试模板、集成测试模板、示例程序结构、benchmark 规范 |

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
