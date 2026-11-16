# 2026-11-16 09-30 交付说明（单用户磁盘提示缓存：第九轮 —— token 与 position 单位分离 + 媒体门禁）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `6b35154`（会话前 HEAD，第八轮交付说明提交） |
| 本次交付终点 | 分支 `main` 顶端（功能提交 `d57d09c`，其后仅有本说明自身的文档提交） |
| 功能提交 | `d57d09c`（全部代码与测试改动） |
| 规划依据 | `betterModification.md` → `2026-11-16 08-38 - test1_3` |
| 构建产物 | `0.9.0-dev (build 33, commit 6b35154)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | **正确性修复（单位语义）+ 保守门禁**（规划 §1 明示不是新算法实验） |

本轮处理规划 §2.2 指出的三处 `token 数 / KV position` 单位混用（位置一：`pos_next` 语义；位置二：端点比较；位置三：checkpoint 位置界），并加入规划 §3.3 要求的媒体写盘门禁。逐项实现细节见 `change.md` 的 `2026-11-16 09-30` 条目；项目现状见 `projectSummary.md`。

实现过程中**自查发现并修正了一处我自己的回归**（见 §5），该回归由端到端数值而非单测发现。

---

## 1. 单位混用缺陷的确定性依据

规划 §2.2 位置一要求核对 `server_tokens::pos_next(int64_t)` 的媒体语义。核对结果：

**`tools/server/server-common.cpp:522`**（`pos_next`）：
- `!has_mtmd` → 返回 `n`（恒等）
- `has_mtmd` → 遍历 token 流，每遇媒体块累加 `mtmd_input_chunk_get_n_pos(chunk)`，并跳过 `mtmd_input_chunk_get_n_tokens(chunk)` 个 token

**`tools/mtmd/mtmd.cpp:3492`**（`mtmd_input_chunk_get_n_pos`）→ 对图像块转调 `mtmd_image_tokens_get_n_pos()`（`3738`），后者按 clip 行数累加（`3755`）：

```cpp
n_pos += 1 + (llama_pos) clip_suffix_rows(entry.suffix_type).size();
```

**结论**：`n_pos` 与 `n_tokens` 是**相互独立**的量，前者由 clip 行数决定，后者是 token 占位数量。因此对含媒体块的前缀：

```
K = prefix token count
P = server_tokens::pos_next(K)   // prefix KV position end, exclusive
P ≠ K  一般情况下成立
```

旧代码把 `K` 与 `llama_memory_seq_pos_max() + 1` 直接比较，属于**源码可确认的语义单位错误**。与规划一致：报告未执行真实 mtmd 测试，因此本轮不声称"已发生实际错误恢复"，只修复该可确认的单位错误。

---

## 2. 逐项处理结果

| 规划定位 | 问题 | 处理 | 验证 |
|---|---|---|---|
| §2.2 位置二 | 端点比较把 token 数与 position 数对比 | 谓词改名 `disk_cache_check_capture_position_bounds()`，三参数统一为 position；调用点用 `pos_next(K)` 求期望端点 | 单测 `test_position_vs_token_units()` 双向复现；**媒体路径未实跑** |
| §2.2 位置三 | checkpoint 位置界对媒体路径用文本专用 `K-1` | 改为传入已验证 exact 的 target `tgt_pos_max` | 单测；**运行中未触发 checkpoint** |
| §3.3 | 媒体状态未验证却进入写盘路径 | 加 `media_state_unverified` 门禁，不 arm、不导出、不写盘 | 单测 6 项；**无多模态环境，未实跑** |

### 2.1 端点谓词的接口变更

```
改动前  disk_cache_check_capture_bounds(prefix_tokens, target_end, has_draft, draft_end)
改动后  disk_cache_check_capture_position_bounds(expected_position_end, target_position_end,
                                                  has_draft, draft_position_end)
