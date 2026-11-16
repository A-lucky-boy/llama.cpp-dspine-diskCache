# 2026-11-15 17-50 - test1_1

## 0. 规划定位与执行约束

**项目**：[`A-lucky-boy/llama.cpp-dspine-diskCache`](https://github.com/A-lucky-boy/llama.cpp-dspine-diskCache)，以本次审阅的 `main` 工作树为起点。

**目标**：在不改变模型数值计算和现有 llama.cpp 推理语义的前提下，将单纯以会话完整状态快照为主的 RAM/Disk 两级提示缓存，逐步扩展为可观测、可按请求边界捕获、能发现公共前缀、可进行前缀分块/增量持久化的缓存系统。优先改善真实可复用前缀覆盖率，同时避免将逻辑命中误当作性能收益。

**文档边界**：本文只记录实施规划、现有问题的修复位置、算法与数据流变更、兼容性以及验证检查清单；不填写预想实验结果、预设性能数值、早停要求或最终优劣裁定规则。开发人员完成代码变更后，应将**实际修改记录**追加到仓库 `change.md`，并将总体架构与现状写入 `projectSummary.md`。本文不应在实施完成后被改写为实验结果报告。

**强制顺序**：首先完成“阶段 0”的代码核查与现有缺陷修复。未经该阶段验证，不启动新缓存单元格式、公共前缀识别或磁盘增量写入的代码实施。后续阶段是**条件化工程路线图**，不是允许跳过修复环节的并行改造指令。每个阶段结束必须回看最新 `change.md` 和上游 baseline 的对应实现，检查修改差异，发现新错误应先修复。

**实施策略**：一次只落地一个阶段；一个阶段可拆多个小提交，每个提交可独立构建、具有可回退边界；禁止在同一次提交中同时改动 KV 物理格式、候选选择策略与 slot 生命周期。所有新增策略均通过开关明确启停；关闭时应走既有路径，保证可通过日志确认。

## 1. 已审阅依据与 baseline 对比

### 1.1 已检查的仓库资料

- `change.md`：截至 `2026-11-15 12-57` 的最新记录。已实现磁盘二级缓存格式 v7、manifest v2、CRC64 检查、TTL、异步写入、LoRA 隔离、前缀落后 4096-token 时抑制重复写入等；最近还记录了重启后首个请求偶发全量重算的未定位现象。
- `experienceHistory.md`：记录了 packed 元数据写侧 CRC 缺失、启动时索引/恢复校验盲点及一些策略的已验证边界；须保持这些事实与新代码一致，不能将曾经实现后回退的方案误视为已上线。
- `README.md` / `README_EN.md`：确认当前默认 `--cache-disk-min-tokens 2048`、`--cache-disk-idle-flush 60`、`--cache-disk-queue-max 24576` MiB、`--cache-disk-max 65536` MiB（示例部署 204800 MiB）、`--n-ctx-checkpoints 32`，部署使用 `-np 1` / `--kv-unified`。
- `tools/server/server-task.cpp` / `.h`：确认 `server_disk_cache::load/store/mirror/read_entry/write_entry/worker`、`server_prompt_cache::load/alloc/flush_to_disk` 等调用关系与现有限制。
- `tools/server/server-context.cpp`：确认 `server_slot::prompt_save/prompt_load`、`get_available_slot` 的选槽逻辑以及 `maybe_flush_prompt_cache` 的空闲镜像流程。

### 1.2 与上游 llama.cpp baseline 的能力差异

| 能力 | 上游 llama.cpp server baseline | 本项目已实现 | 本规划处理方式 |
|---|---|---|---|
| 同 slot 的公共前缀增量续算 | 有：按 token LCP 复用在位 KV | 保留 | 不重做；只补统一统计 |
| RAM 提示缓存、空闲 slot 缓存 | 有相关 server 能力，依配置启用 | 保留并扩展 | 确保调度、slot 语义不变 |
| 通用磁盘二级前缀查找与自动存取 | 上游不具备本仓库相同的自定义 `server_disk_cache` 实现 | 已新增 | 在现有实现上扩展，而非重复建设 |
| 按 token LCP 匹配 | 已存在类似 slot/RAM 前缀逻辑 | Disk 也已实现 | 不重复实现 LCP；增加结构化索引与公共前缀记录 |
| 自动写入会话快照 | 不含本仓库这套定制逻辑 | 已实现空闲镜像、RAM 淘汰落盘 | 保留旧路径作为回退 |
| 请求边界独立的前缀单元 | 无本规划定义的功能 | 尚无通用独立单元机制 | 条件化新增 |
| 公共前缀热点识别及增量前缀物理存储 | 无本规划定义的功能 | 尚无 | 在前置可靠性修复后新增 |

**对比说明**：这里的 baseline 指上游 `ggml-org/llama.cpp` 的 server 行为，不表示可将当前仓库直接无损恢复到上游最新版本；当前仓库还涉及 DeepSeek 类模型、draft/target 双上下文、Windows/CUDA 构建以及原有缓存格式。研发工作应以当前仓库作为兼容基底，并使用上游实现核对选槽、序列移除、checkpoint 与序列状态 API 的语义。不要在未锁定准确上游提交 SHA 的情况下声称已经完成逐行源码 diff。当前程序版本信息记录为 `0.9.0-dev` / `60f3891`，但该短 SHA 不足以单独证明公共上游对应提交；实现者须先记录可解析的完整 commit 和本仓库 HEAD。

### 1.3 与 DeepSeek 的参考边界

DeepSeek 官方 Context Caching 文档描述：缓存以完整、独立的前缀单元复用；创建时机涉及用户输入结束、模型输出结束、跨请求公共前缀以及长输入/输出的固定间隔。**这些属于设计参考，不等于可以直接移植 DeepSeek 的 KV 编码和存储格式**。DeepSeek 的 MLA、可能存在的滑动窗口/混合注意力机制、分布式服务和存储架构，与本项目通用 llama.cpp 模型上下文并不等价。特别注意：旧文档中的“64 token 单元”不得直接解释为“所有模型都可以安全地按 64 token 截出独立 KV 并重组”。

参考链接：
- https://api-docs.deepseek.com/guides/kv_cache/
- https://api-docs.deepseek.com/news/news0802/
- https://github.com/ggml-org/llama.cpp/blob/master/tools/server/server-context.cpp

## 2. 现有缺陷与阶段 0：必须优先修复

### 2.1 问题 A：重启后首个请求偶发全量 prefill

**证据**：`change.md` 的 `2026-11-15 03-47` 条目记录，存在两条具有前缀关系的缓存条目时，重启后的首个请求偶发全量重算，第二次请求正常；记录中只是怀疑启动时 `dedup_prefixes` 与首次查询并发，并未完成定位。**不得直接将竞态当作已经确定的根因**。

**优先排查文件与位置**：
- `tools/server/server-task.cpp`：`server_disk_cache` 构造/`scan()`/`load_manifest()`/`dedup_prefixes()`/`load()`，以及后台 `worker()` 与 `entries` 的锁保护范围。
- `tools/server/server-context.cpp`：`server_slot::prompt_load()`、`get_available_slot()`、`launch_slot_with_task()`、首请求是否沿用 slot KV 导致不走 disk 查找。
- `tools/server/server-task.h`：`entries`、`mtx`、后台线程及初始化状态的所有权。

**修改动作**：
1. 先补充启动期事件日志，包含 `cache_scan_begin/cache_scan_end`、manifest 命中/回退、去重前后条目数、首次 lookup 时间顺序、候选过滤原因；所有事件使用单调时间和 cache 实例标识，方便关联。
2. 明确初始化完成屏障：完成扫描、清单重建、去重、配额修剪之后，才将磁盘缓存标记为可 lookup；如原代码实际已保证此不变量，保留现有同步，不新增无意义互斥。
3. 对比第一次、第二次相同输入时的 token 序列、media 元数据、条目候选集合、配置签名、读取错误和 slot 行为；区分“没有查找”“查找无候选”“匹配后拒绝”“恢复失败”四种情况。
4. 若是去重策略错误，需确保被删除短条目所提供的**合法可恢复前缀**能够由保留长条目覆盖；对成本守卫、media 边界和 checkpoint 恢复能力也必须保持单调兼容，否则不能简单以长条目取代短条目。
5. 仅在查证为并发问题后才新增相应同步；不得通过粗暴全程持锁读取数 GiB 文件来规避竞态。

**数据流影响**：无新的缓存数据语义；只明确 `startup-index-ready → lookup` 顺序，完善索引/候选日志。

### 2.2 问题 B：packed 元数据与 KV 配对完整性

**证据**：`experienceHistory.md` 记录，当前 packed token/media 元数据缺少独立的写侧校验；启动清单重算 hash 不是充分的外部完整性证明。`read_entry()` 对 main/drft/checkpoint 的 CRC 不能替代 packed↔KV 原始对应关系验证。

**优先排查文件与位置**：`server-task.cpp` 中 `disk_cache_pack`、`write_entry`、`read_entry`、`load_manifest`、`scan`、`load` 及文件头版本字段；`server-task.h` 的条目/manifest 元数据。

**修改动作**：
1. 建立清晰的文件信任边界：写入时先确定 token/media 元数据，写侧计算 `packed_crc64`，然后与 main/drft/checkpoint CRC 一起写入同一个原子提交单元。
2. 读取时必须在任何 `llama_state_seq_set_data_ext` 写入**之前**校验 packed 的存储 CRC、格式边界、长度与与索引值的一致性；读取后再进行 media 语义验证。不可使用“重新对坏 packed 算 hash”等价替代写侧校验。
3. 在索引和磁盘读取时建立与写入期绑定的 `packed_hash/CRC` 约束；若清单不能信任，回退严格文件读取或拒绝条目，不允许因为 manifest 存在就跳过关键验证。
4. 文件版本升级必须独立于后续增量分块格式：建议先实现兼容旧读/新写的版本化恢复或明确旧版本忽略策略；**禁止无提示地把旧 v7 文件当新版本读取**。旧缓存可以安全丢弃重建，但必须在日志中说明。
5. 对 KV↔packed 的配对问题，采用原子写/重命名、不可变条目 ID 和提交时元数据绑定，而非声称仅凭恢复时的 KV payload 就能反推出原 token 序列。
6. 保持现有失败返回契约：**首次状态写入前失败可正常回退候选；写入发生后任何失败必须使上层清空 slot 并重新 prefill**。不得插入会造成静默错误输出的 `return true`。

**数据流影响**：`write snapshot → pack + CRC + KV → tmp → atomic commit → manifest`；`load candidate → verify metadata → verify state → state restore`。

### 2.3 阶段 0 验证检查清单

- [ ] 从本仓库记录完整 HEAD、当前代码 hash、可解析的上游基线 commit；将相关差异附入 `change.md`。
- [ ] 复现/排查两条前缀关系条目的重启首请求问题，日志能区分查找阶段与恢复阶段；若尚未定位，必须把未解决原因如实记录，后续新缓存格式不进入实施。
- [ ] 完全重复、分叉前缀、短前缀、长前缀、多媒体缓存的首次/二次/重启请求，检查 token 与返回内容的一致性。
- [ ] packed 元数据单比特破坏、状态 CRC 破坏、manifest 损坏、断电残留 `.tmp`、截断文件都应安全回退，不复用不一致的 KV。
- [ ] 检查 LoRA 变化、draft 模型变化、mmproj 变化、K/V 量化/上下文配置变化时不会跨配置复用。
- [ ] Windows/MSVC/CUDA 构建通过；没有 Linux 特有 API 被无保护引入；Linux 的原有构建路径不被改动。
- [ ] `--cache-disk` 关闭以及新功能开关关闭时，slot/RAM/推理输出路径保持原有行为。

## 3. 阶段 1：精确命中统计与事件可观测性

> 在阶段 0 完成后实施。本阶段不改变查找、写入或淘汰决策。

### 3.1 目标

把“没有命中”拆解为：输入无共享前缀、slot 命中、RAM 命中、磁盘候选不达门槛、磁盘恢复成本守卫拒绝、条目未落盘、磁盘文件读取失败、候选被覆盖/去重、配置不兼容和实际 prefill。每个 token 在一次请求中最多计入一个复用来源。

### 3.2 文件修改

- `tools/server/server-task.h`：增加 `server_cache_lookup_trace`、`server_cache_request_stats`、`cache_reject_reason` 等**轻量值对象**；不要将完整 prompt 或用户文本记入日志。
- `tools/server/server-task.cpp`：在 `server_prompt_cache::load`、`server_disk_cache::load/store/mirror/worker` 增加结构化事件与拒绝原因计数。
- `tools/server/server-context.cpp`：在 token 化完成、slot 选中、真正执行 prefill 前后、响应结束时汇总统计；明确统计计数边界。
- `tools/server/server-http.cpp` 或已有 metrics 代码路径：增加可选 JSON/Prometheus 计数，先核实实际输出组件，避免创建重复指标通道。
- `common/common.h`、`common/arg.cpp`：拟增加 `--cache-stats`（默认关闭，环境变量按现有风格命名）；可选详细事件开关与现有 `-lv` 联动。
- `tools/server/tests/unit/`：增加独立的命中统计回归测试；`experiments/`：增加统计采集/解析脚本。

### 3.3 指标定义

令 `P_i` 为请求 i 进入模型的**实际 prompt token 数**，`R_slot,i` / `R_ram,i` / `R_disk,i` 为实际避免重新 prefill 的互斥 token 数，`M_i` 为实际需要 prefill 的 token 数。

```text
P_i = R_slot,i + R_ram,i + R_disk,i + M_i
H_total = sum(R_slot + R_ram + R_disk) / sum(P)
H_disk  = sum(R_disk) / sum(P)             # 全局输入 token 口径
H_disk_eligible = sum(R_disk) / sum(P_disk_eligible)  # 单独注明可查询盘层的请求集
```

注意实际解码可能因 BOS、强制重算最后一个 token、multimodal placeholder、context-shift、checkpoint 或模型限制而导致统计边界变化；必须以**真实免除的 prefill 工作**为准，不能简单将 `LCP` 全部计入命中。所有比率必须与分子、分母和请求集合同时输出；另统计候选 LCP 与成功复用 LCP 的差值。

**推荐计数器**：`cache_request_prompt_tokens_total`、`cache_reused_tokens_slot_total`、`cache_reused_tokens_ram_total`、`cache_reused_tokens_disk_total`、`cache_prefill_tokens_total`、`cache_disk_candidate_lcp_tokens_total`、`cache_disk_reject_total{reason}`、`cache_disk_enqueue_total{outcome}`、`cache_disk_bytes_read_total`、`cache_disk_bytes_written_total`、`cache_disk_restore_duration_seconds`、`cache_disk_queue_bytes`、`cache_disk_queue_entries`、`cache_disk_entries`、`cache_disk_manifest_fallback_total`。

事件日志应附 `request_id`、`cache_entry_id`（不可泄露 token 文本）、`config_signature_id`、`source_tier`、`candidate_lcp`、`reused_tokens`、`reason`、`read_bytes`、`elapsed_us`；不要把路径、LoRA 参数和客户文本无条件公开到 API 响应。

### 3.4 数据流与流水线影响

`request/tokenize → slot selection → RAM lookup → Disk lookup → actual prefill → response stats`；后台缓存事件以异步记录归属到对应条目，不反向计入已结束请求的命中数。仅观测，不修改 GPU kernel / 模型权重 / 采样算法。

### 3.5 验证检查清单

- [ ] 完全冷缓存首请求的 token 分解相加一致；完全重复请求可区分 slot/RAM/disk。
- [ ] 分叉前缀、非零尾部、媒体占位符、stateless 请求均不会双计数。
- [ ] 破坏缓存文件后，候选 token 与真实复用 token 不混淆。
- [ ] 打开/关闭统计不会改变输出 token 与缓存选择。
- [ ] JSON/metrics 不暴露 prompt、图片或内部磁盘绝对路径。

## 4. 阶段 2：请求边界捕获（不改变文件格式）

### 4.1 目标

把“仅在 RAM 淘汰/全局空闲镜像时产生快照”的行为扩展为“**请求输入边界与生成结束边界可选地提交缓存捕获事件**”，先复用现有 v7/后续完整快照机制。此阶段不宣称具备真正的增量块写入。

### 4.2 文件修改

- `tools/server/server-context.cpp`：在请求 token prefill 完成时和输出解码正常结束时增加边界事件；不能在 prompt KV 尚未计算完成时序列化状态。取消、超时或上下文截断需要单独处理。
- `tools/server/server-task.h/.cpp`：将 `prompt_save` 的“完整快照序列化”与“请求边界事件接收/去重/异步提交”分离；重用已有 `shared_ptr`/队列限流与去重。
- `common/common.h`、`common/arg.cpp`：拟增加 `--cache-boundary-capture none|input|output|both`，默认 `none`；保持 `--cache-disk-idle-flush` 的原有意义。
- `README.md` / `README_EN.md`：更新新选项与与 RAM/disk 缓存关系。

### 4.3 关键算法与所有权约束

1. 定义 `CacheBoundaryEvent{request_id, slot_id, boundary_kind, token_end, model_signature, media_signature, generation}`。
2. `input` 边界只在输入最后 token 的 KV 已经完成计算并且状态位置可序列化时发送；`output` 边界只在有效生成状态完成时发送；必须明确模型要求重算最后 token 的例外。
3. 当前项目 `-np 1`、`--kv-unified` 配置下，确认状态快照捕获在 slot 被清理、重用或下一次 prefill 之前完成。若异步后台线程不能安全访问 live `llama_context`，应在主推理线程安全点同步提取状态字节，仅把磁盘 IO 异步化。
4. 对同一边界、同一 token 前缀和同一模型/媒体签名做去重。必须兼容当前的 `covered_cheaply()`；该规则只能阻止**冗余完整快照写入**，不能妨碍阶段后续对独立公共前缀单元的生成。
5. 不因为请求结束就强制写磁盘；通过排队、预算与事件状态区分 `captured`、`queued`、`persisted`、`rejected`。只有持久化完成才可统计“disk available”。

### 4.4 预期数据流变更

`prefill-input-end / generation-end → boundary event → safe KV snapshot capture → dedup/admission → async disk write → manifest commit`。

**训练流水线**：不涉及训练。**推理流水线**：增加边界处理回调与状态捕获，不能在推理线程进行磁盘大文件写入；不得改变 logits、采样参数和 speculative decoding 的 target/draft 状态一致性。

### 4.5 验证检查清单

- [ ] 请求输入/输出边界只捕获实际可恢复的完整状态。
- [ ] 中止、超时、断流、异常、上下文 shift 等边界不会写入无效条目。
- [ ] target + draft 快照一致，LoRA/多模态守卫保持生效。
- [ ] `none` 模式与原路径一致；磁盘队列拥塞时前台仍能正常推理。
- [ ] 重启后能从旧格式快照中恢复；未持久化事件不会被计入命中。

## 5. 阶段 3：跨请求公共前缀识别与缓存准入

### 5.1 目标

针对 `A+B`、`A+C`、`A+D` 等请求，识别出现频率高的公共前缀 `A`，生成独立**候选前缀边界**。先不直接把任意 KV 字节切片写盘；必须先证明该边界可被底层模型状态 API 安全导出。

### 5.2 文件修改

- 新增 `tools/server/server-prefix-index.h/.cpp`：独立存储 token 前缀目录、引用计数/访问计数、长度、媒体签名及可恢复状态标记。
- `tools/server/CMakeLists.txt`：注册新源文件。
- `tools/server/server-task.h/.cpp`：给每个已接受的请求追加可哈希的 token/media 前缀摘要；接受查询结果并交由统一缓存准入器决定。
- `tools/server/server-context.cpp`：在请求安全点发布输入前缀元数据，避免后台线程访问可变 token 容器。
- `common/common.h`、`common/arg.cpp`：拟增加 `--cache-prefix-discovery`（默认关闭）、`--cache-prefix-min-tokens`、`--cache-prefix-max-candidates`、`--cache-prefix-hit-threshold`。这些是候选发现参数，不应直接覆盖已有 `--cache-disk-min-tokens`。

### 5.3 识别算法

- 先采用**确定性的增量前缀 trie / radix tree 或带碰撞复核的 chunk-hash 索引**；基于 token id 和 media identity，而非原始字符串。
- 每个节点记录 `prefix_length`、`parent`、`last_seen`、`occurrence_count`、`full_digest`、`model_signature`、`media_signature`、`materialization_status`。
- 新请求 `Q` 到达时，用最长公共前缀识别与已有请求集合的分叉点；候选可包括“输入结束位置”“既有公共分叉位置”“对齐到安全边界的固定间隔位置”。不要假定 token 文本长度与 KV 位置一致。
- 只对复用概率高、前缀足够长且能安全导出状态的节点发起物理缓存任务；`occurrence_count` 仅是候选信号，不替代成本和容量控制。
- 哈希命中必须以完整 token/media 元数据复核。避免以 64 位 hash 唯一性作为 correctness 保证；可采用强摘要作为索引，真正复用仍需与 token IDs 逐项匹配。
- 区分 `observed`、`eligible`、`materializing`、`persisted`、`unavailable` 状态；一个共享前缀即使没有物理快照，也可以被识别，但不能计为命中。

### 5.4 核心技术难点

**历史状态切分不可凭空完成**。如果请求 `A+B` 已经执行完毕，却没有保留在 `A` 末尾的可恢复状态，就不能仅凭 `A+B` 的 token ids 把中间的 KV 安全导出为 `A`。可选的正确做法包括：在预先确定的安全点捕获；由支持的 `llama_state_seq_*` API 复制/裁剪完整序列状态；或推迟到下次请求 `A+C` 的安全边界生成 `A` 的状态。禁止直接在序列化的 opaque state blob 中按 token offset 切字节。

### 5.5 数据流与流水线影响

`tokenized request → prefix index → candidate boundary → safety/benefit gate → capture request → existing disk writer`。前缀目录的搜索不得阻塞推理主循环较长时间；内存占用需要上限，淘汰只改变候选信息，不删除还被磁盘条目引用的元数据。

### 5.6 验证检查清单

- [ ] 分叉请求的公共前缀位置被识别；不同 token 序列哈希碰撞不得互命中。
- [ ] 媒体元数据、模型配置、LoRA 与 chat template 差异导致不同候选身份。
- [ ] 公共前缀只有 token 索引而无 KV 实体时不能恢复，日志明确 `not_materialized`。
- [ ] Trie / hash 索引内存有上限，删除、并发读写、关机过程无悬挂引用。
- [ ] 前缀捕获保持 target/draft 双状态与 checkpoint 不变量。

## 6. 阶段 4：可增量持久化的分块 KV 物理格式

### 6.1 目标与先决设计审查

从“完整文件一次性写入/完整文件一次性读取”转向“**不可变共享前缀 + 可增量追加的新状态**”的数据结构，降低写入放大。此阶段的关键是物理格式正确性，而不是照搬某个 64-token 数字。

**实施之前，先做状态表示可行性审查**：
1. 阅读 `src/llama-context.cpp` 中序列状态导出/恢复 API 的真实布局，区分 target 和 draft 的完整状态、checkpoint state、计算缓存及模型特有状态（含混合注意力/状态空间模型）。
2. 确认 `llama_state_seq_get_data_ext` / `set_data_ext` 只保证**整个序列状态**的兼容恢复，还是存在正式 API 可导出“前 N tokens”的自洽状态。如果只支持 opaque 整体状态，**不得把原状态字节切片解释为可复用分块**。
3. 评估实现新序列前缀状态导出 API 的必要性与上游维护成本；若不能确认支持，应保留阶段 3 的独立公共前缀完整快照方案，不进行底层字节分块。
4. 明确位置编码/序列 ID/模型状态信息、checkpoint 的依赖；不同注意力机制、量化方式及 KV 统一模式下均须适用或显式排除。

### 6.2 建议文件结构

- 新增 `tools/server/server-disk-cache-format.h/.cpp`：版本化记录布局、校验与序列化，隔离于查找和策略层。
- 新增 `tools/server/server-disk-cache-store.h/.cpp`：对象存储、引用计数、原子提交、读取范围、垃圾回收。
- 新增 `tools/server/server-disk-cache-index.h/.cpp`：记录 prefix DAG、chunk/segment metadata、逻辑条目到物理对象映射。
- `tools/server/server-task.h/.cpp`：以适配器引入 `legacy_snapshot` 与 `segmented_prefix` 两种后端；禁止一次性删除现有 `server_disk_cache::read_entry/write_entry`。
- `tools/server/server-context.cpp`：明确导出/恢复安全点、物理状态 generation 与 seq id 生命周期。
- `src/llama-context.cpp` / 对应头文件：**仅在必要且经过基线审查时**增加经过类型检查的前缀状态导出/恢复能力；不直接修改模型计算图或采样器。
- `common/common.h`、`common/arg.cpp`：拟增加 `--cache-disk-format snapshot|segmented`（默认 `snapshot`）和物理 `segment_target_tokens`（在功能验证前不暴露为生产建议）。

### 6.3 逻辑模型

```text
CacheEntry
  entry_id, model_sig, tokenizer_sig, media_sig, total_tokens
  logical_prefix_digest, committed_generation, last_used
  segments = [SegmentRef_0 ... SegmentRef_k]

SegmentRef
  parent_prefix_id, end_token_offset, object_id
  state_kind: full-checkpoint | validated-delta
  target_state_ref, draft_state_ref, checkpoint_refs
  payload_crc, metadata_crc, dependency_version

ObjectStore
  object_id -> immutable bytes + size + checksum + reference_count

Manifest
  epoch/generation + set of committed logical entries and referenced objects
```

`validated-delta` **只有在底层提供正确可重放的增量编码与恢复 API 后才能启用**。否则使用 `full-checkpoint` 对象并实现多 entry 的共享引用；这至少能避免完全相同的前缀对象被多次写入，但不保证不同长度快照具有块级去重。

### 6.4 原子写入与恢复顺序

**写入**：安全点捕获 → 序列化完整且自洽的 segment/state → 写临时不可变对象 → CRC/长度验证 → flush/fsync（按平台实现）→ 原子重命名对象 → 提交新的 manifest generation → 将条目标记为可查找。manifest 不应先引用尚不存在的对象。

**恢复**：选取实际可覆盖请求 LCP 的、所有依赖对象都存在的最近可恢复完整状态 → 校验 token/media、模型签名和全部对象校验和 → 在未改写当前 slot 前准备可回退状态 → 恢复 target/draft/checkpoint → 截断无效尾部 → 从正确位置继续 prefill。

不得在持有全局 `entries` 索引锁时进行数 GiB 磁盘 IO。删除/淘汰需要 pin 已选对象或者引用计数，避免读取中的文件被 GC。跨重启重建引用计数应由**已提交 manifest 的可达图**完成，避免以临时文件残留作为有效对象。

### 6.5 相容性

- 旧 v7 snapshot 文件只由旧解析器读取；新的分块格式拥有独立 magic/version/目录或命名空间，避免在同目录中被 `scan()` 当成未知损坏文件删除。
- 配置签名至少覆盖 model、tokenizer、chat template 影响到的 token 序列、mmproj、target/draft、KV 量化、上下文配置、LoRA 语义；若不能安全确定某项，使用保守隔离。
- `--cache-disk-format snapshot` 是明确回退路径，禁止自动尝试从新格式执行未经验证的恢复。
- 禁止直接复用 DeepSeek MLA 特有 KV 压缩假设；支持与否由当前加载模型和底层序列状态 API 决定。

### 6.6 验证检查清单

- [ ] 对同 token 序列，snapshot 与 segmented 恢复后的模型输出及状态一致；覆盖 target-only、target+draft、不同 KV 类型。
- [ ] 对任意前缀长度、尾部截断、重复 token、多媒体边界、context shift，恢复位置正确。
- [ ] 任意时刻断电/强杀后的 manifest 不引用半写入对象；GC 不删正在读取或仍被引用的对象。
- [ ] 单个对象损坏、缺失、版本不兼容均安全回退；没有无提示静默错答。
- [ ] 不支持分段导出的模型显式回退 snapshot，不走危险字节裁切。
- [ ] Windows 与 POSIX 原子替换、文件句柄、同步行为分别检查；不得假设 rename 在两平台语义完全一致。

## 7. 阶段 5：增量写入、范围读取及成本感知查找

### 7.1 目标

在阶段 4 的可靠物理格式上，将“新增长尾部也要重写旧前缀”的 IO 模式替换为“复用共享对象，仅提交新增且可独立验证的对象”，并将查找从“最长 LCP 优先”扩展为“先保证正确性，再选恢复成本适合的候选”。

### 7.2 文件修改

- `server-disk-cache-store.*`：实现对象级增量提交、去重、引用管理、部分读取/按需 checkpoint 恢复。
- `server-disk-cache-index.*`：索引可恢复边界、父依赖、物理字节成本与热度。
- `tools/server/server-task.cpp`：更新 `store/mirror/load/covered_cheaply/drop_prefixes` 与后台队列任务单位；从 `whole_entry_write` 切换为 `object_write` 或 `manifest_only`。
- `tools/server/server-task.h`：待写队列加入对象 ID、实际待写字节及依赖状态；队列预算仍然按真实 pinned 内存/预计待写 IO 管理。
- `common/common.h`、`common/arg.cpp`：拟增加 `--cache-admission-policy legacy|cost-aware`、`--cache-disk-io-budget-mib` 等开关，默认沿用旧路径。

### 7.3 策略

1. 先用 `prefix DAG` 验证与已持久化祖先前缀的严格 token/media 关系；只要相关状态不能证明共享，不做字节层 dedup。
2. 对新增状态只提交**安全可解码的新增数据**；若底层只支持整状态导出，可做对象级重复消除，但不得宣传为真正的增量 KV。
3. 现有 `DISK_CACHE_MAX_PREFIX_LAG_TOKENS=4096` 的抑制策略可在 snapshot 模式下保持；segmented 模式必须单独计算“追加对象的实际代价”，不能继续直接使用此常量决定所有新单元写入。
4. 查找时先获取所有合法完整前缀候选，考虑实际可恢复 token、磁盘读取字节、状态恢复耗时、需要重新 prefill 的 token、是否在 RAM/页缓存中；用保守的在线统计数据选择，缺乏可靠估计时回退现有 LCP 优先策略。
5. 对边界对象建立热度、最近访问、重建成本和字节成本记录；分离目录对象的元数据淘汰与 KV 物理对象 GC。
6. 若候选处于 `queued/writing` 状态，默认按尚未持久化处理；同进程可有安全的 RAM 复用，但不能把 pending 磁盘对象当可跨重启命中。

### 7.4 数据流

`LCP candidates → validity check → cost estimate → choose restorable boundary → read required objects → restore → prefill remainder`；`boundary capture → ancestor lookup → append eligible objects → atomic manifest commit`。

### 7.5 验证检查清单

- [ ] 一系列逐步追加的请求不会重复写入相同物理对象（依据对象 ID/字节统计验证）。
- [ ] 范围读取只读取恢复确需的对象；如回退完整快照，日志准确标记。
- [ ] 同 LCP 不同 IO 成本的候选选择可解释并且不破坏正确性。
- [ ] 缓存空间淘汰、引用计数、后台队列拥塞、重启恢复在压力条件下保持一致。
- [ ] 只有已完成提交的对象对读者可见。

## 8. 阶段 6：热点准入、分层淘汰及多租户边界（可选）

### 8.1 目标

当工作集大于磁盘配额时，避免低复用价值的巨大会话快照挤出频繁使用的公共前缀对象。仅在真实的独立对象存储和事件统计稳定后再设计策略。

### 8.2 修改文件

- `server-prefix-index.*`：记录命中次数、最近使用、空间占用与复建开销。
- `server-disk-cache-store.*`：以对象依赖图为单位执行可达性检查与空间回收。
- `server-task.cpp`：用可配置准入与淘汰策略替代仅依赖完整条目最近使用时间的部分路径；snapshot 模式继续沿用原有 LRU。
- `server-context.cpp`：如运行在多用户 API 场景，确认请求身份信息是否可安全、稳定地传递到缓存命名空间。

### 8.3 策略与安全边界

- 默认单实例、单可信租户；未来如果跨用户共享，必须优先设计隔离 namespace、权限、密钥边界与 side-channel 缓解，不能仅依靠 token hash 做隔离。
- 对象驱逐参考 `reuse_count`、`last_used`、`bytes`、`recompute_cost` 和引用关系；保护高共享度祖先，但不能形成永不回收的对象。
- TTL 与配额管理应在 manifest 图与对象目录层分别处理，防止逻辑条目删除却保留孤儿对象导致磁盘持续增长。
- 不在日志中存储用户文本、图片原始数据及原始 token 序列。

### 8.4 验证检查清单

- [ ] 共享祖先不会在仍被有效条目引用时删除。
- [ ] 删除单条目不会造成其他分支无法恢复。
- [ ] TTL、配额淘汰和并发读取不会引发 use-after-free 或孤儿持续增长。
- [ ] 任何租户隔离方案必须通过跨身份不复用测试。

## 9. 阶段 7：统一工程回归与文档收尾

### 9.1 文件修改

- `tools/server/tests/unit/`：覆盖所有关键缓存行为的自动化测试；区分纯算法测试、文件格式测试、端到端模型测试。
- `experiments/`：新增可复现负载生成器与指标采集器（只写测试工具，不在本文记录评测结论）。
- `README.md`、`README_EN.md`：说明工作原理、兼容性、各开关、支持模型边界、磁盘布局、备份/迁移、指标解释。
- `projectSummary.md`：记录当前完整项目结构、配置、数据流、现存边界和已落地能力。
- `change.md`：逐提交记录修改文件、真实变更、构建验证、兼容性和已确认问题。

### 9.2 建议负载集合

| 场景 | 请求顺序 | 重点检查 |
|---|---|---|
| 冷启动 | `A` | 不存在缓存时全量 prefill 与统计一致 |
| 完全重复 | `A → A` | slot/RAM/Disk 复用来源归因 |
| 多轮追加 | `A → A+B → A+B+C` | 输入/输出边界捕获与追加状态 |
| 分叉共享 | `A+B → A+C → A+D` | 公共前缀识别、独立单元复用 |
| 高频模板 | `System+Tools+Q1 → System+Tools+Q2` | 长固定系统/工具前缀的共享 |
| 静态前缀内含媒体 | `Text+Image1+Q1 → Text+Image1+Q2` | 媒体标识及 chunk 边界校验 |
| 媒体变化 | `Text+Image1 → Text+Image2` | 不跨图片错误复用 |
| 长上下文 | `A → A+10K → A+20K` | 队列、对象复用、checkpoint、安全截断 |
| 频繁重启 | 写入中强杀 / 正常退出 / 重启 | 原子性、清单恢复、孤儿清理 |
| 配额压力 | 持续插入分叉长前缀 | 淘汰正确性与内存上限 |
| LoRA/draft 切换 | 变更生效配置后重复请求 | 缓存隔离、安全回退 |

### 9.3 回归边界

- [ ] `--cache-disk-format snapshot` + 所有新功能关闭时，旧缓存行为与上游兼容路径保持一致。
- [ ] CPU-only 构建、Windows CUDA 构建及使用真实模型的端到端推理均有覆盖。
- [ ] 并发多 slot 需单独验证；当前生产实测 `-np 1` 不构成多 slot 安全性的证据。
- [ ] 所有指标明确统计来源与口径，不能把候选公共 token、已持久化 token、实际复用 token混同。
- [ ] 文档中不承诺“达到 DeepSeek 命中率”，只描述已实现且经过验证的机制。

## 10. 推荐接口与代码组织约束

```cpp
// 仅为接口规划，开发时须与当前命名空间、模型 API、线程模型核对。
enum class cache_lookup_source { slot, ram, disk_snapshot, disk_segmented, none };
enum class cache_unit_state { observed, eligible, capturing, queued, persisted, invalid };
enum class cache_reject_reason {
    none, too_short, no_prefix, media_mismatch, signature_mismatch,
    not_materialized, io_cost, queue_full, memory_limit, disk_quota,
    checksum_error, unsupported_state_api, restore_failed
};

struct cache_prefix_key {
    std::string config_digest;
    std::string token_media_digest;
    uint64_t token_count;
};

struct cache_lookup_result {
    cache_lookup_source source;
    cache_reject_reason reject;
    uint64_t candidate_lcp;
    uint64_t reusable_tokens;
    uint64_t bytes_to_read;
};

struct cache_boundary_event {
    uint64_t request_id;
    uint32_t slot_id;
    uint64_t token_end;
    uint64_t state_generation;
    // boundary kind + immutable config/media digest
};
```

**接口规范**：

1. **职责分离**：索引仅描述“哪里可能复用”，数据存储保证“可恢复且持久化”，恢复器负责“正确写入 target/draft 状态”，统计器负责“真实免除多少 prefill”。
2. **回退语义**：索引匹配但无有效状态时正常 prefill；读盘 CRC 失败时删除/隔离损坏条目；恢复后失败只能清空 slot 并重新 prefill。
3. **并发所有权**：后台线程只接受不可变 token metadata / 不可变 KV 快照，不持有裸 `server_slot*` 或可变 `llama_context*`；主线程安全点捕获状态。
4. **避免双份内存**：继续利用共享缓冲，但必须显式核算 queued 引用造成的 pinned memory；不能因为 shared_ptr 不执行内存拷贝就认为队列不占用内存预算。
5. **兼容模式明确**：`legacy/snapshot` 和新增 format 使用不同 magic、目录或 namespace；配置签名变化时必须安全失效，不自动互相冒险解码。
6. **默认安全**：所有新捕获、发现、分块和成本策略默认关闭，只有配置开启才改变现有磁盘缓存行为。
7. **文件布局与跨平台**：Windows/MSVC、Linux/POSIX 需各自审查文件替换、fsync/FlushFileBuffers、共享删除、路径编码和抗断电语义。

## 11. 实施任务交接顺序

1. **当前唯一立即执行的工作是阶段 0**：先核实重启首请求偶发重算的原因并补上 packed/状态绑定的校验；不能把记录中的疑似竞态视作确诊。
2. 在阶段 0 的问题得到修复或明确排除后，顺序实施阶段 1（统计）与阶段 2（边界捕获）。二者保留既有磁盘格式，不引入底层模型状态切分。
3. 实施阶段 3（公共前缀候选目录），先保证候选与“可恢复状态”严格区分。
4. 阶段 4 进行独立的**状态可分块性技术审查**；只有 API 能保证数值与状态正确性，才允许实际实现段式 KV；否则只保留完整前缀快照方案。
5. 阶段 5–6 才引入增量写入、成本感知读盘、热点准入/淘汰。
6. 阶段 7 整理自动化回归、文档和项目总体总结。

**禁止事项**：禁止修改 token ids 的语义来制造命中；禁止只以 hash 匹配取代 token/media 全量复核；禁止简单切割 opaque KV 字节；禁止读取到一半改写 slot 后当成“未命中继续原路径”；禁止为了提高命中率无条件关闭 LoRA、模型配置、媒体隔离和文件完整性保护；禁止未经验证改变模型计算结果。

---

# 2026-11-15 19-02 - test1_2

## 一、实验目标与适用边界

**本次仅实施“阶段 3：单用户公共前缀识别、可恢复性索引与缓存准入”。** 此次不实施分块 KV 物理格式、任意区间状态切片、增量块写入、跨请求批处理调度或新的热点淘汰算法；不修改模型权重、logits、采样策略或投机解码算法。

1. 服务运行目标为**单用户、单会话入口、单 slot（`-np 1`）**。不存在租户身份隔离、跨用户共享、跨租户配额、跨用户并发与多 slot 调度需求。异步磁盘 worker 与推理线程仍然并发，必须处理线程安全，不能将“单用户”等同于“无并发”。
2. 将请求 token 前缀由“临时 LCP 比对结果”提升为具备生命周期的**公共前缀候选目录**；识别 `A+B`、`A+C`、`A+D` 共享的 `A`，保留候选发现的真实证据以及物理可恢复状态。
3. 明确区分“观察到共同 token 前缀”“当前 RAM/slot 有可复用状态”和“磁盘存在且校验通过的可恢复状态”；任何纯索引命中不能计入现有 `reused_slot / reused_ram / reused_disk`。
4. 在**不改变已有完整快照存储格式**的前提下，让目录可以关联既存 RAM、磁盘 v8 完整快照及其真实可恢复前缀；首次实施不主动截取 opaque KV blob，也不因发现分叉而无条件重做 prefill。
5. 仅在代码已证明存在**准确的前缀结束状态安全点**时，允许对该结束点建立复用现有 `prompt_save` / `mirror` 的完整状态快照入口；若没有这个安全点，本次只发布 `observed` 候选及对既存快照的索引，不伪造 `persisted` 状态，也不绕过阶段 4 的 API 可行性审查。

## 二、实施前强制核查（发现错误立即停止新增功能）

### 2.1 `change.md` 的实际核查与版本锁定

编码人员必须首先在**实验机器的实际工作目录**读取最新 `change.md`，尤其是 2026-11-15 18:18、18:35、18:49 三条记录；随后读取 `experienceHistory.md`、`projectSummary.md`（若存在）与源码当前状态。当前外部可访问的 GitHub `main` 不包含这三条未提交记录，**不能拿远程旧代码替代实验机器上的实际修改**。

本阶段规划依据的实验报告声称：已修复 `touch_entry()` 覆盖 `crc_drft` 的偏移错误；缓存条目 v8、manifest v3；已引入 `--cache-stats`、`--cache-boundary-capture`；单 slot 已做相关验证。以上是**报告所述状态**，在本地核查源码和构建产物之前不视为已独立确认。

必须记录：`git rev-parse HEAD`、`git status --short`、参与修改的源码哈希、已构建 `llama-server.exe` / `llama-server-impl.dll` 的版本与构建时间；如果工作树有未提交修改，先形成可回退提交或独立开发分支，再开展功能实现。`change.md` 须明确该提交与输出二进制的对应关系。

### 2.2 与 baseline 和既有实现对比

至少对以下路径执行**同版本源码对比**：

- 当前 `tools/server/server-context.cpp` 的 `get_available_slot()`、`launch_slot_with_task()`、`server_slot::prompt_save/prompt_load`、请求边界回调、`add_cache_stats()`；对照本仓库修改前的原始实现或锁定 SHA 的上游 `llama.cpp` 对应版本。
- 当前 `tools/server/server-task.cpp/.h` 的 `server_prompt_cache::load/alloc`、`server_disk_cache::load/store/mirror`、`read_entry/write_entry`、`scan/load_manifest`、`covered_cheaply`、`drop_prefixes/dedup_prefixes`；重点核对既有 LCP、状态恢复与锁生命周期。
- `src/llama-context.cpp` 的 `llama_state_seq_get_data_ext`、`llama_state_seq_set_data_ext` 及目标模型实际使用的 memory backend；本阶段不得把序列化字节解释为可切片 token 块。

必须确认：完整快照 v8 和清单 v3 的 CRC/长度验证先于状态恢复；状态写入失败会清空 slot 而不是带损坏 KV 继续生成；目标/草稿模型、KV 类型、mmproj、LoRA 配置与 media 身份守卫有效；`--cache-boundary-capture none` 和新功能关闭时不改变 baseline 原有选槽、prefill、推理输出与缓存恢复行为。

**阻断条件**：如果发现任何已有算法修改存在明显错误（包括首请求错误 prefill、packed↔KV 不一致、错误命中归属、恢复失败后继续用不一致状态、错误的分支去重、数据竞争），应立即停止本阶段的新功能开发，先在原代码位置修复并更新 `change.md`。本文件不授权带着这些问题继续新增公共前缀逻辑。若无法得到最新修改版源码与可复现基线，本阶段仅可完成只读设计审查，不可宣称进入功能实现。

## 三、预期修改文件与职责

| 文件 | 修改类型 | 具体职责 |
|---|---|---|
| `tools/server/server-prefix-index.h` | 新增 | 单实例前缀目录的不可变 key、节点状态、查询与观测接口；禁止保存活的 `llama_context*` 或 slot 指针 |
| `tools/server/server-prefix-index.cpp` | 新增 | token/media 前缀归并、分叉点识别、候选状态转换、上限保护、物理条目引用有效性检查 |
| `tools/server/CMakeLists.txt` | 修改 | 仅注册新文件，遵循现有构建组织 |
| `tools/server/server-task.h` | 修改 | 定义前缀候选与现有 `server_prompt_cache` / `server_disk_cache` 的**只读适配接口**，明确返回语义 |
| `tools/server/server-task.cpp` | 修改 | 从已校验的 RAM/Disk 条目建立物理关联；在 store、restore、drop、TTL、limit、去重、读失败后同步更新引用有效性；保留原查找路径 |
| `tools/server/server-context.cpp` | 修改 | 单 slot 安全点提交输入前缀元数据；按现有统计链记录观察、可恢复、实际复用的不同事件 |
| `common/common.h`、`common/arg.cpp` | 修改 | 新增开关及参数，并按现有命令行/环境变量命名习惯配置 |
| `tools/server/tests/unit/` | 新增/修改 | 前缀目录、媒体隔离、引用失效、边界与禁用回归用例 |
| `experiments/` | 新增 | 单用户请求序列生成与请求级统计收集脚本，复用已有 `test-run/` 工具时避免重复创建 |
| `README.md`、`README_EN.md` | 修改 | 描述“发现≠可恢复≠实际命中”、单用户适用范围、开关和限制 |
| `change.md`、`projectSummary.md` | 更新 | 前者记实际代码改动与核查事实，后者记项目总体架构；本文件不记录实验结果 |

文件名以现场源码路径为准；若目录内已有相同功能模块，优先复用而非并存两套前缀索引。

## 四、核心算法变更

### 4.1 数据定义和状态不变量

建议 `prefix_key` 至少包含：**缓存配置签名**、有效 token ID 序列的摘要、前缀 token 长度、前缀内部媒体占位符的稳定身份摘要。摘要只用于快速筛选，实际判定必须逐 token 比较并核对 media 区间，不能以 64-bit hash 相等作为完整等价证明。配置签名须覆盖当前 v8 缓存使用的模型、draft、上下文、KV 格式及适用媒体相关维度。

建议节点字段：`prefix_length`、`token_sequence`（或共享不可变序列引用及边界）、`media_identity`、`observations`、`last_observed`、`state`、`snapshot_ref`、`snapshot_generation`、`source_tier`。不记录 prompt 明文到日志；如果内存索引保存 token IDs，应有明确容量上限和退出释放路径。

**状态定义**：

- `observed`：已观察到一个公共前缀，但尚未证明存在对应可恢复状态。
- `eligible`：该前缀满足安全准入条件，仍不代表拥有可恢复 KV。
- `materializing`：存在合法、安全的状态捕获/持久化流程已启动；本阶段不得为推进状态强行插入未经验证的 prefill 切分。
- `persisted`：物理快照已经原子提交并且索引引用当前有效；后续恢复仍须通过 v8 CRC、media、配置及状态一致性检查。
- `unavailable`：对应物理快照被淘汰、失效、损坏、配置变化或其他原因撤销。

另外允许“RAM/slot 当前可复用”的**来源标签**，但它不能假冒 disk `persisted`。`observed/eligible` 不得作为磁盘查找的真实命中。

### 4.2 公共前缀发现

单实例只在完整 token 化之后收集本次请求的不可变元数据，按 token 索引与此前观察过的请求进行 LCP 匹配。优先采用**路径压缩 Trie（radix tree）或按前缀层级维护的有界候选表**；切勿在每请求上对无限增长的全部历史完整 token 序列反复做无界扫描。

数据序列示意（仅解释逻辑，不代表已有可恢复状态）：

```text
第 1 次请求：A + B    → 观察并登记该请求的 token 路径
第 2 次请求：A + C    → 查找 LCP，在合法 token 边界发现共享 A
第 3 次请求：A + D    → 更新 A 的观察计数与最近访问元数据

发现 A ≠ 磁盘存在 A 的快照 ≠ 当前请求复用了 A 的 KV。
```

细则：

1. 公共前缀必须从 token 序列起点开始匹配，不做语义相似度匹配，也不复用非前缀中间片段。
2. 若请求间存在媒体块，分叉点不得落在图片、音频或其他 media placeholder 区间内部；前缀中的媒体身份必须一致，不一致时 LCP 截止于媒体块起点。
3. 跨模型配置、不同 draft/target 兼容签名以及被 LoRA 守卫隔离的序列不得共享目录中的可恢复身份。
4. 接口必须对“精确匹配”“严格前缀”“分叉 LCP”“完全无公共前缀”“同一请求重复到达”给出确定性行为。
5. **同一个请求的 input/output 边界不是两次独立用户请求**，不得用它们的重复观测虚增公共前缀的请求频次。单用户不等于只发生一个请求，但统计必须按请求 ID 去重。
6. 目录容量由候选数、索引 token 总量/字节数联合约束；超限只删除**未承诺持久化数据所有权**的观测元数据，不直接删除磁盘文件。

### 4.3 缓存准入与物理可恢复关联

目录候选应在观察次数、最小 token 数、媒体边界合法性和配置兼容性满足条件后进入 `eligible`；建议新建**独立于** `--cache-disk-min-tokens` 的 `--cache-prefix-min-tokens` 和 `--cache-prefix-observations` 参数。其数值只是可配置门槛，不预先假定某个参数更优。

将目录与现有 RAM/Disk 快照关联时：

1. **精确结束点优先**：仅当缓存状态覆盖该前缀结束位置且具有验证过的可恢复语义，才允许将其登记为 `snapshot_ref`。如果只有 `A+B` 的完整快照，不能自动把 `A` 标为独立 `persisted`；旧磁盘 LCP 恢复能力仍由原 `server_disk_cache::load()` 判断。
2. 按已存在的 `server_disk_cache::load()` 完成候选选择、读取、校验和真实恢复；本阶段索引不绕过 `min_tokens`、恢复成本守卫、LoRA 和媒体检查，也不提前修改 slot 的 KV。
3. RAM 条目释放、磁盘 `drop_entry`、去重、容量淘汰、TTL、CRC 失败、配置签名改变时，必须撤销/刷新相应的物理引用，防止悬空 `snapshot_ref`。
4. 后台 `store/mirror` 入队仅是 `materializing` 或 `queued`，**必须在完成写入并完成清单提交后**才能成为 `persisted`；队列满、取消、写入失败时不能虚报可用。
5. 现有 `covered_cheaply()` 的 4096-token 写入抑制只能减少冗余完整快照写入；不得因为它跳过写入，就把未保存的新前缀边界标为 `persisted`。
6. 本次不替换现有 `server_disk_cache::load()` 的候选选择、LCP 守卫和真实读取路径。目录可作为只读候选提示，但任何新提示都须由原恢复路径再次验证。

### 4.4 前缀专属快照的技术边界

即使目录发现公共前缀 `A`，`llama_state_seq_get_data_ext` 的现有调用也只保证当前 `seq_id` 可序列化，不等于支持导出 `A+B` 里的任意前缀 `A`。**严禁对 `common_state_data`、main/drft 状态流或 checkpoint blob 按 token 推算字节位置后截取。**

如果实际代码检查证明可以在一次正常 prefill 的**合法中间边界**捕获完整且自洽的 target + draft 状态，编码人员只允许先提交独立的技术说明，列出上下文位置、序列生命周期、媒体和混合 memory backend 的保证；该捕获路径作为后续独立修改，不在本次直接开启。不得通过为了命中索引而额外重算整段 prompt 的方式暗中增加推理成本。

## 五、预期数据流变更

```text
请求进入 → token 化 / 媒体身份归一化
         → 当前 slot/RAM/Disk 既有复用流程（保持原语义）
         → 真实 prefill 与生成（保持原计算）
         → 输入/输出安全点发布不可变前缀观察事件
         → prefix-index: LCP 发现 / 合法边界 / 观测去重
         → eligible 候选目录
         → 对照现有已提交快照的可恢复引用（若有）
         → 更新目录状态与可观测计数

后台：store/mirror 成功提交、LRU/TTL/损坏删除、配置失效
         → 通知或刷新 prefix-index 的物理引用
```

不得让后台线程读取可变的 `server_slot`、`llama_context` 或 `server_tokens`；事件数据由推理线程在安全点复制或通过有明确生命周期的不可变引用传递。索引锁不能覆盖完整 KV 序列化、大文件 IO 或模型前向计算。新目录默认仅驻留内存，重启后重新观察或从已校验的磁盘条目恢复可恢复引用；**不得额外发明未校验的持久前缀索引文件格式**。

**训练流水线**：无训练，不变更权重与梯度。**推理流水线**：只增加轻量元数据观测与目录管理，不更改 prefill 切分、`n_past`、采样、推理结果或输出顺序。CPU 与内存开销应在日志中独立归因，不写入预想收益。

## 六、开关与建议接口

建议选项（最终名称以现有参数命名约定为准）：

```text
--cache-prefix-discovery            # 默认关闭；启用候选发现
--cache-prefix-min-tokens N         # 前缀候选的最小 token 长度
--cache-prefix-observations N       # 不同请求的最小观察次数
--cache-prefix-max-candidates N     # 元数据候选数上限
--cache-prefix-max-index-mib N      # 索引内存预算上限
```

开关关闭时不得分配或更新前缀目录，也不得改变旧缓存的查找、保存、拒绝、恢复和统计语义。开启但没有任何 `persisted` 候选时，只能提高候选“发现”信息的可见性，不能把 `observed` 记为命中。该功能不依赖多用户 / tenant ID，不增加 HTTP 用户标识或权限系统。

建议抽象接口（表达语义，供编码人员结合实际结构确定最终签名）：

```cpp
// immutable request observation, not a KV restore command
void observe_request(const prefix_observation & event);

// discovered metadata; never claims physical restore success
std::vector<prefix_candidate> get_common_prefix_candidates(
    const prefix_request_view & request) const;

// only verified storage events can publish/withdraw physical references
void on_snapshot_committed(const verified_snapshot_descriptor & snapshot);
void on_snapshot_invalidated(const snapshot_identity & identity);

// status of the index itself, distinct from actual reuse stats
prefix_index_stats stats() const;
```

不得把 `prefix_candidate` 传入 `llama_state_seq_set_data_ext`；真实恢复只能通过现有经验证的 prompt cache 通路。

## 七、与既有修改的兼容性

- **格式兼容**：保持磁盘条目 **v8**、清单 **v3**；此次不增加新磁盘记录类型，不清空已有磁盘缓存，也不引入需要重建缓存的版本升级。
- **统计兼容**：保留已实现的 `slot/RAM/Disk/prefill` 互斥 token 统计，新增 `prefix_observed_total`、`prefix_eligible_total`、`prefix_persisted_refs`、`prefix_invalidated_refs_total` 等**独立目录指标**。不得把候选 LCP 与实际 reused token 合计。
- **捕获兼容**：现有 `--cache-boundary-capture none|input|output|both` 仍独立生效；新目录只订阅事件或已存在的已验证快照，不能在其关闭时自动强制执行完整状态捕获。
- **去重兼容**：保留 `covered_cheaply()`、`drop_prefixes()` 和 `dedup_prefixes()` 现有动作，但需要在删除前后维护引用的一致性；索引不能改变物理删除次序。
- **模型兼容**：保持 target + DSpine draft、KV 量化、`--kv-unified`、媒体身份、LoRA 保护；不新增通用跨模型 KV 复用假设。
- **平台范围**：首先覆盖现有 Windows/CUDA 单用户单 slot 部署；不为非目标多用户或多 slot 模式编写新功能承诺。任何共享状态仍须遵守已有线程同步。
- **关闭开关**：与锁定 baseline 的同请求比较，应保留当前原路径，不允许因为新目录存在而改变旧的缓存候选、token 复用数量或生成结果。

## 八、验证检查清单（只列检查动作，不预设结果）

### 8.1 源码与版本

- [ ] 阅读实验机器最新 `change.md` 全部相关记录，与当前 `git diff`、HEAD、构建产物核对；确认没有明显未修复的旧问题。
- [ ] 与锁定 baseline 的选槽、提示缓存、磁盘缓存、状态 API 逐项对比；提交差异审查清单。
- [ ] 新增 `server-prefix-index.*` 后构建通过；函数声明、头文件循环依赖及线程所有权清晰。

### 8.2 单用户前缀发现

- [ ] 输入 `A+B`、`A+C`、`A+D`，检查共享 `A` 候选长度、不同请求观测去重和引用状态。
- [ ] 完全重复输入、严格前缀输入、部分分叉、完全不同输入、短于门槛输入，逐一检查逻辑结果。
- [ ] 使用不相同但 hash 碰撞的模拟 key，验证强制逐 token 等价确认。
- [ ] 包含 media 的请求在媒体块边界内不得产生非法可复用前缀；不同 media 身份不得共享物理条目。
- [ ] 单个请求的 input/output 事件不得被统计为两次独立请求。

### 8.3 物理状态与失效

- [ ] 仅有 `observed/eligible` 且无可恢复 KV 时，查询路径不声明 `persisted`、不增加 `reused_disk`。
- [ ] RAM 命中、disk 命中、冷 prefill 场景下，候选 LCP、restored 和真实 reused 保持独立。
- [ ] 物理条目被 `drop_entry`、LRU、TTL、CRC 失败、manifest 失效或去重删除后，引用自动撤销或在查询前完成有效性核验。
- [ ] 写队列饱和、`covered_cheaply()` 抑制、新条目尚未完成写入时，索引不提前报持久化成功。
- [ ] 模型配置签名变化、LoRA 生效、target/draft 不一致时不复用错误状态。

### 8.4 推理与资源

- [ ] `--cache-prefix-discovery` 关闭后，与相同源码/参数的旧路径逐请求比较输出及缓存统计。
- [ ] 开启目录发现后，核查不触发额外的整段 prefill、隐式完整快照序列化或后台强制状态读取。
- [ ] 验证目录内存预算、候选过期/移除及服务退出后释放；检查高频请求下的索引锁竞争。
- [ ] 强杀重启后，磁盘 v8/manifest v3 仍由原通路安全恢复；内存目录可重新建立。
- [ ] 对取消/超时或流中断的请求检查事件清理，不保留无效“已持久化”引用。
- [ ] 保持原有数值一致性检查：冷 prefill 与真实恢复后的输出不因索引逻辑改变。

## 九、实施顺序与交付边界

1. **源码核对**：先阅读最新 `change.md`、核对实验报告的代码版本并完成 baseline 差异审查；存在问题时只处理缺陷。
2. **目录数据结构**：实现有界索引、token/media 精确匹配、观测去重、合法分叉点和纯元数据状态转换；先做无需启动模型的单元测试。
3. **安全事件接入**：在已有单 slot 请求边界发布不可变观察，不触碰 live KV；关闭时完全旁路。
4. **快照引用适配**：对已校验、已提交的既有 RAM/Disk 条目登记可恢复来源；接入失效通知，真实恢复仍由原缓存路径处理。
5. **回归与文档**：完善单用户请求序列测试、资源边界测试；实际实现细节写入 `change.md`，总体结构写入 `projectSummary.md`。

**本次实验的实施终点**是安全、可核查的公共前缀候选目录及其与已有完整快照之间的正确关联。**不包含**“将 `A+B` 的完整 opaque KV 文件直接拆出 `A` 的物理 KV 块”，不包含新的缓存物理格式，也不包含独立的按块写入/读取路径。后续是否实施物理增量 KV，应由底层状态导出、恢复语义的只读技术审查单独确认。

---

# 2026-11-15 19-51 - test1_2

## 一、当前实验的代码审查修复补充

**本记录仅用于修复当前算法实现中已发现的正确性、生命周期与资源边界问题，不创建新的实验方向，不实施分块 KV、不调整缓存命中率策略，也不开展独立公共前缀物理化。** 此处与前文属于同一实验的延续修复，原有实验设计记录全部保留。

**项目约束**：单用户、单实例、单 slot（`-np 1`）；但磁盘 writer 与推理线程仍有并发。必须保留已有 `server_prompt_cache`、`server_disk_cache`、完整状态快照 v8、manifest v3、target/draft 状态和媒体/LoRA 兼容守卫；默认关闭公共前缀目录时必须保留既有推理语义。源码问题修复优先，不允许在本轮同时实现新的磁盘分块、任意 token 前缀状态导出、主动前缀捕获或新的成本感知淘汰算法。

### 1.1 代码版本与核查依据

核查对象：

- 最新仓库 `A-lucky-boy/llama.cpp-dspine-diskCache` 的 `main`，本次检索到的 HEAD 为 `342fd7d4178e75b348682c19e70d64dda618b5db`（实验记录文档提交）。
- 最近功能提交：`8325ae4f463c940f67413b691a3362c39b91a334`，其父提交为 `01d5686333b520f4913631bb69646211819ebe1d`。
- 本次首先阅读仓库 `change.md` 最后的 `2026-11-15 19-35` 记录，并对照 `projectSummary.md` 和 `tools/server/server-prefix-index.*`、`tools/server/server-task.*`、`tools/server/server-context.cpp` 的实现。
- GitHub 对比 `01d5686..8325ae4`：新建约 602 行前缀索引实现及 173 行头文件；`server-task.cpp` 增加缓存条目提交/失效回调，`server-context.cpp` 增加公共前缀观察和启动时物理引用登记；基线状态恢复与 KV 格式未改变。
- 更早的本项目 baseline 为 `eb7681889f44304d2bd5b3e41572f5530af91a1d`，此后的两次功能提交实现了统计/边界捕获与公共前缀目录。**后续修复时须用这两个锁定提交比较增量差异，不得用新版本直接替代 baseline 而省略对照。**

代码地址：

- `https://github.com/A-lucky-boy/llama.cpp-dspine-diskCache/blob/8325ae4f463c940f67413b691a3362c39b91a334/tools/server/server-prefix-index.cpp`
- `https://github.com/A-lucky-boy/llama.cpp-dspine-diskCache/blob/8325ae4f463c940f67413b691a3362c39b91a334/tools/server/server-task.cpp`
- `https://github.com/A-lucky-boy/llama.cpp-dspine-diskCache/blob/8325ae4f463c940f67413b691a3362c39b91a334/tools/server/server-context.cpp`

### 1.2 优先修复的问题与代码证据

**错误 A（已确认，阻断）：容量超限时可能无法退出的循环。**

- 位置：`tools/server/server-prefix-index.cpp` 中 `server_prefix_index::evict_one()`（约 277 行）、`server_prefix_index::note_capacity()`（约 308 行）。
- 现状：当无可淘汰的叶节点时，`evict_one()` 执行 `counters.n_rejected++` 后返回；`note_capacity()` 用 `n_evicted + n_rejected` 是否改变来判定“能否继续”。拒绝计数同样发生变化，因此 `for (;;)` 继续执行，容量条件仍不满足时产生无限循环。
- 可达条件：配置的节点数或索引内存上限低于根节点加上所有受物理引用保护的节点所需空间，或者已被保护的节点路径和驻留媒体元数据造成实际内存预算越界。在单用户环境中仍可触发，不能因正常短测试通过而忽略。
- 修复要求：使 `evict_one()` 返回**是否真的删除一个节点**（`bool` / 枚举），只以“真实释放节点”作为 `note_capacity()` 循环的继续依据；仅在不能释放时增加一次拒绝/预算超限事件，立即返回。将“超过 soft budget、但受持久引用保护”明确记录，而不是把受保护节点强删或继续无限循环。
- 不能以 `n_rejected`、日志数变化或时间判断是否取得淘汰进展；严格确保每轮减少活节点数或退出。

建议伪代码：

```cpp
bool server_prefix_index::evict_one() {
    const int32_t victim = find_evictable_leaf();
    if (victim < 0) return false;
    detach_and_retire(victim);
    counters.n_evicted++;
    return true;
}

void server_prefix_index::note_capacity() {
    while (over_node_limit() || over_memory_limit()) {
        if (!evict_one()) {
            counters.n_rejected++;
            return; // no progress possible: retained physical refs are protected
        }
    }
}
```

**错误 B（已确认，阻断）：删除失败仍扣减磁盘配额。**

- 位置：`tools/server/server-task.cpp` 的 `server_disk_cache::drop_prefixes()`（约 3222 行）、`dedup_prefixes()`（约 3265 行）、`sweep_expired()`（约 3862 行）、`enforce_limit()`（约 3891 行）。
- 现状：这些分支对 `std::filesystem::remove(path, ec)` 设置 `error_code`，但未依赖删除结果，随后继续扣减 `total_size` 或从索引删除文件。相反，`drop_entry()` 已经考虑了删除失败不扣额度。四条路径存在不一致。
- 修复要求：统一封装安全删除与记账，区分“文件已删除”“路径已不存在”“因共享冲突/权限等失败且文件仍存在”，只有确认已释放物理文件时才扣除实际占用。日志包含原因和路径标识；处理不能删除的条目时应明确保留空间占用或将其隔离并记录账面占用，避免继续作为可恢复条目使用和配额无界失真。
- `enforce_limit()` 需要对删除失败条目执行有界跳过/延后重试，不能反复选中同一个失败对象形成另一种循环；不得为了填平容量差额而错误删除受引用保护的其他对象。
- 若 `drop_prefixes()` 因文件占用而删除失败，不应无条件宣布短快照已被成功清理；是否保留短前缀属于后续缓存策略研究，此次只修正文件系统事实与统计语义。
- 保留 manifest 的原子更新、磁盘写队列、签名和 CRC 检查逻辑；禁止因删除失败而对不完整状态进行恢复。

**风险 C（需按交错时序验证，可能造成错误 `persisted`）：提交与失效回调顺序。**

- 位置：`tools/server/server-task.cpp` 的 `server_disk_cache::worker()`（约 3917–4030 行）、`drop_entry()`（约 3156 行）以及其他失效路径；索引的 `on_snapshot_committed` / `on_snapshot_invalidated` 位于 `server-prefix-index.cpp` 约 513 / 554 行。
- 现状：worker 在锁内验证条目仍存在并复制 `committed`，释放磁盘锁后才调用 `on_entry_committed`。`drop_entry()` 在释放锁后调用失效回调。若删除发生在 worker 释放锁和通知之间，两个通知的到达顺序可能与物理状态变化顺序不一致。
- 修复动作：建立明确的**提交/失效事件顺序不变量**。可选实现为 (a) 对所有索引通知统一在磁盘锁保护的状态变更序列中同步触发，同时证明索引绝不反向获取磁盘锁；或 (b) 在磁盘锁内分配单调 epoch、记录有序事件，以单一串行分发器交付，前缀目录拒绝比当前引用更旧的事件。最终只选一种，避免同步/异步两套途径并行。
- 不得简单把 `on_entry_committed` 放到锁外后再做一次无锁 `entries` 查找；也不得让回调内部再调用 `entries_snapshot()` 形成锁反转。切换回调策略时同时审查析构先后顺序，防止目录析构后后台 writer 仍触发闭包。
- 记录一个可控交错测试：提交完成但尚未通知 → 同路径失效 → 迟到提交通知。测试过程中真实磁盘条目已不存在时，目录不得最终把它登记为 `persisted`。

**风险 D（索引元数据完整性与资源控制）：无效 media / 索引驻留开销。**

- 位置：`server-prefix-index.cpp` 的 `to_units()`（约 51 行）、`intern_media()`（约 38 行）、`candidates()`（约 467 行）、`approx_bytes()`（约 257 行）。
- 现状：media 三组数组长度不一致时，`to_units()` 直接返回未携带媒体身份的 token 序列；未认识的 media ID 会在只读 `candidates()` 中被插入驻留表；内存预算估算没有完整计入所有容器的占用，且只读候选查询不会主动执行预算处理。
- 修复要求：media 元数据不合法时 fail-closed，不能降级成纯 token 目录查询；`candidates()` 只查已有媒体身份，未知身份应得不到错误共享且不增长驻留表；`observe()` 的媒体驻留必须受索引容量预算约束。`approx_bytes()` 至少覆盖 media 字符串及 map、观测去重表、快照引用表与主节点表的大宗内存。现有下限/上限选项和默认关闭语义不变。
- 对截断媒体块、重复/重叠 chunk 索引、空媒体 ID、超出 token 长度的范围建立可控失败路径。**这些候选错误不会直接绕过现有 Disk KV 校验，但不能因为只作用于观察层就容忍错误的 `eligible/persisted` 元数据。**

**风险 E（查找候选状态与前缀端点的一致性）：媒体钳制。**

- 位置：`server-prefix-index.cpp` 的 `clamp_candidates()`（约 365 行）。
- 现状：如果候选终点被钳制到媒体块起点，`prefix_tokens` 被缩短，但 `state` 和 `snapshot_path` 原样保留；如果原候选为 `persisted`，缩短的前缀不再与物理快照的精确端点一致。
- 修复要求：一旦候选边界发生变化，必须重新计算所对应端点的合法状态和物理引用，或清除 `snapshot_path` 并降为仅可观测候选。不得将较短的钳制前缀继续宣称为原快照的精确可恢复前缀。

## 二、预期修改文件

| 文件 | 修改目标 |
|---|---|
| `tools/server/server-prefix-index.h` | 为淘汰操作定义明确返回语义；如选择事件 epoch 方案则增加引用版本；将无效 media 拒绝作为明确 API 结果 |
| `tools/server/server-prefix-index.cpp` | 修复 `note_capacity/evict_one` 的终止性，完善索引预算，修复 `to_units/intern_media/candidates` 的只读性及 `clamp_candidates` 的状态一致性，接入有序快照引用事件 |
| `tools/server/server-task.h` | 抽象统一条目删除结果与回调顺序所需的事件元数据；无必要不得新增盘层格式字段 |
| `tools/server/server-task.cpp` | 统一删除失败语义与 quota 记账；落实条目提交/删除通知的线性化点，检查 `worker/drop_entry/drop_prefixes/dedup_prefixes/sweep_expired/enforce_limit` |
| `tools/server/server-context.cpp` | 核对索引/磁盘层对象生命周期，保持构造/析构和回调注销次序；只有在需要适配新事件接口时修改 |
| `tools/server/tests/unit/test_cache_prefix_index.cpp` | 扩充容量不可淘汰、只读媒体查询、格式错误媒体、候选状态修正、事件逆序等纯 CPU 测试 |
| `tools/server/tests/unit/` 或 `test-run/` | 增加跨文件删除失败注入、后台提交/失效交错、实际磁盘空间计数的回归脚本 |
| `change.md` | 逐文件记录实际修复动作、版本差异和验证事实 |
| `projectSummary.md` | 更新全项目的生命周期与资源不变量；不放本轮实验结果报告 |

保持 `common/common.h`、`common/arg.cpp`、CMake、新增缓存开关及磁盘条目 v8 / manifest v3 **不变**，除非修复的 ABI/接口改动确实需要同步；任何变动都必须解释其必要性。

## 三、实施顺序（严格按现有正确性修复划分）

### 第 1 步：容量控制终止性

1. 锁定 HEAD，备份当前索引模块并记录 baseline 的原有代码路径（baseline 无当前 radix-tree 索引）。
2. `evict_one()` 仅以真实删除作为“成功”，不能把拒绝计数当作执行进度；修正 `note_capacity()`。
3. 构造“仅存在持久化引用且超出 node/byte 预算”场景；函数必须返回，索引节点仍可用于后续查询，文件引用不得被强行驱逐。
4. 对不同的容量限制与节点生命周期执行边界回归，核查活节点计数和已引用节点不变量。

### 第 2 步：删除返回值与配额账本

1. 建立 `remove_result`（或复用现有结果类型）区分 `removed/already_missing/failed`；统一 `total_size` 修改入口。
2. 将所有删除路径改为以结果驱动索引及 manifest 更新，出现删除失败必须解释条目是否仍可查询、是否计入物理容量、是否进入后续清理队列。
3. 验证同一文件反复尝试删除、Windows 共享冲突、文件提前消失、无法更新 manifest 等边界，不允许 silent quota mismatch。
4. 保留当前文件格式、恢复校验与 CRC 行为，不增加新缓存对象格式。

### 第 3 步：物理引用通知顺序

1. 在盘层锁保护下定义提交与失效的唯一排序点。分析 `worker()`、`drop_entry()`、`drop_prefixes()`、`sweep_expired()`、`enforce_limit()` 以及关机/初始化回调注册的全链路。
2. 完成有序事件派发，回调不得在错误时序下把已经删除的条目重新标记为 `persisted`。
3. 增加确定性并发交错测试；仅覆盖磁盘 worker 与推理线程这两类实际运行中的并发，不新增多用户/多 slot 功能。
4. 确保 `entries_snapshot()` 的返回内容、manifest 可见条目与目录最终物理引用可对应。

### 第 4 步：索引输入、媒体边界和内存约束

1. 对 `to_units()` 增加输入合法性检查和失败返回，媒体数组不一致、区间越界/重叠等不能 silently fallback 为无媒体序列。
2. 只读目录查找不新增 `media_intern` 条目；新增身份时必须满足内存预算或采取安全的拒绝路径。
3. 修正 `clamp_candidates()`：调整端点后重新验证 `persisted` 的精确对应关系，否则清空引用。
4. 核对 `approx_bytes()` 覆盖主要驻留对象；不允许小容量参数下出现无界增长、死循环或永久保留无引用垃圾。
5. 检查 `insert/compact` 在父/子节点和快照引用重映射时没有悬挂索引；必要时增补单元测试。

### 第 5 步：单用户回归、变更与总结

1. 执行不加载模型的前缀索引单元测试：精确分叉、请求去重、媒体不一致、容量保护、候选钳制、引用失效、并发交错。
2. 使用当前真实模型配置执行单用户、单 slot 请求序列，核查冷启动、RAM/slot/Disk 真实复用、重启后的可恢复性、文件损坏安全回退与文本数值一致性。
3. 使用 `--no-cache-prefix-discovery` 完成改造开关关闭态对照；不因修复元数据/磁盘删除而改变模型计算、`n_past` 或采样路径。
4. 将实际变更、发生的错误及修复事实写入 `change.md`；全项目架构与已落地能力更新到 `projectSummary.md`。

## 四、预期数据流变更

**当前已存在的数据流：**

```text
用户单请求 → tokenizer → slot/RAM/Disk 真实复用 → prefill/generate
          → 输入边界的不可变观察 → prefix index
后台 writer → 完整快照写入 → manifest → 物理条目提交通知 → prefix index
条目删除/TTL/配额淘汰 → 失效通知 → prefix index
```

**本轮修复后的要求：**

```text
prefix.observe / candidates
  → media 输入校验 → 只读/有界身份查找 → LCP 候选
  → 物理引用与前缀端点复核 → 有界状态更新 → 返回

prefix.note_capacity
  → 检查上限 → 尝试真实淘汰
  ├─ 成功释放一个节点 → 继续检查
  └─ 无可删节点 → 记录拒绝并返回（不无限循环）

writer / deletion
  → 在 disk lock 内定义条目状态变更与通知顺序
  → 根据真实文件删除结果修改 bytes/index/manifest
  → 前缀目录按有序事件更新 persisted/unavailable
```

**训练流水线**：不涉及训练，模型参数与训练过程不变。**推理流水线**：保留真实缓存查找和 KV 恢复路径，仅修改元数据的有效性、物理引用顺序与删除/配额错误处理。不能把候选 LCP 转成实际 reused token，不得新增未经支持的部分状态导出操作。

## 五、与现有实现的兼容性

1. **关闭态**：`--no-cache-prefix-discovery` 不应初始化新的前缀索引，真实 KV 复用、推理输出和日志中的实际 token 分解口径保持一致。通用磁盘删除语义修复可以影响异常情况下的磁盘账本，但不得改变健康路径的输出。
2. **格式**：继续使用 v8 完整状态快照和 v3 manifest；不动头部 offset、CRC64、签名、target/draft blob 和 checkpoint 数据。
3. **模型**：保持 target Qwen 与 DSpine draft 的独立序列状态校验；不修改投机解码接入、温度/采样、GPU 层分配及 `--kv-unified` 行为。
4. **单用户**：不实现用户 ID、多租户 namespace、不同用户 cache 共享、多 slot 抢占调度；worker 与推理线程仍必须严格同步。
5. **资源**：容量预算中索引元数据不等于磁盘占用；磁盘删除失败不能错误降低实际占用；目录节点数与驻留媒体身份数应各自可约束。
6. **回退**：若修复引入新事件接口，任何无法确认的物理引用应降为未持久化元数据；真正恢复只能走现有经过 CRC 验证的 `server_disk_cache::load()`。

## 六、验证检查清单

### 6.1 版本与 baseline

- [ ] 确认 `change.md` 最新记录、当前 HEAD 和本轮拟修改文件一致。
- [ ] 对照锁定 baseline 的选槽/缓存恢复路径，检查修复没有引入不必要的 KV 修改。
- [ ] 检查提交差异仅涉及现有缺陷相关逻辑，无新的性能优化策略混入。

### 6.2 确定的错误修复

- [ ] `note_capacity()` 在超过上限且所有剩余叶节点都受物理引用保护时能正常返回，不增殖无限拒绝事件。
- [ ] `evict_one()` 的返回值只表示实际节点淘汰，不把拒绝视为成功。
- [ ] `drop_prefixes()`、`dedup_prefixes()`、`sweep_expired()`、`enforce_limit()` 在删除失败时不虚假扣减磁盘字节。
- [ ] 清理失败、配额仍超限时不会在同一不可删条目上形成无限循环。

### 6.3 生命周期及输入数据

- [ ] 物理条目已经删除后，较晚执行的提交回调不能恢复其 `persisted` 状态。
- [ ] 回调释放与析构顺序不会产生对已销毁 `prefix_index` 的访问。
- [ ] 无效媒体数组、媒体位置重叠、媒体块内部截断、未知媒体 ID 不会形成假等价或错误的持久化引用。
- [ ] `candidates()` 只读查询不无界增加媒体驻留表；内存预算检查覆盖主要索引内存。
- [ ] 候选长度被钳制后不保留与原长度不匹配的 `snapshot_path`。
- [ ] 引用在 compact、删除、TTL、重启重建后仍可对齐正确物理文件。

### 6.4 单用户回归

- [ ] `-np 1` 下的重复请求和分叉共享候选在关闭/开启索引时均保持原有生成语义。
- [ ] 实际复用 token 分解仍满足 `prompt = reused_slot + reused_ram + reused_disk + prefill`。
- [ ] 未持久化的 `observed/eligible` 不被算入真实命中；CRC 失败不会产生虚假 `persisted`。
- [ ] 强杀后重启由已验证磁盘条目恢复目录引用，未写入不可信的额外磁盘索引文件。
- [ ] 文件删除失败、损坏、异常通知与关闭索引的组合均能安全退回正确 prefill。

## 七、交接边界

本补充仅授权**修复现有目录的终止性、索引输入一致性、物理引用时序，以及磁盘删除/空间账本问题**。不授权修改 KV 物理序列化格式、不授权直接切分 opaque state、不授权为提高表面命中数放松 `io_cost`/媒体/CRC 约束。

本轮完成后，开发人员需要提供：更新的 `change.md`、实际修复 commit、单用户回归日志及更新后的 `projectSummary.md`，供下一次源码审查使用；这项交接要求不构成新的性能实验规划。

---

# 2026-11-15 20-31 - test1_2

## 一、本轮定位：修复磁盘缓存的错误覆盖与错误去重

本轮是已有单用户提示缓存实现的**缺陷修复补充**，不是新算法实验。不得在现有缓存写入/替代逻辑存在明显缺陷时同时开发主动公共前缀捕获、分块 KV、KV 增量序列化或改变缓存成本阈值。

### 1.1 已核对的代码版本与依据

- 目标仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`，本轮审查时 `main` HEAD `bc0c9c97414022c30fe72f7d363ca2ed9350f888`（文档追加提交），实际修复代码提交 `57d9fce6402a39deff5328350a1af175878f68bc`。
- 在提出本轮任务前，已读取 `change.md` 最新的 `2026-11-15 20-22 - Change Record`、`projectSummary.md` 和《2026-11-15-delivery-for-experiment-planning.md》，并核对 `8325ae4 → 57d9fce` 的提交文件差异。
- 已核查 `tools/server/server-task.cpp` 的 `load()`、`covered_cheaply()`、`store()`、`mirror()`、`worker()`、`drop_prefixes()`、`dedup_prefixes()`；`tools/server/server-prefix-index.cpp` 的终止性修复；`tools/server/server-context.cpp` 的启动登记；与改造前 baseline 提交 `eb7681889f44304d2bd5b3e41572f5530af91a1d` 及当前修复前提交 `8325ae4` 进行能力/变更对比。
- 修复提交已建立 `quarantined` 状态，且 `load()` 的候选选择排除该状态。但在其他重用判定路径中仍未一致排除。
- 锁定原有缓存条目格式 **v8**、manifest **v3**、CRC64、target/draft 状态校验、恢复失败回退语义、单用户单 slot `-np 1` 作为兼容前提；本轮不得更改这些协议或修改模型计算。

上述版本和代码位置是本轮缺陷位置定位依据。编码人员开始修改前须再次读取实验机的最新 `change.md`、本地 HEAD、`git status` 和构建产物版本；如本地又有新提交，须先逐行核对相应函数是否已经改变，避免覆盖他人修复。

### 1.2 已确认问题 A：不可恢复条目仍可能阻止有效写入

**错误位置**：`tools/server/server-task.cpp`

1. `server_disk_cache::load()` 的候选循环显式跳过 `e.quarantined`。
2. `server_disk_cache::covered_cheaply()` 遍历 `entries` 时没有 `e.quarantined` 过滤。`store()`、`mirror()` 均调用该函数。
3. `server_disk_cache::store()` / `mirror()` 的“`hash` 相同且 `tokens.size()` 相同即已保存”条件未排除 `quarantined`，也未核对完整 token + media 身份。
4. `server_disk_cache::worker()` 写盘前的“存在更长前缀就跳过”判断没有排除 `quarantined`。

因此，条目可同时满足“加载时不可用”与“写入时被认为已覆盖请求”两种矛盾状态。该错误与 `quarantined` 的新生命周期有关，属于必须优先修复的旧代码/新状态兼容性缺陷。

**修复要求**：

- 定义统一的 `is_reusable_cache_entry(e)` 或等价条件：至少要求条目**未隔离、仍被索引视为可查找、配置身份有效**。该函数仅表示候选有效，不免除 `load()` 的文件校验。
- `covered_cheaply()`、`store()`、`mirror()`、`worker()` 的每条“已有条目可替代新条目”路径统一使用候选有效性判断。
- 已隔离条目仍留在磁盘账本并占用配额；不得因排除其写入抑制作用而调用 `remove_file()` 强行删除、提前扣减 `total_size` 或恢复其 `persisted` 引用。
- 如果可写配额已被不可删除文件占满，新条目应通过原队列/空间限制返回明确拒绝；**不能以“已有可用缓存”伪装为去重成功**。
- 变更须保留磁盘锁内的 `notify_committed/notify_invalidated` 顺序与 `epoch` 语义，不增加独立异步通知通道。

### 1.3 已确认问题 B：“更长快照替代短快照”并不总是成立

**错误位置**：`tools/server/server-task.cpp` 的 `worker()`、`drop_prefixes()`、`dedup_prefixes()`、`covered_cheaply()`。

当前几条写入/删除路径主要依赖纯 token 前缀关系来判断短快照冗余，但真实 `load()` 在执行恢复前还会检查：

- 最小可用匹配长度；
- token 前缀下的双向媒体身份、媒体区间和合法边界；
- 条目是否被隔离；
- `e.size_file > lcp * DISK_CACHE_MAX_BYTES_PER_TOKEN` 的读盘成本守卫；
- 最终仍需要通过 v8 文件校验以及 target/draft 状态恢复。

因此，“A 的 tokens 是 B 的严格前缀”并不充分证明“B 可以取代 A 服务未来对 A 的请求”。现有实验材料记录过短快照被删除后，长快照因 `io_cost` 不可恢复的真实情况；本轮应修复这一可替代性判定，而不是放宽成本守卫。

**修复要求**：

1. 提取一个**不执行 KV 读取、不修改状态**的保守复用判定辅助逻辑，专供写入抑制、worker 冗余判断以及短快照去重使用；其判定规则必须与真实 `load()` 的确定性前置过滤条件一致。
2. 对候选请求的有效 LCP 做**双向媒体校验**：已有和待保存条目的媒体元组（位置、长度、ID）在可用前缀范围内必须等价；候选结束位置不能在媒体块内部。禁止仅验证“短条目的媒体是长条目的子集”。
3. 对“长条目完全覆盖短请求”情形，确认该长条目的文件字节成本可通过现有阈值，并且 LCP 满足实际最小长度。仅比较 token 长度不构成替代证明。
4. `covered_cheaply()` 保留原 4096-token 尾部容忍语义，但将签名补充为接受待写 prompt 的媒体元数据；对于已有缓存能覆盖的那一段，必须先通过相同的媒体和可恢复成本约束。
5. `drop_prefixes()` 与启动期 `dedup_prefixes()` 在不能证明较长条目可替代较短条目时，应保留短条目；不引入复杂的新收益函数，也不把阈值调大来实现去重。
6. 若冗余关系成立，仍必须先按现有文件删除原语进行操作，删除成功之后才能扣减实际字节并同步撤销前缀引用；删除失败不改配额账本。
7. 若某一策略需要比较 `pending` 与 `entry` 的 metadata，必须使用已经由 `disk_cache_pack()` 产生并验证的 token/media 元组，不能在 worker 线程重新读取 live `server_slot` 或未同步的推理状态。

可参考的职责抽象：

```cpp
struct cache_prefix_match {
    size_t token_lcp = 0;
    size_t usable_lcp = 0;  // after bidirectional media/boundary checks
    bool   media_compatible = false;
};

cache_prefix_match match_restorable_prefix(
    const cache_prompt_identity & lhs,
    const cache_prompt_identity & rhs);

bool eligible_as_existing_replacement(
    const server_disk_cache_entry & existing,
    const cache_prompt_identity & wanted,
    size_t required_usable_tokens);
```

上述代码仅为接口意图，实际应沿用项目既有命名、锁约束和 `disk_cache_*` 工具函数。**不能将静态可替代性判断误当作文件 CRC 已重新验证**，真实 `load()` 仍需做完整校验。

### 1.4 已确认问题 C：hash + token 数不足以证明“同一缓存条目”

**错误位置**：`store()`、`mirror()` 两处使用 `e.hash == hash && e.tokens.size() == pending.ids.size()` 的去重判断。

该判断没有区分不可恢复状态，也没有对完整 token 与 media 元组做等价确认。哈希只应作为索引加速条件，不能直接作为输入身份的唯一证明。

**修复要求**：

- 先跳过 `quarantined`；hash/长度相同后，再逐 token 比较 `ids`，并逐项比较 `media_indices/media_token_counts/media_ids`。
- 发生 hash 冲突但元数据不同时，不能把新条目计为 `dedup`；由于现有文件名依赖 hash，本轮应先安全拒绝此冲突并记录明确原因，禁止无版本规划地覆写原文件。
- 对真正的完全相同条目，保持现有去重行为和计数，不增加不必要的序列化。

## 二、预期修改文件

| 文件 | 修改位置 | 变更内容 |
|---|---|---|
| `tools/server/server-task.h` | 磁盘条目和辅助函数声明 | 增加/抽取纯元数据身份校验与复用资格判断；不更改格式 v8 |
| `tools/server/server-task.cpp` | `covered_cheaply()` | 检查 `quarantined`、媒体身份、有效 LCP 及 IO 守卫；保持尾部容忍 |
| `tools/server/server-task.cpp` | `store()`、`mirror()` | 对精确去重先过滤不可用条目，再核对 token/media 全量身份 |
| `tools/server/server-task.cpp` | `worker()` | 修复“较长缓存存在就跳过”逻辑，排除不可恢复和无法经济替代的条目 |
| `tools/server/server-task.cpp` | `drop_prefixes()`、`dedup_prefixes()` | 仅在被保留的长快照实际可服务短请求时删除短快照；继续调用现有删除原语 |
| `tools/server/server-task.cpp` | `load()` 辅助判断区域 | 复用同一媒体前缀判定规则，避免两个路径出现不同语义；不改变 KV 恢复/CRC 过程 |
| `tools/server/tests/unit/` | 新增/扩展独立单元测试 | 验证隔离、双向媒体、成本守卫、真实冗余、hash 冲突分支 |
| `test-run/` | 回归脚本与日志工具 | 构造单用户、单 slot 的条目隔离、前缀分叉和重启场景 |
| `change.md` | 追加 | 记录实际缺陷、代码位置和修复过程 |
| `projectSummary.md` | 更新既有对应章节 | 总结已落地后的缓存可替代性语义，不写规划或未实现承诺 |

不要为了这轮修复引入新的缓存前缀对象文件格式、跨 slot 状态、后端 KV API、线程池或额外用户隔离逻辑。

## 三、算法与数据流修改

### 3.1 在所有写入抑制点共用相同的“物理可替代”定义

**修改前**：

```text
Store/Mirror/Worker 接收准备写入的 P
  → entries 中有 token prefix / 相同 hash
  → 直接认为已覆盖或已保存
  → 跳过新快照

但 Load 查询时：
  → e.quarantined / media_mismatch / io_cost
  → 无法使用该缓存
```

**修改后**：

```text
Store/Mirror/Worker 接收 P 与 packed metadata
  → 检查候选条目状态（非 quarantined）
  → 按全 token + media 比较真实前缀
  → 钳制到合法媒体边界
  → 用现有 min_tokens / io_cost 判断是否可恢复
  → 只有能替代 P 的条目才允许抑制写入
  → 其余情况继续原有写入/预算路径

Load：
  → 使用相同的静态前缀匹配规则
  → 文件完整性校验与真正的 KV 恢复维持原样
```

此处不增加新的命中率奖励、不调整 `--cache-disk-min-tokens`、不修改 `DISK_CACHE_MAX_BYTES_PER_TOKEN`、不改变 `DISK_CACHE_MAX_PREFIX_LAG_TOKENS`。

### 3.2 快照删除与引用通知

```text
删除短快照之前
  → 判断长快照是否确实可替代短快照
     ├─ 否：保留短快照，物理引用与配额不变
     └─ 是：调用 remove_file()
             ├─ 成功/已不存在：更新 entries/total_size
             │                 → notify_invalidated(epoch) → manifest
             └─ 失败：保留账面字节
                       → 按原有错误/隔离语义处置，不伪造已删除
```

对一个**仍然有效、仅因文件被占用而删除失败**的短快照，保留其有效条目和引用；对于已因 CRC/恢复问题被明确判定无效的条目，则遵循现有隔离流程。不能混淆“删除失败”和“缓存数据损坏”。

### 3.3 与单用户环境的关系

继续只支持单用户、单实例、`-np 1`。单用户不消除后台 worker 和前台请求线程的同步需要；所有 `entries` 生命周期判断必须遵守原 `mtx`，磁盘锁内的单一通知通道与 `event_epoch` 保留。新 helper 不得在持有 disk lock 时反向获取会访问 disk 的其他锁。

**训练流水线**：无训练，完全不变。**推理流水线**：仅修改缓存是否入队、是否被认为冗余、何时删除；不修改模型计算图、解码、draft/target 选择、logits 或 n_past。现有 v8 快照的真正读取、CRC 校验和 KV 恢复仍由 `server_disk_cache::load()` 处理。

## 四、与既有修改的兼容性

1. **Baseline 行为**：在没有 quarantined 条目、媒体身份相同且候选确实满足真实加载成本守卫时，保留原有缓存查找与去重语义。不要为简单的无媒体完全重复请求引入新的差异。
2. **关闭态**：`--no-cache-prefix-discovery` 时仍需修复磁盘共用路径；修复不依赖前缀目录是否开启。不能错误地将通用磁盘层缺陷掩盖在 prefix 开关之下。
3. **前缀目录**：`server-prefix-index.*` 仍是只读元数据目录；不参与物理候选挑选，也不把 `eligible` 当命中。真正快照被删除才撤销引用。
4. **写盘与配额**：保持 `remove_file()` 真实删除结果驱动账本更新、quarantined 仍占空间、受保护引用与单调 `epoch`；不允许为了提高命中写入而突破磁盘配额。
5. **文件格式**：v8 条目、v3 manifest、CRC 覆盖范围、配置签名、target/draft/checkpoint 状态不变。
6. **性能守卫**：`768 KiB/token` 和 4096-token lag 两项现有常量保持原值；本轮只修正其使用前的候选有效性。
7. **模型/媒体安全**：不能放松 LoRA、mmproj、媒体块边界、媒体 ID/长度、KV 量化与 context 签名的原隔离要求。
8. **回退**：任何不确定或元数据不一致的候选不能用于替代性判断；仍可以走原正常的写盘/空间限制路径，不能借此要求新模型 KV 恢复。

## 五、验证检查清单

### 5.1 版本与静态核对

- [ ] 先检查实验机 `change.md` 的最新记录、HEAD、工作树状态和当前运行二进制。
- [ ] 将本轮变更与改造前 baseline、修复前提交分别比较，确认修改范围只覆盖缓存状态/复用判定与相应单元测试。
- [ ] 逐个检查 `store`、`mirror`、`covered_cheaply`、`worker`、`drop_prefixes`、`dedup_prefixes` 的候选有效性是否一致。

### 5.2 隔离条目不可阻止有效写入

- [ ] 构造已存在、与待写请求完全相同但 `quarantined=true` 的条目；分别经过 `store()`、`mirror()`，检查不会被错误统计为已有可用快照。
- [ ] 构造 `quarantined` 长条目与待写短前缀；检查 `worker()` 不因长条目的纯 token 覆盖而丢弃待写任务。
- [ ] 配额被隔离条目占满时检查 `cache_disk_bytes`、`cache_disk_quarantined`、队列拒绝/去重原因；不得将账面占用抹除或强行突破预算。
- [ ] 模拟隔离条目的删除失败/句柄释放/后续回收，并检查通知、配额与磁盘索引一致。

### 5.3 长短快照的真实可替代性

- [ ] 准备合法的长/短条目，二者 token 前缀完全相同，但长条目恢复短前缀会触发 `io_cost`；检查短快照不会因纯前缀关系被错误抑制或删除。
- [ ] 准备长条目能够通过现有静态恢复门槛的情况，检查去重判断仍走原正常路径。
- [ ] 检查 `drop_prefixes` 与 `dedup_prefixes` 两条删除路径使用完全一致的前缀身份和成本约束。
- [ ] 通过原 `load()` 再次对可替代与不可替代的条目做实际复用归属检查，候选 LCP、restored 和 reused 不得混算。

### 5.4 媒体与精确身份

- [ ] 使用相同 token placeholder、不同媒体 ID 的两段请求，检查 `worker`、`covered_cheaply`、`drop_prefixes` 不能互相错误覆盖。
- [ ] 在较长条目中加入位于短条目前缀内部的媒体块，验证短条目仍能保留其独立状态。
- [ ] 相同媒体 ID 但 token count 不同、媒体块结束位置不同、media 三元组不一致的候选全部采用保守处理。
- [ ] 同 hash/同 token 长度但不同 token 或 media 的构造测试不得进入 `dedup` 分支；不允许覆盖现有文件。

### 5.5 端到端单用户回归

- [ ] Windows/CUDA 单用户 `-np 1`：冷启动、完全重复、追加、公共前缀分叉、输入/输出边界、强杀重启、文件删除失败后恢复。
- [ ] 现有 13 组 `test_cache_prefix_index.cpp` 自检仍能够执行；本轮磁盘层规则新增独立测试，不复用仅验证 radix tree 的断言代替缓存写入验证。
- [ ] `--cache-prefix-discovery` 启用和禁用两种路径：检查 `prompt = reused_slot + reused_ram + reused_disk + prefill` 恒等式，以及输出与同配置冷 prefill 的一致性。
- [ ] 删除失败后重试的日志必须能区分 `dedup`、写入队列满、配额限制及条目被隔离，不应把这四种事件混为一个原因。
- [ ] 验证所有修复不引入 KV 字节切片、修改现有缓存格式或绕过 CRC。
- [ ] 检查取消/中断时遗留的写入任务仍不会把已隔离条目当作有效替代来源；只覆盖单用户与后台 IO 并发，不扩展至多 slot。

## 六、交付与记录规范

- `change.md`：只记录实验人员真实修改的文件、函数、错误位置、实际构建验证情况，以及和 baseline 的差异；不得预填未执行的结果。
- `projectSummary.md`：仅更新整个项目已实现的缓存结构与“可替代条目”的一致性约束。
- 本文件保持为实验规划和算法修改日志，不收录未来收益预测、测试数值估计、性能计算报告、早停条件或最终性能优劣判定。
- 此修复完成前，不授权新缓存文件格式、新 KV 分块物理对象或新的前缀自动捕获算法实现。

---

# 2026-11-15 20-53 - test1_2

## 一、实验性质、前置核验与目标

**性质：对当前缓存可替代性实现进行缺陷修复，不启动新的算法实验。** 保留现有全部实验设计记录和代码改进；本轮仅校正媒体前缀匹配、长短快照替代的真实字节代价，以及内容哈希文件名发生冲突或已被隔离文件占用时的写入安全。此任务限定为单用户、单实例、单 slot（`-np 1`），不涉及多租户、分布式或多 slot 调度。

**已核对依据**：在规划前检查仓库 `change.md` 最新 `2026-11-15 20-47` 条目、`projectSummary.md`、本次交付的 `2026-11-15-replacement-semantics-change.md`，以及远程实际实现。当前审阅时 GitHub `main` 为 `6f9b0decd42e2d19368fe83d0f48acf0067b4986`（文档提交），可替代性算法代码在 `0c289e7044f2f9dc18876d7c1a13bd25c9f3e18e`。已将该代码与修复前 `bc0c9c97414022c30fe72f7d363ca2ed9350f888` 以及上游能力改造前的仓库 baseline `eb7681889f44304d2bd5b3e41572f5530af91a1d` 进行提交文件差异检查。特别查看 `tools/server/server-disk-cache-match.cpp/.h`、`tools/server/server-task.cpp`、`tools/server/tests/unit/test_disk_cache_match.cpp` 和 `src/llama-context.cpp` 的序列状态 API。

已确认新匹配模块让写入抑制/去重与 `load()` 的前置筛选共用元数据逻辑，且隔离条目已被过滤；**此前报告的主要修复保留**。但是，当前实现另有以下三处可由源码直接定位的逻辑风险，必须先补齐，才能进一步规划新的物理缓存实验。静态发现不代表已完成实验机动态复现；实验人员应先按下列独立单测构造复现，然后修复。

### 问题 A：双向媒体匹配使用短路 `||`，可能未检查更早的另一侧差异

**代码位置**：`tools/server/server-disk-cache-match.cpp` → `disk_cache_match_prefix()`：

```cpp
if (!media_side_ok(a, b, res.usable_lcp, &clamp_to) ||
        !media_side_ok(b, a, res.usable_lcp, &clamp_to)) {
    res.usable_lcp = clamp_to;
}
```

这里的 `||` 具有 C++ 短路求值语义。若第一方向发现不一致，第二方向不会执行；而第二方向可能存在更靠前的差异，当前 `clamp_to` 便不一定是最早的媒体不一致位置。源代码声称进行“双向比较”，但控制流不能保证双向都被扫描。因 `load()` 也使用该结果选择可恢复前缀，本问题优先于任何性能优化。

**必须实现**：

1. 将两方向检查分别计算，并把结果取逻辑与；无论先前方向是否发现不一致，都执行另一个方向。保留同一个 `clamp_to` 作为最小差异位置，或让两个方向分别返回位置再取 `min`。
2. 把媒体元数据完整性核验与身份比较分开。至少要求三数组形状一致；在 helper 接受的元数据范围内，区间合法、顺序合法、不重叠、数量非零、ID 非空。对无法验证的输入应 fail-closed（可用前缀 0），不能仅因为数组长度一致便信任其结构。
3. 不允许将媒体身份不同的 token 区间加入可用前缀；媒体分叉边界如落在块内部须钳制到块起点。对于来自真实 `server_tokens` 的正常输入，保留此前相同身份可命中的语义。
4. 更改应集中于纯元数据 helper；`read_entry()` 的 CRC 校验与 `llama_state_seq_set_data_ext` 的调用顺序完全不变。

**确定性单元测试构造**（不加载模型）：构造同一 token ID 序列、有效的非重叠媒体描述，A 的媒体块在偏后位置（如 12），B 的块在更早位置（如 4），两方向的不匹配位置不同；分别测试 `match(A,B)` 与 `match(B,A)`，确保 `usable_lcp` 均不超过**最早**不匹配的媒体起点，不能依赖参数顺序。再增加多块、同一媒体 ID 但 token_count 不同、块区间部分覆盖、元数据越界/重叠/乱序及空 ID 的输入测试。构造测试时应同时检验源 token placeholder 约束，明确区分“helper 的完整输入域”与“当前模型实际可产生的 prompt”。

### 问题 B：删除短快照时，长快照的恢复成本采用估算字节而非实际文件大小

**代码位置**：`tools/server/server-task.cpp` → `server_disk_cache::worker()`：`write_entry(pending, path, size_file)` 已经得到写入后**真实 `size_file`**，但后续调用 `drop_prefixes(..., bytes)`；其中 `bytes = pending.bytes`。`drop_prefixes()` 随后把该估算值写入 `longer.file_bytes` 并交由 `disk_cache_can_replace()`，而真正的 `load()` 用 `e.size_file` 进入成本守卫。

这不满足“替代性判断必须与实际 load 成本门槛一致”的不变量。哪怕估算与实际只差文件头/packed 元数据的几 KiB，只要位于阈值附近，仍可能删除唯一可以经济恢复的短快照。

**必须实现**：

1. `worker()` 必须向 `drop_prefixes()` 传递当前已成功写入的**真实 `size_file`**，不能传 `pending.bytes`。函数参数命名应改为 `committed_file_bytes` 或等价名称，清楚指示来源。
2. 如果当前新写文件在 `enforce_limit()` 中被立即淘汰，则不能以它作为替代依据继续移除短快照。应调整提交/去重/配额流程，保证先确认长快照实际存活，再做可能破坏短快照的删除；或者为配额场景保守保留短快照。文件成功落地不等同于它已被最终 manifest 接纳。
3. 当被保留的长条目来自 `entries`，统一使用 `e.size_file`；当新长条目来自后台写盘结果，统一使用写完后的 `size_file`。写前估算仅允许用于排队、RAM pinned memory 和预配额预算，不允许决定现存可用快照是否应删除。
4. 执行任何实际文件删除前，还必须核对新长条目未被隔离、未发生 hash 文件名冲突，并且将作为可查询的有效条目存在。

**确定性边界测试**：构造短前缀长度为 `L`，长快照 `pending.bytes` 不超过 `L × 768 KiB`、但实际文件大小 `size_file` 超过该值的情况；检查短条目保留、`load()` 没有被迫使用不可经济恢复的长条目。另测实际字节恰好等于阈值、差 1 B、差 1 KiB 的行为。

### 问题 C：hash 相同但 token 长度不同仍可走相同磁盘路径；隔离条目占用同名文件

**代码位置**：`tools/server/server-task.cpp` → `store()`、`mirror()` 的精确去重循环仅在 `e.hash == hash && e.tokens.size() == pending.ids.size()` 时分类。若相同 hash 但 token 数不同，则直接跳过该检查；而 `write_entry()` 的文件名由 hash 决定，写入时仍先 `fs::remove(path, ec)` 再 `fs::rename(tmp, path, ec)`。这使“hash 不等于身份”的保护存在缺口。

此外，已有 `quarantined` 条目被有意排除在复用和去重之外，但其同名 `.bin` 文件**可能仍然存在且占用配额**。如果新写入的请求与该条目使用相同 hash 路径，必须先解决目录对象所有权，而不能一边保留旧 `entries` 记录、一边覆盖或尝试覆盖相同文件名。

**必须实现**：

1. 对所有 `e.hash == hash` 的已存在条目先进行文件路径/身份冲突分类；只有 token 序列、media 元数据**完整一致且记录可用**时才可判 `identical`。长度不同也属于冲突，不能跳过。
2. 对 hash 相同但身份不同的情况，立即**安全拒绝写入并记冲突原因**，既不能冒险覆盖同名文件，也不能虚报 `dedup`。本轮不引入新文件命名协议和新的持久化格式。
3. 对同名已隔离条目：继续保留其真实字节账本。可以先在磁盘锁控制下尝试走现有 `remove_file()` 及引用失效流程；若删除不成功，必须返回“路径仍被占用/无法提交”的明确写入失败，而非覆写。若成功删除，应真实扣减旧条目占用并从 `entries` 移除，再允许新提交；不得出现两个 `entries` 同时指向同一路径。
4. 后台 worker 写盘前必须再次核验路径保留状态：从 `store()`/`mirror()` 入队到实际写盘之间，索引与磁盘目录可能变化；**只有在确认目标路径所有权清晰时才能删除或替换文件**。不得因为单 worker 就假定前台查找、删除和后台 commit 没有并发。
5. `write_entry()` 当前无条件尝试 `fs::remove(path, ec)` 的行为应改成受已验证路径所有权前提约束的写入行为；仍然通过临时文件 + 原子提交完成，不能直接写原始 `.bin`。
6. 如发生文件名冲突，不改变 v8 格式和 manifest v3；保持已持久化快照、`total_size`、quarantined 及索引引用一致。`cache_disk_hash_collision_total` 与路径被隔离占用的情况应可区分，不混为成功去重。

**单测/故障注入**：构造相同 hash、不同 token 长度（不需真正寻找 FNV 碰撞，可向 helper 注入同一个 hash）；相同 hash、相同长度但不同 media；同名 quarantined 文件被 Windows 打开句柄阻止删除；释放句柄后重新写入；后台入队期间旧条目失效；每种情况核验文件字节、`entries` 数量、账面配额、`manifest`、引用 epoch、写入统计。

## 二、预期修改文件

| 文件 | 修改位置和任务 |
|---|---|
| `tools/server/server-disk-cache-match.cpp` | 修复双向媒体检查短路；必要时加强媒体结构完整性检查；保留准确的 `usable_lcp` |
| `tools/server/server-disk-cache-match.h` | 如需扩展 helper 结果状态，在此定义；不改变既有模型 API |
| `tools/server/server-task.cpp` | `worker()` 向 `drop_prefixes()` 传真实文件字节；修复 hash/路径所有权检查、同名隔离条目安全处理及写入前路径审查 |
| `tools/server/server-task.h` | 调整 `drop_prefixes()` 语义参数，必要时增加纯状态枚举；继续保留单一磁盘锁和 epoch 通知 |
| `tools/server/tests/unit/test_disk_cache_match.cpp` | 添加媒体双向最早差异、参数交换一致、非法媒体、真实字节成本边界、同 hash 异长度测试 |
| `tools/server/tests/unit/`、`test-run/` | 补写盘文件名冲突和隔离路径占用的最小复现与独立回归用例 |
| `change.md` | 仅追加实际修改、差异、构建和验证记录 |
| `projectSummary.md` | 仅更新**已落地**的状态/路径所有权不变量与文件删除规则 |

如某一所列文件实际不存在对应实现，以当前 HEAD 为准；禁止新造重复缓存索引或第二条磁盘通知通道。

## 三、算法与数据流的预期修改

### 3.1 前缀匹配

```text
已有条目 A + 待查询请求 B
  → raw token LCP
  → A→B 的媒体差异边界
  → B→A 的媒体差异边界  【两个方向均执行】
  → 取最早媒体差异位置
  → 对两侧媒体块范围进行合法边界钳制
  → usable_lcp
  → min_tokens / io_cost 守卫
  → 旧有文件校验与 KV 状态恢复
```

不改 `candidate_lcp`、`restored`、`reused` 的定义，不对 token placeholder 做虚假拼接，也不因检出候选就写入模型状态。

### 3.2 长快照替代短快照

```text
write_entry() 得到 actual_size_file
  → 检查新条目最终有效性/路径所有权
  → 用 actual_size_file 与短前缀有效长度进行成本比较
  → 若能替代，才可通过原 remove_file() 删除短条目
  → 更新账面字节、manifest、prefix invalidation epoch
  → 若不能替代，则保留短条目
```

写入队列 `pending.bytes` 只服务于排队与内存预算，不能决定现有短文件的生死。

### 3.3 文件名冲突与隔离占用

```text
待提交写入 (hash, token/media identity)
  → 在磁盘锁保护下查询同名路径及其 entries 状态
     ├─ 完全相同、有效 → 沿用已提交条目，不重复写入
     ├─ 同 hash、不同身份 → 冲突拒绝，不触碰旧文件
     ├─ 同 hash、原条目隔离且文件存在
     │    → 尝试合法清理并真实调整账本
     │       ├─ 可移除 → 释放路径后再提交
     │       └─ 不可移除 → 明确失败，不覆盖、不伪报 dedup
     └─ 无同名占用 → 临时文件落盘 → 原子提交 → manifest
```

不能把 hash 冲突判定逻辑混入 `server_prefix_index` 的 `observed/eligible` 状态机；文件路径所有权只由磁盘层负责。

## 四、兼容性与实现纪律

1. 只适配单用户、单实例、单 slot，保留后台 worker 与前台任务线程之间的锁语义。
2. 继续使用缓存条目 **v8**、索引清单 **v3**、CRC64、现有 target/draft/checkpoint 状态格式和恢复失败清空 slot 的语义。
3. `--cache-prefix-discovery` 保持元数据观察功能，不可用 `eligible`、`persisted` 替代 `reused_disk`。
4. 不调整 `768 KiB/token`、4096-token 写入抑制门槛；也不因本轮修复而改变当前 `-np 1` 的选槽与实际 prefill 算法。
5. 真正删除成功才扣减 `total_size`、撤销物理引用；删除失败时不得伪装为成功或绕过配额。
6. 前缀索引中的提交/失效通知保持磁盘锁内同步与单调 epoch，不引入线程外第二通道；不允许反向锁调用。
7. 所有新增候选身份判定以完整 token + media 三元组为准；hash 仅为目录索引键，不是语义等价证据。
8. 不开发分块 KV、任意 token 区间状态导出、前缀主动重算、独立持久 trie、对象级去重或其他新缓存算法；本轮任务仅限已定位错误的修复与验证。

## 五、验证检查清单

### 5.1 版本与 baseline

- [ ] 在实验机器读取最新 `change.md`、`projectSummary.md`、`git rev-parse HEAD`、`git status --short`，与本文件锁定的提交差异核对；如源码有更新先复核错误位置。
- [ ] 与改造前 baseline 的磁盘缓存 LCP、选槽、CRC、状态恢复、写入文件路径语义对比；修复只触及匹配判定及磁盘文件提交/删除，不改变计算图。

### 5.2 媒体匹配

- [ ] 新增两侧媒体差异位于不同位置的确定性用例，验证每次都检查 A→B 和 B→A，不因 C++ `||` 短路漏掉较早分歧。
- [ ] 参数交换、不同媒体 ID、同 ID 不同块长、多媒体块、部分共享媒体、块内前缀、非法区间/重叠/空 ID 分别检查；明确真实 prompt 的占位 token 约束。
- [ ] 无媒体纯文本与相同媒体输入的历史兼容路径保持原语义；不改变 CRC 和 `read_entry()`。

### 5.3 实际文件大小与短快照保护

- [ ] 构造 pending 估算大小通过而写后真实字节大小未通过成本阈值的边界场景；短快照必须保留。
- [ ] 真实字节恰好等于阈值、相差 1B、相差 1KiB 三种情况均由同一判断式处理。
- [ ] 配额淘汰可能影响刚写入的长条目时，不得先不可逆地删除被其替代的短条目。
- [ ] 可经济替代时原有去重/抑制路径仍可执行，失败删除保持账本真实。

### 5.4 文件名与身份安全

- [ ] 相同 hash 不同 token 长度、相同 hash 同长度不同 token、相同 hash 同 token 不同媒体均拒绝覆盖；不计成功 dedup。
- [ ] 同名 `quarantined` 文件被句柄锁住时，新写入不能覆盖或产生重复 path 记录；释放句柄后流程可安全恢复。
- [ ] 对 `write_entry()` 的目标路径存在与 `entries` 状态变化之间的交错做故障注入，确保 manifest、CRC、`total_size` 和 epoch 一致。
- [ ] 强杀重启后的扫描不出现同一路径两个有效索引条目，不会读到文件内容与元数据对应错误的旧条目。

### 5.5 单用户集成回归

- [ ] `-np 1`、Windows/CUDA、真实 target + draft 组合下，冷启动、完全重复、分叉、追加、输入/输出边界捕获、存盘/读盘、强杀重启、隔离及配额压力测试。
- [ ] `--cache-prefix-discovery` 开关两种模式下，原有输出一致性测试与 `prompt = reused_slot + reused_ram + reused_disk + prefill` 恒等式仍成立。
- [ ] 现有纯 CPU 的 prefix-index 和 disk-cache-match 单测保留，不以单元测试替代真正文件/manifest 回归。
- [ ] 检查 `cache_disk_hash_collision_total`、隔离、写入拒绝、真实文件数量与读盘拒绝原因可区分，不发生候选命中虚报。

## 六、交付记录

- 将实际代码修复、静态定位和已执行验证追加到 **`change.md`**，不要把尚未执行的测试写成已经通过。
- 仅将项目当前**已经实现**的路径所有权、媒体匹配和快照生存期语义更新到 **`projectSummary.md`**。
- 本 `betterModification.md` 仅保留规划与修改日志，不加入预想实验结果、停止训练条件、性能计算报告或优劣裁决。

---

# 2026-11-15 21-55 - test1_2

## 一、实验目标与实施范围

**本轮属于现有单用户磁盘提示缓存的缺陷修复，不启动新的缓存算法实验。** 目标是修复“新长快照提交、短快照被其替代、配额淘汰”三步之间的不可逆状态转换，确保不会因为新长快照最终被淘汰，而已经失去原本有效的短快照。此次继续支持单用户、单实例、`-np 1`；后台磁盘 worker 与推理线程之间已有的同步约束仍需遵守。

本轮只调整磁盘快照的**准入顺序、替代关系确认、配额淘汰和失败回滚**。不修改 `llama_state_seq_get/set_data_ext`、不实现 KV 分块、不得切片序列化状态，不改投机解码、token 序列、采样与 `n_past`；磁盘条目 v8、manifest v3、CRC64、当前模型及媒体配置签名不变。

### 1.1 规划前已完成的核查

- 已读取远端 `main` 当前 `change.md` 的 `2026-11-15 21-35` 修复记录、`projectSummary.md`、本次实验人员报告 `2026-11-15-handover-disk-cache-fixes.md`；已核对 GitHub 最新提交链：修复代码提交 `5627aed32b74dcf516e4a041d61829ad20e69619`，报告发布后的 HEAD `a37ac4e0e0ea762ec52fa44d657b88af61cbc5b9`。
- 与原始项目基线提交 `eb7681889f44304d2bd5b3e41572f5530af91a1d` 的 `server-task.cpp`、后续新增的 `server-disk-cache-match.*`、`server-prefix-index.*` 的功能差异进行了比对：当前方案新增了隔离配额、磁盘路径所有权、以可恢复成本约束长短前缀替代的策略。
- 已检查 `tools/server/server-task.cpp` 中 `server_disk_cache::worker()`：在成功 `write_entry`、添加新条目到 `entries` 后，先执行 `drop_prefixes(...)`，再执行 `enforce_limit()`；尽管代码检查新条目是否“已加入 `entries`”，但这个条件不能证明**后续配额淘汰不会删除该条目**。
- 实验人员报告明确指出“`enforce_limit()` 立即淘汰新写入条目时的短快照保护”**未构造端到端用例**。以下风险是有明确代码路径的静态问题，尚无真实运行复现证据；本规划要求使用故障注入与独立缓存目录补充证明。

编码人员**开始修改前**，必须重新读取实验机器的最新 `change.md`、`git rev-parse HEAD`、`git status --short`、当前 `projectSummary.md` 和实际运行二进制；将上述路径与最新源码逐项对照。若已有其他人改动本次问题函数，必须先重新确认缺陷是否仍在。发现新的明显缺陷时先修复，不能顺带引入新策略。

## 二、已定位问题：先删替代对象，后淘汰新条目

### 2.1 代码位置

`tools/server/server-task.cpp`：

1. `server_disk_cache::worker()` 的成功写入分支：
   - `total_size += size_file`，`entries.push_back(std::move(e))`；
   - 确认新路径已经出现在 `entries` 中；
   - `drop_prefixes(..., size_file)`；
   - `enforce_limit()`；
   - `save_manifest()` 和最终 `notify_committed()`。
2. `server_disk_cache::drop_prefixes()`：只要新长条目的真实 `size_file` 能满足 `disk_cache_can_replace()`，便执行旧短快照的不可逆文件删除并撤销前缀引用。
3. `server_disk_cache::enforce_limit()`：在总占用超过上限时按 LRU 选择可删除条目，跳过 `quarantined`。如果剩余可删除对象只有刚写入的长快照，则它也可能被删除。
4. `server_disk_cache::sweep_expired()`：可能遇到被 Windows 句柄占用、TTL 已过但无法实际删除的条目，使这部分字节继续计入 `total_size`，同时不能用于恢复或淘汰。

### 2.2 确定性的失败路径

示意状态，不代表已经实测的性能数字：

```text
磁盘配额：限定上限
Q：已隔离且物理文件暂时不可删除，占用配额
S：可恢复的短公共前缀快照
L：新写入的长快照；对 S 满足 token/media/成本可替代性

写入前：Q + S 在配额内
写入 L：Q + S + L 超额
旧顺序：drop_prefixes(L) 先删除 S
        → Q + L 仍超额
        → enforce_limit 跳过 Q，删除 L
最终：Q 仍在、S 与 L 都已不存在
```

此时 `drop_prefixes()` 的静态“L 在当前时刻可以替代 S”不足以保证**L 在本次提交事务完成后依然存在**。这与此前已修复的真实文件大小判据是两种独立问题：`size_file` 正确，也不能保证长快照在配额下的长期可保留性。

### 2.3 必须维持的安全不变量

- **替代存活不变量**：如果因为“长快照 L 能替代短快照 S”而删除了 S，则同一次提交/容量处理完成时，L 必须仍是有效的、未隔离的、物理存在且可作为恢复候选的条目。不能先删 S 再把 L 淘汰掉。
- **真实占用不变量**：`total_size` 始终包括所有尚未成功删除的物理条目（尤其是 `quarantined`）；删除失败不能虚扣或假装释放空间。
- **无重复路径不变量**：任何 `pc-<hash>.bin` 不得有两条互相矛盾的有效记录；写前 `claim_path_locked()` 的身份/路径所有权规则不变。
- **引用时序不变量**：`notify_committed`、`notify_invalidated` 仍在磁盘锁内同步交付，递增 `event_epoch`，目录中的 `persisted` 只能指向最终存在的已提交条目。
- **恢复安全不变量**：`load()` 的 v8/CRC、media、状态恢复及失败后重置 KV 语义不受修改。

## 三、拟修改文件与职责

| 位置 | 修改要求 |
|---|---|
| `tools/server/server-task.h` | 如需新增私有辅助类型，定义磁盘写入准入计划、受保护路径集合与事务状态；不得在公共 API 暴露活的 KV 指针 |
| `tools/server/server-task.cpp::worker()` | 重排“新文件落盘—容量准入—短前缀去重—manifest/引用通知”的步骤，避免在确认新条目可保留之前删除 S |
| `tools/server/server-task.cpp::enforce_limit()` | 允许上层在一个受锁保护的处理过程内提供短暂的受保护条目集合或等价只读配额模拟；不可破坏原先通用 LRU/隔离语义 |
| `tools/server/server-task.cpp::drop_prefixes()` | 仅在长条目获得本次容量准入保障后允许删除短条目；继续使用真实 `size_file`、`disk_cache_can_replace()` 与 `remove_file()` |
| `tools/server/server-task.cpp::sweep_expired()` | 与受保护路径/配额事务协作，隔离文件既不能被视为释放空间，也不能作为可替代候选 |
| `tools/server/tests/unit/` | 增加能按虚拟条目/虚拟文件删除结果注入故障的容量-替代顺序测试；只靠现有 12+13 组纯匹配单测不足以覆盖 |
| `test-run/` | 在单用户、单 slot、独立缓存目录执行含文件句柄占用的端到端场景；复用已有环境变量配置 |
| `change.md` | 记录真实源码改动、基线差异和实际执行的验证项目 |
| `projectSummary.md` | 仅更新本轮完成后已经真实实现的快照准入及替代存活机制 |

新 helper 建议继续放在 `server_disk_cache` 私有实现中，不必另设复杂缓存管理服务；`server-disk-cache-match.*` 只负责**静态等价与成本判定**，不要将配额事务、磁盘 IO 或锁的职责混入纯匹配模块。

## 四、算法变更方案：保守的配额准入和原子替代

### 4.1 写入完成之后先做“可保留性”确认

要将“新文件已写入成功”与“新文件已获准作为持久化恢复来源”区分。不能仅凭 `entries` 中存在新路径就认为可以删除原有前缀。

建议在磁盘锁内通过一个明确的**准入事务**控制：

1. 读取新文件的实际 `size_file`（不得用 `pending.bytes` 决定旧文件生死）；创建临时的候选条目描述，但尚不删除旧快照。
2. 根据 `limit_size`、`total_size`、`quarantined` 和 LRU 元数据，进行**只读容量规划**：确定在保护现有有效短快照的前提下，是否可以同时承诺保留新长快照；其中“可能被 L 安全替代”的短快照也必须先视为受保护对象，直到 L 获得准入保障。
3. 在已证明配额可满足时，才开始执行必要的删除/替换操作；所有删除只通过 `remove_file()`，每次根据真实成功/失败更新账本与引用。在某个删除操作失败、配额规划被破坏时，**不得把尚无稳定替代的短快照删掉**。
4. 如果新长快照不能进入可恢复的保留集合，应执行保守回滚：删除本次新写入的文件（必要时按现有隔离/记账机制处理删除失败），保留原先有效的短快照，并在日志中说明无法准入而不是计为“已保存”。
5. 一旦已经实际删除了任何受新长快照替代的短快照，本次事务不得再淘汰这份新长快照。对于此前无法预见的删除失败导致的配额超额，必须保留替代状态一致性、显示真实账面超额并拒绝进一步不受控写入；不得通过删掉 L 造成无缓存状态。
6. 最终再统一提交 `entries`/manifest 和对应 `notify_*`，不得让目录在临时状态中提前将 L 标记为 `persisted`。

实现人员可选用轻量的 `quota_admission_plan` 或 `protected_paths` 参数，但需保证“计划—物理动作—确认”在已有 `mtx` 的正确粒度下执行。不能在磁盘锁内做完整 KV 序列化，也不能在锁外发布中间的物理引用。

### 4.2 推荐的状态流

```text
write_entry(pending) 成功 → 已获得真实 size_file
    ↓
磁盘锁内：准备 provisional L（不对 prefix-index 宣告 persisted）
    ↓
试算配额：包含隔离 Q 的真实占用
    ├─ 无法保留 L → 仅清理 provisional L，保留全部有效 S，记录准入失败
    └─ 可以保留 L → 先处理不会影响原有前缀可恢复性的容量清理
                         ↓
                    获得 L 的保留保障
                         ↓
                    按静态可替代关系检查 S
                         ↓
                    删除 S（失败则按实际状态保留并调整计划）
                         ↓
                    最终确认 L 的有效性及真实配额
                         ↓
                    save_manifest + notify_*（严格同步顺序）
```

不允许以“把 `enforce_limit()` 简单移到 `drop_prefixes()` 之前”作为唯一修复：普通 LRU 也可能先淘汰 S 再淘汰 L，并不能提供保留保障。

### 4.3 不改变的现有策略

- `DISK_CACHE_MAX_BYTES_PER_TOKEN = 768 KiB/token` 与 `DISK_CACHE_MAX_PREFIX_LAG_TOKENS = 4096` 原值不变。
- 当前 `disk_cache_can_replace()` 的 token/media 及真实文件大小判断保留；本轮只增加“替代快照必须真正存活”的生命周期约束。
- 不能因为新写入失败而触发额外的强制 prefill 或修改正在生成的 slot KV；写失败仍只是加速器未保存成功。
- 不能删除 `quarantined` 的账面字节，不能绕过 `claim_path_locked()`，不能新建与 v8 不兼容的物理格式。
- 只适配 Windows/CUDA 单用户 `-np 1`，不增加用户配额、租户、跨 slot 并发调度。

## 五、验证检查清单

### 5.1 最小故障复现

- [ ] 先记录实验机 `change.md` 最新条目、HEAD、工作树、编译产物；锁定修改前版本，建立可以回退的独立提交。
- [ ] 在不加载模型的磁盘准入测试中构造 Q（不可删除）、S（有效短前缀）、L（可替代长前缀），使写前 Q+S 在配额内，写后 Q+S+L 超额，且写后如果先删 S 仍有 Q+L 超额；确认原顺序会使 S 与 L 均不可用，然后验证新顺序不会发生这种状态。
- [ ] 覆盖“旧短 S 已存在、新长 L 写入实际文件大小超过预测/恰好触及上限”的边界；所有入库规划均按真实 `size_file` 计算。

### 5.2 配额和删除异常

- [ ] 可删除条目均可清理时，长条目的正常替代与原有磁盘上限处理正常进行。
- [ ] Q 无法删除、Q 在稍后能够删除、其他旧对象无法删除、S 删除失败、L 回滚删除失败等分支分别检查；确认真实磁盘字节与 `cache_disk_bytes` 对应。
- [ ] 验证 `enforce_limit()` 永不反复选择同一失败对象陷入循环，并且不会在已经删除 S 的同一事务中淘汰其唯一替代 L。
- [ ] 测试限额小于新文件单体大小、配额空间恰好足够、配额只差一个受保护短对象等边界；不以估算字节替代实际大小。
- [ ] 在任何失败分支，不出现“文件已不存在但 entries 仍可查找”“短快照消失且其替代快照也消失”或“未提交但目录 `persisted` 增加”。

### 5.3 引用和持久化

- [ ] 在一个提交事务中记录 L/S 的磁盘实际路径、状态转换、`event_epoch` 与 `notify_*` 顺序，确认索引与 manifest 不可见中间状态。
- [ ] 强制终止后重启，`scan()` / `dedup_prefixes()` / `enforce_limit()` 不会抹除仍需保护的短快照，目录仅根据有效条目恢复物理引用。
- [ ] 验证任意同路径 hash 冲突仍由 `claim_path_locked` 处理，不发生未经授权的覆盖。

### 5.4 单用户端到端

- [ ] Windows/CUDA 双卡与现有模型/草稿，`-np 1`，使用独立缓存目录，执行冷 prefill、重复请求、公共前缀分叉、短/长快照、TTL/文件句柄占用、配额淘汰、服务重启。
- [ ] `--cache-prefix-discovery` 开启/关闭各做一组；新修复只影响磁盘准入，不能把 `observed/eligible/persisted` 当作命中。
- [ ] 校验 `prompt = reused_slot + reused_ram + reused_disk + prefill`，原有输出一致性及 CRC 恢复安全仍成立。
- [ ] 继续运行已有 `test_disk_cache_match.cpp`、`test_cache_prefix_index.cpp` 自检；增加本轮磁盘准入生命周期专用测试，不以纯 metadata 单测代替写盘测试。

## 六、与既有实现的兼容性和日志要求

1. 当前磁盘快照恢复、媒体身份、LoRA 隔离、target/draft/checkpoint、CRC 与配置签名完全保留。
2. 短前缀删除只在“静态可替代 + 新替代快照获得保留保障”时执行；回退流程不能触碰仍有效的旧快照。
3. 仍使用磁盘锁内的唯一提交/失效通知通道，禁止新增延后通知导致 `epoch` 逆序。
4. `change.md` 记录真实文件修改位置、基线 SHA、错误复现步骤与实际验证事实；`projectSummary.md` 仅记录已实现的总体结构。
5. `betterModification.md` 只记录本轮修复规划及修改日志，不写实验收益预测、停止条件、报告计算或最终方案裁决。

**交付范围**：本次仅完成容量准入与快照替代的状态一致性修复以及相应可复现用例；不规划新的算法试验或扩大单用户以外的场景。
---

# 2026-11-15 22-40 - test1_2

## 一、修复定位、目标与已有改动核查

**本节是单用户磁盘缓存既有实现的缺陷修复补充，不启动新的算法实验。** 范围严格限定为本机部署、单用户、单实例、单 slot（`-np 1`），不引入网络鉴权、租户隔离、多 slot 调度或远端 API 防护。需要保持的“API 正确性”仅指本地 KV 序列状态导出/恢复不能改变推理语义，与网络攻击面无关；本轮不修改该 API。

### 1.1 本轮已执行的审查

在规划本次修复前，已首先审阅仓库 `change.md` 中最新的 **`2026-11-15 22-30 - Change Record`**，随后阅读 `projectSummary.md`、交付报告 `2026-11-15-handover-v2-disk-cache-fixes.md`，核对 GitHub `main` HEAD `27994007aadf55ce57fd7089381e9bde1e79ec7d`，核心算法提交 `0e46af42f62b298dfdf80430c1f4874bec685c5a`，并将缓存改造路径与较早 baseline 提交 `eb7681889f44304d2bd5b3e41572f5530af91a1d` 对比。

确认当前实现已有：v8 快照、v3 manifest、CRC、请求级复用统计、请求边界捕获、公共前缀元数据目录、媒体双向匹配、路径所有权判定、只读配额规划以及“先准入后替代”顺序。最新交付报告记录了 37/63/38 条断言、单用户读写与重启回归。但下面两处在新增事务实现中仍存在**代码级风险**，必须先修复，不能继续叠加新缓存算法。

编码人员动手前须在实验机再次执行 `git rev-parse HEAD`、`git status --short`、阅读最新 `change.md`，确认本节列出的代码仍与实际工作树一致，并固定一份可回退的提交。若现场已经有更新提交修复这些问题，应先核对差异，不允许重复改写。

### 1.2 已确认问题 A：`enforce_limit(keep)` 的保护指针可能失效

**位置**：`tools/server/server-task.cpp` 的 `server_disk_cache::worker()`、`server_disk_cache::enforce_limit(const std::vector<const server_disk_cache_entry *> & keep)` 和 `sweep_expired()`。

现有 `worker()` 对新条目入 `entries` 后，扫描 `entries`，把 `&cur` 保存到 `keep`。`entries` 是 `std::vector<server_disk_cache_entry>`。随后 `enforce_limit(keep)` 首先调用 `sweep_expired()`，然后又可在 LRU 淘汰中调用 `entries.erase(it)`。**`std::vector::erase` 会使被删除元素及其后的元素引用/指针失效**。如果删除位于新条目前的旧条目，受保护新条目位置将移动，但 `keep` 中的旧地址不更新。

因此，`std::find(keep.begin(), keep.end(), &*cur)` 不能继续可靠地识别新条目。容量处理可能错误地将本次保护对象纳入淘汰范围，破坏“删除短快照时其替代长快照必须存活”的不变量。这里是基于源码和 C++ 容器规则的静态确认，**不能把尚未运行的端到端故障测试写成已完成**。

**修复要求**：

1. 删除跨 `entries.erase` 保存 `server_disk_cache_entry*` 的保护集合，改用**稳定路径身份**，例如 `std::unordered_set<std::string> protected_paths` 或不可变、唯一的 `snapshot_identity`。选择路径时必须复制字符串，不保存指向 `entries` 内元素的裸指针/`string_view`。
2. 将 `enforce_limit()`、其 LRU 过滤和 `sweep_expired()` 对保护对象的处理统一到相同的稳定身份。如果 TTL 清扫仍在配额处理前执行，应确保清扫不会删除本次事务受保护的新条目；不能简单地改完 LRU 检查却漏掉前置清扫。
3. 每次 `entries.erase()` 后重新依据有效身份检查保护，不能拿旧迭代器、旧引用或旧下标推断 `entries` 中新对象的位置。
4. `worker()` 在最终执行 `drop_prefixes()` 之前，必须再次基于**实际 `entries` 路径**检查新条目未隔离且文件存在。仅靠历史上的 `admitted=true` 或内存旧指针不可作为持久替代证明。
5. 单用户并不意味着无并发：前台请求与后台 worker 的 `entries` 修改仍须遵守原 `mtx`，`notify_*` 保留锁内同步与单调 `event_epoch`。

建议接口（表达职责，最终签名服从现有代码风格）：

```cpp
using disk_cache_protected_paths = std::unordered_set<std::string>;

void enforce_limit(const disk_cache_protected_paths & protected_paths);
void sweep_expired(const disk_cache_protected_paths & protected_paths);

bool is_protected(const server_disk_cache_entry & entry,
                  const disk_cache_protected_paths & protected_paths);
```

对于完全独立于 `entries` 的配额规划模块 `server-disk-cache-quota.*`，维持当前只读计划及稳定字符串存储原则；本轮不将 LRU 策略重新设计成热点淘汰算法。

### 1.3 已确认问题 B：新文件回滚删除失败时成为“未记账的孤儿文件”

**位置**：`tools/server/server-task.cpp` 的 `server_disk_cache::worker()` 在 `if (!plan.fits)` 分支中调用 `remove_file(path); counters.n_write_rejected++;`。

当前代码在写入成功、获得真实 `size_file` 之后执行配额试算；若 `plan.fits == false`，尝试删除新文件，但**忽略 `remove_file(path)` 返回的 `removed / already_missing / failed`**。在 Windows 上文件被其他进程持有时，删除可能失败。该文件实际仍占用磁盘，但既未进入 `entries`，也未纳入 `total_size`，同时缺少 manifest 生命周期与路径占用记录。此问题与之前修复的“删除失败不能假装腾出磁盘”不变量相冲突。

**修复要求**：

1. 对试算拒绝后的删除结果作显式分支：`removed` / `already_missing` 代表回滚成功；`failed` 表示文件仍然占用真实字节，**不得假设回滚成功**。
2. 删除失败时采用已有隔离机制登记此文件：保留真实 `path`、`size_file`、hash、token/media 身份和 `quarantined=true`，计入 `entries` 与 `total_size`；它不能参与 `load()`、`covered_cheaply()`、`worker()` 去重或前缀目录 `persisted` 引用。
3. 隔离登记必须在磁盘锁内完成，manifest、账本、`cache_disk_quarantined`、路径所有权管理保持一致。已有相同路径记录时必须通过 `claim_path_locked()` 的既有冲突语义处理，禁止重复登记或覆盖。
4. 如果文件已经不可定位、大小无法可信确认，必须保守记录具体文件系统失败事实和再扫描/回收需求，不得虚报配额已回收；不能将一个未知状态文件作为有效缓存恢复来源。
5. 与 `claim_path_locked()` 的隔离文件清理路径对接：当文件句柄释放后，可以真实删除，再扣减账面占用和撤销引用；不得把 `write_rejected` 计成成功写入或者 `dedup`。
6. 不引入另一套异步删除机制或新的磁盘格式；失败处理继续复用 `remove_file()`、`quarantine_entry()`、`save_manifest()` 与既有物理事件通道。

### 1.4 需要随修复核对的问题 C：配额试算与实际删除结果是否一致

`disk_cache_plan_quota()` 是纯元数据规划，它只能判断**假定候选可以删除**时的容量可行性。`enforce_limit()` 实际删除可能失败并把条目隔离，因此 `plan.fits` 本身**不是**“实际配额已经满足”的凭证。本轮应在真实回收后重新核对 `total_size` 与 `limit_size`，并明确两种情形：

- 如果本次尚未删除任何受新文件替代的短快照，且因实际回收失败导致超额，应优先尝试按问题 B 的安全回滚路径处理新文件，保留旧文件；回滚若也失败，要记录隔离账本。
- 如果事务中已完成会影响可恢复性的替代删除，则不能通过删除唯一替代者来“修平”配额；必须保留真实账本、记录超额并阻止额外无控制写入。不得把这类路径隐式当作正常的 `plan.fits` 已兑现。

本轮不要求重写配额规划器的算法，但必须显式区分“计划可行”和“物理删除兑现”，避免测试仅检查 `plan.fits` 而忽略文件系统错误。

## 二、预期修改文件和精确职责

| 文件 | 修改位置 | 修改内容 |
|---|---|---|
| `tools/server/server-task.h` | `enforce_limit()`、`sweep_expired()` 声明与 `worker()` 辅助声明 | 将保护集合从 `server_disk_cache_entry*` 转换为不可随 `entries.erase` 失效的稳定身份 |
| `tools/server/server-task.cpp` | `worker()` 准入事务、配额拒绝回滚 | 使用稳定保护身份；对回滚 `remove_file()` 结果分支处理；处理隔离记账 |
| `tools/server/server-task.cpp` | `enforce_limit()`、`sweep_expired()` | 保护检查覆盖 LRU 和 TTL，避免 erase 造成指针失效 |
| `tools/server/server-task.cpp` | `claim_path_locked()`、`quarantine_entry()`、`save_manifest()` 调用处 | 对回滚失败产生的占用路径建立一致生命周期，不修改正常路径所有权语义 |
| `tools/server/server-disk-cache-quota.h/.cpp` | 只读计划结构与调用适配（若必要） | 保持稳定身份、隔离占用和受保护对象不淘汰；本轮不改准入策略本身 |
| `tools/server/tests/unit/test_disk_cache_quota.cpp` | 扩充测试 | 新增顺序删除后保护身份稳定性、不可删除导致实际超额、回滚删除失败的状态机单测 |
| `test-run/` | 新增或扩充单用户故障注入脚本 | 用可控文件句柄/模拟删除结果复现条目移动和回滚失败，不修改真实生产缓存目录 |
| `change.md` | 文末追加 | 记录本轮实际代码变更、核查方式、实测与未覆盖项目 |
| `projectSummary.md` | 快照生命周期和配额不变量章节 | 仅在实现完成后更新已经证实的现有结构 |

**不修改** `src/llama-context.cpp` 的 KV 序列化、模型权重/草稿模型、prompt token、LoRA、采样、DSpine 推理逻辑或 v8/v3 的字段布局。

## 三、算法与预期数据流变更

### 3.1 稳定保护身份

```text
当前 worker：真实新文件 L 已写入
  → disk_cache_plan_quota(items, protected=L)
  → 若 fits：把 L 加入 entries
  → protected_paths = {L.path}  （独立字符串副本）
  → enforce_limit(protected_paths)
       → sweep_expired(protected_paths)
       → 逐个尝试删除可淘汰旧条目（每次 erase 后仍按 path 判断保护）
  → 重新验证 L 仍存在、未隔离、磁盘文件存在
  → 仅在 L 真正有效时调用 drop_prefixes(..., replacement_is_admitted=true)
```

不得把 `keep` 的 `vector<const server_disk_cache_entry *>` 简单替换成对 `std::vector` 元素的 `string_view`：字符串对象和它所在的 entry 同样可能被 `erase()` 移动或销毁。也不得以 `reserve()` 替代稳定身份方案：`reserve` 只避免扩容，不保证 `erase()` 之后元素地址不变。

### 3.2 回滚删除失败的账本

```text
配额计划 !fits
  → remove_file(new.path)
      ├─ removed / already_missing
      │      → 新条目不进入 entries；原缓存不变；记录准入拒绝
      └─ failed
             → 新文件仍占用真实字节
             → 在磁盘锁内记录隔离条目（不允许恢复、不允许抑制新写入）
             → total_size 计入真实 size_file，保存 manifest
             → 后续真实清理成功后再核减 bytes / 移除条目
```

### 3.3 保持既有业务语义

- **训练流水线**：无训练，完全不变。
- **推理流水线**：只更改磁盘缓存的准入/回滚、淘汰保护与账本处理；不改变当前请求的模型执行、KV 位置、logits 或生成文本。
- **缓存成本**：保留 `768 KiB/token` 读盘成本守卫与 `4096 token` 写入抑制阈值；不把逻辑前缀发现当作 `reused_disk`。
- **存储协议**：保留 v8 完整快照、v3 manifest、CRC64、target/draft/checkpoint 关联与配置签名，不引入新的数据格式。
- **适用范围**：本机、单用户、`-np 1`；不增加网络安全配置和多用户调度。

## 四、验证检查清单

### 4.1 指针失效与淘汰保护

- [ ] 在本地锁定版本先运行现有 3 套独立单元测试；记录与 baseline 对应缓存路径的差异。
- [ ] 创建至少两个先于新条目存入 `entries` 的旧条目，执行删除旧条目、`entries.erase()`、后续继续配额处理，验证新条目仍通过**稳定 path** 被识别为受保护对象。
- [ ] 验证 `sweep_expired()` 前置清扫不会删除本次事务保护对象；验证 TTL=0 与 TTL>0 两条路径。
- [ ] 有一个旧条目删除失败并进入隔离时，继续尝试其他淘汰，确认新条目不会因指针失效被当作候选。
- [ ] 在同一事务中验证短快照被替代删除时，替代长快照在 `entries` 中且未隔离、物理文件存在。

### 4.2 回滚删除失败

- [ ] 直接针对 `plan.fits=false` 构造“新文件写盘成功但 `remove_file` 失败”的故障注入；检查新文件被隔离并计入真实字节。
- [ ] 确认该隔离文件不进入 `load()`、写入抑制、worker 冗余、精确去重及目录 `persisted` 引用。
- [ ] 解除句柄占用或撤销注入后，执行合法回收，确认 `total_size` 与物理文件数量同步变化，manifest 不出现重复路径。
- [ ] 验证计划可行但实际某个候选删除失败的路径；禁止将未满足配额的结果误报为已兑现。

### 4.3 单用户回归

- [ ] `--cache-prefix-discovery` 开启/关闭各运行冷启动、同请求重复、续写扩展、短/长公共前缀、强杀重启与磁盘文件占用场景。
- [ ] 检查 `prompt = reused_slot + reused_ram + reused_disk + prefill`、`candidate_lcp/restored/reused_disk` 的分层口径及原有输出一致性。
- [ ] 保持 `test_cache_prefix_index.cpp`、`test_disk_cache_match.cpp`、`test_disk_cache_quota.cpp` 既有断言；增加能定点触发 `entries.erase` 和回滚失败的用例。
- [ ] 在实验记录中分别标识“静态确认的问题”“故障注入复现”“真实模型端到端验证”，不得以元数据单测代替磁盘恢复测试。

## 五、与既往修改的兼容性和实施顺序

1. 本轮仅修复已经实现的磁盘配额事务与条目状态一致性，不改动公共前缀目录的发现策略。
2. 开工先查最新 `change.md`、HEAD 与 baseline 差异；如果现场发现不同的新缺陷，先记录位置并修复，不叠加新实验。
3. 按“稳定保护身份 → 回滚删除记账 → 物理回收后复核 → 单元与单用户回归 → 更新 `change.md` / `projectSummary.md`”的顺序实施。
4. 现有唯一物理删除函数 `remove_file()`、锁内 `notify_*` 和单调 `event_epoch` 语义不得破坏。
5. 当前方向保留；本文件只记已有实现的缺陷修复规划，不写预想收益数值、早停要求、计算报告或最终优劣判定，也不在本节规划新的 KV 分块实验。

---

# 2026-11-15 23-17 - test1_2

## 一、本轮定位与既有代码核查

本轮属于现有磁盘提示缓存的**状态一致性修复**，继续沿用原有 `test1_2`；不开始新的缓存算法实验，不修改文件格式，也不把没有完成真实 KV 恢复的“逻辑前缀”记为磁盘命中。

实施前核对依据：

- 仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`。
- 审查时远端 HEAD：`c5d403d24d0812618f2ef9ac8bafd76fb4f3356a`（交付说明文档提交）。
- 最新代码修复提交：`059e267b1d81a10b06d0ea88c1368ae1887101c2`。
- 已首先检查远端 `change.md` 的 `2026-11-15 23-15 - Change Record`，并逐项核对上传报告《2026-11-15-handover-v3-quota-transaction.md》与 GitHub 最新 `server-task.cpp`、`server-context.cpp`、`server-prefix-index.cpp`。
- 已与改造前 baseline 提交 `eb7681889f44304d2bd5b3e41572f5530af91a1d` 对比。baseline 无新增的 `server_prefix_index` 物理引用回调，当前问题由新增磁盘回滚隔离生命周期与前缀目录提交通知的组合产生，不涉及原模型的计算或采样逻辑。
- 上轮修复的路径键保护集、`plan.fits` 与物理回收复核、回滚失败登记 `quarantined` 等实现已经存在；现有测试说明主路径成立。但发现新的**明确控制流不变量违背**，下述缺陷须在继续算法开发前修复。

实验人员着手修改时，仍须读取实际工作树最新 `change.md`、`projectSummary.md`、`git status --short`、`git rev-parse HEAD`，将对应源码与上述审查版本及原始 baseline 再比对。若已有后续代码已修复相同问题，应先确认状态，避免重复修改。

## 二、已定位问题：隔离条目可能被通知为 `persisted`

### 2.1 精确问题位置

文件：`tools/server/server-task.cpp`，`server_disk_cache::worker()` 的回滚/隔离分支与结尾“提交通知”分支；相关接收方为：

- `tools/server/server-context.cpp` 的 `on_entry_committed` 回调；
- `tools/server/server-prefix-index.cpp` 的 `on_snapshot_committed()`。

最新 `worker()` 已支持：

1. `!plan.fits` 且 `remove_file(path)` 失败时，执行 `quarantine_entry(e, ...)`、`total_size += size_file`、`entries.push_back(std::move(e))`；
2. `plan.fits` 但物理回收后仍超配额、`superseded == 0`、回滚删除失败时，对已入 `entries` 的新条目调用 `quarantine_entry(cur, ...)`。

这两种情况下，**条目保留在 `entries` 仅意味着要对实际磁盘占用记账，不代表快照可恢复**。

然而 `worker()` 当前末尾只以 `e.path == path` 判定：

```cpp
for (const auto & e : entries) {
    if (e.path == path) {
        committed = e;
        have_committed = true;
        break;
    }
}
if (have_committed) {
    notify_committed(committed);
}
```

因此两个回滚失败分支都可能把 `quarantined=true` 的条目交给 `notify_committed()`。`server_prefix_index::on_snapshot_committed()` 按精确 token/media 身份建立快照物理引用并刷新为 `persisted`，没有收到 `quarantined` 状态；与此同时 `server_disk_cache::load()` 会排除该条目。

**错误结果是目录中出现无法被真实磁盘层恢复的 `persisted` 引用。** 当前源码可静态证明该路径，尚无本机故障注入的动态复现记录，不得在 `change.md` 写成“已实测”。

### 2.2 修改原则

- `entries` 中存在：仅代表**有文件或账面占用**。
- `quarantined`：代表**不可作为可恢复候选**，即使文件仍占用空间。
- `persisted`：只能来自经过本次提交事务确认的、**仍存活且可供查找**的磁盘条目；不是“任一同路径条目存在”的同义词。
- `notify_committed()` 应只用于此次事务真正入库的有效条目；被拒绝而仅为记账保留的条目不得触发提交事件。
- 原有启动登记 `server-context.cpp` 已跳过 `e.quarantined`。必须让运行时通知采用同一资格判定；不能出现“运行期间 persisted，重启后消失”的虚假状态。
- 不为此增加第二个异步通知通道。沿用磁盘锁内同步回调及原有单调 `event_epoch`。

## 三、预期修改文件

| 文件 | 预期修改位置 | 修改内容 |
|---|---|---|
| `tools/server/server-task.cpp` | `server_disk_cache::worker()` 末尾通知资格判断 | 将“同路径存在”改为“同路径且 `is_reusable_entry()` 为真”；明确拒绝态、隔离态和有效入库态 |
| `tools/server/server-task.cpp` | `!plan.fits` 回滚失败、`total_size > limit_size` 回滚失败分支 | 确认两分支均不会向前缀目录发布提交；保留真实账本 |
| `tools/server/server-task.cpp` | `notify_committed()` / `notify_invalidated()` 附近 | 仅在有助于防止未来误用时加最小断言或诊断日志；不得改变事件顺序和锁语义 |
| `tools/server/server-task.h` | 可选：提交资格辅助函数/测试钩子声明 | 仅当为了避免逻辑重复或可测试性确有必要时修改 |
| `tools/server/tests/unit/` | 新增/扩展针对提交资格的测试 | 覆盖有效提交、两种回滚失败、回滚成功、配额超额但有效存活 |
| `test-run/` | 磁盘层与前缀目录联动回归脚本 | 注入回滚删除失败，核查 `entries`/统计/前缀物理引用的一致性 |
| `change.md` | 追加实际代码修改记录 | 明确“静态发现”与“动态验证”的证据范围 |
| `projectSummary.md` | 已实现的不变量章节 | 修复完成之后补充“隔离条目绝不产生持久化引用”的现状描述 |

不要求修改 `src/llama-context.cpp`、训练脚本、模型权重、target/draft 推理路径或任何网络/API 权限控制。

## 四、具体算法与状态流

### 4.1 提交通知必须取“有效提交”而不是“账面存在”

伪代码仅展示逻辑约束，实际实现沿用现有结构及命名：

```cpp
// mtx held: after quota / rollback / replacement / save_manifest
bool has_usable_commit = false;
server_disk_cache_entry committed;

for (const auto & cur : entries) {
    if (cur.path != path) {
        continue;
    }

    // non-reusable entries are accounted bytes, not cache hits or persisted refs
    if (!is_reusable_entry(cur)) {
        continue;
    }

    committed = cur;
    has_usable_commit = true;
    break;
}

if (has_usable_commit) {
    notify_committed(committed);
}
```

如后续代码已引入显式 `admitted` 或事务结果枚举，优先在最终状态确认为有效后发出提交事件。不得用原 `plan.fits`、`write_entry()` 的 `ok`、`path_owned`、文件存在或 `total_size` 的任意单一条件代替“条目可供恢复”的最终判断。

### 4.2 五个必须区分的事务结局

| 最终状态 | `entries` | `total_size` | `load()` 候选 | `on_entry_committed` |
|---|---|---|---|---|
| 新快照成功入库且有效 | 包含非隔离条目 | 计入 | 允许按原校验查找 | **应发送** |
| `!plan.fits`、新文件回滚成功 | 不包含新条目 | 不计入新文件 | 不可用 | **不得发送** |
| `!plan.fits`、回滚删除失败 | 包含隔离条目 | 计入残留文件 | 不可用 | **不得发送** |
| `plan.fits`，物理回收后超额、无替代删除、回滚成功 | 不包含新条目 | 核减新文件 | 不可用 | **不得发送** |
| 同上但回滚失败 | 包含隔离条目 | 保留真实字节 | 不可用 | **不得发送** |

另需覆盖“新条目已替代短快照且被保护保留，虽仍存在不可回收的超额字节但本身未隔离”的状态；在当前既有语义下，它仍属于**有效缓存条目**，不能误把所有超配额条目排除于 `persisted`。

### 4.3 回调接收端与通知顺序

`server-context.cpp` 目前以：

```text
server_disk_cache::on_entry_committed
    → server_prefix_index::on_snapshot_committed
    → 精确路径登记、refresh_state() → persisted
```

处理物理引用。该回调不能被要求猜测磁盘条目的隔离状态；**发送方**负责只传递可用条目。

如果此前同名文件因隔离、回收或数据损坏已经发送失效，保持该 `notify_invalidated(path)` 的原路径；新一轮回滚失败不能以较新的 commit epoch 把失效引用“复活”。

### 4.4 可测试性与故障注入

直接靠 Windows 外部句柄在回滚瞬间抢占文件名不稳定。因此建议在测试构建中增加最小、仅测试用的确定性删除故障注入，局限于**指定缓存路径与指定事务分支**。不得在生产构建默认开启；不得让测试钩子改变 `remove_file()` 在正常模式下的语义。

至少完成两层检查：

1. 纯元数据/单元测试：验证最终 `entries` 状态 → 允许提交事件的布尔资格，确保隔离条目永远为 false。
2. 磁盘层 + 前缀目录集成测试：当回滚失败且 `quarantined=true` 时，真实磁盘账本包含该文件，但前缀目录没有该文件的 `persisted` 引用；文件回收后记录数与真实路径一致。无需加载大模型即可构造磁盘文件与回调的最小集成夹具；对真实推理路径仍做一次正常回归。

单用户本机 API 不需要网络鉴权测试、多用户隔离测试、租户配额设计或多 Slot 调度设计；保留对内存、文件与 KV 恢复正确性的验证。

## 五、推理流水线影响与兼容性

- **推理前处理、tokenize、prefill、采样及 draft/target 计算**：不变。
- **磁盘缓存有效性**：保留 `is_reusable_entry()`、CRC64、全 token/media 身份校验、`768 KiB/token` 成本守卫和实际 `load()` 的校验逻辑。
- **条目格式**：继续使用缓存文件 v8、manifest v3；无需迁移已有文件。
- **后台写入**：仍由单个 worker 负责持久化；保持前台与磁盘线程间已有锁及事件时序。
- **配额**：quarantined 文件仍占用 `total_size`；不得为了消除错误 `persisted` 状态而直接移除已占用字节。
- **前缀目录**：只修改物理引用登记的资格，不提高观察次数，不改变 `eligible` 判定，不虚增 `reused_disk`。
- **功能关闭态**：`--no-cache-prefix-discovery` 继续保持原磁盘层行为；修复通知资格不应改变模型输出或正常快照的保留行为。
- **不添加多用户/公网 API 安全机制**：本项目仅考虑本机单用户、`-np 1`。
- **不在本次修改中实施新的主动前缀捕获、分块 KV 或序列化格式变更**。发现本问题后，须先完成本轮修复，再单独规划新的算法实验。

## 六、验证检查清单

- [ ] 核对实际工作树的 `change.md`、`projectSummary.md`、提交 SHA、baseline 差异与本次文件影响范围。
- [ ] 审查 `worker()` 所有回滚、隔离和 quota 分支，不允许单凭“路径仍在 `entries`”发出成功通知。
- [ ] 模拟 `!plan.fits` 且回滚删除失败：`total_size` 包含新文件，`entries` 有 quarantined，`load()` 排除该条目，前缀目录 `persisted_refs`/`snapshots` 不增加。
- [ ] 模拟物理回收未兑现、`superseded == 0` 且回滚删除失败：重复上述资格验证，并核查 `n_rollback_failed` / `n_quota_unreclaimed` 的真实递增位置。
- [ ] 两种回滚删除成功的场景：无新条目、无新物理引用、账本核减真实已删除字节。
- [ ] 有效入库：确实发送一次提交事件，`prefix_index` 精确路径与 token/media 匹配，真实缓存 `load()` 可按原路径恢复。
- [ ] 有效新条目已替代短快照但仍因旧隔离文件无法完全回收配额：新条目仍可恢复，删除短条目的失效通知与新条目的提交通知次序一致。
- [ ] 同名快照先失效、后发生回滚失败：最新事件不会把 quarantined 路径重新标为 persisted；必要时检查 `event_epoch` 和 tombstone。
- [ ] 以单元测试或集成测试覆盖“进程内 quarantined 路径不应作为 persisted；强杀重启后其状态也不应被错误登记”，禁止把本轮仅有静态检查的情形记录为已完成动态复现。
- [ ] 已有磁盘配额 19 组测试、媒体匹配测试、前缀索引测试继续执行；单用户 `-np 1` 的冷启动、重复请求、追加请求与重启回归正常。
- [ ] `--cache-stats` 的 `candidate_lcp`、`restored`、`reused_disk` 与前缀目录 `persisted` 分别按真实语义统计；不能将物理文件存在等同于成功恢复。
- [ ] 不更改 v8/v3、CRC、签名、`768 KiB/token`、4096-token lag、模型计算与采样路径。
- [ ] 实施完成后更新 `change.md` 的实际修改日志；`projectSummary.md` 仅写已落地的总体状态。

## 七、记录要求

本文件仅记录本轮代码修复规划与算法修改日志，不编写性能收益设想、早停要求、计算报告或最终判定规则。保留现有实验设计记录，不覆盖历史章节。

本次修复是推进后续单用户公共前缀物理缓存的先决步骤，不在本节规划新的实验代码。完成后，应重新审查 `change.md` 与 baseline 差异，再决定是否进入下一个独立实验。

---

# 2026-11-15 23-38 - test1_3

## 一、实验目标

**实验名称：公共前缀主动捕获与独立 KV 快照持久化（Eligible Prefix Materialization）**

当前单用户缓存系统能够在 radix-tree 元数据目录中识别多次出现的公共 token/media 前缀，也能将已经真实提交的磁盘快照登记为 `persisted`。但是，发现公共前缀 `A` 并不等于磁盘上存在能独立恢复 `A` 的 KV 状态。已有输入/输出边界捕获在完整请求边界工作，无法自动把两个不同请求的公共中间前缀变成独立可恢复对象。

本实验只引入**前瞻式、精确边界的完整序列状态捕获**：在下一次经过已经识别的合格公共前缀时，准确停止本次 prefill 批次，在目标与草稿模型状态均完成到该位置后，使用现有 `llama_state_seq_get_size_ext/get_data_ext` 导出完整序列状态，复用现有磁盘写入、完整性校验、提交/失效与恢复机制保存一个独立前缀快照。

目标是打通如下实际数据链路：

```text
多个请求包含相同的公共前缀 A
    → 公共前缀目录返回 eligible(A)
    → 后续真实请求的 prompt prefill 经过 A 的结束位置
    → 目标/草稿状态在该位置完整、可同步导出
    → 捕获 A 对应的完整序列 KV 状态
    → 正常的磁盘缓存队列与配额事务
    → 磁盘条目真实提交后，目录才登记 persisted(A)
    → 重启后由既有 disk.load() 读取该条目，按 n_past 真实复用
```

**本次不实现任意字节偏移的 KV 分块、差量序列化或新的物理文件格式**。已经存在的 state API 的使用范围仅是完整序列状态的导出/恢复；“API 正确性”指模型状态与 token 位置一致，不是网络/API 权限安全审查。本机单用户、单实例、单 Slot（`-np 1`）为唯一目标范围。

## 二、源码依据与前置核对

编写规划前已按顺序核对远端 `change.md` 的最新 `2026-11-15 23-40 - Change Record`、`projectSummary.md`、实际源码，并与改造前 baseline 对比：

- 本轮审查仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`。
- 远端审查 HEAD：`147001c920436d03688428bf6388db77e75472e3`（文档提交）；最新代码修复提交：`1ab6528464e7cdfbbb43dedf3ef895eab2e6f268`。
- 改造前 baseline：`eb7681889f44304d2bd5b3e41572f5530af91a1d`。
- 已核对 `tools/server/server-task.cpp` 的配额与可替代性路径、`server-disk-cache-quota.cpp` 的提交资格谓词、`server-context.cpp` 的输入边界捕获与 prefill/post-decode、`server-prefix-index.cpp` 的 `candidates()`/`observe()`，以及 `src/llama-context.cpp` 的完整序列 state IO。
- 最新修复的隔离引用撤销、有效条目提交通知、路径保护、删除失败的真实字节记账已进入源码。主功能没有发现阻止开始本实验的新增算法错误。
- **源码一致性整理**：当前 `worker()` 内的直接提交资格使用 `is_reusable_entry(cur)`，而新增 `disk_cache_commit_eligible()` 仅被独立测试直接调用；两者现有谓词等价，但不符合“运行时代码和测试使用同一函数”的原交付叙述。实施时应消除这一重复判定，并把该代码整理与本轮功能改动分别记录。另将提交诊断日志里的字面量 `@NL@` 替换为正确的换行符，确保日志按行可解析。不得因此改变隔离/有效条目的真实提交资格。

实验人员开始修改时仍须先读取**当前本地** `change.md`、`projectSummary.md`、Git HEAD 和未提交差异；如远端再次变化，应先逐函数核查上述基础逻辑，避免覆盖修复。

## 三、实验边界与方案选择

### 3.1 仅捕获真实计算已经完成的前缀

选择**前瞻式 prefill 边界捕获**，而不是在已计算完整请求 `A+B` 后，直接把该序列化字节裁剪成 `A`。

原因：`llama_state_seq_get_data_ext()` 导出的是完整的当前序列内存状态，现有文件格式并未证明字节区间与 token 区间一一对应。只有实际执行到边界 `A` 且其 KV 已完成的时刻，导出的状态才能明确属于该前缀。

禁止以下替代实现：

- 把已有 `A+B` 的 KV blob 按 token 比例截断；
- 把目录的 `eligible` 直接变成 `persisted`；
- 让后台文件线程访问实时 `llama_context`；
- 改变恢复端的 `n_past`、CRC 校验、target/draft 恢复顺序或成本守卫；
- 为了一次捕获在推理线程中主动重算已经跳过的前缀。

### 3.2 触发条件

为每个当前请求的 slot 维护一个轻量的、非持久的 `prefix_capture_target`，至少包含：

```text
request_id
prefix_tokens
token/media identity（读取时用同一请求的不可变 token 视图）
generation/version（防止 slot 被复用后执行旧计划）
state = none | scheduled | reached | exporting | queued | skipped
```

配置建议新增 `--cache-prefix-materialize`，**默认关闭**。只在原本已启用磁盘层与 `--cache-prefix-discovery` 时允许开启；未具备条件则清晰拒绝或说明该开关无效。

从 `server_prefix_index::candidates()` 读取已存在目录的候选，不增加观察次数，不修改目录。选择规则：

1. 候选 `state == eligible`，且 `prefix_tokens >= --cache-disk-min-tokens`、不超过本次输入长度。
2. 优先选当前请求**最长**的一个合法、尚无独立物理引用的公共前缀；单次请求最多安排一次主动捕获。
3. 候选对应的 token/media 身份必须与当前任务真实前缀完全一致，结束位置不能位于媒体块内部。
4. 本次只执行**纯文本输入**的主动捕获；遇到 mtmd 嵌入块、跨越媒体边界、上下文转换/稀疏位置等无法明确界定完整序列边界的情况，记录 `skipped` 并保留原推理路径，不生成伪快照。
5. `prefix_tokens <= n_past` 时，只有已有序列状态**恰好停在**捕获边界、目标与草稿状态确认一致，才能直接捕获；如已经计算越过该位置则跳过，禁止倒退裁剪 live KV。
6. 如果本次请求结束后才首次成为 `eligible`，只允许后续请求执行；不对已完成的前缀进行事后重算或伪造提交。

`--cache-prefix-min-tokens` 仍只决定目录观察资格；磁盘层继续使用独立的 `--cache-disk-min-tokens` 门槛，不为了捕获短前缀而暗中降低原磁盘恢复要求。

### 3.3 真实 prefill 分段与捕获安全点

在请求 token 进入批处理之前，确定目标 `K=prefix_tokens`。若 `n_past < K < task.n_tokens()`：

- 在 `server-context.cpp` 已有 `add_prompt` / batch filling 循环中，不允许单个 prefill 批次跨越 `K`；当 `slot.prompt.n_tokens() == K`，结束本批填充。
- 必须等本批 `llama_process(ctx_tgt, ...)` 成功、`common_speculative_process(...)` 完成必要的 draft 状态更新，且目标/草稿上下文已同步以后，才允许导出。
- 再次校验 `slot.prompt.tokens.size()`、`llama_memory_seq_pos_max()` 与 `size_up_to_pos` 一致地覆盖到且只覆盖到 `K`；若目标/draft 的位置或状态边界不同步，取消本次捕获但继续正常 prefill。
- 在当前推理线程、允许序列化的安全点调用已有完整 state 导出 API。导出完成后恢复原 prefill 流程，从 `K` 继续处理剩余输入；不改变生成采样及后续 output 边界捕获。
- 对原有 checkpoint，只有确属 `K` 及以前、且与实际 target/draft 状态位置相容的检查点才纳入前缀快照。不能让 checkpoint 描述超出 `K` 的内存。
- 任务取消、异常、模型状态导出不完整或 batch decode 失败时，不生成文件、不通知 `persisted`；仍走原有请求错误/清理路径。

实现时优先沿用现有 `capture_boundary()` / `server_slot::prompt_capture()` 内的序列化逻辑，将“从 live 状态获取准确完整序列”抽取为可复用的内部操作。**不能简单在 `post_decode()` 的早退分支后附加代码而遗漏普通中间 prefill 批次**；必须定位能覆盖所有成功中间批次、且 target/draft 都已更新的真实调用点。

### 3.4 独立磁盘对象，不受常规 RAM 长前缀去重误阻

现有 `server_prompt_cache::alloc()` 会在 RAM 中已有更长序列覆盖待捕获前缀时直接返回 `nullptr`，也可能在加入长序列时移除其短前缀。这属于正常会话 RAM 缓存策略，但不能用来决定公共前缀是否有必要形成**独立磁盘对象**。

为本次功能增加独立于普通 RAM 驻留策略的短生命周期捕获路径：

1. 导出 `server_prompt` 的前 `K` 个 token、对应媒体元数据与合规 checkpoint；
2. 使用已有 `server_prompt_cache_state` 兼容的数据载体，暂存目标与草稿完整序列状态；
3. 将捕获对象通过磁盘层已有 `mirror`/写队列接口或等价薄封装入队，由其继续执行 token/media 身份、重复路径、成本守卫、配额事务和真实提交；
4. 排队成功后由后台 worker 持有缓冲区直至写入结束，不允许悬空引用；无需把该独立前缀常驻在常规 RAM `states` 中；
5. 如果正常写入抑制判定认为已有长快照能够经济地替代 `K`，按现有政策拒绝冗余新文件即可；**不能**因此声称新增独立前缀文件，也不能提前登记 `persisted(K)`；
6. 只在真实文件写入、配额准入、manifest 和有效提交资格全部通过后，继续使用原 `notify_committed()` 令目录精确节点转到 `persisted`；删除/隔离仍走原失效通知。

本次不改变磁盘缓存 v8 条目协议和 manifest v3；不增加第二套前缀文件名空间、不引入新的后台队列线程。

## 四、预期修改文件

| 文件 | 修改内容 |
|---|---|
| `common/common.h` | 增加默认关闭的主动前缀物化布尔参数 |
| `common/arg.cpp` | 注册 `--cache-prefix-materialize` 参数及原有缓存开关的组合校验 |
| `tools/server/server-context.cpp` | 候选前瞻查询、slot 级捕获计划、prefill 边界批次切分、decode 成功后的状态同步/捕获及任务结束清理 |
| `tools/server/server-task.h` | 最小的独立前缀快照提交接口或已有内存载体薄封装；必要时加入可观测指标 |
| `tools/server/server-task.cpp` | 复用 `server_prompt_cache_state` 的导出结果进入磁盘正常写队列；避免普通 RAM `alloc()` 的长前缀抑制误挡独立捕获；整理单一提交资格调用与诊断换行 |
| `tools/server/server-prefix-index.h/.cpp` | 原则上仅调用既有 `candidates()` / 提交回调；若需要增加只读的“精确节点是否已有物理引用”查询，保持其无副作用 |
| `tools/server/server-common.h`（必要时） | 将主动捕获指标纳入 `/metrics`，不能与真实复用统计混用 |
| `tools/server/tests/unit/` | 候选选择、媒体边界、重复请求、只捕获一次及清理状态的纯元数据测试 |
| `test-run/` | 增加单用户多分支请求、进程重启、磁盘恢复与关闭态对照脚本 |
| `change.md` | 实施人员真实修改的路径、函数、数据流和回归记录 |
| `projectSummary.md` | 实验实际落地后更新项目总体架构与功能限制；未实现的不写成已实现 |

不修改模型权重、训练程序、网络服务权限、推测采样语义，也不触及多用户/多 Slot 设计。

## 五、数据流修改

### 5.1 原始请求链路保持可用

```text
tokenize → 选槽 / 尝试 slot、RAM、disk 恢复
         → 普通 prompt prefill
         → 输入边界捕获与目录观察
         → 生成与输出边界捕获
```

### 5.2 新增的可选分支

```text
tokenize / 已知本次请求 token-media 身份
  → 查询只读前缀目录 candidates()
  → 筛选 eligible + K ≥ disk_min + 媒体边界合法
  → 保存 request-scoped capture target K
  → 经过既有 slot/RAM/disk 复用后确定 n_past
       ├─ KV 已越过 K：skipped（不倒退状态）
       ├─ KV 恰好在 K：核对状态，执行一次捕获
       └─ KV 尚未到 K：
            ├─ prefill batch 不得越过 K
            ├─ target / draft decode 成功并同步
            └─ 状态恰好覆盖 K，执行一次捕获
  → 原有完整序列 state_get_* 导出 target + draft
  → 使用现有 disk mirror/worker 写盘和配额管理
       ├─ 被原有成本/去重/配额规则拒绝：未 persisted
       └─ 真实提交且有效：notify_committed(K) → persisted
  → 继续正常 prompt prefill、采样及生成
```

与现有目录观察的时序保持独立：`candidates()` 是不增加观察数的只读查询；输入边界的 `observe()` 在原位置仍然执行一次。新物理捕获不应重复增加 `observations`。

## 六、训练与推理流水线影响

**训练流水线：**本项目不涉及训练，训练数据、模型参数与 checkpoint 均不修改。

**推理流水线：**增加一次只读候选筛选、最多一次精确 prefill 分段和一次完整 state 导出。捕获本身属于可选磁盘加速路径，不影响采样 token、DSpine draft 选择、模型权重、模型配置或原缓存恢复条件。目标/draft 的序列 state 导出必须保持原 API 语义；缺失/不一致时不能产生新缓存条目。

缓存可用性统计分层记录：

- `prefix_materialize_scheduled`：已安排，尚不代表文件存在。
- `prefix_materialize_exported`：推理线程已成功导出完整状态。
- `prefix_materialize_queued`：磁盘写队列接收了对象。
- `prefix_materialize_committed`：磁盘层成功准入后真实发布有效引用（只能由成功提交事件增加）。
- `prefix_materialize_skipped`：本次没有物理对象，需区分已经越过边界、媒体、过小、已有经济可用覆盖、队列/预算等原因。

以上指标名称可按现有 metrics 风格落地，但须保持**候选/捕获/物理提交/真实恢复四个层次互不冒充**。真正节省的 prefill 仍只使用现有 `reused_slot/reused_ram/reused_disk` 统计。

## 七、与现有缓存实现的兼容性

1. **默认关闭兼容**：新参数缺省 false 时，不改变原始 batch 分割、目录观察、`capture_boundary` 或 RAM/disk 快照策略。
2. **真实文件兼容**：沿用 v8、manifest v3、CRC64、配置签名、完整 token/media 身份、`768 KiB/token` 读盘成本守卫、4096-token 写入抑制与路径所有权。
3. **回滚隔离一致**：失败删除的文件继续计入 `total_size` 并 `quarantined`，不能因新主动捕获变成 `persisted`。
4. **前缀引用一致**：只在磁盘有效条目提交后通知。既有 `on_snapshot_invalidated`、`event_epoch`、tombstone、启动重建不改变。
5. **RAM 生命周期一致**：新增专用捕获不得修改普通 `states` 的容量裁剪/会话缓存策略，不把短前缀强行设为 RAM 常驻对象。
6. **线程一致**：`llama_context` 的 state 导出仅在推理安全点；后台 worker 仅处理已经完成序列化的数据与文件 IO。单用户也不能忽略这一线程约束。
7. **上下文位置一致**：遇到 context shift、target/draft 位置不同步、checkpoint 超界、媒体块边界或取消任务时不宣称导出成功。
8. **兼容不同配置**：若模型未启用 draft，则只导出目标 state；启用 draft 的当前部署必须同时保留并验证两个上下文的原恢复要求。
9. **不做网络功能**：本机部署不添加鉴权、权限、限流、多租户或公网风险相关功能。
10. **不做原始 KV blob 分块**：不对序列化状态随意 `memcpy` 切片，不改 `state_seq_write_data()` 的协议。

## 八、验证检查清单

### 8.1 修改前审查

- [ ] 复读最新本地 `change.md` 并和 baseline / 本文审查提交核对，确认原配额、恢复、隔离、提交通知和提交资格逻辑没有新问题。
- [ ] 消除 `disk_cache_commit_eligible()` 与运行时资格判定的重复实现；修复提交日志的 `@NL@` 字面字符串；确认等价分支仍由单测直接覆盖。
- [ ] 检查开关默认关闭以及关闭时 batch 构造顺序与原始路径一致。

### 8.2 前缀发现和调度

- [ ] 构造纯文本共同前缀 `A` 与两个不同后缀，观察目录的 `observed/eligible` 变化；候选未达到资格时不能安排捕获。
- [ ] 对符合磁盘最小 token 门槛的最长 `eligible` 前缀安排一次捕获；不增加本次 `observe()` 次数。
- [ ] 同一 slot 后续复用给新的 request_id 时不得执行上一请求遗留的计划。
- [ ] 边界不足最小长度、已有经济可用的物理条目、已越过边界、媒体输入等场景不制造错误文件。
- [ ] candidate 仅为 metadata 时，不得增加 `reused_disk`、`restored` 或 `persisted`。

### 8.3 KV 捕获正确性

- [ ] 将预定边界 K 选在原 batch 中间，确认 batch 不跨越 K，且真正捕获发生在该批 target/draft 成功处理并同步以后。
- [ ] 证明 `slot.prompt.tokens`、目标 `llama_memory_seq_pos_max()`、草稿状态与 K 的对应关系。若不同步，必须记录跳过原因且保留正确的后续 prefill。
- [ ] 对完整 target/draft state 导出检查 `get_size_ext` 和 `get_data_ext` 字节计数一致；不得导出不完整对象。
- [ ] 检查 checkpoint 没有超出前缀 K；不可移除/稀疏序列不允许伪造连续状态。
- [ ] 故障/取消/重试、decode 批次缩减时不会重复捕获或对提前追加但尚未真正计算的 token 做快照。
- [ ] 捕获后生成流程仍正确从原任务位置继续，不发生 token 重复或漏算。

### 8.4 物理持久化与真正复用

- [ ] 在独立缓存目录中以 `A+B`、`A+C`、`A+D` 等请求组合产生合格候选，使用单 Slot、固定配置及真实 target/draft 模型验证单个前缀独立快照的入队与落盘。
- [ ] 只以磁盘 worker 的有效提交作为 `persisted` 依据；排队成功或文件只被写到临时路径不算提交。
- [ ] 检查 `pc-<hash>.bin`、manifest 与实际 token/media 身份一致；未改变旧格式可读性。
- [ ] 强制退出进程并用**完全相同**模型/上下文配置重启，使 slot/RAM 在位状态不再存在，再验证目标公共前缀通过 `source=disk`、`restored` 与 `reused_disk` 真实恢复；不以目录候选数代替磁盘复用。
- [ ] 比较相同输入的原有关闭态与开启态生成输出一致性；确认 `prompt = reused_slot + reused_ram + reused_disk + prefill` 恒等式保持成立。
- [ ] 校验重复分支、较长前缀、配额淘汰、短快照替代、TTL 失效以及隔离删除失败后目录引用同步撤销。
- [ ] 对仍受原读盘成本守卫拒绝的候选，保留其正常拒绝语义；不可通过调大阈值伪装为物理命中。

### 8.5 交付

- [ ] 将实际修改文件、函数、数据流、执行过的单测与真实恢复检查追加到 `change.md`。
- [ ] 将已经实现的新功能与范围更新到 `projectSummary.md`，不把计划项写成已完成。
- [ ] 本文件只保留实验规划与修改日志，不填写未来性能收益、早停规则、计算资源报告或最终判定规则。

## 九、实施纪律

- 本次只实施这一项算法改动，不捆绑分块 KV、增量读写、热点淘汰或多用户系统的工作。
- 不更改模型计算图、采样概率、tokenizer、DSpine 逻辑或训练参数。
- 若实际状态 API 在所需位置无法产出可完整恢复的 target/draft 状态，应如实记录具体 API、函数、位置和失败现象，不允许把不完整 blob 当作快照写入磁盘。
- 不要求重新运行基线训练；对照基于同配置、同请求、相同服务端开关关闭的原有推理路径完成。

---

# 2026-11-16 01-12 - test1_3

## 1. 本轮目标与实施约束

**本节是既有公共前缀主动物化实现的正确性修复和必要回归补充，继续保留本方向全部设计记录；不是新缓存算法实验。** 当前已实现的“前缀目录 → 精确 prefill 安全点 → 完整 target/draft 序列状态导出 → 现有磁盘队列 → 有效提交 → 跨重启恢复”链路继续保留。现在的任务是收紧状态边界和输入边界，不能在尚有明显实现缺口时直接建设 KV 分块或增量存储。

部署边界：单用户、本机、单实例、`-np 1`；不开发登录鉴权、网络安全、多租户缓存隔离或多 slot 调度。**模型状态一致性是算法正确性，不是网络 API 安全机制**，仍属于本轮必检内容。

本轮只处理以下四个问题：

1. draft 状态可能越过捕获前缀边界却未被拒绝；
2. 捕获前缀恰好等于当前完整输入末尾时，slot 状态切换会跳过物化；
3. 多个媒体块之间的合法边界可能被单个粗粒度区间错误拒绝；
4. checkpoint 端点存在需要确定性确认的边界包含关系，防止超出被捕获前缀的检查点进入快照。

另外补齐已有开关依赖说明，不允许静默无效；该配置处理属于兼容性修正，非新算法方向。

## 2. 前置核对与 baseline 对比

本次规划前已先检查仓库 `change.md` 最新 `2026-11-16 01-05 - Change Record`，检查上传的《2026-11-16-handover-v5-prefix-materialize.md》，核对 `projectSummary.md` 以及 GitHub 上相关模块源码：

- 仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`；检查时 `main` HEAD 为 `010c68ad4675cbde1c2c7c303ab5d1ba11459546`（本轮交付报告提交），对应实际功能代码提交 `ab6bac21c1d87e4c63aa9e6e673b0134cb14d61b`。
- 功能起点 `147001c920436d03688428bf6388db77e75472e3`，原始改造前 baseline `eb7681889f44304d2bd5b3e41572f5530af91a1d`。已用提交对比核查新增修改集中于 `server-context.cpp`、`server-task.cpp/.h`、`server-prefix-materialize.*`、参数与文档；原始 baseline 没有主动前缀物化与捕获计划。
- 已检查 `server_prompt_cache::materialize()` 的完整状态导出：使用 `llama_state_seq_get_size_ext/get_data_ext`，检查 target/draft 实际写入字节数与预计大小相等，复用 `disk->store()`。该链路没有实施序列化 blob 的任意 token 切片。
- 已检查 `server_context::plan_prefix_materialize()`、`materialize_prefix()`、prefill 填充循环与 `post_decode()` 的状态迁移；下面三个缺口均定位于新增逻辑。报告所述单用户纯文本主路径已有端到端与关闭态测试，因此不推翻已实现的路径。

编码人员开工**先再次阅读本地最新 `change.md`，再核对工作树与 baseline 差异**。记录本地 `git rev-parse HEAD`、`git status --short`、构建二进制版本、当前 `projectSummary.md`；若远端又有变化，先确认本节缺陷是否已经修复。若发现额外的明显错误，先处理错误，不同时引入新的缓存策略。

## 3. 缺陷 A：draft 序列上界未检查

### 3.1 位置及成因

文件：`tools/server/server-context.cpp`，`materialize_prefix(server_slot & slot)`。

主模型执行：

```cpp
const llama_pos pos_max = llama_memory_seq_pos_max(llama_get_memory(ctx_tgt), slot.id);
// pos_max + 1 == K 才导出
```

草稿模型当前只排除：

```cpp
if (dft_pos_max < 0 || (size_t) dft_pos_max + 1 < K) {
    // reject
}
```

缺少 `dft_pos_max + 1 > K` 分支。即便常规 `common_speculative_process()` 应使两者对齐，也不能把“调用成功”直接等同于“两个 seq 的边界必然相同”；当前接口已经检查 draft 位置，就应检查**完整等值关系**。这是可以直接从分支条件确认的静态正确性缺口，但尚未证明常规测试环境会自然产生 draft 超前状态。

### 3.2 修改要求

1. 导出前计算 `target_end`、`draft_end`，要求当前序列的捕获端点都严格等于 `K`；对 `pos_max < 0`、`pos_max + 1 < K`、`pos_max + 1 > K` 分别处理。
2. **target 已到 K 而 draft 尚未到 K**：不得导出不成对状态；记录缺失同步条件，保守取消本次物化或交由已经存在、受控的状态同步流程完成，不能为了缓存自行重算已算 token。
3. **draft 超过 K**：必须拒绝当前捕获，不能从 draft opaque state 中按字节裁剪；若后续使用了正式的 seq truncate API，也必须先证明该调用不会影响正在执行的真实推理任务，本轮优先拒绝。
4. 当 `ctx_dft == nullptr` 时保持 target-only 原路径，不把草稿检查误作为强制前置条件。
5. 在生成 `server_prompt_cache_state` 之前完成所有边界检查，避免无效状态造成 GiB 级内存申请与文件排队。

建议引入可独立测试的状态边界判定函数，供生产 `materialize_prefix()` 直接调用，单测不要复制另一套相似条件。

## 4. 缺陷 B：当 K 等于输入终点时可能漏掉合法捕获

### 4.1 位置及成因

`tools/server/server-context.cpp` 的 prefill batch 构造和 `decode()` 中 `post_decode()` 之后的物化循环。

当前物化循环仅处理 `SLOT_STATE_PROCESSING_PROMPT`；对 `SLOT_STATE_DONE_PROMPT` 且 `K <= slot.prompt.n_tokens()` 的计划直接置 `done` 并统计 skipped。对于被目录识别出的合法前缀 `K == slot.task->n_tokens()`，最后一个 prefill batch 结束时 slot 正好转为 `DONE_PROMPT`，从而跳过独立物化。现有 `capture_boundary(INPUT)` 可以在某些配置下保存 RAM 状态，但功能默认 `none` 时不能依赖它自动生成独立磁盘快照。

### 4.2 修改要求

1. 不得把 `SLOT_STATE_DONE_PROMPT` 统称“已经越过 K”。要依据**实际 target/draft 状态端点**判断是 `==K` 还是 `>K`。
2. 对 `K == 本次输入终点`，应在模型状态已经准确计算到 K、尚未采样新 token、且 target/draft 同步的安全点捕获一次；复用 `materialize_prefix()`，不得复制第二套状态导出和写盘代码。
3. 需核对与现有 `post_decode()` 内 `capture_boundary(INPUT)`、`observe_prefix()`、`SLOT_STATE_GENERATING` 状态变更的顺序，确保物化发生在生成前且本请求仍持有正确 token/media 身份。
4. 对 `K < 本次输入终点` 的已测中间批次流程保持原状；如果 DONE_PROMPT 时 `pos_max + 1 > K`，必须保守跳过，不能把更长序列写成 K 快照。
5. `n_past == K` 的已修复路径应维持直接捕获，不应再次进入空批次循环；`K == n_input` 与 `n_past == K` 可同时发生，须避免重复导出和双计数。
6. 对取消、任务切换及同一任务的 prompt 重试，利用已有 request-scoped `task_id/armed/done` 防止旧计划在 slot 复用后触发。

## 5. 缺陷 C：多媒体块被当成单个连续媒体区间

### 5.1 位置及成因

`tools/server/server-context.cpp::plan_prefix_materialize()` 当前把 `media_indices.front()` 到 `media_indices.back() + media_token_counts.back()` 作为单个范围，填入 `disk_cache_materialize_request.media_chunk_start/end`。`server-prefix-materialize.cpp` 则只检查这一段。

对于媒体块 `[10, 20)` 与 `[40, 50)`，位于 `K=30` 的真实前缀终点**不在任何一个媒体块内部**，却落在 `[10, 50)` 的聚合区间内部，从而被误拒绝。该问题是过度保守的误拒绝，不能据此声称有错误 KV 被恢复。报告中的真实媒体端到端测试尚未覆盖。

### 5.2 修改要求

1. 将请求侧的单一 `media_chunk_start/end` 改为**有序、互不重叠的真实媒体块区间集合**，来源于当前任务的 `media_chunk_indices()` / `media_chunk_token_counts()`。
2. 对任意 K，只有当存在实际媒体块满足 `start < K < end` 时拒绝。位于块起点、块终点、两个媒体块之间或全部媒体块之后的 K，在这项边界检查中均应视为合法；其能否形成完整状态仍由真实 target/draft 安全点检查决定。
3. 输入校验要求区间端点计算无溢出、块长度非零、范围不越过 token 序列长度、区间排序且不重叠。任何输入结构不合法都应拒绝物化，而不是将其当成无媒体输入。
4. 统一复用已有磁盘缓存 `disk_cache_match_prefix()` 的媒体身份语义；不改变 token 占位符、media ID 或底层多模态编码逻辑。
5. 如当前真实 mtmd 处理路径仍不能保证媒体块之间的 target/draft 状态完全一致，应额外采用保守的“本请求有媒体则跳过主动捕获”保护。只在能证明物化完整状态可恢复之后，才允许真正写盘；不能因为媒体范围检查通过就宣称整个多模态路径被验证。

## 6. 缺陷 D：checkpoint 边界的包含关系需要核查并修正

### 6.1 位置与待证明前提

文件：`tools/server/server-context.cpp::materialize_prefix()`：

```cpp
if (ckpt->pos_max <= (llama_pos) K) {
    snapshot.checkpoints.push_back(ckpt);
}
```

同一方法的 target KV 边界要求 `pos_max + 1 == K`。若状态位置从 0 开始且前缀总计 K 个 token，则实际最大合法 KV 位置为 `K-1`，而条件 `ckpt->pos_max <= K` 可能接纳一个额外位置的检查点。已有通用边界捕获路径也采用近似写法，因此**不能仅凭表面不等式断言当前测试模型已经损坏**；需要结合位置映射、`n_tokens`、`pos_min` 和混合记忆后端说明进行确定性证明。

### 6.2 修改要求

1. 阅读当前仓库的 `server_prompt`/checkpoint 位置元数据和 `llama_memory_seq_pos_max()` 的定义，明确 pos 与 token 数映射，覆盖非零起始位置与 SWA/混合记忆场景。
2. 用实际已验证的 KV 末端 `pos_max`，而不是直接假设 `K` 即最大合法位置，筛选 checkpoint；核对 `ckpt->pos_min`、`ckpt->pos_max`、`ckpt->n_tokens` 均不越过捕获端点。
3. 若确认代码存在 off-by-one，修复物化路径；同时审查常规 `capture_boundary()` 是否有同类问题。若后者确有通用缺陷，只做最小必要修复，不借机改变 checkpoint 策略或引入新存储格式。
4. 在没有可用真实 checkpoint 模型时，构造纯元数据的边界测试，但明确记录“未执行真实模型检查点恢复”，不能将纯单测结论当成真实模型验证。

## 7. 配置依赖与轻量诊断修正

报告已明确指出：`--cache-prefix-materialize` 在未启用 `--cache-prefix-discovery` 或未配置磁盘层时会静默无效。更改为启动参数校验或一次性清晰诊断（任选其一，且遵循现有 CLI 风格）：

- `--cache-prefix-materialize` 开启但前缀目录关闭：说明需要同时启用发现功能；
- 开启但磁盘层不可用：说明需要启用磁盘缓存；
- 开启且两个依赖齐全：正常启动、无需额外日志噪声；
- 功能未开启：默认关闭态的日志与执行路径不应发生变化。

本机单用户部署不需要新增 HTTP 鉴权、权限系统或多租户安全逻辑。本节只修复配置可理解性。

## 8. 预期修改文件

| 文件 | 修改内容 |
|---|---|
| `tools/server/server-context.cpp` | 修复 target/draft 精确边界判断；支持合法 `DONE_PROMPT` 安全点捕获；按真实媒体块集合构造候选约束；收紧 checkpoint 过滤；加必要的依赖提示 |
| `tools/server/server-prefix-materialize.h` | 将媒体范围从单个首尾区间改为多个实际区间；如添加端点状态纯函数，保持结构简单 |
| `tools/server/server-prefix-materialize.cpp` | 修正多媒体块合法边界判断、输入验证和纯元数据拒绝原因；不新增 KV 序列化逻辑 |
| `tools/server/tests/unit/test_prefix_materialize.cpp` | 增加多个媒体块、中间合法边界、target/draft 提前/超前、输入末端 K、上下文错位、checkpoint 边界等针对性测试 |
| `tools/server/server-task.cpp/.h` | 原则上不改磁盘写盘、配额或 v8/v3；仅当单一状态判定函数需要共享时做最小改动 |
| `common/arg.cpp` | 如选择启动时参数依赖诊断，则在此实现；不新增更多用户配置 |
| `test-run/pmat.py` 及现有脚本 | 新增 `K == input_end`、`n_past == K`、多分支请求、重启恢复与取消/重试场景，覆盖异常状态回收 |
| `change.md` | 实现人员只记录真实修改、实际执行的单测/集成检查及仍未覆盖项 |
| `projectSummary.md` | 修复完成后更新整体项目的状态导出边界、输入末端捕获与多媒体块限制，保持现状一致 |

不得为了本轮修复修改 `src/llama-context.cpp` 的 opaque state 格式、目标/草稿模型计算、DSpine speculative decoding、RAM/Disk 容量算法、768 KiB/token 成本守卫或 4096-token 写入抑制。

## 9. 预期数据流变化

### 9.1 捕获计划与 prefill

```text
tokenize → 精确 token/media 候选识别 → 计划 K
     → n_past 与真实 KV 状态定位
        ├─ K < n_past：跳过，绝不回退切片
        ├─ K = n_past：验证 target/draft 恰好到 K → 直接捕获
        └─ K > n_past：以 K 截断成功 prefill 批次
               ├─ slot 仍 PROCESSING_PROMPT → 到 K 后捕获
               └─ slot 已 DONE_PROMPT 且 K=input_end
                    → 在生成前检查两处状态恰好 K → 捕获
```

### 9.2 实际状态检查与物理提交

```text
到达 K → 确认 target_end == K
       → 若有草稿，则确认 draft_end == K
       → 核对 prompt token/media 身份与 checkpoint 范围
       → 由推理安全点调用完整序列 get_size_ext/get_data_ext
       → 复用 disk->store()、配额准入、manifest 和提交通知
       → 仅真实入库的有效条目变成 persisted
```

任何状态不同步只允许取消本次可选物化，不得中断正常已合法的 prefill，也不得写入不一致的 KV 快照；捕获报错若影响底层状态，应沿用原有状态清理/推理错误机制，不自行添加不安全恢复路径。

## 10. 对推理/训练流水线及既有实现的影响

- **训练流水线**：无训练；权重、数据、梯度、训练 checkpoint 完全不变。
- **推理流水线**：只完善新增物化支路的正确边界、合法候选选择和诊断。未启用 `--cache-prefix-materialize` 时，不得新增 prefill 分批、序列状态导出或额外快照入队。
- **DSpine draft**：不得把 draft 位置超过 K 的完整状态与 target 精确 K 状态组合成一个快照。保持原有 target/draft 参数及其状态恢复 API。
- **文件协议**：v8 快照、v3 manifest、CRC64、LoRA/mmproj/config 签名、完整性先校验后恢复全部保留；不需要缓存文件迁移。
- **缓存身份**：公共前缀 `observed/eligible/persisted` 与真实 `reused_slot/reused_ram/reused_disk` 不混算；本轮不通过降低门槛来增加表面命中。
- **生命周期**：继续使用单一磁盘 worker、稳定路径所有权、隔离记账、配额准入和单调事件 epoch；不新增第二套前缀写入/通知系统。
- **适用范围**：本机单用户 `-np 1`；多 slot、多租户、公网鉴权、Linux 不是本轮开发目标。正常关闭态仍为 baseline 行为。

## 11. 验证检查清单

### 11.1 版本、源码、关闭态

- [ ] 复读最新 `change.md`，核实 commit、`git status`、二进制构建版本与当前 baseline；确认变更仅涉及本轮相关文件。
- [ ] `--no-cache-prefix-materialize` 下完整请求 prefill token 分解和输出 sha 与同源码关闭态一致，无物化特征日志和磁盘额外条目。
- [ ] 显式开启物化但关闭任一依赖，产生清晰配置说明，不发生静默假成功。

### 11.2 target/draft 精确状态

- [ ] 用纯函数单测分别构造 target=K、draft=K；target=K、draft<K；target=K、draft>K；target<K、draft=K；target>K、draft=K；仅 target 无 draft 等状态。除合法边界外不得调用完整状态导出。
- [ ] 通过可控的 seq-position 夹具验证草稿“超前 K”确实被拒绝，而不是仅检验“小于 K”分支。
- [ ] 对所有失败分支，确认不生成 `disk->store()`，`exported` 不增加，slot 后续正常请求处理不改变。

### 11.3 输入末端与批次边界

- [ ] `K < n_input` 的既有主路径仍能在 K 的中间批次精确捕获。
- [ ] `K == n_input`、`n_past < K`、捕获发生在 `DONE_PROMPT` 到 `GENERATING` 之前时，可以导出准确的前缀状态，不被误标“越过边界”。
- [ ] `K == n_input` 且 `n_past == K` 的组合不产生空批次、不双重捕获、不执行未必要的重复 decode。
- [ ] `K < n_input` 但状态已经超过 K 时，跳过物化，不把长序列写成短前缀文件。
- [ ] 校验实际新分支与 `capture_boundary(INPUT)`/`observe_prefix()` 的调用顺序、观察次数、已有写盘去重机制的兼容性。
- [ ] context shift、取消和 decode 重试时不残留上一请求计划；不制造一批 0 token 的永续循环。

### 11.4 媒体块与 checkpoint

- [ ] 两个媒体块间存在合法 K 时不会仅因首尾合并区间被拒绝；落入任一真实块内部时必须拒绝。
- [ ] K 恰好等于单块起点/终点、两块之间、多块末尾；媒体信息重叠/乱序/越界；均有确定性结果。
- [ ] 多模态状态可否安全捕获，须通过真实输入/模型检查；未运行真实多模态测试时不能宣称全媒体支持。
- [ ] checkpoint 在 K-1、K、K+1 附近的 pos_max 和 n_tokens 要验证合法性；超出实际 KV 端点的 checkpoint 不能进入快照。
- [ ] 如果当前模型没有 checkpoint，需记录未验证的实际模型类别，不得把元数据单测冒充完整端到端恢复。

### 11.5 磁盘复用和资源

- [ ] 新导出的前缀仍走原 `disk->store()`、真实配额准入、完整性校验与原子写入；不绕过 RAM 时序规则或伪造物理引用。
- [ ] 按相同模型/上下文配置重启，从纯磁盘读取恢复合法 K 前缀，使用 `source=disk` 与 `reused_disk` 核对真实复用；`restored`、`candidate_lcp` 不代替 `reused_disk`。
- [ ] 比较同请求的开启态/关闭态输出、`prompt = reused_slot + reused_ram + reused_disk + prefill`；报告实际测试口径。
- [ ] 对目标/草稿不同步、媒体元数据错误、checkpoint 超界、删除/配额失败，保持原有隔离和回退语义。

## 12. 交付与文档约束

- 开发人员交付实际源码提交、更新的 `change.md`、相关单元与单用户回归记录；只有已经实施的结构写入 `projectSummary.md`。
- 本 `betterModification.md` **仅记录实验规划与算法修改日志**，不预填实验结果、性能预测、早停要求、计算报告或最终优劣判定规则。
- 本次不启动分块 KV、增量写入、范围读取、热点淘汰等新的算法实现；先修复现有前缀物化路径的问题。

---

# 2026-11-16 08-38 - test1_3

## 一、本轮定位与目标

本节是**既有公共前缀主动 KV 物化算法的正确性修复补充**，不启动新的缓存算法实验，不更改分块格式或推理模型。保留前面全部实验设计与实现记录。当前仅针对本机单用户、单实例、单 Slot（`-np 1`）运行，不设计网络鉴权、多用户隔离或跨机器存储系统。

**目标**：消除主动前缀物化中“token 数”和“KV position”被当作相同量比较的实现错误，并确保未经实际恢复验证的多模态输入不会进入物化写盘路径。保留已经通过实际测试的纯文本前缀捕获、配额事务、完整 KV 序列化以及跨重启磁盘恢复能力。

本轮不增加其他缓存候选策略、不改变缓存 token 门槛、不降低读取成本守卫，也不修改 `llama_state_seq_get_data_ext`、`llama_state_seq_set_data_ext` 的底层语义。

## 二、实施前代码核查与 baseline 对比

### 2.1 已查验依据

在提出本轮修复前，首先查阅 GitHub 最新 `change.md` 的 `2026-11-16 03-20 - Change Record（test1_3：物化路径边界收紧）`，随后核对 `projectSummary.md`、上传的 `2026-11-16-handover-v6-materialize-bounds.md` 和仓库实际源码。审查时远端 `main` HEAD 为 `6b35154a05350b5b599aa59a48aea83993e7178d`（后续说明文档提交）；关键功能提交依次为 `7aedd5604bb7564198cd2ebcf4175edd4442fa5f` 与 `d0537f11eda91c6e557cfef988a23b4c9b01e3ed`。

对比了本阶段之前的 `010c68ad4675cbde1c2c7c303ab5d1ba11459546` 以及原始改造前的 baseline `eb7681889f44304d2bd5b3e41572f5530af91a1d`：原始 baseline 无 `server-prefix-materialize.*` 主动物化分支；新增分支负责候选选择、精确 prefill 截断和完整 target/draft KV 捕获，不能把其中新增错误归咎于原始推理框架。原有 `server_tokens::pos_next()` 已明确处理多模态位置膨胀，因此新物化代码必须服从该旧有位置语义。

**报告确认的已落地工作继续保留**：target/draft 成对端点判定、`DONE_PROMPT` 终点捕获、媒体块合法端点筛选、checkpoint 双边界、配置依赖诊断、纯文本实际磁盘恢复及默认关闭兼容性。它们不需要推倒重写。

实验人员动手前必须再次读取实验机器最新 `change.md`，并与本仓库 baseline及本轮提交比较。记录当前 `git rev-parse HEAD`、`git status --short`、构建产物及实际源码 SHA。若修改已经被其他人合并，先核查当前差异，不允许重复覆盖现有修复。若发现更严重的已存在代码错误，先暂停本轮并按错误位置修复。

### 2.2 本轮明确缺陷：媒体 token 数不等于实际 KV position 数

**位置一**：`tools/server/server-common.cpp` 的 `server_tokens::pos_next(int64_t n_tokens)`。

在 `!has_mtmd` 时，`pos_next(K) == K`。存在媒体块时，函数遍历 token 流，每遇到媒体块，会把 `mtmd_input_chunk_get_n_pos(chunk)`（实际模型位置数）加到 `pos`，再跳过 `mtmd_input_chunk_get_n_tokens(chunk)`（token 占位数量）。因此对于一个包含媒体块的公共前缀，一般情况下：

```text
K = prefix token count
P = server_tokens::pos_next(K)       // prefix KV position end, exclusive
P may differ from K
```

**位置二**：`tools/server/server-context.cpp` 的 `materialize_prefix(server_slot & slot)`，目前直接用：

```cpp
const disk_cache_capture_bounds bounds = disk_cache_check_capture_bounds(
        K, (int64_t) tgt_pos_max + 1, ctx_dft != nullptr, (int64_t) dft_pos_max + 1);
```

`disk_cache_check_capture_bounds()` 的首参在现有实现中是 `prefix_tokens`，生产代码因此把 `K` 与真实的 `pos_max + 1` 直接比较。对纯文本有效，对媒体块 `n_pos != n_tokens` 的输入无法正确判断 exact/short/past：可能错误拒绝本应合法的捕获，也可能在特殊位置差异下误把不一致状态视为 exact。此处属于**源码可确认的语义单位错误**，但报告未执行真实 mtmd 测试，不能将“实际错误恢复已发生”写成实验事实。

**位置三**：同一函数的 checkpoint 筛选：

```cpp
disk_cache_checkpoint_fits({ ckpt->pos_min, ckpt->pos_max, ckpt->n_tokens },
        K, disk_cache_prefix_pos_max_text(K))
```

`disk_cache_prefix_pos_max_text(K)` 明确只适用于纯文本，返回 `K-1`。对于带媒体块、实际最高有效 KV 位置为 `P-1` 的前缀，应使用真实位置边界而非文本专用计算。报告本身已指出媒体下 token 数与位置数不同，但当前调用点仍对媒体路径使用文本计算公式，必须修正。

**额外核查**：本次报告提及多媒体块端点“几何合法”不等于真实导出成功。除核对 `pos_next(K)`，还应静态审查从 `slot.task->tokens` 进入 `slot.prompt.tokens`、`keep_first(K)`、以及 prefill 分批与 `n_past` 的语义：前者是 token 索引、后者在当前缓存调度层是复用 token 数，不能因为 `KV position` 膨胀就把所有 token 计数替换为位置数。

## 三、修复方案：统一 token 与位置的显式类型语义

### 3.1 将端点检查改为 KV position 领域

建议在 `materialize_prefix()` 获取请求限定的前缀序列位置：

```cpp
const size_t K_tokens = slot.materialize.prefix_tokens;
const llama_pos expected_end_pos = slot.prompt.tokens.pos_next((int64_t) K_tokens);
const llama_pos tgt_end_pos = llama_memory_seq_pos_max(llama_get_memory(ctx_tgt), slot.id) + 1;
const llama_pos dft_end_pos = ctx_dft
    ? llama_memory_seq_pos_max(llama_get_memory(ctx_dft), slot.id) + 1
    : -1;
```

必须先验证 `K_tokens` 在 `slot.prompt.tokens` 范围内、媒体块完整且状态未发生 context shift 等不兼容变换；随后**仅用位置端点比较位置端点**。对有 draft 的情况依然要求 target/draft 两边严格等于 `expected_end_pos`；target-only 保留单模型比较。若实际 seq position 不连续，或模型当前 memory backend 的 `pos_max` 不足以证明已覆盖完整前缀，则保守拒绝物化。

建议将纯函数接口从混合语义的 `disk_cache_check_capture_bounds(prefix_tokens, target_end, ...)` 调整为命名明确的 `disk_cache_check_capture_position_bounds(expected_position_end, target_position_end, has_draft, draft_position_end)`（也可采用其他等价名称）。`prefix_tokens` 仍用于快照的 token 元数据、文件身份和最小长度判断，不作为真实 KV end。

不能把 `pos_next(K)` 直接拿去截断 token 容器、决定缓存 token 身份或修改 disk LCP：prefill 批次及缓存身份仍按原 token 索引 `K` 进行；只有状态边界与 checkpoint 位置比较按 `expected_end_pos` / `pos_max` 判断。

### 3.2 checkpoint 双边界保持独立

将 checkpoint 检查显式写作：

```cpp
disk_cache_checkpoint_fits(checkpoint_view, K_tokens, verified_tgt_pos_max)
```

`verified_tgt_pos_max` 必须来自已验证 exact 的真实 target memory 位置，而不是无条件调用 `disk_cache_prefix_pos_max_text(K)`。在纯文本无 shift 且从位置 0 开始的既有测试范围内，二者应等价。针对媒体与 position shift 的路径，不因使用新的上界而自动认为状态可复用，仍须单独校验实际 state API 与 checkpoint 的恢复语义。

保留 `disk_cache_prefix_pos_max_text()` 作为只在文本单测或明确文本模式中使用的辅助函数，不要求删除；如果生产代码已不需要它，不得为了制造使用点而继续错误引用。

### 3.3 未验证的媒体实际状态：先采用保守禁用门禁

尽管新的位置比较能修复单位错误，但用户当前的实验设备**没有真实 mtmd 模型与图片输入的端到端验证条件**，不能由纯元数据用例推出媒体+draft 实际导出/恢复一定可行。

因此在本轮修复中加入**默认保守的实际写盘门禁**：当本请求 `server_tokens` 含有媒体块，`plan_prefix_materialize()` 可继续完成只读候选观察与区间合法性判定，但不得 arm 主动物化、不得触发真实 state 导出或 disk store；必须返回明确的 `media_state_unverified`（或已有的准确跳过理由）。在没有真实恢复证据之前不提供“强制实验性写入”绕过选项；将未来真实媒体支持作为单独设计，不在本轮承诺。

注意这项限制**只作用于新的主动前缀物化路径**，不得关闭原有 llama.cpp 多模态推理、RAM/磁盘已知完整快照缓存或前缀元数据观察能力。新功能关闭态行为不变。

### 3.4 补齐流程安全点与异常分支审查

- `n_past == K` 直接捕获与 `DONE_PROMPT` 精确结束捕获共用同一个位置端点判定；保持一次请求至多导出一次。
- 模型当前 KV 已超过预计位置时只能跳过当前物化，不能对已导出的 opaque state 作按 token 字节裁剪。
- 发生 context shift、请求取消、`decode()` 缩批重试、不同 request 的 slot 复用或 target/draft 不同位置时不导出、不排队；保持已有正常前向流程。
- `capture_boundary()` 通用路径已经使用实际 `pos_max` 筛选 checkpoint，不将它改回 `K-1`；仅比较本次新增分支与其位置边界语义的一致性。
- 不修改缓存的 v8 快照、v3 manifest、CRC、文件名、IO 守卫、隔离与配额事务；新物化写入仍只通过原有 `server_prompt_cache::materialize()` 与 disk worker。

## 四、预期修改文件

| 文件 | 具体修改位置 | 修改要求 |
|---|---|---|
| `tools/server/server-context.cpp` | `plan_prefix_materialize()` | 加入“真实媒体输入禁用主动写盘”门禁；保留媒体元数据校验与独立前缀目录观察 |
| `tools/server/server-context.cpp` | `materialize_prefix()` | 用 `server_tokens::pos_next(K)` 建立 expected KV position end；严格检查 target/draft；checkpoint 使用真实位置上界 |
| `tools/server/server-prefix-materialize.h` | capture-bounds 接口与 skip enum | 明确参数单位，区分 token 数与 position；必要时新增 `media_state_unverified` |
| `tools/server/server-prefix-materialize.cpp` | 端点谓词与 planner | 只比较同单位的 position，媒体禁用分支返回准确原因，纯文本路径保持原样 |
| `tools/server/tests/unit/test_prefix_materialize.cpp` | 既有单元测试基础上扩展 | 增加位置数与 token 数不一致、exact/short/past、draft 领先/落后、媒体门禁与 checkpoint 的最小复现 |
| `test-run/pmat.py` | 单用户纯文本回归脚本 | 复用请求中间位置、输入末端、`n_past==K`、重启后的磁盘真正复用；检查新参数默认关闭和媒体跳过日志 |
| `change.md` | 最新条目之后追加 | 仅记实际实现、错误位置与执行过的测试，不预写性能结果 |
| `projectSummary.md` | 现有整体架构说明 | 在实现完成后更新“已验证纯文本/多媒体尚未验证”的真实能力边界 |

如无需新增源文件或参数则不新增，继续复用当前四套单测与现有 CLI 选项。

## 五、预期数据流变更

```text
请求 token 化、媒体识别
    ↓
prefix_index::candidates()（只读，不改变观察数）
    ↓
合法候选 K_tokens
    ├─ 含媒体且未验证 → skipped(media_state_unverified)
    │                      → 原推理与原缓存路径继续，不触发主动捕获
    └─ 纯文本 → request-scoped materialize plan(K_tokens)
                    ↓
                 前缀 prefill 批次截至 K_tokens（token 单位）
                    ↓
                 expected_pos_end = prompt.tokens.pos_next(K_tokens)
                    ↓
                 target_pos_end == expected_pos_end
                 draft_pos_end  == expected_pos_end（如配置）
                    ├─ 任一不等 → skipped，不触碰 disk
                    └─ 两者准确 → checkpoint 的 token 和 position 分别验证
                                     ↓
                                 完整 seq state 导出
                                     ↓
                                 原 disk store / queue / quota / manifest
                                     ↓
                                 有效提交后 persisted；重启真实恢复计入 reused_disk
```

**训练流水线**：不存在训练，不修改模型权重、数据集或梯度。**推理流水线**：修复仅发生在启用前缀物化后的计划与安全点；普通 prefill、speculative decode、采样和关闭态保持不变。`K_tokens` 是被缓存的输入 token 数，`expected_pos_end` 是模型状态位置的 exclusive 上界，两者不得混记或直接相加。

## 六、与既往修改的兼容性

1. 单用户本机 `-np 1`，不增加网络鉴权、多租户权限、多 Slot 调度或分布式功能。
2. 保留 `--cache-prefix-discovery` 与目录的 `observed/eligible/persisted` 生命周期；候选不计入真实命中。
3. 原有纯文本 `K==pos_next(K)` 路径沿用，已经通过的中间 prefill、`DONE_PROMPT`、`n_past==K` 三类捕获继续具备相同语义。
4. 维持 target/draft 成对状态、完整 state 导出、v8/v3 文件格式、CRC、配置签名、checkpoint、`768 KiB/token` 读盘守卫及 `4096 token` 写入抑制。
5. 保留原有隔离/配额/真实删除记账和单一磁盘 worker，不允许通过错误的 `persisted` 引用绕过既有磁盘层校验。
6. 媒体门禁只禁止**新主动中间前缀物化**，不得影响多模态模型本身的推理和已存在的一般 KV 缓存。
7. 关闭 `--cache-prefix-materialize` 时无额外批次划分和状态导出；无理由改变 baseline 输出。

## 七、验证检查清单

### 7.1 版本与源码

- [ ] 实验机重新阅读最新 `change.md` 和 `projectSummary.md`，锁定当前 HEAD、构建产物，并与 baseline 比对。
- [ ] 搜索 `materialize_prefix` 与 `capture_boundary` 中所有 `pos_max + 1`、`K-1`、`pos_next(K)`，确认每处变量单位清晰。
- [ ] 所有纯元数据 helper 由生产路径调用同一份实现，不复制单测专用逻辑。

### 7.2 token-position 映射

- [ ] 纯文本 `K=0/1/2/1024`：`pos_next(K)` 与原纯文本关系一致，K=0 不允许物化。
- [ ] 用模拟多媒体块创建 `n_tokens != n_pos` 的前缀，分别令目标/草稿端点等于、短于、超过 `expected_pos_end`；新的捕获谓词只按真实 position 结果判断。
- [ ] 双向验证 `expected_pos_end`：包含一个媒体块、两个分离媒体块、K 在媒体起点/末端/块间以及 token 索引合法但位置膨胀的情况。
- [ ] 对 metadata 有媒体的请求，即使候选边界合法也不会发起真实物化，不增加 exported/queued/persisted；仍能正常完成原推理请求。
- [ ] 不从媒体 token 数直接推断 checkpoint 最大 position；纯文本下 checkpoint 的 `K-1` 规则仍由原单测覆盖。

### 7.3 边界与实际恢复

- [ ] 纯文本预先建立 eligible 公共前缀，以 `K<input_end` 验证中途批次精准物化。
- [ ] `K==input_end` 与 `n_past==K` 的回归均只导出一次，不出现空批次循环。
- [ ] target/draft 的 exact/short/past 六种状态仍按原判定处理；状态不一致不能导出完整快照。
- [ ] context shift、任务取消与 decode retry 不执行旧请求遗留计划；若环境无法注入，明确记录尚未覆盖，不能声称测试通过。
- [ ] 采用独立磁盘目录，服务重启后从 `source=disk` 真实恢复；核对 restored、reused_disk 与 prompt token 分解恒等式。
- [ ] 关闭新功能后对照同参数相同请求的输出与 token 统计；不改变正常磁盘缓存、RAM 或单 Slot 在位 KV 复用。

### 7.4 报告与交接

- [ ] `change.md` 只写本轮真实修改位置、已执行检查和仍缺少的实测条件。
- [ ] `projectSummary.md` 更新真实能力边界，不能在没有媒体+draft 端到端测试的情况下宣称支持多模态主动 KV 物化。
- [ ] 本文件仅保留实验规划和算法修改日志，不填实验结果预想、训练早停要求、性能计算报告或最终优劣判定规则。

## 八、实施顺序

第一步核查当前最新 `change.md`/源码与 baseline 差异，确认错误尚未被其他提交处理；第二步完成 token-position 语义修复及媒体门禁；第三步补齐确定性单测；第四步运行单用户纯文本请求、实际重启恢复与关闭态对照；第五步将真实变更追加到 `change.md`，同步项目总体结构至 `projectSummary.md`。

**本轮授权范围只到已有主动前缀物化路径的正确性修复**，不开始新的物理 KV 格式、差量写入或范围读取实现。

---

# 2026-11-16 09-35 - test1_3

## 一、实验目标与问题定位

**本节仅修复当前单用户磁盘前缀缓存中的 in-flight 去重身份判定，不授权实施新的 KV 物理格式、分块持久化、压缩、主动捕获策略或其他缓存算法。** 已完成的前缀主动物化、target/draft 序列位置校验、媒体门禁、配额事务及跨重启恢复全部保留。

目标是保证所有“跳过一条待写入快照”的结论均基于**完整 token/media 身份**，而非仅基于 64-bit 内容 hash。此修复针对的是本地缓存数据与写入队列的正确性，不是公网 API 安全，也无需设计多用户鉴权、多租户或多 slot 调度。项目仍只适配本机、单用户、单实例、`-np 1`；但前台请求与后台 writer 的锁同步要求必须保持。

### 1.1 开始前已检查的版本与 baseline

在提出本轮修复前，已先读取仓库最新 `change.md` 的 `2026-11-16 09-30` 条目、上传的 `2026-11-16-handover-v7-token-position-units.md`、`projectSummary.md`，随后逐一核对 GitHub 源码。

- 仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`。
- 本次审查时远端 HEAD：`b4b831c29ece3086fb9d5cf9b849b478990bfc17`（说明文档提交）；功能代码提交：`d57d09c31a3f505ab6117f8ea2fdacf781c7970f`。
- 原始 baseline：`eb7681889f44304d2bd5b3e41572f5530af91a1d`。
- GitHub 对比显示现有缓存写入/恢复、公共前缀物化和媒体位置校验等功能已经落地。此前发现的 token/position 混用已按 position 域修正，纯文本下的独立前缀快照与重启复用有真实测试记录。
- **需要指出本次缺陷的来源**：原始 baseline 已有 `std::unordered_set<uint64_t> inflight`，该问题不是最近才引入的；不过现行代码已经强化“hash 不是身份”的落盘条目判定，却尚未让排队中的条目遵守同一身份规则。因此新旧路径存在可明确定位的不一致。

实验人员开始修改前，应在本地重新检查最新 `change.md`、`projectSummary.md`、HEAD、`git status --short`、当前实际构建二进制，并比较上述 baseline 与待改函数。若现场已修复本问题，需先审查差异，不应重复引入新结构。

### 1.2 已确认的控制流缺陷

**文件与位置**：

- `tools/server/server-task.h`：`server_disk_cache::inflight`（当前约 1005 行）定义为 `std::unordered_set<uint64_t>`；
- `tools/server/server-task.cpp`：`server_disk_cache::store()`（当前约 3933 行）、`server_disk_cache::mirror()`（约 4044 行）均执行 `if (inflight.count(hash)) { n_enqueue_dedup++; return; }`；
- 同文件后台 `worker()` 多条处理分支使用 `inflight.erase(hash)` 释放占位（当前约 4287–4337 行）。

已提交的磁盘条目会使用 `disk_cache_identity_equal()` / `disk_cache_classify_duplicate()` 对 token 序列与媒体三元组逐项核对；**但等待写入或正在写入的条目没有保存可比较的完整身份**。两个不同的 prompt A、B 若得到同一个 64-bit hash，B 进入 `store()`/`mirror()` 时会被误记为“已经在写入”，直接跳过，而不是被判为路径身份冲突。

该行为的逻辑缺陷由源码确认，可以通过向元数据夹具注入相同 hash、不同 token/media 身份稳定重现；**目前没有证据表明用户实际请求中已经发生 hash 碰撞**，不得把构造性边界风险写成已发生的性能损失或错误恢复事件。

此问题的核心不变量为：

> `hash 相同 + 完整身份相同` 才可以视为 in-flight duplicate；`hash 相同 + 完整身份不同` 必须视为同一路径的身份冲突，安全拒绝而不是计入去重成功。

## 二、预期修改文件

| 文件 | 预期修改 | 目的 |
|---|---|---|
| `tools/server/server-task.h` | 将 `inflight` 从 `unordered_set<uint64_t>` 改为保存 hash 到**不可变、拥有自身内存的 token/media 身份**的有界映射；必要时添加私有身份结构 | 保证队列中、worker 已取走但尚未提交的条目都可用完整身份分类 |
| `tools/server/server-task.cpp` | `store()`、`mirror()` 共用同一 in-flight 身份判断；在占位插入/删除和队列入队/出队处保持一致生命周期 | 相同身份正常去重，不同身份明确冲突，任何分支都不丢失占位或错误去重 |
| `tools/server/server-disk-cache-match.h/.cpp` | 仅在确有必要时增加可独立测试的 in-flight 身份分类薄函数；优先直接复用已有 `disk_cache_identity_equal()` | 单测与生产路径共用真正的完整身份判定，不额外创造第二份语义 |
| `tools/server/tests/unit/test_disk_cache_match.cpp` | 追加相同 hash 下的不同长度 token、同长度不同 token、相同 token 不同 media、完全相同身份四种分类测试 | 不依赖制造实际 64-bit 哈希碰撞即可稳定验证判断 |
| `tools/server/tests/unit/` 或 `test-run/` | 添加最小队列/worker 测试夹具：写任务排队、被 worker 取出后、成功/失败/拒绝/取消清理等阶段 | 验证 `inflight` 占位从提交到释放的完整生命周期 |
| `change.md` | 仅追加实施人员实际修改文件、代码差异与已执行的验证 | 保持算法修改日志和验证事实分离 |
| `projectSummary.md` | 修复完成后更新“队列去重也必须匹配完整身份”这一已实现能力 | 项目总体说明只记录已落地事实 |

不修改 `src/llama-context.cpp`、模型/草稿模型、`server-prefix-materialize.*` 的 token/position 语义，不改变文件格式 v8、清单 v3、CRC64、媒体身份编码、`768 KiB/token` 成本守卫及磁盘配额策略。

## 三、算法变更与数据流

### 3.1 保存 in-flight 对象的不可变身份

目前的 `inflight` 仅保留 hash。改造后应保留可独立于 `pending`/`queue` 生命周期存在的轻量身份，例如：

```cpp
struct disk_cache_inflight_identity {
    llama_tokens              tokens;
    std::vector<uint32_t>    media_indices;
    std::vector<uint32_t>    media_token_counts;
    std::vector<std::string> media_ids;
};

std::unordered_map<uint64_t, disk_cache_inflight_identity> inflight;
```

以上为语义示例，允许使用项目现有轻量类型或 shared-owned immutable metadata，**不得在 map 中存放指向 `queue` 的元素指针、`string_view` 或在 worker `pop_front()` 后失效的引用**。不需要复制主/草稿的 GiB 级 KV buffer；这里只保存 token ID 与媒体块身份，长度由现有写队列数量上限约束。

### 3.2 统一 `store()` 和 `mirror()` 的判定

对已经通过现有 `disk_cache_pack()` 产生合法完整身份的待写对象 `pending`：

```text
待写入 prompt → 计算 hash + 提取完整 token/media identity
    → 查询已提交 entries（维持现有完整身份/隔离判据）
    → 查询 inflight[hash]
       ├─ 不存在 → 继续既有覆盖抑制、队列预算和入队
       ├─ 存在，且完整身份相同 → 计入 enqueue_dedup 并跳过
       └─ 存在，完整身份不同 → 计入 hash collision/path conflict，拒绝该次写入
```

- 必须复用 `disk_cache_identity_equal()` 或其现有确定性逻辑，媒体三元组必须逐项完全一致；token 数相同或 hash 相同都不足以认定相同内容。
- 对 hash 冲突只能拒绝当前写入，**不得**在既有 `pc-<hash>.bin` 上覆盖，不得篡改先到达写任务的队列对象；不需要改变 hash 算法或文件命名格式。
- 对相同身份的正常 in-flight 重复请求仍按原 `n_enqueue_dedup` 语义处理。
- 不修改先前已通过的 `covered_cheaply()`、配额准入、提交资格、隔离生命周期与前缀目录通知条件。

### 3.3 占位生命周期覆盖 worker 的整个写入过程

`inflight` 需要覆盖两个阶段：

1. **已在队列中**：`queue.push_back()` 后，别的请求必须能看到该条目的完整身份。
2. **worker 已出队但还没有结束提交/清理**：`pending` 虽已从 deque 移走，`inflight` 的身份仍须存在，直到成功提交或明确失败结束。

每条已有的 `continue`/失败路径都必须释放占位，且释放操作与队列字节计数处于正确的磁盘锁保护之下。还需检查占位插入与 `queue.push_back()` 发生内存分配失败时不会留下“hash 永久占用但没有对应写任务”的状态；可使用小范围 RAII 或显式回滚，不需要另造后台线程。

**并发处理**：只保留现有 `mtx` 及单一后台 writer。若 worker 在提交时从 `inflight` 释放 hash 并登记到 `entries`，必须保证这两个动作在同一个锁保护的不可观测临界区内完成，以免前台看到短暂“既不在 queue/inflight、也不在 entries”的空窗。

### 3.4 与主动前缀物化的兼容

主动前缀物化使用 `server_prompt_cache::materialize()` 导出完整 target/draft 序列，再调用既有 `disk->store()`。本轮不修改它何时在 prefill 安全点执行、如何得到 `expected_position_end`、是否跳过多模态输入，也不改变物化计数；仅让其进入写队列时的“排队中是否已经存在相同快照”的判断更准确。

应补充现有主链路的回归测试，确保 `slot.prompt.tokens.size() < K` 的中间批次仍保持**等待下一批**而非置 `done`；`K == n_past`、`K == input_end` 等安全点仍能正常触发。上述检查是防止影响已实现功能的回归，**不在本轮重新设计捕获状态机**。

## 四、对推理/训练流水线的影响

- **训练**：无训练；不影响模型权重、数据或 checkpoint 训练流程。
- **推理**：tokenize、选槽、KV 恢复、prefill、采样和 DSpine speculative decoding 不变。
- **磁盘写入**：只改变 `store()`/`mirror()` 在已入库条目判定之后、写队列入队之前的 in-flight 完整身份比对；worker 负责保持该身份直到所有结果得到处理。
- **恢复**：仍通过 `server_disk_cache::load()` 校验 CRC、v8 header、manifest 和 target/draft 状态；本轮不修改实际 KV bytes。
- **统计**：碰撞与真正重复必须分开；不增加 `reused_disk`、`restored` 或前缀目录 `persisted` 的伪成功事件。
- **部署**：单用户、本机、`-np 1`，不加入 API 权限、多租户和公网防护。

## 五、兼容性与实施边界

1. 默认配置与已有开关的含义保持不变，不增加新 CLI 参数。
2. 文件命名 `pc-<hash>.bin`、磁盘格式 v8、manifest v3、CRC64、配置签名和 media 身份规则保持不变。
3. 先到达并有效的 in-flight 条目不被碰撞请求替换；后到达的不等价对象返回可诊断的写入拒绝，而非计成功去重。
4. in-flight 身份的存储上限与当前磁盘写队列有界容量一致；不另行持有 target/draft 主状态副本。
5. 现有单调 `event_epoch`、锁内提交/失效通知、路径所有权与隔离记账必须保持。
6. 当前媒体主动写盘门禁不变；不能借这次 hash 修复扩大未验证媒体 KV 捕获范围。
7. 本节只修复已经确认的身份不一致缺陷，不启动新的捕获准入预检、物理分块、压缩、差量写入和范围恢复算法。

## 六、验证检查清单

### 6.1 版本与 baseline

- [ ] 先核对当前实验机的 `change.md`、HEAD、工作树状态、二进制版本与 `projectSummary.md`；审阅 `server-task.cpp`/`server-task.h` 的最新实现，确认问题仍在。
- [ ] 对照原始 baseline 与当前提交，说明 `inflight` 的 hash-only 语义是继承的旧限制，本轮修复只是让它和当前完整身份规则统一。
- [ ] 确认 `store()`、`mirror()` 的代码只在需要时改动，仍共用相同元数据身份判定。

### 6.2 哈希相同但身份不同

- [ ] 在纯元数据测试夹具中**直接注入相同 hash** 而非寻找真实 hash 碰撞；覆盖：不同 token 长度、同长度不同 token、同 token 不同媒体 ID、同媒体 ID 不同起点/长度、完全一致身份。
- [ ] 除完全一致身份外，其余同 hash 分支均不得增加 `enqueue_dedup`；必须返回明确的冲突/拒绝原因，不得修改先到达条目的任务和文件。
- [ ] 真实相同身份的两次入队请求仍正常合并，不重复固定前缀快照写盘。

### 6.3 后台队列状态

- [ ] `queue` 中的 A 在等待时，提交同 hash 且不同身份 B；确认 B 拒绝而非 dedup，A 正常写盘。
- [ ] worker 已 `pop_front()` 但尚未 `write_entry()` 完成时重复以上测试；不能因队列元素已移走而失去 in-flight 身份。
- [ ] worker 正常写盘成功后，身份转移为 `entries` 的已提交身份；没有未持有锁的可观测空窗。
- [ ] 依次注入 write_entry 失败、路径冲突、已被已有条目覆盖、quota 拒绝、回滚失败、worker 停止等分支；核查 map 占位全部按真实任务生命周期释放。
- [ ] 注入入队内存分配失败或模拟异常，确认不会留下 `inflight` 孤儿 hash 和错误 `queue_bytes` 记账。

### 6.4 单用户现有算法回归

- [ ] 运行已有磁盘媒体身份、quota、prefix-index 与 prefix-materialize 四套独立单测。
- [ ] 单用户单 slot 的纯文本 `A+B / A+C / A+D` 前缀物化序列，保留中途批次、`K == n_past` 与 `K == input_end` 的正确流转。
- [ ] 使用现有固定配置重启服务器，确认 `source=disk` / `restored` / `reused_disk` 真实复用；只以真正保存和恢复的文件建立 persisted 引用。
- [ ] 关闭新物化功能后与开启态对照，验证推理输出一致、`prompt = reused_slot + reused_ram + reused_disk + prefill` 恒等式。
- [ ] 本轮不要求真实媒体端到端；现有 `media_state_unverified` 的主动捕获门禁继续生效，且多模态正常推理能力不应受影响。

## 七、交接与记录

将实际文件修改、直接定位到的错误分支、确实执行过的单测/故障注入/端到端检查追加到 `change.md`。只有已实现并验证的总体语义写入 `projectSummary.md`。

本 `betterModification.md` 只记录实验规划与算法修改日志，不填写预想的性能数值、未执行的实验结果、训练早停、计算资源报告和最终优劣判定。保留此前的全部规划记录，当前修复完成后再重新核对最新 `change.md` 与 baseline 差异，评估下一项算法实验。

---

# 2026-11-16 10-20 - test1_4

## 一、实验目标

**实验名称：公共前缀 KV 状态导出前的轻量级缓存准入（Pre-export Materialization Admission）**。

当前公共前缀物化已经能在真实 prefill 安全点导出目标与草稿模型的完整序列状态，并经原有磁盘 worker 写盘、重启后恢复。但主动捕获的执行路径仍是：确定一个合格公共前缀 → 分段 prefill 到准确边界 → `server_prompt_cache::materialize()` 分配并导出完整 target/draft KV → `disk->store()` 才检查磁盘中是否已有同一身份、in-flight 是否重复、是否有可经济恢复的更长条目覆盖请求。

本实验仅针对这一**准入判定发生在昂贵的 KV 状态导出之后**的资源开销。目标是在不修改任何模型计算和真实恢复语义的前提下，把现有磁盘层可确定、只依赖 token/media 身份和已提交条目元数据的判断提前。对于本来就会被已有磁盘条目或在途写入抑制的前缀，不执行无意义的完整序列状态导出、巨型缓冲分配和写入队列准备。

本轮是**一次独立算法优化**，不是 KV 物理分块实验。严禁修改 opaque state 字节格式、降低 CRC 与媒体校验要求、绕开成本守卫，或者把 preflight 认为“已有覆盖”计算成真实 `reused_disk`。仅支持本机单用户、单实例、单 slot（`-np 1`）；不规划鉴权、多租户、并发 slot 调度和网络防护。

## 二、规划前查验和 baseline 差异

### 2.1 已查验的现有修改

本规划编写前首先读取仓库 `change.md` 最新 **`change.md` 中 `2026-11-16 10-10` 的最新变更记录**，然后核对 `projectSummary.md`、上传的《2026-11-16-handover-v8-inflight-identity.md》、相关实现与提交差异。

- 仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`。
- 本轮审查时 `main` HEAD：`4decdf40bb736ede4c344a11872bf36ee57581c3`（交付文档提交）。
- 最近实际功能提交：`12075e4c244c76dd0de84dec07c1e950428e2e8b`。
- 对照原始 baseline：`eb7681889f44304d2bd5b3e41572f5530af91a1d`。
- 对比确认：baseline 已包含磁盘缓存、后台写队列及 hash-only in-flight 集合；当前代码新增了完整 token/media 身份判定、公共前缀目录、主动前缀导出、状态端点 position 检查、配额事务、隔离与提交事件。最新变更将 in-flight 改为 `unordered_map<hash, inflight_identity>`，由 `store()`、`mirror()` 共用同一身份判定，worker 的释放路径已改用统一辅助函数。
- 静态检查 `tools/server/server-task.cpp`：`server_prompt_cache::materialize()` 在调用 `disk->store()` 之前执行 `llama_state_seq_get_size_ext/get_data_ext` 并分配完整状态缓冲；`server_disk_cache::store()` 此后才执行 `disk_cache_pack()`、完整身份去重、`inflight_check_locked()`、`covered_cheaply()` 和队列容量检查。这形成一个明确的可优化顺序，而非未知的性能瓶颈猜测。
- 最新提交未发现需要阻止本轮优化的新明显正确性缺陷。现有故障注入、空闲镜像与媒体端到端测试仍有覆盖缺口；不得在文档中声明其已经通过，也不得借本轮扩大未验证媒体主动捕获的范围。

实验人员动手前需重新查验**当时最新**的 `change.md`、HEAD、工作区状态与 `projectSummary.md`，再对照上述 baseline 和当前功能提交。若源码已变化或出现新错误，先处理实际错误，不能直接覆盖最新实现或混入新的缓存策略。

### 2.2 必须保留的现有不变量

1. 前缀目录 `observed / eligible / persisted` 与实际 `reused_disk` 是不同概念；只有真实有效磁盘文件才具有物理引用。
2. 已提交条目与 in-flight 条目的“同一身份”都使用**完整 token + media 三元组**，hash 只能作为索引键。
3. 长条目是否替代短条目，必须复用当前确定性媒体边界、最小长度和 `DISK_CACHE_MAX_BYTES_PER_TOKEN` 读盘成本规则，使用**已提交文件的真实 `size_file`**。
4. 隔离条目不能参与可恢复覆盖、精确重复或前缀 `persisted` 登记；它仍按真实字节计入配额。
5. 磁盘写入队列、配额准入、删除失败回滚与有序提交通知由现有磁盘层负责；预检查不能私自跳过最终提交校验。
6. target/draft 的序列位置端点统一按 position 比较；输入含媒体时仍遵循 `media_state_unverified` 的保守主动写盘门禁。

## 三、算法变更

### 3.1 在完整 KV 导出之前增加纯元数据准入

在 `server_disk_cache` 增加一个**只读、同步、线程安全**的 `preflight_materialize()`（命名可服从代码风格）。该 API 只接收前缀 token/media 身份，并返回明确的判定原因；不得接收、序列化或修改 `llama_context`，不得依赖还不存在的待写文件大小。

建议语义（仅示意接口，不要求照搬命名）：

```cpp
enum class disk_cache_preflight_result {
    export_needed,          // 未发现可静态确定的拒绝理由，允许继续导出
    identical_committed,    // 已有身份完全相同且可复用的磁盘条目
    covered_by_committed,   // 已有条目按现有媒体+token+成本规则覆盖本前缀
    identical_inflight,     // 同一完整身份正在排队或写入
    hash_path_conflict,    // 同 hash，但 token/media 身份不同
    invalid_identity,      // 无法安全建立元数据身份
    cache_unavailable,     // 当前磁盘层不可用
};

disk_cache_preflight_result preflight_materialize(const server_tokens & prefix) const;
```

具体实现必须：

1. **复用**已有 `disk_cache_pack()`、`disk_cache_hash()`、`disk_cache_identity_equal()` / `disk_cache_classify_duplicate()`、`inflight_check_locked()` 和 `disk_cache_suppresses_write()`，不要重新编写另一套 token/media 匹配逻辑。
2. 将前缀身份的 token/media 元数据在进入缓存锁前构造好；获取现有 `mtx` 后检查已提交 `entries` 与当前 in-flight 表，全部判断在该临界区内完成。不得在持锁时执行任何 KV state API、磁盘文件读写或大规模状态分配。
3. 只有 `is_reusable_entry(e)` 为真时，才可将现存条目作为精确重复或成本合格的覆盖条目。对 `quarantined`、未知文件有效性、路径所有权尚待回收等状态**不可误报为可恢复覆盖**。
4. 对 in-flight 的同 hash 请求，使用完整 token/media 身份区分真正重复与路径冲突。碰撞不是 `dedup`；不得覆盖先到条目。
5. `covered_by_committed` 必须采用与现有 `covered_cheaply()` 相同的媒体边界、最小 token 数、尾部容忍与真实文件字节成本门槛。严禁以“LCP 很长”替代可恢复性判断，或用本次未导出的预估 KV 字节代替现存文件的 `size_file`。
6. 对无法静态确认的情形，返回 `export_needed` 或明确的非覆盖状态；**不凭猜测**磁盘配额、最终 `pending.bytes`、worker 的下一次淘汰结果或文件 CRC 来阻断导出。
7. 预检查仅用于决定**是否值得导出状态**；绝不写 `entries`、不更新目录 `persisted`、不改变后续 `load()` 的真实校验与恢复。

### 3.2 物化路径调用顺序

在 `tools/server/server-context.cpp::materialize_prefix()` 中，保持当前边界安全点与全部端点判定逻辑不变；在已经确认 `K`、目标/草稿位置及前缀 token 视图合法后、执行 `server_prompt_cache::materialize()` 之前，增加 preflight：

```text
目录选出 eligible K
  → 原有 prefill 边界切分与 target/draft 同步
  → 验证 request_id、token 视图、K、expected_position_end 和两处实际状态端点
  → 构造仅包含前 K 个 token 和合法媒体身份的前缀元数据
  → disk.preflight_materialize(prefix_identity)
      ├─ identical_committed / covered_by_committed
      │     → 记录 pre-export skip，不调用完整 KV 导出
      ├─ identical_inflight
      │     → 记录等待队列已有相同快照，不重复导出
      ├─ hash_path_conflict / invalid_identity / cache_unavailable
      │     → 记录准确拒绝原因，不执行重 KV 导出
      └─ export_needed
            → 原有 checkpoint 筛选
            → server_prompt_cache::materialize()
            → 原有 disk->store()
            → 原有磁盘 worker、配额事务、manifest、有效提交通知
```

`preflight` 不修改当前请求的 KV，不改变 `K` 的位置、批次、采样结果、常规 RAM 缓存或用户响应。即便被预检查抑制，也仍是正常推理请求，后续 prefill/生成照常进行。

**竞态处理**：preflight 与后续 `store()` 之间存在时间窗口。`store()`/worker 必须继续执行原有完整身份、in-flight、覆盖和容量判断，作为**最终权威检查**。不得因为 preflight 曾返回 `export_needed` 就绕过正式检查。若 preflight 发现已提交有效覆盖，而该条目稍后被淘汰，仅意味着本次可选物化失去一次缓存机会；不会生成错误 KV 或虚报 `persisted`，后续请求仍可重新触发捕获。可以保留现有事件 epoch/失效通知语义，不需要引入长期“前缀已处理”的全局标记。

### 3.3 直接测试真实生产控制流，而不只测试谓词

已有 `materialize_prefix()` 曾因改变 token 视图长度检查顺序，错误把“本批尚未填到 K”变成永久 skipped，纯元数据单测无法发现。此次新增的 preflight 会再引入一个早退点，因此要将**本次物化状态动作决策**抽为小型纯函数并由生产函数实际调用，避免单测只检查另一份相似实现。

建议决策的输入包含 `armed/done`、当前 request_id 是否匹配、`K`、token 视图已填充长度、`expected_position_end`、实际 target/draft position 端点以及 preflight 结果；输出至少区分：

- `wait_next_batch`：当前 token 视图未填满 K，或 target 状态尚未到达 K。保留 armed，不能置 done、不能调用 preflight 和状态导出。
- `export_state`：token 视图足够、两处位置都恰好结束在 K、preflight 允许导出。
- `skip_export`：已有经济可恢复覆盖、重复在途写入、身份冲突或确定性不可恢复条件。此处只终结本次可选物化，不改模型处理状态。
- `cancel_plan`：请求身份改变、target/draft 超出 K、上下文迁移后不再具备准确捕获位置。不能使用旧 slot 快照。

实现应尽量复用 `server-prefix-materialize.*` 现有纯函数 `disk_cache_check_capture_position_bounds()`，仅抽取当前控制流关键的状态转移，不重构整个 server 状态机；生产逻辑与单测必须通过同一个可测入口。

**不得**在本轮把媒体主动物化门禁解除；真实多模态状态导出/恢复仍需要独立验证才能扩大能力范围。

### 3.4 观察指标与日志口径

在现有物化统计之外，为 preflight 单独记录检查次数、具体跳过原因和允许继续导出的次数（按项目现有 `/metrics` 风格实现，可采用累计计数）：

```text
cache_prefix_materialize_preflight_checked_total
cache_prefix_materialize_preflight_skip_total{reason=...}   // 如现有 metrics 不支持标签可拆为命名计数
cache_prefix_materialize_preflight_export_allowed_total
```

这里的 skipped 仅表示“不再进行**本次额外导出**”，不是磁盘命中、真实已复用 token 或有效快照提交。`scheduled`、`exported`、`cache_disk_entries/persisted_refs`、`restored`、`reused_disk` 继续各按原语义统计。预检查拦截的请求不应误增加 `n_enqueue_dedup`：该值表示实际走到磁盘入队判定的路径；可通过新增的 preflight 原因单独统计。

## 四、预期修改文件

| 文件 | 预期修改 |
|---|---|
| `tools/server/server-task.h` | 为磁盘层新增只读 preflight 结果枚举、方法声明和必要计数；不暴露 live KV 结构 |
| `tools/server/server-task.cpp` | 复用现有 `disk_cache_pack`、`disk_cache_hash`、完整身份判定、`inflight_check_locked`、`covered_cheaply` 实现元数据预准入；原 `store()`、`mirror()`、worker 权威检查保持 |
| `tools/server/server-context.cpp` | 在 `materialize_prefix()` 的精确安全边界之后、`server_prompt_cache::materialize()` 之前插入 preflight；按结果决定是否导出并记录诊断；既有 prefill 批次逻辑不变 |
| `tools/server/server-prefix-materialize.h/.cpp` | 增加最小的物化动作决策纯函数，生产路径直接调用；保持 token/position 两种单位明确区分 |
| `tools/server/server-common.h`（需要时） | 扩展 preflight 专用指标，避免与真实 cache hit 统计混用 |
| `tools/server/tests/unit/test_prefix_materialize.cpp` | 追加 wait/export/skip/cancel 控制流、两种位置端点、`K == n_past`、`K == input_end` 和 preflight 结果分支单测 |
| `tools/server/tests/unit/test_disk_cache_match.cpp` 或新增缓存元数据单测 | 追加已提交精确重复、成本合格覆盖、成本拒绝、隔离文件、同 hash 不同身份、in-flight 真重复与冲突测试 |
| `test-run/pmat.py` 及独立缓存测试脚本 | 覆盖真实请求分叉、重启、重复捕获、队列未提交阶段、关闭态对照及实际导出次数核验 |
| `change.md` | 仅记录开发人员真实实施的算法和代码变更 |
| `projectSummary.md` | 仅在实现完成后更新已存在的预准入模块、数据流和统计口径 |

## 五、数据流与流水线影响

**原数据流**：

```text
eligible K → 精确 K 的 target/draft 安全点
           → get_size_ext + resize(KV buffers) + get_data_ext
           → disk.store() → 身份/覆盖/in-flight 判定 → 队列/worker
```

**调整后的数据流**：

```text
eligible K → 精确 K 的 target/draft 安全点
           → 小型 token/media 元数据 preflight（磁盘锁内只读）
                ├─ 已提交且有效覆盖 / 精确重复 → 不导出 KV
                ├─ 同一身份已在途 → 不重复导出 KV
                ├─ 同 hash 不同身份 → 明确拒绝，不导出 KV
                └─ 无已有可用覆盖 → get_size_ext / resize / get_data_ext
                                         → disk.store() 既有最终校验
                                         → worker / 配额 / manifest / 事件通知
```

训练流水线无变化；不涉及数据集、反向传播、模型权重或训练 checkpoint。推理流水线只在可选主动物化支路中新增一次低成本的只读判断；单用户原 prefill、投机解码、采样、slot/RAM/Disk 实际复用与生成输出路径保持原样。`--cache-prefix-materialize` 关闭时，不能因新增实现而产生额外 KV 导出或额外 prefill 分段。

## 六、兼容性与实现约束

1. 继续使用现有 v8 快照、manifest v3、CRC64、target/draft 完整状态导出与恢复接口；不得增设不同版本的前缀文件。
2. 不改变原 `768 KiB/token` 恢复成本守卫、4096-token 写入抑制、`--cache-disk-min-tokens`、媒体身份/边界和 checkpoint 筛选语义。
3. 不允许 preflight 直接改变 `entries`、`inflight`、队列字节账本、隔离标记、配额、文件路径所有权或目录引用；不允许把 preflight 结果缓存为永不失效的全局准入表。
4. 在 in-flight 和已提交条目查找时使用相同锁，返回值应为拥有自身数据的枚举/结果对象，不能把指向 `entries` 或 `inflight` 容器元素的裸指针带出锁。
5. `store()` 和 worker 仍具有最终决定权，避免预检查与正式检查产生两个不同的缓存身份定义。
6. 如果新预检查不能确定是否已有经济可恢复快照，继续执行现有导出路径，不凭无法核实的 KV 大小、队列未来容量或文件是否会腐坏而推断拒绝。
7. 不能把缺少真实媒体输入验证的主动多模态物化从 `media_state_unverified` 中放行；原有多模态正常推理和既有完整快照行为不改变。
8. 本地单用户 `-np 1` 即可；不开发 HTTP 安全、鉴权、多租户、多 slot 性能或网络访问层功能。

## 七、验证检查清单

### 7.1 前置版本及 baseline 复核

- [ ] 施工前再次检查最新本地 `change.md`、`projectSummary.md`、HEAD、`git status --short` 与当前二进制构建版本。
- [ ] 对比原 baseline 与当前新功能路径，确认本轮没有无关模型计算、磁盘格式、配额或网络改动。

### 7.2 元数据准入及 in-flight 一致性

- [ ] 相同 token/media 身份已经被有效磁盘条目持久化：应在 KV state size 查询和 buffer 分配之前被识别为无需导出。
- [ ] 较长已提交快照能够通过全部媒体、token、最小长度、真实 `size_file` 成本守卫覆盖当前前缀：不重复导出；若成本守卫拒绝，则不能将其视为覆盖。
- [ ] 已提交条目被隔离、删除、TTL 失效或路径被占用：不得被当成可用覆盖或精确成功重复。
- [ ] 同一完整身份在队列中及 worker 已出队但尚未提交阶段：preflight 均能识别为 in-flight duplicate。
- [ ] 同 hash、不同 token 长度或 media 身份：明确报路径冲突，不计入成功去重，不触碰先到条目。
- [ ] 正常无重复的合格前缀：依旧进入现有完整状态导出与磁盘存储路径；不会因为新增 preflight 误阻断。
- [ ] 故障/异常分支中，preflight 不修改 `queue_bytes`、in-flight 占位或文件状态；最终 `store()` 仍能进行相同的权威检查。

### 7.3 控制流真实可测性

- [ ] `slot.prompt.tokens.size() < K`：动作必须是 `wait_next_batch`，计划仍 armed；不得把导出跳过记为永久失败。
- [ ] target 状态尚未到期望 position、draft 尚未同步：保守等待或沿用当前安全拒绝语义，不导出非法状态。
- [ ] target/draft 两者恰好结束在 `pos_next(K)`，且 preflight 允许：只执行一次状态导出。
- [ ] target/draft 已越过 K 或 request_id 变化：不能从更长状态导出标成短前缀，也不能复用前请求的计划。
- [ ] `K == n_past` 时没有空 prefill batch；`K == input_end` 时在生成前捕获；二者均由真正的生产决策函数覆盖。
- [ ] `media_state_unverified` 及 checkpoint 边界不被此次预准入逻辑绕过。

### 7.4 单用户实际推理及日志核对

- [ ] 使用独立缓存目录，构造 `A+B / A+C / A+D` 公共前缀与已有较长物理快照覆盖的请求序列；记录物化调度、preflight 拒绝原因、真实导出、真实磁盘入库四个层次。
- [ ] 在本该被已提交有效快照覆盖的场景，核查完整 KV 导出 API 没有被调用（建议仅测试构建加入轻量函数调用计数，严禁把日志是否出现当成唯一证据）。
- [ ] 已在途同一身份的重复请求不导出第二份状态；同 hash 不同身份使用单元测试/故障注入构造，不依赖自然碰撞。
- [ ] 使用开启/关闭物化同配置请求，检查输出一致性、prompt token 分解恒等式；`reused_disk` 只表示实际磁盘恢复。
- [ ] 让无已有覆盖的公共前缀通过原完整写盘链路，重启服务后验证真实 `source=disk`、`restored`、`reused_disk`，而非仅检查 `eligible/persisted` 数量。
- [ ] 运行现有 match、quota、prefix-index、prefix-materialize 四套独立单测并补充本轮决策控制流测试；真实内存分配故障、worker 停止及媒体端到端若未实际执行须明确记录。

## 八、记录范围

本文件只记录本次独立实验的目标、修改位置、算法与数据流、对推理/训练流水线的影响、与既有实现的兼容性及验证检查清单。不写假定收益数字、尚未发生的实验结果、训练早停要求、计算报告或最终优劣判定规则。

开发人员将**实际修改**记入 `change.md`，将**项目已落地的总体结构**写入 `projectSummary.md`；本规划不能替代实际代码提交或执行日志。

---

# 2026-11-16 11-25 - test1_5

## 一、实验目标与技术定位

**实验名称：面向完整 KV 序列状态的可逆分段压缩存储（Segmented Lossless Snapshot Codec）。**

当前单用户磁盘提示缓存已经能够在合法的 prefill 边界导出 target/draft 的完整序列状态，通过已提交缓存文件跨重启恢复；导出之前的元数据准入也已能避免确定无效的完整 KV 导出。但对于真正需要写盘的状态，现有 `server_disk_cache::write_entry()` 仍将 `main`、`drft` 与每个 checkpoint 的 `spec/tgt/dft` 字节缓冲区直接原样存盘；`read_entry()` 则从文件读回完整主/草稿状态，并仅读取本次恢复所需的一个 checkpoint。

本实验只改进**磁盘上如何无损保存和取回上述 opaque 数据**。目标是探索是否能够在不改变 tokenizer、模型计算、DSpine 投机解码、完整序列状态 API、前缀身份或可恢复 token 语义的前提下，让磁盘条目按独立段选择无损压缩。压缩必须完全可逆：传入 `llama_state_seq_set_data_ext()` 的每一字节都与当前 `llama_state_seq_get_data_ext()` 的原始输出一致。

**不进行 token 范围 KV 切割。** 本节中的“段”只表示已有文件中自然独立的字节载体：`main state`、`draft state` 以及各 checkpoint 的 `spec/tgt/dft`，不是从 opaque state 内推断 token 边界，也不支持只恢复某个 token 子区间。本机单用户、单实例、`-np 1` 是唯一部署范围；不涉及鉴权、多租户或公网服务。

## 二、规划前核验与 baseline 对比

在制定本实验前，已首先检查远端 `change.md` 最近的 `2026-11-16 11-10 - Change Record`，随后阅读《2026-11-16-handover-v9-preflight-admission.md》、`projectSummary.md`，并核对实际 GitHub 源码。审查时仓库 `A-lucky-boy/llama.cpp-dspine-diskCache` 的 `main` HEAD 为 `a85e66c870030eefe25bbf58b87cbc2c1eeff698`（交接文档提交），功能代码提交为 `e004f717f3b0a12069325fe964d3e26c8718967e`；原始项目 baseline 为 `eb7681889f44304d2bd5b3e41572f5530af91a1d`。

当前源码核对结论：

- `tools/server/server-context.cpp::materialize_prefix()` 在 target/draft 位置精确到达边界后调用 `server_disk_cache::preflight_materialize()`，只在准入结果需要导出时进入 `server_prompt_cache::materialize()`。生产路径直接使用 `disk_cache_decide_materialize_action()`；无明显阻断性代码缺陷。
- `tools/server/server-task.cpp::server_prompt_cache::materialize()` 仍使用 `llama_state_seq_get_size_ext/get_data_ext` 生成完整 target/draft 字节串。磁盘队列的 `server_disk_cache_pending` 通过 `shared_ptr` 持有这些不可变缓冲，不通过实时 `llama_context` 写盘。
- `write_entry()` 当前 v8 文件含 magic/version、配置签名、packed token/media 身份、主/草稿大小、checkpoint 数量、时间、CRC64；随后依次原样写出 `main`、`drft` 和 checkpoint 的 `spec/tgt/dft`。文件先写 `.tmp`，再提交目标路径。
- `read_entry()` 校验 packed 身份与 CRC，再把完整主/草稿缓冲读入内存；对于 checkpoint 已经通过偏移跳过本次恢复不使用的段，且只将选择到的 checkpoint 数据读入并校验。新增编码不能破坏这一特性。
- 原始 baseline 已具有完整序列状态文件机制；当前相较 baseline 的新增能力集中于前缀捕获、物化准入、完整身份、可替代性与磁盘生命周期治理。本实验不重新实现这些机制，只增加文件内部的可选无损编码与兼容读写。

**实验机开工硬性核查**：先读本机当前 `change.md`、`projectSummary.md`，核对 `git rev-parse HEAD`、`git status --short` 与实际编译二进制版本；再次逐函数比较 `write_entry/read_entry/scan/load/touch_entry` 和 baseline 对应路径。若发现现有修改有新的明显错误，应立即优先修复并记录，暂停本实验的物理格式修改。此前交付中尚未直接完成的导出 API 调用计数验证须通过测试夹具补齐，不得把间接计数当作直接证明。

## 三、单项算法变更：按自然状态段进行可逆压缩

### 3.1 编码单元与判定

使用**无损、可流式处理**的块压缩器作为唯一新算法变量，优先评估 Zstandard（Zstd）实现。编码器必须显式声明固定版本/配置；不要依赖机器隐式安装的随机 DLL。开发人员首先核对当前 Windows/CUDA 构建是否已有可链接且许可证兼容的压缩实现；如需集成依赖，必须固定版本、通过项目现有 CMake/构建脚本显式链接，并保证无编码器时原 v8 路径仍能构建。

以下每个段**独立压缩或原样存储**，每段有自己的原始字节长度、磁盘字节长度、编码器类型以及 CRC：

1. 当前 target 完整序列状态 `main`；
2. 当前 draft 完整序列状态 `drft`；
3. 每个 `checkpoint` 的 `data_spec`；
4. 每个 `checkpoint` 的 `data_tgt`；
5. 每个 `checkpoint` 的 `data_dft`。

当压缩后的段连同必要的编码元数据**不小于**原段时，该段可以按原始方式存储；允许同一快照同时包含原始与压缩段。是否压缩只影响编码字节，不改变 checkpoint 的数量、顺序和状态含义。

每段独立编码尤为重要：`read_entry()` 只选择一个 checkpoint；不得把所有 checkpoint 串联进同一压缩流并要求解压不相关 checkpoint 才能取出目标。对 `main` 和 `drft` 也分别编码，不将不同模型上下文混成一个不可独立校验的流。

### 3.2 文件协议与兼容性

- 引入**明确的新条目格式版本 v9**（仅对编码开启时的新写入使用）。**默认配置继续写 v8**，因此关闭功能时与当前源码的文件布局和物理恢复路径等价。
- 新格式的固定头继续包含：magic、version、配置签名、完整 token/media packed 元数据及其 CRC、源数据段的原始长度、时间字段和源数据 CRC。另增加每段的编码器编号、原始长度和磁盘长度，保证读取端能无歧义定位全部 payload。
- `pc-<hash>.bin` 的名称算法不变；同一身份在磁盘已存在时继续由 preflight/store/worker 的现有去重逻辑处理，不尝试为了将旧文件升级为压缩格式而覆盖它。
- 对 v8 文件必须继续按原来的无压缩读法恢复；新版本读取代码不得把 v8 字节解释成 v9 描述。`scan()`、`read_entry()`、`touch_entry()`、配额与日志必须明确区分版本，不因默认写入策略关闭而无法读取已经存在的 v9 文件。
- manifest 仍优先使用 v3：它继续记录**原始 main/draft 长度**、文件物理大小、packed CRC 和身份；只要启动扫描与读盘可无歧义识别版本且所有原索引不变量成立，就不需要为本实验擅自升级 manifest。如果经实际源码审查发现其字段无法承载必要信息，先说明冲突位置，最小增补版本支持，不能静默重解释旧字段。
- 注意 `touch_entry()` 对 `last_used` 的偏移读写：新布局如能保留旧前置字段顺序，就保持该字段位置；否则必须为 v8/v9 独立解析偏移，避免写时间戳覆盖 CRC 或段表。
- 不对 v8 目录执行原位迁移。不同编码写策略可在单目录内读已有合法条目，但正式对照应使用独立目录，避免“先有同 hash v8 条目”使新压缩试验无法入库。

### 3.3 写入数据流

```text
有效前缀边界 → 既有 preflight → 完整 target/draft 状态导出
  → 既有 store()/inflight/队列（RAM 持有的是原始 payload）
  → 后台 worker 通过原路径所有权与冗余检查
  → 将每个自然段独立进行无损编码（流式/有限工作缓冲）
  → 写入 v9 段描述 + 编码 payload 到 .tmp
  → 根据真实文件 size_file 执行既有配额准入/替代判断
  → 完成提交、manifest、回调与可恢复引用登记
```

写侧必须保留以下既有约束：

1. 编码阶段只能访问 `server_disk_cache_pending` 已经持有的不可变数据，不允许 worker 访问 live `llama_context`。
2. 压缩使用有界流式工作缓冲；不能一次额外复制整个数百 MiB `main/drft` 段进行“先压缩到内存，再整体写入”的巨量分配。
3. `pending.bytes` 继续按**原始状态内存大小**计算队列内存占用；不允许因为磁盘可压缩而低报内存或预先突破入队预算。现有写入前的保守 `limit_size` 校验可保持原义；本轮只采用成功提交后实际 `size_file` 作配额与读盘成本判定。
4. 所有 `CRC64` 必须明确绑定到**原始解压后的字节**；可另外校验编码片段的封装 CRC，但不得以“压缩流有效”替代原始状态完整性校验。
5. 如果压缩失败、编码器不可用或输出发生错误，采取明确的写入失败路径并清理临时文件；不能提交只有部分段的 v9 文件。
6. 文件仍用临时路径完整写入、刷新并原子提交；压缩编码不允许绕过当前 `claim_path_locked()`、回滚隔离、`event_epoch` 或 `notify_committed()`。

### 3.4 读取与恢复数据流

```text
磁盘候选选择（完整 token/media 身份 + 原现存 size_file 成本守卫）
  → read_entry() 检查文件头与版本，读取 packed 元数据并校验 CRC
  → 按段表定位 main/drft 以及所有 checkpoint
  → main/drft：读取物理段，解压到完整原始缓冲并校验原始 CRC
  → checkpoint：选中需要的一项，仅读取/解压该项的 spec/tgt/dft
  → 所有必需段通过校验后，沿用原 set_data_ext 恢复
  → 原有 draft/target、checkpoint、n_past 与输出逻辑保持
```

1. **不得先恢复 KV 再做校验**：包括解压失败、CRC 不匹配、声明长度与实际输出长度不符、分段偏移越界、codec ID 不可识别等情况，一律在 `set_data_ext` 之前拒绝该候选，沿用现有安全回退。
2. 对 `raw_len/stored_len/segment_count` 做文件长度、整数溢出、项目内存预算与协议上限检查，避免损坏头造成错误分配。不依赖网络攻击场景，保护的是本机输入/缓存文件结构正确性。
3. 不能因“只解压一个 checkpoint”而改变原选择规则；对未选中的 checkpoint 只作元数据范围检查与 `seek`，不强制读取/解压，保持已有按需读取能力。
4. `read_bytes` 统计**本次真正从磁盘读取的压缩字节和相关元数据**，与 `file_bytes` 的完整物理文件大小区分。不可用“解压后的原始长度”冒充读盘 IO；`restored` 与 `reused_disk` 仍按真实 token 复用计算。
5. 已存在快照的 `disk_cache_cost_ok()` 以实际压缩后的完整物理 `size_file` 与可用 token 数判定，仍使用现有 `768 KiB/token` 门槛。这一门槛不因压缩实验调整。

### 3.5 控制变量与可观测性

建议新增可选配置：

```text
--cache-disk-codec none|zstd       # 默认 none；none = 保留 v8 写盘
--cache-disk-zstd-level N         # 固定、显式并受限的 level；只在 zstd 模式使用
```

本实验只改变一种机制——物理状态段是否经过可逆编码。参数水平是同一机制的实现参数，不得在同一改动中另外调整 `--cache-disk-min-tokens`、4096-token 写入抑制、热点淘汰、前缀发现频率、target/draft 模型配置或 KV 量化方式。

新增观察口径（使用已有统计框架或受控测试日志）：`raw_bytes`、`stored_bytes`、`codec_bytes_read`、`encode_us`、`decode_us`、`codec_fallback_raw_segments`、文件格式版本；现有 `cache_disk_bytes` 必须继续代表实际物理占用。诊断值不得改动 `reused_disk`、`restored` 或 `candidate_lcp` 的定义。

## 四、预期修改文件

| 文件 | 预期修改 |
|---|---|
| `common/common.h`、`common/arg.cpp` | 新增默认关闭的 codec 配置和受限级别参数；与已有缓存命令行风格保持一致 |
| `tools/server/server-disk-cache-codec.h/.cpp`（新增） | 定义独立段描述、codec ID、流式编码/解码与无损校验辅助；不接触模型推理状态 |
| `tools/server/server-task.h` | 扩展必要的私有 v9 文件段元数据及统计；保持 token/media 身份、pending 原始内存预算语义 |
| `tools/server/server-task.cpp` | 调整 `write_entry()`、`read_entry()`、`scan()`、`touch_entry()` 及文件版本识别；在原磁盘 worker 提交路径接入编码和解码，保留 CRC、配额、路径所有权与隔离语义 |
| `tools/server/CMakeLists.txt` 与相关依赖构建文件（按实际仓库确认） | 添加固定版本 codec 依赖、编译选择与 Windows/CUDA 链接，保证默认关闭态可构建 |
| `tools/server/tests/unit/` | 新增独立段 codec 与 v8/v9 兼容读写测试，包含损坏头、长度边界、CRC、未选 checkpoint 跳过等用例 |
| `test-run/` | 追加单用户、单 Slot 的真实 KV 快照纯文本对照、混合版本重启以及读盘恢复脚本 |
| `README.md`、`README_EN.md` | 仅在功能已实现后记录具体开关、依赖、格式兼容与已知限制 |
| `change.md` | 实施人员填写实际代码修改与完成的检查 |
| `projectSummary.md` | 仅在落地后更新当前总体结构、格式和数据流；不写未实现承诺 |

## 五、与既往变更的兼容性

- 原始 v8 主/草稿完整状态与 checkpoint 恢复必须继续工作。新写 v9 仅在 codec 显式开启时发生；功能关闭后原始序列化 API、文件写入和模型输出路径保持不变。
- `server_disk_cache::preflight_materialize()`、`committed_check_locked()`、`inflight_check_locked()`、`covered_cheaply()`、`disk_cache_can_replace()`、配额事务与同步提交/失效通知不改身份和状态语义。
- 目标和草稿恢复仍必须消费与原 state API 完全相同的原始字节；checkpoint 的保存范围/选择逻辑不变。
- 目录只登记真实、完整、已提交并符合有效资格的条目；codec 编码成功不等于已经 `persisted`，预检查跳过更不是缓存命中。
- `server_tokens::pos_next(K)` 与位置端点判定不改；媒体主动捕获仍处于 `media_state_unverified` 门禁，不能借此实验解除。
- 多媒体输入的既有完整缓存恢复保持原有规则，但本实验在没有真实媒体环境时不得宣称其新格式兼容性已由端到端验证。
- **不新增多用户、公网 API、安全鉴权或多 slot 调度能力**。

## 六、验证检查清单

### 6.1 开工前

- [ ] 确认当前 `change.md`、`projectSummary.md`、源码 HEAD 与原 baseline 差异，以及本地构建产物实际版本。
- [ ] 验证 `--cache-disk-codec none` 的 v8 写入与原始二进制布局、元数据 CRC、last_used 偏移、manifest v3 与缓存命中一致。
- [ ] 为已存在的导出前预准入分支提供一次**直接的测试构建函数调用计数**：被 preflight 拦住的路径不得进入 `llama_state_seq_get_data_ext()`；不能只以累计导出日志作为证明。该检查不属于新算法变量。

### 6.2 单段纯函数和文件布局

- [ ] 空段、短段、不可压缩段、可压缩段、混合 codec/raw 段的编码-解码逐字节完全相等。
- [ ] 多个 checkpoint 独立段：只选择其中一个时，其余段无需读取/解压，但段偏移与边界必须可验证。
- [ ] 固定元数据与 CRC 的 v8 样本由新读端正常识别和恢复；v9 样本由新读端按段表独立校验，不尝试使用 v8 字段偏移解析 v9。
- [ ] 新版时间戳 touch 不覆盖大小、CRC、段描述；读写时间字段前后的文件内容按预期不变。
- [ ] 当压缩数据损坏、部分写入、段长溢出、codec 不认识或 CRC 不一致时，拒绝恢复且不得先写入 target/draft 上下文。

### 6.3 真实 KV 状态与跨重启恢复

- [ ] 固定相同模型、draft、KV 类型、上下文与输入序列，分别在独立目录中运行旧版 v8 和新 v9 的相同请求；每条路径在关闭/开启物化时保持原有 token 分解口径。
- [ ] 首次真实导出后检查 raw/stored 物理字节来源、CRC、段数；以文件系统实际大小核对 `size_file` 和 `cache_disk_bytes`。
- [ ] 强制退出并重启，对 v8、v9 及同目录混合版本进行实际 `source=disk` 恢复；`restored`、`reused_disk` 和 `prompt = reused_slot + reused_ram + reused_disk + prefill` 仍符合原定义。
- [ ] 在输出对照中检查逐字节生成结果一致，且 `set_data_ext()` 接收的解压后原始 KV payload 与 `get_data_ext()` 导出的 bytes 一致；不得仅比较最终模型输出。
- [ ] 核对 v8/v9 的 `read_bytes` 为真实物理读取字节，`decode_us` 仅表示编码器恢复耗时，与磁盘 IO 分离。
- [ ] 比较正常 prefill、正常重复、公共前缀分叉、磁盘配额/TTL 淘汰、文件被 Windows 句柄占用后回收、强杀重启等路径；不改变原文件所有权与删除失败隔离机制。

### 6.4 checkpoint、媒体和异常

- [ ] 构造多个有效 checkpoint，分别恢复可选择首个、中间、最后一个的请求，验证 checkpoint 选择与恢复所需数据等价；没有真实 checkpoint 的环境必须说明未验证，不得用纯结构样本代替真实模型证据。
- [ ] 验证原始/压缩段混存、未选 checkpoint 的 seek、解压失败后完整状态回退、rollback 删除失败和配额拒绝时的 manifest/目录状态。
- [ ] 媒体身份元数据、边界与完整缓存不变量仍被严格检查；真实 mtmd 模型不可用时，保持门禁且明确注明未做真实端到端。
- [ ] 不调用任何任意 token 区间的 KV blob 切片；禁止因低成本读盘而绕过 checkpoint、CRC 或 target/draft 完整恢复校验。

## 七、交付记录与范围

- 本文件只记录**当前这一个算法修改**的实验规划和修改日志，不写预想提升值、训练早停、计算资源报告、最终性能裁决规则。
- 本项目无训练过程；训练流水线不变。推理仅增加对既有完整状态字节的无损分段编码/解码，关闭态不受影响。
- 实验人员在实际改动后追加 `change.md`；`projectSummary.md` 只描述真实已实现的架构和格式。
- 若编码实验需要修改 llama.cpp 底层状态导出 API 才能实现，立即记录阻塞位置，保留既有 v8 缓存路径，不在同一实验中转向另一套 KV 状态切分算法。
