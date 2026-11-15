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
