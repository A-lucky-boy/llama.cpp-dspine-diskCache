# 2026-11-16 08-30 交付说明（单用户磁盘提示缓存：第八轮 —— 物化路径边界收紧）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `010c68a`（会话前 HEAD，第七轮交付说明提交） |
| 本次交付终点 | 分支 `main` 顶端（功能提交 `d0537f1`，其后仅有本说明自身的文档提交） |
| 功能提交 | `d0537f1`（全部代码与测试改动） |
| 规划依据 | `betterModification.md` → `2026-11-16 01-12 - test1_3` |
| 构建产物 | `0.9.0-dev (build 31, commit d0537f1)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | **正确性修复 + 回归补充**（规划 §1 明示不是新算法实验） |

本轮为上一轮（`ab6bac2` 公共前缀主动物化）的边界收紧。规划 §1 列出四个问题，全部处理；另外补上规划要求的配置依赖诊断，并在实现过程中**自查发现一处交付级测试缺口**（见 §5）。

逐项实现细节见 `change.md` 的 `2026-11-16 03-20` 条目；项目现状见 `projectSummary.md`。

---

## 1. 四个缺陷的处理结果

| 缺陷 | 规划定位 | 处理 | 端到端验证 |
|---|---|---|---|
| A：draft 越过 K 未拒绝 | §3 | 抽出 `disk_cache_check_capture_bounds()`，六种端点状态，仅 `exact` 放行 | 单测 12 项；**运行中未构造出自然场景** |
| B：K 等于输入终点被跳过 | §4 | `DONE_PROMPT` 也交给 `materialize_prefix()`，按真实端点判定 | **已定点命中并导出**（见 §4.1） |
| C：媒体块被聚合成单区间 | §5 | 改为有序互不重叠的真实块集合，仅 `start < K < end` 拒绝 | 单测 4 类非法结构 + 10 个边界点；**无多模态环境，未实跑** |
| D：checkpoint 位置上界 off-by-one | §6 | 双边界判定（`n_tokens` 对齐声称 token 数、`pos_max` 对齐 KV 末端位置） | **确定性证据 + 单测**；**无真实 checkpoint 触发** |

### 1.1 缺陷 D 的确定性证据

规划 §6.1 明确要求"不能仅凭表面不等式断言当前测试模型已经损坏，需要结合位置映射、`n_tokens`、`pos_min` 和混合记忆后端说明进行确定性证明"。证据如下：

**代码侧**：`server_tokens::pos_next(n)` 在 `!has_mtmd` 时直接返回 `n`；`create_checkpoint()` 调 `update_pos(slot.prompt.n_tokens() - n_tokens_cur, pos_min, pos_max)`。即 N 个 token 占位置 `[0, N)`，最高合法位置为 `N-1`。

**实测侧**：本机真实 checkpoint 日志（历史运行）：

```
created context checkpoint 1 of 32 (pos_min = 8238, pos_max = 8238, n_tokens = 8239)
created context checkpoint 2 of 32 (pos_min = 8238, pos_max = 8238, n_tokens = 8239)
created context checkpoint 1 of 32 (pos_min = 7214, pos_max = 7214, n_tokens = 7215)
```

多条记录一致满足 `pos_max = n_tokens - 1`。

**结论**：原判定 `ckpt->pos_max <= (llama_pos) K` 把 **token 数**当作**位置上界**。K 个 token 的快照其最大合法位置是 `K-1`，因此 `pos_max == K`（即 `n_tokens == K+1`）的 checkpoint 会被错误接纳，而快照只声称 K 个 token。

修复后的判定同时检查两个边界，各查各的：
- `n_tokens > prefix_tokens` → 拒绝（覆盖的 token 数超过快照声称的）
- `pos_max > prefix_pos_max` → 拒绝（位置越过实际 KV 末端）

文本下 `prefix_pos_max = K - 1`；媒体下两者本就不等（一个媒体块占的位置多于其 token 数），必须分开传。

### 1.2 通用路径的同类问题（规划 §6.2 第 3 条要求审查）

`capture_boundary()` 第 2064 行原为 `ckpt->pos_max <= (llama_pos) n_covered`，**存在同一 off-by-one**，且 `n_covered` 是 token 数，媒体下与位置不等价。

已改为调用同一判定，位置边界传入**真实的 KV 末端 `pos_max`**（该变量在函数内已从 `llama_memory_seq_pos_max()` 取得）。这是最小必要修复，未改动 checkpoint 策略、未引入新存储格式。

---

## 2. 缺陷 B 的实现要点（规划 §4.2 有六条细目）

原代码在 `post_decode()` 中只处理 `SLOT_STATE_PROCESSING_PROMPT`，对 `DONE_PROMPT` 且 `K <= slot.prompt.n_tokens()` 一律置 `done` 并记 skipped。

修复后两种状态都交给 `materialize_prefix()`，由**真实 target/draft 端点**决定结局：

```text
target_short  → 保留计划，等下一批（不记 skipped、不置 done）
exact         → 捕获
其余          → 记 skipped + 置 done
```

关键点：**slot 状态只说明提示词处理完，不说明停在哪里**。`DONE_PROMPT` 可能是 `K == 输入终点`（应捕获），也可能是 `K < 输入终点`（已越过，应跳过）——只有端点能区分。

### 2.1 与既有流程的顺序（规划 §4.3 第 3 条）

捕获仍在 `post_decode()` 中、`common_speculative_process()` 之后执行，即在 `capture_boundary(INPUT)` 与 `observe_prefix()` 之前：

```
llama_process(DECODE) → common_speculative_process → [materialize_prefix]
                                                       ↓
                          （slot 状态迁移后）→ capture_boundary(INPUT) → observe_prefix()
