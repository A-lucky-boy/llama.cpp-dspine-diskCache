# 交付说明：两级提示缓存改造（阶段 0–3 + 修复轮）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache`，分支 `main`，远端已同步至 `3ba52dc` |
| 交付时间 | 2026-11-15 20:25（GMT+8） |
| 规划依据 | `betterModification.md`：`test1_1`（阶段 0–7 路线图）、`test1_2`（阶段 3 实施）、`test1_2` 19-51（修复轮） |
| 阅读对象 | 实验规划人员（决定下一轮实验做什么、验收标准怎么定） |
| 本文件的定位 | 只讲"现在有什么能力、哪些数字可信、哪些约束不能破、还缺什么"。字段级变更在 `change.md`，架构在 `projectSummary.md`，排查结论在 `experienceHistory.md` |

---

## 1. 一句话结论

规划里的 **阶段 0（缺陷修复）、阶段 1（精确命中统计）、阶段 2（请求边界捕获）、阶段 3（公共前缀目录）** 已全部落地并通过单用户回归；**阶段 4–7 未开始**，其中阶段 4（分块 KV）存在必须先解除的前置阻断：`llama_state_seq_get/set_data_ext` 是否支持"导出前 N token 的自洽状态"尚未审查，不得据此切片。

**重要边界**：本轮没有任何"命中率提升"的数字。阶段 1/2 是观测与捕获能力，阶段 3 只产出元数据（发现 ≠ 可恢复 ≠ 命中）。任何性能结论都需要另立实验。

---

## 2. 交付范围（提交链）

| 提交 | 内容 | 对应规划 |
|---|---|---|
| `01d5686` | 请求级 token 分解统计（`--cache-stats`、`llamacpp:cache_*`）、请求边界捕获（`--cache-boundary-capture`） | 阶段 1 + 阶段 2 |
| `8325ae4` | 单用户公共前缀目录 `server-prefix-index.*`、5 个 `--cache-prefix-*` 开关、`cache_prefix_*` 指标、独立自检 | 阶段 3 |
| `57d9fce` | 修复轮：容量终止性、删除账本、引用通知时序、索引输入校验与只读性、钳制一致性 | `test1_2` 19-51 |
| `342fd7d` / `3ba52dc` | 实验记录、`change.md`、`projectSummary.md`、README 同步 | 文档 |

阶段 0（v8 条目格式 + v3 清单 + `touch_entry` 偏移修复 + 先校验后恢复）在更早的提交中，已于 `change.md` 的 `18-18` / `18-35` 记录。

---

## 3. 现在具备的能力

| 能力 | 开关（默认） | 状态 |
|---|---|---|
| RAM/Disk 两级快照缓存 + v8 完整性校验 | `--cache-disk PATH`（关） | 已有，本轮未改格式 |
| 请求级 token 来源分解与拒绝原因 | `--cache-stats`（关） | 新增，实测自洽 |
| 输入/输出边界主动捕获快照 | `--cache-boundary-capture`（none） | 新增 |
| 公共前缀发现（只读元数据目录） | `--cache-prefix-discovery`（关） | 新增 |
| 物理快照引用与失效（精确端点 + epoch 顺序） | 随目录自动 | 新增 |
| 删除失败隔离与账面配额 | 无开关（语义修正） | 新增 |

所有新能力默认关闭；实测关闭态与改造前的 `prompt_n / cache_n`、输出文本 sha 完全一致。

---

## 4. 可复用的实测数字（双卡 4070TiS+3090，`-ts 38,62`，语料 `E:\小说\正文.md`）