```

后者的三个参数**全部是 position**，接口名字本身说明单位。`prefix_tokens`（token 数）继续用于快照 token 元数据、文件身份与最小长度判定；按规划 §3.1 未用于截断 token 容器、未用于修改 disk LCP。

按规划 §3.1 还补了两条：
- `expected_end <= 0` 判据：前缀映射不到任何位置时无法捕获（新增拒绝路径）
- token 视图长度检查（`slot.prompt.tokens.size() < K`）前置，因为 `pos_next` 只对视图实际持有的前缀有定义

### 2.2 checkpoint 处理（规划 §3.2）

生产调用点原为：

```cpp
disk_cache_checkpoint_fits({...}, K, disk_cache_prefix_pos_max_text(K))
```

`disk_cache_prefix_pos_max_text()` 是 `d0537f1` 引入的文本专用辅助（返回 `K-1`），对媒体路径不成立。改为：

```cpp
disk_cache_checkpoint_fits({...}, K, (int64_t) tgt_pos_max)
```

`tgt_pos_max` 来自 `llama_memory_seq_pos_max(ctx_tgt, slot.id)`，是**已验证 exact 之后的真实 target 端点**。

按规划 §3.2 要求，`disk_cache_prefix_pos_max_text()` **保留但生产路径不再引用**（`grep` 确认只剩单测），并更新其文档说明"仅用于固定文本恒等式 `pos_next(N) == N`，禁止用作媒体路径的位置界"。未为了制造使用点而保留错误引用。

规划 §3.4 明确"`capture_boundary()` 已使用实际 `pos_max` 筛选 checkpoint，不将它改回 `K-1`"——该处上一轮已改为传入真实 `pos_max`，本轮未改动。

### 2.3 媒体门禁（规划 §3.3）

`disk_cache_plan_materialize()` 在请求含媒体块时：

- **仍然执行**：候选选择、长度与可达性判定、块边界合法性判定（这些用于诊断）
- **不再执行**：不 arm 捕获、不导出 state、不触发 `disk->store()`
- **返回理由**：`media_state_unverified`

未提供"强制实验性写入"绕过选项（规划 §3.3 明确要求）。

**门禁位置修正过一次**：初版置于函数开头，导致媒体块的几何边界判定成为不可达代码——单测立刻暴露 15 项失败。改置于"将要 arm"处后，两个理由各自可达且可区分：

| 情形 | 理由 |
|---|---|
| K 严格落在某媒体块内部 | `media`（几何不合法） |
| K 在块起点/终点/两块之间/全部块之后 | `media_state_unverified`（几何合法，仅状态未验证） |

该门禁只作用于新的主动物化路径。未关闭 llama.cpp 多模态推理、RAM/磁盘既有完整快照缓存或前缀元数据观察（规划 §3.3、§6.6）。

---

## 3. 未改动项（规划 §3.4、§6）

- v8 快照、v3 manifest、CRC、文件名、IO 守卫、隔离与配额事务：未改
- 新物化写入仍只经 `server_prompt_cache::materialize()` 与单一磁盘 worker：未改
- `768 KiB/token` 读盘守卫、`4096 token` 写入抑制、配置签名：未改
- `llama_state_seq_get_data_ext` / `set_data_ext` 底层语义：未改
- `server_tokens::pos_next()` 本身：未改（本轮服从其既有语义）
- `n_past == K` 与 `DONE_PROMPT` 两条捕获路径：逻辑未变，仅端点单位改为 position 域

---

## 4. 验证证据

### 4.1 端到端核心链路（`test-run/logs/fix9-w2.log`）

参数：`--cache-prefix-materialize --cache-ram 512 --cache-disk-min-tokens 256`，单 slot，三请求共享 3000 字前缀后各接不同后缀，第四请求复用。

```
plan_prefix_: materialize = skipped (no_candidates), n_past = 1476
plan_prefix_: materialize = scheduled, prefix = 2102 tokens, n_past = 1476
materialize_: materialize = exported, prefix = 2102 tokens
plan_prefix_: materialize = scheduled, prefix = 2101 tokens, n_past = 1478
materialize_: materialize = exported, prefix = 2101 tokens

cache_prefix_materialize_scheduled_total 2
cache_prefix_materialize_exported_total  2
cache_prefix_materialize_skipped_total   0
cache_disk_entries                       1
cache_prefix_persisted_refs              1
```

规划 §7.3 要求"纯文本预先建立 eligible 公共前缀，以 `K<input_end` 验证中途批次精准物化"——本次两次导出均为 `K < input_end` 的中途批次（`2102 < 2506`、`2101 < 2540`）。

### 4.2 重启后磁盘真实恢复（`fix9-w2r.log`，同配置重启）

```
cache_scan_end: 1 entries (manifest 1, header 0, stale index records 0), 538.51 MiB
prefix discovery: ... 1 snapshot(s) registered

