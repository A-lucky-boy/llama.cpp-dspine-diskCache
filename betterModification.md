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