```

此时本请求仍持有正确的 token/media 身份，且候选查询用的是 `slot.task->tokens`（只读，不增加观察次数）。

### 2.2 与 `n_past == K` 路径的去重（规划 §4.2 第 5 条）

上一轮已修复的 `n_past == K` 直接捕获路径**保持原状**：`plan_prefix_materialize()` 在 `n_past == K` 时立即捕获并 `return`，**不会 arm 批次截断**，因此不会进入 `post_decode()` 的循环。两条路径互斥，不存在重复导出或双计数。

已验证：`fix8-n1.log` 中 task 32 走 `scheduled at n_past` 分支，`fix8-f1.log` 中 task 14 走中间批次分支，各自只导出一次。

### 2.3 请求隔离（规划 §4.2 第 6 条）

沿用已有的 request-scoped 字段：`slot.materialize.{armed, prefix_tokens, task_id, done}`，在 `get_available_slot()` 中随 `cache_restore` 一并重置；`materialize_prefix()` 首行校验 `task_id == slot.task->id`，不匹配即整体清空计划并返回。

---

## 3. 缺陷 C 与配置诊断

### 3.1 媒体块

`disk_cache_materialize_request` 的 `has_media` + 单一 `media_chunk_start/end` 替换为 `std::vector<disk_cache_media_chunk>`（半开区间 `[start, end)`）。

判定顺序（规划 §5.2 第 3 条要求）：

1. **先校验结构**：空块、重叠、乱序、越界 → 立即返回 `skipped (media)`，**不降级为"无媒体输入"**
2. 再按候选逐个判定：仅 `start < K < end` 判为块内

合法边界（规划 §5.2 第 2 条）：块起点、块终点、两块之间、全部块之后。

未复用 `disk_cache_match_prefix()`（规划 §5.2 第 4 条提到"统一复用已有媒体身份语义"）：该函数做的是**媒体身份匹配**（token 占位符与 media ID 的对应），而这里的检查是**位置区间判定**，两者不重叠。媒体身份仍由原有路径独立校验，未改动。

### 3.2 配置依赖诊断（规划 §7）

两条警告已实测触发（`-ngl 0` 快速启动后终止）：

```
W srv load_model: --cache-prefix-materialize needs --cache-prefix-discovery:
      there is no prefix directory to take candidates from, so no prefix will be captured
W srv load_model: --cache-prefix-materialize needs a disk cache tier (--cache-disk):
      a captured prefix has nowhere to be stored, so no prefix will be captured