cache_stats: prompt = 2496, reused_slot = 0, reused_ram = 0, reused_disk = 2102,
             prefill = 394, source = disk, candidate_lcp = 2102, restored = 2102,
             entry_tokens = 2102, read_bytes = 564668394, restore_us = 384229
```

`read_bytes` 与磁盘文件大小 564668394 B 精确一致。恒等式 `0 + 0 + 2102 + 394 = 2496` 成立。规划 §7.3 的"`restored`、`reused_disk` 与 prompt token 分解恒等式"三项均已核对。

### 4.3 开启/关闭态输出一致性

同一组 4 个请求，开启态（`fix9-w2.log`）与关闭态（`fix9-off.log`）：

| 请求 | 开启态 sha | 关闭态 sha |
|---|---|---|
| 1 | `f283d36a836120c4` | `f283d36a836120c4` |
| 2 | `9bceec30889f8bda` | `9bceec30889f8bda` |
| 3 | `ad661337d3aa8896` | `ad661337d3aa8896` |
| 4 | `b9febb26949711a2` | `b9febb26949711a2` |

连同前一次运行（`fix9-w1.log`）共 **12 个请求 sha 逐字节一致**。关闭态三个 materialize 指标全 0，`cache_disk_entries 0`。规划 §7.3 末条的关闭态对照已满足。

### 4.4 恒等式抽查（`fix9-w2.log`）

```
2504 = 0    + 0    + 0    + 2504
2468 = 1476 + 0    + 0    + 992
2506 = 1476 + 0    + 0    + 1030
2540 = 1478 + 0    + 0    + 1062
```

全部成立（格式：`prompt = reused_slot + reused_ram + reused_disk + prefill`）。

### 4.5 四套独立单测

```
all disk-cache match checks passed
all disk-cache quota checks passed
all prefix-index checks passed
all prefix-materialize checks passed
```

其中本轮新增/改写：

- `test_position_vs_token_units()`（新增）：同一组数字（`K=2102`、`expected_end=2104`）在 position 域判为 `exact`，在旧混合语义下判为 `target_past`；反向构造旧语义误判 `exact` 而 position 域正确报 `target_short`
- `test_media_state_unverified_gate()`（新增）：单块/多块/候选在块后/几何不合法优先/无媒体对照/理由名，共 6 项
- `test_media_boundary()`、`test_media_gap_is_legal()`、`test_media_chunks_malformed()`：改写为区分 `media` 与 `media_state_unverified`
- `test_capture_position_bounds()`：原 12 项 + `expected_end <= 0` 三项 + 宽位置跨度三项

### 4.6 构建

```
0.9.0-dev (build 33, commit 6b35154)
built with MSVC 19.44.35226.0 for Windows AMD64
```

CUDA 目标：sm_86-real + sm_89-real。本轮 2 次构建，`0 FAILED`。

---

## 5. 本轮自查发现的回归（请重点审查）

### 5.1 情况

我在改写 `materialize_prefix()` 时，把 token 视图长度检查从原位置**提前**，并把它写成了硬失败：

```cpp
if (slot.prompt.tokens.size() < K) {
    SLT_WRN(slot, "cache_stats: materialize = skipped (prompt has %zu < %zu tokens)\n", ...);
    slot.materialize.done = true;
    counters_materialize.skipped++;
    return;
}
```

**后果**：中间批次时 `slot.prompt.tokens` 本就还没填到 K（该批次只填了一部分），于是**所有**中途批次捕获全部失效。端到端表现为 `scheduled 2 / exported 0 / skipped 2`，日志为 `skipped (prompt has 1478 < 2102 tokens)`。

这是**功能性回归**：`--cache-prefix-materialize` 会完全失效，却不报错、不中断推理。

### 5.2 发现方式与处理

**单测未能发现**——`test_prefix_materialize.cpp` 只覆盖纯元数据（候选选择规则），不执行 `materialize_prefix()`，因此对调用点的顺序/状态问题无感知。该回归是**端到端数值**发现的：`exported_total` 从预期的 2 变成 0。

修正为"未填满则 return 等下一批"，与 `target_short` 分支同等对待：

```cpp
if (slot.prompt.tokens.size() < K) {
    return;   // mid-prompt batch: the batch that reaches the prefix will arrive
}
```

修正后 `scheduled 2 / exported 2 / cache_disk_entries 1`，即 §4.1 的结果。

### 5.3 与上一轮 `d0537f1` 同一类问题的对照

`d0537f1` 的提交信息记录了类似教训（"checkpoint 位置上界 K-1 的算术留在调用点，谓词有测试而算术没有"）。本轮再次出现"谓词有测试、调用点的控制流没有"的同类缺口，但这次后果更严重（功能完全失效而非边界错误）。

**这提示一个系统性缺口**：`materialize_prefix()` 的控制流（各 return/跳过分支的触发条件与顺序）目前没有任何测试覆盖，只能靠端到端数值发现异常。纯文本端到端能覆盖主要分支，但媒体分支、checkpoint 分支、异常状态分支覆盖不到。

### 5.4 请规划人员判断

是否需要为 `materialize_prefix()` 的控制流建立可测结构（例如把"给定 token 视图长度、K、target/draft 端点 → 应执行的动作"抽成纯函数并单测）？本轮未做，因为规划 §4 未要求改动该函数的结构，且这会触及本轮授权范围边界。

---

## 6. 未覆盖项与已知残留

### 6.1 已实现但无真实环境验证（规划 §7 明确要求"不能声称测试通过"）

| 项 | 状态 | 说明 |
|---|---|---|
| 媒体门禁的实际生效 | **未运行** | 测试机无多模态模型与图片输入。门禁路径（不 arm、不导出、不写盘）**只有单测覆盖**，未在真实服务上触发过；也未验证"含媒体请求仍正常完成原推理" |
| `pos_next()` 的媒体分支 | **未运行** | 生产调用点的 `pos_next(K)` 只在纯文本下走过（此时恒等于 K）。媒体分支的 `while` 循环由既有代码与静态核对确认 |
| checkpoint 过滤（含本轮 `tgt_pos_max` 改动） | **未运行** | 本机 `--checkpoint-min-step` 默认 8192，且还需 `is_user_start` 或 `near_prompt_end`；两轮共尝试 14990 token 纯文本输入仍未触发任何 checkpoint。`tgt_pos_max` 替代 `K-1` 的改动在运行中未被执行 |
| `expected_end <= 0` 判据 | **未运行** | 单测覆盖，运行中未构造 |
| target/draft 位置不一致 | **未运行** | `common_speculative_process()` 之后两者始终对齐，未能自然构造，也未能注入 |
| context shift / 任务取消 / decode retry | **未运行** | 规划 §7.3 要求"若环境无法注入，明确记录尚未覆盖，不能声称测试通过" |

### 6.2 规划提到但我未实施

- 规划 §7.2 要求"用模拟多媒体块创建 `n_tokens != n_pos` 的前缀，分别令目标/草稿端点等于、短于、超过 `expected_pos_end`"。我用**纯元数据**在 `test_position_vs_token_units()` 中构造了 `K=2102 / expected_end=2104` 的分离情形，覆盖了等于/短于/超过三种关系，但**没有通过真实 mtmd 请求或模拟 `server_tokens` 对象**执行该路径。判断：在无多模态环境下，构造真实 `server_tokens` 需引入测试专用的媒体夹具，超出本轮授权范围。
- 规划 §7.2 末条"纯文本下 checkpoint 的 `K-1` 规则仍由原单测覆盖"——已由 `test_prefix_pos_max_text()` 保持，未改动。

### 6.3 范围外

分块 KV、增量写入、范围读取、热点淘汰等新算法未启动（规划 §1、§8 明确不在本轮授权范围）。多 slot、多租户、公网鉴权、Linux 未涉及。

---

## 7. 审查建议

1. **优先审查 §5 的控制流缺口**：这已是连续两轮出现"调用点未被测试覆盖"的问题。是否需要为 `materialize_prefix()` 建立可测的控制流结构，请规划判断。
2. **确认 §6.1 的未验证项是否可接受交付**：媒体门禁是本轮新增的保守保护，其正确性目前只有单测支撑，未在真实多模态输入下验证过。若需要真实验证，请提供多模态模型与图片测试条件。
3. **核对 checkpoint 位置界改动的语义**：`tgt_pos_max` 替代 `K-1` 对纯文本等价（两者在从位置 0 起、无 shift 时相同），但对媒体与 position shift 路径的语义变化**未经运行验证**。

---

## 8. 推送状态

见推送后确认。
