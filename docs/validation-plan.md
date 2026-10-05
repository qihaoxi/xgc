# xgc 验证计划 —— 正确性与性能（承蓝图 §15.1 / INTERFACE §7.5）

> 日期：2026-09-03 · 性质：验证体系设计（实现按 P1→P3 推进）
> 先例：HotSpot `VerifyBeforeGC/VerifyDuringGC`、Go `gccheckmark`、.NET GCStress、
> 差分 oracle 为 GC 测试界标准做法（文档 17 §2 已有设计，本文收敛为计划）。

---

## 0. 三层验证体系（正确性）

| 层 | 手段 | 抓什么 bug | 状态 |
|---|---|---|---|
| L1 单元/substrate | 10 个 substrate+smoke 测试（heap/bitmap/worklist/card_table/barrier/descriptor/reclaim/invariants/runtime + 两算法 smoke） | 已知场景、边界 | ✅ 已有 |
| L2 差分（oracle） | 随机对象图 + 影子可达性独立对比 | **误收活对象**（致命） | P1 新建 |
| L3 模糊/压力/阴性对照 | 种子化随机操作序列 + 故意注错 | 漏根 UAF、漏屏障丢对象、并发竞态 | P1/P2 |

GC bug 的特征是**延迟爆炸**（free 后很久才炸、漏标后很久才丢）——常规单测抓不到，
L2/L3 是主战场，不是附属。

### 0.1 L2 差分测试（oracle，文档 17 §2 落地）

- 影子预言机：测试自持对象登记表 + 独立 BFS 可达性；**禁复用 GC 的 `trace_slots` 与
  标记位**（防同源 bug 互瞒）；子引用枚举用测试自己的布局知识。
- 断言**单向**：`回收集 ⊆ oracle 死集`（可达对象必须活）。不反向断言——保守漏收
  （延迟回收）是合法行为，反向会挡住增量式/并发收集器的演进。
- 随机图 + 随机根集，PRNG 固定种子可复现；每轮 GC 后断言 + finalize 计数核对。
- **按算法参数化**：同一 harness 跑全部算法插件，按 `GcAlgorithmCaps` 裁剪断言
  （non-moving 算法跳过"槽地址在 GC 后不变"断言；moving 算法经 handle 解引用比对）。
  新算法接入即被覆盖——这是"可插拔"与"易验证"的会合点。

### 0.2 L3 压力与阴性对照

- **种子化随机操作序列**：alloc / store_ref（随机 old→new 边）/ read / drop / 手动
  collect，固定种子回放；ASan 开启跑（抓 premature free 的 UAF）。
- 对象图形状显式覆盖：树（无环）、DAG、**环**（bacon-rajan 目标场景）、老→新引用
  （写屏障/remembered set）、pin 期间收集、finalize 顺序、weakref。
- **阴性对照**（constraints §8.5 纪律，每个验证器启用前做一次）：
  - 故意漏注册一个根 → ASan 必报 UAF（证明根扫描+ASan 真的会咬）；
  - 故意绕过 `gc_store_ref` 直写堆字段 → minor GC 后必须丢活对象（证明屏障必要性
    可被观测）；
  - 注入"mark 位复用不清理"→ 下一轮 GC 必错（证明 bitmap 测试覆盖到位）。
  注错用 scratch 补丁不入库，跑完还原。
- **并发（P2）**：多线程 alloc + 写屏障 + safepoint 竞态 → TSan；debug 构建全程开
  `core/invariants` 检查器（现状只查数据结构形状，P2 扩到 GC 语义不变量：双色违规、
  悬边、card 覆盖完整性）。
- **毒化**：sweep/reclaim 后填充死模式（0xDD 类），误用已死对象立刻现形（debug 构建）。

### 0.3 sanitizer 矩阵（全部测试趟）

ASan（UAF/越界）+ TSan（并发）+ UBSan + LSan（soak 长跑泄漏）+ MSan（新分配空间
未初始化读——手写内存管理高发区，Clang-only，首跑核心套件渐进扩）。探测式 configure
与降级语义按主仓库 `cmake/sanitizer.cmake`（constraints §8.5）平移。

---

## 1. 性能验证——指标体系（不能只有 wall）

GC 的第一性能指标是**停顿分布**，不是吞吐。

| 指标 | 回答什么 | 优先级 |
|---|---|---|
| 分配吞吐 alloc/ns | 基础路径成本 | ✅ `bench/alloc_small` 已有 |
| **停顿 p50/p99/max** | 交互延迟（看分布不看均值） | P1 补（stopwatch 分布采样） |
| 标记/拷贝吞吐、每周期回收字节、触发频率 | collector 效率 | P2（`core/stats` 接上） |
| **Mutator Utilization（AMU）** | 滑窗内 mutator 工作占比——GC 对应用的真实干扰（Cheng&Blelloch；G1/ZGC 论文同款口径） | P3 |
| vs no-GC arena 基线 | GC 整体代价（只分配不回收的对照分配器） | P2 |
| old→young 写屏障成本 | 屏障税 | ✅ `bench/old_to_young_writes` 已有 |

负载形态矩阵（对齐覆盖矩阵方法论）：
- die-young 小对象风暴（验证分代假设：多数对象朝生暮死）
- 指针密集图 churn（标记成本主导；深链/深树测 worklist vs 栈）
- 大对象路径（LOS）
- old→young 写密集（✅ 已有）
- 长跑大堆（fragmentation / RSS 曲线，LSan+soak 合并跑）

**方法论**：xgc 是子仓库、位于 xvm 依赖之下，**不引 xvm_perf**（依赖方向）；bench 输出
**对齐 xvm_perf 的 JSONL schema**（同 schema 独立 serializer——子仓库自治），xvm 侧
perf P3 的 regress 编排（目录即趋势 + fence 分层）可直接消费 xgc run，GC 停顿作为
secondary metric 进同一回归门。禁止在 xgc 里复制 compare 引擎（单一权威实现）。

CI 分层：正确性门 **blocking**（三趟 sanitizer + 全测试）；
bench/soak **advisory**（nightly 记录趋势，不阻断；无稳定基线机前不做性能 blocking）。

---

## 2. 实施顺序

| 阶段 | 内容 | 前置 |
|---|---|---|
| **P1** | oracle 差分 harness（marksweep 先接）+ 种子压测 + 三项阴性对照 + 停顿分布进 bench | 无（单线程可复现，正确性主战场） |
| **P2** | invariants 扩语义不变量 + TSan 并发用例 + LSan/MSan 趟 + arena 基线 bench + stats 接上 | P1 绿 |
| **P3** | AMU 指标 + JSONL run 输出对齐 + nightly advisory 趋势 | P2 + xvm perf P3 |

每阶段验收 = 对应测试 ASan/TSan 零报错 + 阴性对照证明"门会咬" + bench 输出新指标。

## 3. 明确不做

- 运行时热切换算法（构建期 `-DGC_ALGORITHM` 已够，工业无一例外）
- Valgrind 进 CI（与 ASan/LSan 覆盖重叠；仅本地排查）
- 形式化验证（研究界方向，测试基基建已足够当前阶段）
- 性能 blocking 门（无稳定基线机；先 advisory 趋势，连续绿再议收紧）