```

依赖齐全时**不输出任何额外日志**；功能未开启时该段代码不进入，默认关闭态日志与执行路径完全不变。

---

## 4. 验证证据

### 4.1 缺陷 B 定点命中（关键证据）

`test-run/logs/fix8-n2.log`，task 37：

```
cache_stats: prompt = 2477, reused_slot = 2101, reused_ram = 0, reused_disk = 0, prefill = 376
plan_prefix_: materialize = scheduled, prefix = 2477 tokens, n_past = 2101
materialize_: materialize = exported,  prefix = 2477 tokens
```

即 **K（2477）恰等于本次输入总长**，`n_past = 2101 < K`，最后一个 prefill 批次恰好结束在 K 并使 slot 转入 `DONE_PROMPT`——修复前这里会被一律当作"已越过 K"而跳过。修复后成功导出。

### 4.2 缺陷 C、D 的缺陷复现

**C**（把判定临时改回单区间语义）→ 7 项失败：

```
FAIL ...:199: plan.capture
FAIL ...:200: plan.prefix_tokens == 800 (0 vs 800)
FAIL ...:206: plan.capture
FAIL ...:210: plan.capture
FAIL ...:301: !disk_cache_prefix_in_media(20, chunks)
FAIL ...:302: !disk_cache_prefix_in_media(30, chunks)
FAIL ...:303: !disk_cache_prefix_in_media(40, chunks)
```

即 `K=800` 落在 `[600,1000)` 聚合区间内部被误拒，块终点 700 / 块起点 900 也被误判为块内。

**D**（把位置上界改回 token 数语义）→ 2 项失败：`pos_max == K` 与 `K == 1` 两个越界 checkpoint 被接纳。

`---` 两处均在恢复修复版本后全部通过。

### 4.3 四层计数的四个层次仍未相互冒充

| 层次 | 指标 | 本轮实测 |
|---|---|---|
| 规划 | `cache_prefix_materialize_scheduled_total` | 1（或 2） |
| 导出 | `cache_prefix_materialize_exported_total` | 1（或 2） |
| 物理提交 | `cache_disk_entries` / `cache_prefix_persisted_refs` | 1 / 1 |
| 真实复用 | `cache_reused_tokens_disk_total` | 2102（重启后） |

### 4.4 重启后磁盘真实恢复

`test-run/logs/fix8-n2r.log`（同配置重启）：

```
cache_scan_end: 1 entries (manifest 1, stale index records 0), 373.23 MiB
prefix discovery: ... 1 snapshot(s) registered
cache_stats: prompt = 2496, reused_slot = 0, reused_ram = 0, reused_disk = 2102,
             prefill = 394, source = disk, restored = 2102, read_bytes = 391363294