| 事实 | 数值 | 证据 |
|---|---|---|
| 语料密度 | 1.44 字符/token；2000 字符 ≈ 1385 token | 全部运行 |
| 条目体积（v8，27B + 草稿） | 1385 token → 505.77 MiB；6233 token → 1225.98 MiB（约 25–40 KiB/token，随检查点数量变化） | `logs/prefix-runB.log` |
| 读盘恢复 | 530,543,650 B / 436 ms（1385-token 前缀）；366 ms（1596-token 前缀） | `logs/stats-run2.log`、`logs/boundary-run3.log` |
| 写盘 | 505.97 MiB / 482–503 ms（C: 盘，生产缓存盘更慢） | `logs/boundary-run2.log` |
| 冷 prefill | 1385 token ≈ 1.4–1.5 s（≈ 1.0 ms/token） | 全部运行 |
| 增量续算 | 同会话追加 211 token 时 `reused_slot=1381, prefill=215` | `logs/fix-run1.log` |
| 删除失败时的账面 | 505.97 + 1059.99 MiB 全部计入 `cache_disk_bytes=1.64203e+09` | `logs/del-run.log` |
| 修复后回收 | 句柄释放 → `cache_disk_bytes=7.00719e+08`、`quarantined=0` | 同上 |

**一条容易被误用的数据**：`candidate_lcp=1596 / restored=0 / reused_disk=0 / reject=io_cost`（1225.98 MiB ÷ 1596 = 786 KiB/token > 768 KiB 成本闸门）。候选前缀**不是**命中量，任何统计口径都必须落在 `reused_*`。

---

## 5. 做实验时不能破坏的不变量

1. `prompt = reused_slot + reused_ram + reused_disk + prefill`：每个输入 token 只计入一类。
2. `observed / eligible / persisted` 一律不计入 `reused_*`；`persisted` 只表示"存在精确对齐且校验通过的快照"。
3. 磁盘条目 v8 与清单 v3 是硬边界；不得为省事改变头部偏移、CRC 覆盖范围或签名维度。
4. 恢复失败语义：写入 KV 之前失败 → 干净回退候选；写入之后失败 → 清空 slot 重新 prefill。
5. 删除失败必须继续占用配额（`cache_disk_bytes` 含隔离条目），不得为填平差额删除受引用保护的对象。
6. 提交/失效通知只有一条通道：磁盘锁内同一临界区同步交付 + 单调 `epoch`；不得再加一条异步路径。
7. 前缀目录只读：不参与候选选择、不触发序列化、不改变 `load()` 的结果。

---

## 6. 已知限制与"不能外推"的结论

| 项 | 现状 | 影响 |
|---|---|---|
| 多 slot（`-np > 1`） | 完全未验证 | 并发下的目录与缓存竞争无数据 |
| 中断 / 超时 / 断流 | 未构造用例 | 边界捕获与引用清理在异常路径下无实测 |
| `clamp_candidates()` | 防御性路径，真实请求无法触发（媒体身份含 token 数 + 区间校验） | 只能由单元测试覆盖，不要据此设计实验 |
| 磁盘层"迟到提交"交错 | 锁内同步交付后**不可能发生**，端到端无法复现 | 验证靠静态证明 + 索引侧逆序用例 |
| 目录收益 | **未测，也不该期望**：它不改变复用行为 | 想评估收益必须先有"按公共前缀驱动捕获"的实现 |
| 前缀目录持久化 | 不持久化，重启由已校验条目重建（epoch 从 0 起算） | 跨重启的 tombstone 语义不保留 |
| Linux/POSIX | 未构建 | 删除/替换语义仅 Windows 实测 |
| 部署 | 未部署，`D:\AiHelper\llama.cpp` 仍是旧二进制 | 生产数据不能代表本轮代码 |

---

## 7. 对下一轮实验的输入（供规划判断，不是既定计划）

### 7.1 阶段 4 的前置审查（建议作为下一轮唯一必做项）

判据只有一条：**是否存在正式 API 能导出"前 N token 的自洽状态"**。

- 若"否"：阶段 4 的分块 KV 不能实施，路线应转向"阶段 3 的独立公共前缀完整快照 + 阶段 5 的对象级去重"，即只对**完全相同**的前缀对象做共享引用，不承诺块级增量。
- 若"是"：仍需先验证跨 target/draft、KV 量化、`--kv-unified`、检查点组合下的数值一致性，再谈格式设计。
- 审查对象：`src/llama-context.cpp` 的序列状态导出/恢复实现，以及 `llama_state_seq_get_data_ext` 的文档语义。

