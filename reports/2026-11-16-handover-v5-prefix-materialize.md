# 2026-11-16 00-50 交付说明（单用户磁盘提示缓存：第七轮 —— 公共前缀主动物化）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `147001c`（会话前 HEAD，第六轮交付说明提交） |
| 本次交付终点 | `ab6bac2` |
| 功能提交 | `ab6bac2` |
| 规划依据 | `betterModification.md` → `2026-11-15 23-38 - test1_3` |
| 构建产物 | `0.9.0-dev (build 27)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | **新算法功能**（非缺陷修复），含规划要求的源码一致性整理 |

> **构建产物的版本串说明**：二进制内嵌的提交号显示为 `commit 147001c`，因为构建脚本读取的是构建时刻的 `HEAD`，而功能改动在此之前已落到工作树。`ab6bac2` 是包含全部改动的提交，两者代码内容一致。重新构建一次即可让版本串对齐，此处如实标注而不修改构建脚本。

逐项实现细节见 `change.md` 的 `2026-11-16 01-05` 条目；项目现状见 `projectSummary.md`。

---

## 1. 本轮目标与达成情况

规划要实现的是：**发现公共前缀不等于磁盘上存在能恢复它的 KV 状态**。此前输入/输出边界捕获只在完整请求边界工作，无法把两个不同请求共享的中间前缀变成独立可恢复对象。

规划要求打通的链路：

```text
多请求共享前缀 A → 目录返回 eligible(A) → 后续请求 prefill 经过 A 结束位置
    → 目标/草稿状态在该位置完整 → 导出 A 的完整序列 KV
    → 磁盘写队列与配额事务 → 真实提交后目录登记 persisted(A)
    → 重启后由 disk.load() 读取并按 n_past 真实复用
```

**该链路已端到端贯通**（证据见 §4）。

| 目标 | 状态 |
|---|---|
| 前瞻式 prefill 边界捕获 | 已实现并实测 |
| 只捕获真实计算已完成的前缀 | 已实现（`pos_max + 1 == K` 才导出） |
| 独立磁盘对象不受 RAM 长前缀去重误阻 | 已实现并实测（538.51 MiB > `--cache-ram 512`） |
| 只在真实提交后登记 `persisted` | 已实现（复用第六轮的提交资格判定） |
| 默认关闭与关闭态行为不变 | 已实现并实测（输出逐字节一致） |
| 不改变 v8/v3、CRC、成本守卫、4096-token lag | 保持不变 |

---

## 2. 前置整理（规划 §2.2 要求，与功能改动分开记录）

规划在审查中指出了上一轮遗留的两处源码一致性问题，本轮已消除：

| 问题 | 处理 |
|---|---|
| `worker()` 内直接提交资格使用 `is_reusable_entry(cur)`，而新增的 `disk_cache_commit_eligible()` 仅被单测直接调用，不符合"运行时代码和测试使用同一函数" | 改为先构造 `commit_view` 并调用 `disk_cache_commit_eligible()` 作唯一判定，命中后再取条目记录 |
| 提交诊断日志里的字面量 `@NL@` | 替换为真正的换行符；`grep -c "@NL@" tools/server/server-task.cpp` 现为 0 |

第二处是我上一轮改用按行替换时漏掉的，属于真实交付瑕疵。两处改动均**未改变**隔离条目与有效条目的真实提交资格。

---

## 3. 实现要点

### 3.1 触发条件与候选选择

新增 `--cache-prefix-materialize`，**默认关闭**。为每个 slot 维护 request-scoped 的 `prefix_materialize_plan { armed, prefix_tokens, task_id, done }`，`task_id` 防止 slot 复用后执行上一请求的遗留计划。

候选筛选集中在新增模块 `server-prefix-materialize.*`（不取锁、不碰文件、可独立单测），按固定次序：

| 次序 | 条件 | 拒绝原因 |
|---|---|---|
| 1 | 目录状态为 `eligible` | `no_candidates`（`observed` 不算候选） |
| 2 | 尚无物理引用 | `already_persisted` |
| 3 | 达到**磁盘层**最小 token | `too_short` |
| 4 | 落在本次输入内 | `beyond_input` |
| 5 | 结尾不在媒体块内部 | `media` |
| 6 | 未被 KV 越过 | `past` |

通过者取**最长**一个（它是较短候选的超集）。第 3 条的阈值取自 `--cache-disk-min-tokens`，与目录的 `--cache-prefix-min-tokens` 相互独立——不会为了捕获短前缀而暗中降低磁盘层的恢复要求。

查询用只读的 `prefix_index->candidates()`，**不增加 `observe()` 次数**。

### 3.2 精确 prefill 截断与安全点导出

- **截断**：主填充循环开头，若计划已 armed 且 `slot.prompt.n_tokens() >= K` 则 `break`，使本批恰好结束于 K；下一轮从同一位置继续，不跳过也不重算。
- **导出点**：`post_decode()` 中，`llama_process(ctx_tgt, ...)` 与 `common_speculative_process(...)` 都返回之后，且 slot 仍为 `PROCESSING_PROMPT` 时。这是请求内**唯一**两处上下文已知描述同一前缀的时刻。
- **导出前的三重校验**：`pos_max + 1 == K`（小于则等下一批，大于则记 `skipped` 而非导出）；`slot.prompt.tokens.size() >= K`；草稿上下文同样到达 K。checkpoint 只保留 `pos_max <= K`。
- **不误判**：`pos_max + 1 < K` 的中间批次直接 return，不记 skipped、不置 done。

### 3.3 绕过 RAM 驻留策略的独立导出路径（规划 §3.4）

这是本轮最关键的一处设计。RAM 缓存的 `alloc()` 会在"已有更长序列覆盖待捕获前缀"或"超出 RAM 预算"时返回 `nullptr`——它回答的是"本会话要不要把状态留在内存"，与"该前缀是否值得一个独立磁盘文件"是两个问题。

**实测印证**：首次实现沿用原有 `prompt_capture()` 路径时，538.51 MiB 的状态被 RAM 预算直接拒绝：

```
alloc: - prompt state size 538.501 MiB exceeds cache size limit 512.000 MiB, skipping
materialize = export_failed, prefix = 2102 tokens
```

改为新增 `server_prompt_cache::materialize()`：先 `get_size_ext` 取两处上下文大小，分配缓冲，`get_data_ext` 写入并**校验写入字节数与预估一致**（不一致即失败，不产生不完整对象），产出 `server_prompt_cache_state` 直接交给 `disk->store()`。磁盘层仍独立执行身份、去重、读盘成本守卫、配额事务与提交资格判定——**只是不再被 RAM 预算挡在门外**。

### 3.4 四层计数互不冒充（规划 §6）

| 指标 | 含义 |
|---|---|
| `cache_prefix_materialize_scheduled_total` | 已安排，尚不代表文件存在 |
| `cache_prefix_materialize_exported_total` | 已成功导出完整状态 |
| `cache_disk_entries` / `cache_prefix_persisted_refs` | 真实提交后才有 |
| `cache_reused_tokens_disk_total` | 真实恢复才计入 |

真正节省的 prefill 仍只使用现有 `reused_slot` / `reused_ram` / `reused_disk`。

---

## 4. 验证证据

### 4.1 端到端：物化链路贯通

场景：三个请求共享 3000 字正文前缀，各自接不同后缀（这样每个请求都不是另一个的前缀，RAM 与磁盘都无法互相覆盖，A 是唯一共享部分）。配置：`-np 1`、`--cache-ram 512`、`--cache-disk-min-tokens 256`、`--cache-prefix-min-tokens 256`、`--cache-prefix-observations 2`。

```
[第 2 请求] plan_prefix_: materialize = skipped (no_candidates), n_past = 1476
[第 3 请求] plan_prefix_: materialize = scheduled, prefix = 2102 tokens, n_past = 1476
           materialize_: materialize = exported, prefix = 2102 tokens
           worker: disk cache: stored 2102 tokens, 538.51 MiB in 512.51 ms
           metrics: scheduled 1 / exported 1 / cache_disk_entries 1 / persisted_refs 1
           磁盘文件: pc-bf495e2785d51ca5.bin, 564668394 B