```

`read_bytes` 与文件大小 391363294 B 一致；恒等式 `0 + 0 + 2102 + 394 = 2496` 成立。

### 4.5 关闭态一致性与无噪声

`fix8-off.log`（未加 `--cache-prefix-materialize`）：三个 materialize 指标全 0、`cache_disk_entries 0`；输出 sha 与开启态对应请求一致；无依赖诊断日志。

### 4.6 恒等式抽查（`fix8-n1.log` / `fix8-n2.log`）

`2504 = 0+0+0+2504`、`2468 = 1476+0+0+992`、`2506 = 1476+0+0+1030`、`2540 = 1478+0+0+1062`、`2512 = 2101+0+0+411`、`2477 = 2101+0+0+376`，全部成立。

### 4.7 四套独立单测

```
all disk-cache match checks passed
all disk-cache quota checks passed
all prefix-index checks passed
all prefix-materialize checks passed
```

---

## 5. 本轮自查发现的交付缺口（请重点审查）

### 5.1 情况

规划 §3.2 建议"引入可独立测试的状态边界判定函数，供生产 `materialize_prefix()` 直接调用，单测不要复制另一套相似条件"。我照此抽出了谓词 `disk_cache_checkpoint_fits(ckpt, prefix_tokens, prefix_pos_max)`。

**但位置上界 `K-1` 的算术仍留在调用点**：

```cpp
// 我最初的写法（server-context.cpp）
disk_cache_checkpoint_fits({ ... }, K, (int64_t) K - 1)
```

谓词有测试，而 `K - 1` 这段算术**没有**。我构造回归验证这一点时确认：把调用点的 `K - 1` 改回 `K`（即重新引入 off-by-one），**四套单测全部通过**——测试完全无法发现它。

### 5.2 处理

已把该算术移入被测模块，调用点不再自行做减法：

```cpp
int64_t disk_cache_prefix_pos_max_text(size_t prefix_tokens);   // 返回 N-1；N==0 时返回 -1
disk_cache_checkpoint_fits({ ... }, K, disk_cache_prefix_pos_max_text(K))
```

并补了 `test_prefix_pos_max_text()`：直接断言 4 个取值、`N==0` 的拒绝对照、以及 `N=1..32` 的循环一致性（N 个 token 恰好通过、N+1 个被拒）。回归验证：把 helper 改回 `return prefix_tokens` 后，**4 项失败直接命中**。

### 5.3 请规划人员判断

- 这类"判定函数已测、但调用点的入参算术未测"的缺口，是否在其它新增路径上也存在？我能确认的是通用路径 `capture_boundary()` 直接传真实 `pos_max`、无算术；`materialize_prefix()` 的另两个入参（`K`、target/draft 端点）都来自 `llama_memory_seq_pos_max() + 1` 的直读，无算术。
- 是否应把"边界算术必须位于被测模块内"作为后续新增代码的通用约束。

---

## 6. 未覆盖项与已知残留

按规划 §11 检查清单，以下**未完成或未验证**：

### 6.1 已实现但无真实环境验证

- **真实模型 checkpoint 恢复**：规划 §6.2 第 4 条允许"没有可用真实 checkpoint 模型时，构造纯元数据的边界测试，但明确记录未执行真实模型检查点恢复"。本机 `--checkpoint-min-step` 默认 8192，且 `create_checkpoint()` 还需 `is_user_start` 或 `near_prompt_end` 条件（纯文本无 `message_spans` 时不满足）。我用 **14990 token** 的纯文本输入仍未触发任何 checkpoint（`grep "created context checkpoint"` 在本轮日志中为 0）。因此：**位置映射的确定性证据来自历史真实日志 + 代码语义核对，checkpoint 过滤的运行验证未执行**。规划 §11.4 第 5 项"如果当前模型没有 checkpoint，需记录未验证的实际模型类别"——即此。
- **媒体输入**：测试机纯文本，无多模态模型与图片输入。`media_chunks` 构造、块内判定、非法结构拒绝全部由单测覆盖，但**未在任何真实 mtmd 输入下运行过**。
- **draft 越过 K**：单测覆盖 `draft_past` 分支，但运行中 `common_speculative_process()` 之后 draft 与 target 始终对齐，**未能自然构造或注入**该场景（规划 §11.2 第 2 项要求"通过可控的 seq-position 夹具验证草稿超前 K 确实被拒绝，而不是仅检验小于 K 分支"——我只能做到单测层的夹具，未能做运行层夹具）。
- **context shift / 取消 / decode 重试**：捕获点已限定状态并要求端点严格等于 K，但规划 §11.3 要求逐一构造的这些异常时序**未执行**。
- **多个媒体块的真实输入**：本轮无法构造，仅单测覆盖。
- **配额淘汰 / 短快照替代 / 隔离删除失败对物化对象的交互**：物化对象走同一 `store()` 与配额事务，前几轮语义应适用，但**未针对该对象单独回归**。

### 6.2 规划提到但我判断不应实施

- **规划 §5.2 第 5 条的保守保护**：原文"如当前真实 mtmd 处理路径仍不能保证媒体块之间的 target/draft 状态完全一致，应额外采用保守的'本请求有媒体则跳过主动捕获'保护"。
  **我未实现该保护**——因为在无多模态环境下无法判断其是否必要。若规划人员认为应当无条件保守，这是一行改动（在 `plan_prefix_materialize()` 开头对 `!media_chunks.empty()` 直接 return）。
- **规划 §4.4 曾建议的"测试用确定性删除故障注入钩子"**：前两轮已说明理由，本轮同样未引入（会向生产源码加入测试专用路径）。

### 6.3 范围外

多 slot、多租户、公网鉴权、Linux：规划 §1 限定单用户本机 `-np 1`，均不在本轮范围。

---

## 7. 审查建议

建议按以下次序审查本轮改动：

1. **`disk_cache_check_capture_bounds()` 的六种状态归类**（`server-prefix-materialize.cpp`）——特别是 `draft_past` 与 `target_past` 的优先级：当前 target 的问题先报。若规划认为"draft 超前"应优先于"target 落后"上报，这是判定顺序的调整。
2. **`post_decode()` 中 `DONE_PROMPT` 的引入**——是否会影响 `n_cmpl > 1`（父子任务共享前缀）的场景。当前 `DONE_PROMPT` 的 slot 会被 `materialize_prefix()` 访问，该函数只读 `prompt.tokens` / `checkpoints` 与两处 context 的 `seq_pos_max`，不写 slot 状态；但父子任务的状态复制发生在同一函数的后半段，值得确认无交互。
3. **`capture_boundary()` 的 checkpoint 修复**——这是对通用路径的改动（非仅物化路径）。虽然只是把边界条件收紧一个位置，但它影响所有边界捕获。规划 §6.2 第 3 条授权"只做最小必要修复"，请确认修复范围与力度合适。
4. **`disk_cache_prefix_pos_max_text()` 的调用点覆盖**——是否还有其它地方在自行计算 `K-1`（见 §5）。

---

## 8. 推送状态

| 项 | 值 |
|---|---|
| 本地 HEAD | 分支 `main` 顶端；功能提交 `d0537f1` |
| 远端 | `git@github.com:A-lucky-boy/llama.cpp-dspine-diskCache.git` |
| 分支 | `main` |
| 工作树 | 干净（无未提交改动） |
| 构建产物版本串 | `0.9.0-dev (build 31, commit d0537f1)` —— 与交付终点一致 |

本轮全部改动（含交付文档与 §5.2 的测试缺口修复）已在 `d0537f1` 提交并推送至 `main`。