### 7.2 可由现有指标驱动的实验方向

| 方向 | 已有观测手段 | 需要的验收标准（建议） |
|---|---|---|
| 真实流量下的前缀复用结构 | `cache_stats:` 每请求一行 + `cache_prefix_*` | 冷/暖请求的 `reused_*` 分解；分叉点分布；`reject` 原因分布 |
| 边界捕获的写入代价与收益 | `cache_disk_bytes_written_total`、`cache_disk_enqueue_*`、`restore_seconds` | 单位写入字节换回多少 `reused_disk` token |
| 成本闸门（`768 KiB/token`）是否合适 | `reject=io_cost` 计数 | 在不同盘（NVMe vs USB）上重测阈值，不要在旧阈值上争论命中率 |
| 写入抑制（4096 token lag） | `enqueue_dedup_total` 与 `prefill` | 抑制后剩余 prefill 是否仍在可接受范围 |

### 7.3 明确不建议现在做的

- 为提高表面命中率放宽 `io_cost`、媒体隔离或 CRC 校验。
- 在未审查 API、未验证数值一致性的前提下切分 opaque state。
- 用 `candidate_lcp` 或目录的 `persisted` 计数当性能指标。

---

## 8. 复现方式

```text
构建：build-cuda-shared.bat                    # 产物在 output\
单请求：python test-run/probe.py <chars|start:end> <n_predict> <label> [ext:N]
指标：python test-run/metrics.py              # cache_* 与 cache_prefix_*
自检：cl ... tools/server/tests/unit/test_cache_prefix_index.cpp tools/server/server-prefix-index.cpp
      （13 组纯 CPU 检查，不加载模型；完整命令见该文件头）
回归日志：test-run/logs/{stats-run*,boundary-run*,prefix-run*,fix-run*,del-run}.log
```

---

## 9. 交付物清单

| 文件 | 内容 |
|---|---|
| `tools/server/server-prefix-index.h/.cpp` | 前缀目录（radix tree、媒体身份、引用与 epoch、容量控制） |
| `tools/server/server-task.h/.cpp` | 磁盘层：v8 格式、统一删除原语、隔离、锁内通知、指标 |
| `tools/server/server-context.cpp` | 边界事件、观察发布、请求级统计、目录生命周期 |
| `tools/server/tests/unit/test_cache_prefix_index.cpp` | 13 组自检 |
| `common/common.h`、`common/arg.cpp` | 7 个新开关（stats / boundary / 5 个 prefix） |
| `change.md` | `18-18`、`18-35`、`18-49`、`19-35`、`20-22` 五条记录 |
| `projectSummary.md` | 结构、数据流、不变量、阶段进度 |
| `reports/2026-11-15-cache-status-report.md` | 阶段 1/2/3 的状态快照 |
| `reports/2026-11-15-test1_2-experiment-record.md` | 阶段 3 的判据与实测记录 |
| `experienceHistory.md` | 已排除方案与"不能外推"的结论 |

---

## 10. 需要规划人员确认的四件事

1. **阶段 4 的审查是否作为下一轮的硬门槛**：如果不做审查就规划分块 KV，等于接受"可能切错 KV"的风险，这在本项目的规则里是禁止项。
2. **是否接受"目录不产生收益"这一现状**：若希望它产生收益，需要先规划"按 `eligible` 前缀主动捕获快照"的实验（当前未实现，且必须复用现有 v8 快照，不得引入新格式）。
3. **性能实验的基准环境**：现有数字来自 C: 盘测试实例与 16K 上下文，生产是 `E:\llama.cpp.cache`（USB）+ 更大上下文，两者不可直接比较。
4. **是否需要在部署前补多 slot 与中断路径验证**：当前生产是 `-np 1`，如果规划里出现并发场景，这部分现在没有数据支撑。