```

目录状态推进也可见：第 2 请求时 `best_state = observed`，第 3 请求时 `best_state = eligible`。

### 4.2 端到端：重启后按磁盘真实恢复

用**完全相同**的模型与上下文配置重启（slot/RAM 在位状态全部消失）：

```
cache_scan_end: 1 entries (manifest 1, stale index records 0), 538.51 MiB
prefix discovery: ... 1 snapshot(s) registered
cache_stats: prompt = 2540, reused_slot = 0, reused_ram = 0, reused_disk = 1478,
             prefill = 1062, source = disk, candidate_lcp = 2101, restored = 2101,
             entry_tokens = 2102, read_bytes = 564668394, restore_us = 329856
```

- `source = disk`、`reused_disk = 1478 > 0`：**真实复用**，不是目录候选数
- `read_bytes = 564668394` 与文件大小**精确一致**
- 恒等式 `0 + 0 + 1478 + 1062 = 2540` 成立

### 4.3 开启态 / 关闭态输出一致性

同一组 4 个请求，`--cache-prefix-materialize` 开与关各跑一遍：

| 请求 | 关闭态 sha | 开启态 sha |
|---|---|---|
| 1 | `f283d36a836120c4` | `f283d36a836120c4` |
| 2 | `9bceec30889f8bda` | `9bceec30889f8bda` |
| 3 | `ad661337d3aa8896` | `ad661337d3aa8896` |
| 4 | `b9febb26949711a2` | `b9febb26949711a2` |

**逐字节一致**。关闭态 `fix7-off.log` 与 `fix7-reg.log` 中 `materialize` 出现次数均为 **0**，三个 materialize 指标全为 0，`cache_disk_entries 0`。

### 4.4 关闭态标准回归

未加 `--cache-prefix-materialize`，保留 `--cache-prefix-discovery` 与 `--cache-boundary-capture both`：

- `1385/0`、`4/1381`、`215/1381` 与上一轮**逐项相同**，输出 sha 相同（`fd7c5bda…`、`03e0e517…`）
- `cache_prefix_persisted_refs 1`（原路径行为不变）

### 4.5 恒等式抽查

```
2468 = 1476 + 0 + 0 + 992
2506 = 1476 + 0 + 0 + 1030
2540 = 1478 + 0 + 0 + 1062
2512 = 2101 + 0 + 0 + 411
```

### 4.6 四套独立单测

| 测试 | 结果 |
|---|---|
| `test_disk_cache_match.cpp` | `all disk-cache match checks passed` |
| `test_disk_cache_quota.cpp` | `all disk-cache quota checks passed` |
| `test_cache_prefix_index.cpp` | `all prefix-index checks passed` |
| `test_prefix_materialize.cpp`（新增 15 组） | `all prefix-materialize checks passed` |

新增单测覆盖：关闭态、无候选、最长者胜出、磁盘最小长度与目录阈值相互独立、短候选不阻塞长候选、已 persisted 跳过、`observed` 不算候选、`eligible` 优先于 `observed`、超出输入长度、媒体块边界三点（块内拒绝 / 恰在块起点 / 恰在块终点）、已越过边界、`n_past == K` 允许、零长度忽略、共享前缀真实场景、拒绝原因名互不相同。

---

## 5. 实施中新引入并自查修复的缺陷

**这是我实现中的问题，不是原代码问题，如实列出供审查。**

`n_past == K` 时（KV 恰好停在捕获边界），计划会 arm 批次截断；但填充循环第一轮就满足 `n_tokens >= K` 而 `break`，本批一个 token 都不加。空批次会使 `decode()` 打印 `no tokens to decode`，连续超过 3 次触发 `GGML_ABORT`——**请求会直接中止服务**。

修正：该情形不再 arm 截断，而是立即从当前状态捕获（正是规划 §3.2 第 5 条"恰好停在捕获边界即可直接捕获"的要求）。修正后实测第 5 个请求走该分支成功导出：

```
plan_prefix_: materialize = scheduled at n_past, prefix = 2101 tokens
materialize_: materialize = exported, prefix = 2101 tokens
```

输出与关闭态一致。

---

## 6. 未覆盖项与已知残留

按规划 §8 的检查清单，以下项目**未完成或未验证**，请规划人员据此判断下一步：

### 6.1 规划要求但未实现

- **开关组合的"清晰拒绝"**：规划 §3.2 要求 `--cache-prefix-materialize` 在未启用磁盘层或 `--cache-prefix-discovery` 时"清晰拒绝或说明该开关无效"。当前实现是 `plan_prefix_materialize()` 开头直接 `return`，**静默不生效，没有任何显式提示**。这一条只做到了"不生效"，没做到"清晰说明"。

### 6.2 已实现但未在真实环境验证

- **媒体输入**：测试机为纯文本。`skipped (media)` 分支（结尾落在媒体块内）只有单测覆盖，**未在真实多模态输入下运行过**。
- **checkpoint 超界**：截断逻辑有单测，但当前部署为纯文本 Qwen，`do_checkpoint` 路径未实测。
- **context shift / 任务取消 / decode 重试下的重复捕获**：捕获点已限定 `PROCESSING_PROMPT` 且校验 `pos_max + 1 == K`，但规划 §8.3 要求的这些异常时序未单独构造用例。
- **配额淘汰与短快照替代对物化对象的交互**：物化对象走同一个 `store()` 与配额事务，前几轮已验证的准入/替代/隔离语义对它应同样适用，但**未针对该对象单独回归**。
- **同名快照先失效、后物化的时序**：未构造。

### 6.3 其他

- 多 slot、Linux、中断路径：本轮范围外（规划限定单用户 `-np 1`）。
- 未实现规划 §4.4 曾建议的"测试用确定性删除故障注入钩子"。前一轮已说明理由：它会往生产源码引入测试专用路径，而现有单测已能覆盖纯元数据层。本轮同样未引入。

---

## 7. 审查建议

建议按以下次序审查：

1. **`post_decode()` 中的捕获调用点**（`server-context.cpp`）——这是唯一的导出时点，其前置条件（batch 成功、spec 处理完成、slot 仍为 `PROCESSING_PROMPT`）是否足以保证两处上下文描述同一前缀。
2. **填充循环中的截断条件**——是否会与 `wait_shared`、alora、decision 等既有 break 条件冲突；`n_past == K` 的旁路是否覆盖了所有会截断出空批次的路径。
3. **`server_prompt_cache::materialize()`**——绕过 RAM 准入后，磁盘层的成本守卫与配额事务是否仍完整生效（`store()` 未被改动，应无问题，但值得确认）。
4. **四层计数**——是否确有互相冒充的残留。

---

## 8. 推送状态

| 项 | 值 |
|---|---|
| 本地 HEAD | `ab6bac2` |
| 远端 | `git@github.com:A-lucky-boy/llama.cpp-dspine-diskCache.git` |
| 分支 | `main` |
| 推送前状态 | 本地领先远端 1 个提交 |
| 工作树 | 干净（无未提交改动） |
