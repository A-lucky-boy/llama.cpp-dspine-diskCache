# llama.cpp-dspine-diskCache

[English](README_EN.md) | 简体中文

基于 [llama.cpp](https://github.com/ggml-org/llama.cpp) 的定制分支，面向 **Qwen3.8 27B** 长上下文本地推理：在双消费级显卡上跑投机解码，并让长前缀的 KV 状态跨重启、跨会话复用。

目标硬件：RTX 4070 Ti SUPER 16GB + RTX 3090 24GB（单机双卡，层切分）。生产用途是本地 agent 后端。

## 特性

| 能力 | 说明 |
|---|---|
| **DSpine 投机解码** | `--spec-type draft-dflash`，1.2 GiB 草稿模型批量预测，目标模型只做一次校验 |
| **两级前缀缓存** | RAM 提示缓存 + 磁盘 KV 状态缓存；长前缀命中后跳过 prefill，跨重启有效 |
| **完整性体系** | 配置签名、CRC64-ECMA 校验、启动清单索引、损坏条目隔离、LoRA 感知 |
| **写入抑制** | 存量条目已覆盖绝大部分前缀时跳过重写，避免机械盘上的 GB 级重复写入 |
| **可观测性** | 分级日志覆盖命中、恢复、落盘、跳过写入、淘汰与草稿解码形态 |

缓存层不改变任何计算结果：磁盘里存的是当初算出的 KV 字节，命中即原样读回。

---

## 1. 环境与依赖

| 项 | 版本 |
|---|---|
| 编译器 | MSVC 14.44.35207（v143 工具集） |
| CUDA | 12.8 |
| 构建 | CMake ≥ 3.14 + Ninja |
| 显卡架构 | `86-real;89-real`（RTX 3090 / RTX 4070 Ti SUPER） |

CUDA 12.8 的 `nvcc` 不接受 MSVC 14.50，编译器路径必须固定到 14.44。

模型文件不随仓库分发，放在部署目录的 `localModel/` 下：

```
Qwen3.8-27B-pi-Q4_K_M.gguf          # 目标模型，64 层，head_count 24 / kv 4 / head_dim 256
Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf  # DSpine 草稿模型（block_size 16）
mmproj-Qwen3.8-27B-pi-BF16.gguf     # 视觉投影，可选
```

## 2. 构建

```bat
build-cuda-shared.bat
```

脚本完成 configure 与增量构建。**产物直接写入 `output/`** —— `CMakeLists.txt` 把 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 与 `CMAKE_LIBRARY_OUTPUT_DIRECTORY` 指向该目录，无需再拷贝。脚本只额外补上不参与构建的 CUDA runtime DLL。

```
配置  Release, BUILD_SHARED_LIBS=ON, GGML_CUDA=ON,
      LLAMA_CURL=OFF, LLAMA_BUILD_TESTS=OFF, GGML_NATIVE=OFF
```

`output/` 是交付布局：含 47 个 `.exe` 与 19 个 `.dll`，在版本控制中只保留空目录（`.gitkeep`）。核心三个：

| 文件 | 作用 |
|---|---|
| `llama-server.exe` | 服务启动器 |
| `llama-server-impl.dll` | 服务实现；磁盘缓存、LoRA 守卫等逻辑位于此 |
| `llama-common.dll` | 公共参数与命令行解析 |

`llama-server.exe --version` / `--help` 可验证产物与参数注册。部署时把 `output/` 中的 exe/dll 同名覆盖到部署目录即可，不需要改模型或参数。

## 3. 运行

生产配置的完整参数（`--cache-disk` 指向缓存盘，其余为固定项）：

```bat
llama-server.exe ^
  -m localModel\Qwen3.8-27B-pi-Q4_K_M.gguf ^
  -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fit off -fa on ^
  -np 1 -c 262144 -ctk q4_0 -ctv q4_0 -t 8 -tb 16 --backend-sampling ^
  --spec-type draft-dflash ^
  --spec-draft-model localModel\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf ^
  --spec-draft-n-max 6 --spec-draft-n-min 0 ^
  --spec-draft-device CUDA0 --spec-draft-ngl all ^
  --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
  --kv-unified --cache-prompt --cache-ram 25600 --cache-idle-slots ^
  --cache-disk E:\llama.cpp.cache --cache-disk-max 204800 ^
  --cache-disk-min-tokens 2048 --cache-disk-idle-flush 60 ^
  --mmproj localModel\mmproj-Qwen3.8-27B-pi-BF16.gguf --mmproj-device CUDA0 ^
  --jinja --reasoning on --reasoning-format deepseek-legacy --reasoning-preserve ^
  -b 4096 -ub 1024 -lv 3 --api-key sk-0 --host 0.0.0.0 --port 18080
```

部署目录外的启停脚本（不在本仓库）在此基础上额外做三件事：从 GGUF 导出 chat template 并注入 `reasoning_effort` 兼容前缀、校验模型文件存在、后台拉起服务并写 PID 文件。就绪检查：

```bash
curl http://127.0.0.1:18080/health
```

日志：stdout 与 stderr 分开，模型加载与缓存行为在 stderr。

## 4. 模型放置与上下文

| 项 | 当前配置 |
|---|---|
| 目标模型放置 | `-dev CUDA0,CUDA1 -sm layer -ts 38,62` |
| 草稿模型放置 | `--spec-draft-device CUDA0`，`--spec-draft-ngl all` |
| 视觉投影 | `--mmproj-device CUDA0` |
| 草稿长度 | `--spec-draft-n-max 6`、`--spec-draft-n-min 0` |
| KV 量化 | 目标 `q4_0/q4_0`，草稿 `q8_0/q8_0` |
| 上下文 | `-c 262144`、`--kv-unified`、`-np 1` |

按层切分的分工由 `38,62` 固定。CUDA 后端不提供 split buffer 支持，`-sm row` 在模型加载阶段即被拒绝，双卡只剩 `-sm layer` 一条路。按 4070 Ti SUPER 672 GB/s、3090 936 GB/s 计算，带宽最优点约 42/58，`38,62` 与其等价；30/70、45/55 实测均无收益。

草稿模型放在 CUDA0 而非 CUDA1：1.26 GiB 的草稿若与目标模型 62% 的层共用 3090，会争抢同一块 HBM。移到 4070 Ti SUPER 后每轮耗时下降 2–5%，同时给 3090 释放 2.1 GiB 显存。代价是 prefill 略慢 1–2% —— 草稿模型在 prefill 阶段也要处理整段 prompt 以建立自身 KV，而 4070 Ti SUPER 的 SM 数（66）少于 3090（82），这段计算是算力受限的。

## 5. 解码性能

`--spec-draft-n-max 6`、单 slot、128 token 解码段、温度 0。语料为中文长文本（1.406 字/token），实测上下文规模由 prompt 前缀决定：

| 上下文 | 投机解码 | 无投机 | 投机收益 | 每轮耗时 | 接受率 / mean len |
|---|---|---|---|---|---|
| ~6K | 43.9 t/s | 37.4 t/s | 1.06× | 54.9 ms | 0.09 / 2.4 |
| ~33K | 48.7 t/s | 30.9 t/s | 1.52× | 60.9 ms | 0.14 / 3.2 |
| ~90K | 37.3 t/s | 22.8 t/s | 1.55× | 73.3 ms | 0.12 / 2.8 |

判读方式：表观 t/s 受草稿接受率波动影响很大，比较配置应看**每轮耗时**（`predicted_ms / (draft_n / n_max)`），它与接受率无关；`ms/token = 每轮耗时 / mean len`。

prefill 吞吐（冷缓存、单次全量）：6K 约 1.3–1.4K t/s，90K 约 1.25–1.4K t/s；123K token 全量 prefill 约 98 s。UB=2048 在双卡层切分下反而慢 10%，`-ub 1024` 是当前最优点。

## 6. 两级前缀缓存

### 6.1 数据流

```
请求 → RAM 提示缓存（--cache-ram）
         ├─ 命中：直接复用 KV
         └─ 淘汰 / 空闲镜像 → 磁盘待写队列（后台线程）
                                 └─ 原子写 pc-<hash>.bin → 更新 pc-index.dat
新请求未命中 RAM → 磁盘层按前缀查找 → 恢复 KV 与检查点 → 从分歧点续算
```

条目进入磁盘的时机：RAM 缓存淘汰时迁移、空闲 N 秒后镜像当前会话、后台队列写入、进程退出时排空队列。序列化与落盘在独立线程完成，不阻塞推理；写入按每 256 MiB 让出 10 ms 节流。

### 6.2 条目格式与完整性

| 组成 | 内容 |
|---|---|
| 条目文件 | `pc-<hash>.bin`，格式版本 8；头含 magic、版本、配置签名、packed 长度、main/drft 尺寸、检查点个数、时间戳与 CRC |
| 状态载荷 | `main`（目标 KV）、`drft`（草稿 KV）、每个检查点的 `data_spec / data_tgt / data_dft` |
| packed 元数据 | token id 序列 + media 占位符区间与媒体标识；写侧对其单独计算 CRC64 并存入条目头 |
| 校验和 | CRC64-ECMA（反射多项式 `0xC96C5795D7870F42`），slice-by-8；覆盖 main、drft、每个检查点的三块状态，以及 packed 元数据 |
| 启动清单 | `pc-index.dat`，格式版本 3；每条记录含 hash、`size_file`、`size_main`、`size_drft`、`last_used`、packed 的 CRC 与 packed 元数据，整个记录块另有 CRC |
| 配置签名 | 目标/草稿模型路径 + `size@mtime`、目标 K/V 类型、`n_ctx`、`n_parallel`、草稿 K/V 类型、`kv_unified`，启用 mmproj 时追加 `mmproj:path@size@mtime` |

清单在启动时先行校验：magic、版本、签名长度与内容一致、记录块 CRC 正确，且每条记录能重新解包并重算 hash 的，才走快速路径；否则回退逐文件头校验。清单以「临时文件 + 替换」写入，条目集变化时自动重写。

**格式版本是硬边界**：低版本条目（如 v7）不含 packed 校验和，无法在读侧验证，启动扫描会连同日志一起丢弃重建（`entry format v7 is older than v8, the cache will be rebuilt`），不会把旧文件当新格式解析。清单记录中的 packed CRC 与条目头 CRC 绑定，任一不一致即视为条目与索引不符并丢弃，避免用错误的 token 前缀参与匹配。

### 6.3 命中与恢复

- 按 **token 级最长公共前缀** 匹配，取最长者；命中部分需 ≥ `--cache-disk-min-tokens`。
- 恢复闸门要求条目尺寸不超过 `lcp × 768 KiB`（`DISK_CACHE_MAX_BYTES_PER_TOKEN`），因此 `best_lcp ≥ 1` 恒成立；只读取不越过分歧点的那个检查点，其余检查点直接 seek 跳过。
- 恢复后 `prompt.tokens` 保留公共前缀，`seq_rm` 截掉 KV 尾部，`--cache-idle-slots` 继续管理显存。
- 读盘顺序固定为「先校验、后恢复」：packed 的存储 CRC、结构边界、与索引记录的一致性，以及全部状态载荷的 CRC 都在**任何** `llama_state_seq_set_data_ext` 之前完成；任一不通过都不会改写 slot 的 KV。
- `load()` 的返回值契约：任何**尚未改写 ctx 状态**的返回都是 `true`（KV 与提示仍自洽）；**已写入 KV 之后**的失败返回 `false`，由调用方清空 slot 并重新 prefill。
- 候选回退：某个条目读盘校验失败时不会立刻放弃整次查找，而是在剩余条目中继续选次优候选（单次请求最多 4 个），避免一个坏条目把可复用的前缀一起作废。
- 失败分层：确定性损坏（CRC/长度/签名不符、packed 与索引不符）删除条目；可能瞬时的失败（文件打不开、显存压力）保留条目供下次重试。

### 6.4 失效、淘汰与写入抑制

- 配置签名不匹配、TTL 到期、条目损坏都会在启动扫描或访问时丢弃。
- 超出 `--cache-disk-max` 时按 `last_used` 最小的条目循环删除；启动时也按当前限额修剪，并清理遗留 `.tmp`。
- 新条目若包含已有短前缀条目，短条目会被去重删除。
- **写入抑制**：存量条目已与待写提示共享除末尾一小段外的全部前缀时，跳过本次写入。重写一份 2.7 GiB 条目在 150 MiB/s 的盘上约 18 s，而重算 4K token 尾段约 3 s。允许落后的幅度由 `DISK_CACHE_MAX_PREFIX_LAG_TOKENS`（4096 token）限定，避免存下的前缀过于陈旧。判据用最长公共前缀而非严格 token 前缀——追加文本会让切点附近的 token 重新合并。
- 待写队列由 `--cache-disk-queue-max`（MiB）与最多 8 条待写条目共同约束。
- 命中时间戳在内存中更新，优雅退出时先写清单再回写条目头，保证跨重启的 LRU 顺序。

### 6.5 LoRA 感知

`lora_blocks_cache()` 判定「任一生效适配器 scale ≠ 0 且非 aLoRA」。满足时：不把该 slot 状态写入共享缓存；命中守卫时保留 slot 自身 KV；per-request LoRA 在选槽阶段即被预判；生效适配器集变化时清理 slot 提示状态；启动时输出 INFO 说明共享缓存不可用。

### 6.6 参数

| 参数 | 默认 | 部署值 | 说明 |
|---|---|---|---|
| `--cache-prompt` | 开启 | 开启 | 提示缓存总开关 |
| `--cache-ram N` | 8192 MiB | 25600 | RAM 提示缓存上限；`--cache-disk` 依赖它 |
| `--cache-idle-slots` | 开启 | 开启 | 新任务开始时把空闲槽存入缓存 |
| `--cache-disk PATH` | 关闭 | `E:\llama.cpp.cache` | 磁盘缓存目录 |
| `--cache-disk-max N` | 65536 MiB | 204800 | `-1` 不限，`0` 禁用 |
| `--cache-disk-min-tokens N` | 2048 | 2048 | 短于该长度的前缀不入磁盘 |
| `--cache-disk-idle-flush N` | 60 s | 60 | 空闲 N 秒后镜像 RAM 条目，`0` 关闭 |
| `--cache-disk-ttl N` | 0 | 0 | 条目闲置 N 秒后删除，`0` 不过期 |
| `--cache-disk-queue-max N` | 24576 MiB | 24576 | 待写队列字节预算 |
| `--cache-stats` | 关闭 | 关闭 | 逐请求输出 token 来源分解与拒绝原因，并开放对应 /metrics 计数 |
| `--cache-boundary-capture none\|input\|output\|both` | none | none | 在请求输入/输出边界主动捕获完整快照 |
| `--n-ctx-checkpoints` | 32 | 32 | 每个 slot 的上下文检查点上限 |

### 6.7 请求边界捕获

默认关闭（`--cache-boundary-capture none`）。开启后，除「RAM 淘汰落盘」与「空闲镜像」外，缓存还会在两个请求边界主动捕获完整快照：

| 边界 | 触发点 | 写入目标 |
|---|---|---|
| `input` | 最后一个输入 token 的 KV 已算完、即将开始采样 | 仅 RAM 层 |
| `output` | 生成因停止条件正常结束 | RAM 层 + 立即排队写盘 |

- 只捕获内存实际持有位置覆盖的 token：以 `llama_memory_seq_pos_max` 为上界，未算出 KV 的最后一个采样 token 会被裁掉，条目不会声称一段没算过的前缀；恢复时照旧由 `seq_rm` 截掉多余尾部。
- `input` 边界不直接写盘：正常请求随后就到达 `output` 边界，更长的条目会覆盖前者，提前写盘只白耗带宽。被中断（超时、取消、上下文截断）的请求保留 RAM 条目，仍由淘汰迁移与空闲镜像落盘。
- 去重沿用既有机制：RAM 层由 `alloc()` 的包含判定拦截，磁盘层由 hash / inflight / `covered_cheaply()` 拦截，因此边界捕获不会绕过写入抑制。
- 代价：每次捕获都要序列化整个序列状态（本机 1.4K token 约 0.2 s / 506 MiB），因此默认关闭，只在需要以「轮次」为粒度复用前缀时开启。
- 与 `--cache-disk-idle-flush` 互不替代：前者按请求事件触发，后者按空闲时间触发。

### 6.8 实测条目构成

条目不是一个整体，拆开看由三段组成。同一套配置下实测：

| 前缀长度 | 文件 | main | drft | 检查点 | 检查点数 |
|---|---|---|---|---|---|
| 123,674 token | 3039 MiB | 2326 MiB | 88 MiB | 624 MiB | 3 |
| 5,852 token | 727 MiB | 255 MiB | 62 MiB | 410 MiB | 2 |

两点值得注意：

- 目标模型是混合架构，单个状态有约 **151 MiB 的固定开销**（与上下文长度无关），所以短前缀条目的单位成本天然偏高——6K token 条目约 128 KiB/token，而 123K 条目约 25 KiB/token。这不是缺陷。
- 检查点数量随会话增长而非固定：短会话 1–3 个，长会话会一路涨到 **31 个**（上限即 `--n-ctx-checkpoints 32`），单点约 200 MiB。生产日志里 8.6 GiB 的条目就是这样产生的，其中检查点占约 6 GiB。因此调小 `--n-ctx-checkpoints` 能显著压缩条目，代价是部分命中时可选的原点变少。

写入吞吐在机械盘上实测 130–155 MiB/s，恢复读取约 180 MiB/s：3 GiB 条目落盘约 20 s、恢复约 17 s，而 123K token 全量 prefill 约 98 s。

**命中实践**：系统提示、工具定义、few-shot 静态化并置于最前；时间戳、用户名等动态内容后置；多轮对话只追加不改写。

## 7. 可观测性

`-lv 3` 为默认 INFO，`TRACE=4`、`DEBUG=5`。

| 日志 | 级别 | 用途 |
|---|---|---|
| `causal_attn=... selector_top_k=... is_dflash2=...` | INFO | 区分草稿模型的 lattice 路径与每行词表 top-k 路径 |
| `disk cache: restored N/M tokens (a + b bytes)` | DEBUG | `N` 实际复用前缀、`M` 条目长度 |
| `entry with length ... lcp = ... f_keep = ...` | TRACE | 逐条候选的匹配情况 |
| `disk cache: stored ... tokens ... in ... ms` | INFO | 落盘耗时与体积 |
| `covered up to a N-token tail ... skipping a X MiB write` | INFO | 写入抑制生效，附被跳过的体积 |
| `disk cache: limit reached, removing ...` | WRN | LRU 淘汰 |
| `dropping ... different model or context configuration` | WRN | 签名不匹配的条目清理 |
| `prompt cache effectively disabled: active LoRA ...` | INFO | 当前 LoRA 配置下共享缓存不可用 |
| `write queue is full` | WRN | 待写队列达到字节或条目上限 |
| `cache_stats: prompt = ..., reused_slot/ram/disk = ..., prefill = ...` | INFO | `--cache-stats` 开启时每请求一行，token 来源分解 |
| `cache_stats: boundary = input/output, captured = yes/no, tokens = ...` | INFO | `--cache-stats` 开启时的边界捕获结果 |

### 7.1 缓存指标（`/metrics`，需 `--metrics`）

| 指标 | 含义 |
|---|---|
| `llamacpp:cache_request_prompt_tokens_total` | 已统计请求的输入 token 总数（命中率分母） |
| `llamacpp:cache_request_prompt_tokens_disk_ok_total` | 其中查询过磁盘层的请求的输入 token 数（磁盘命中率分母） |
| `llamacpp:cache_reused_tokens_slot_total` / `_ram_total` / `_disk_total` | 三层各自实际免除 prefill 的 token 数，互斥 |
| `llamacpp:cache_prefill_tokens_total` | 实际执行 prefill 的 token 数 |
| `llamacpp:cache_disk_candidate_lcp_tokens_total` | 最佳候选前缀长度之和，含被拒绝的候选 |
| `llamacpp:cache_disk_reject_total{reason}` | 磁盘层未能服务的原因（`no_prefix` / `too_short` / `io_cost` / `media_mismatch` / `checksum_error` / `read_error` / `restore_failed` / `signature_mismatch`） |
| `llamacpp:cache_disk_enqueue_queued_total` / `_dedup_total` / `_rejected_total` | 写盘入队、去重跳过、容量或队列拒绝 |
| `llamacpp:cache_disk_bytes_read_total` / `_written_total` | 读盘与写盘字节 |
| `llamacpp:cache_disk_restore_seconds_total` | 读盘 + 状态恢复耗时 |
| `llamacpp:cache_disk_entries` / `_queue_entries` / `_queue_bytes` | 条目数与待写队列 |
| `llamacpp:cache_disk_manifest_fallback_total` | 清单不可信、回退全量扫描的次数 |

三条口径不可混用：`candidate_lcp` 是候选共享前缀，`restored` 是候选条目实际恢复的长度，`reused_*` 是恢复后仍被 `n_past` 认可、真正免掉 prefill 的 token 数（还会被检查点与「末位 token 必须重算」等守卫削减）。恒等式为 `prompt = reused_slot + reused_ram + reused_disk + prefill`。

## 8. 行为边界

- CUDA 后端只支持按层切分（`-sm layer`）；`-sm row` 在模型加载阶段即被拒绝。
- 生效的非 aLoRA 适配器下，共享缓存对该 slot 不写入；slot 自身的增量续算仍可用。
- packed 元数据自 v8 起有独立 CRC：写侧写入条目头，读侧在**任何**状态写入之前校验，并与启动清单记录绑定；清单不可信时回退逐文件头校验，不会因为清单存在就跳过验证。
- 条目按前缀匹配，首次请求必然全量 prefill；`-np 1` 下单 slot 串行服务。
- 强制终止进程不触发析构，持久化依赖空闲镜像（60 s）或淘汰落盘。
- 恢复永远读取整个条目文件，因此命中率低的长条目会白读一段；`DISK_CACHE_MAX_BYTES_PER_TOKEN` 只拦极端情况。
- 多轮对话每增长一段就会产生一条新条目；写入抑制只挡小增量，累计增量超过 4096 token 后仍会重写。
- 长会话的条目会因检查点累积涨到 8 GiB 量级，而待写队列受 `--cache-disk-queue-max` 约束，写入速度（8 GiB ≈ 57 s）赶不上轮次增长时会被丢弃，日志记 `write queue is full, skipping`。生产日志中曾连续出现 25 次。

## 9. 实现位置

| 文件 | 职责 |
|---|---|
| `common/common.h` / `common/arg.cpp` | 缓存参数定义与命令行注册 |
| `tools/server/server-task.h/.cpp` | `server_disk_cache`：条目格式、CRC64、落盘队列、恢复、清单、LRU/TTL、写入抑制 |
| `tools/server/server-context.cpp` | 缓存初始化、配置签名、slot 保存/加载守卫、LoRA 切换清理、空闲判定 |
| `tools/server/server-common.h/.cpp` | `lora_blocks_cache()` 与 LoRA 比较工具 |
| `tools/server/server-queue.h/.cpp` | 空闲回调钩子 |
| `common/speculative.cpp` | 草稿解码实现与启动形态日志 |
| `CMakeLists.txt` | 产物目录指向 `output/` |
| `build-cuda-shared.bat` | configure、增量构建、补齐 CUDA runtime DLL |

## 10. 实验记录

- `change.md` — 逐次代码变更记录，含修改前备份路径与验证结果
- `experienceHistory.md` — 已排除方案与依据、确定性分级、排查纪律
- `experiments/` — 压测脚本（`bench_decode.py`、`start-bench*.ps1`）与实测数据（`RESULTS.md`、`RESULTS-OPT.md`）

## 11. 征集加速想法

本地部署稠密模型的加速远没到尽头。这套双卡配置上的时间主要花在三个地方，任何一环有想法都欢迎提：

| 环节 | 当前实测 |
|---|---|
| 解码 | 37–49 t/s，每轮 55–73 ms；投机解码的固定开销约 36 ms，与上下文长度无关 |
| prefill | 约 1.3K t/s，123K token 全量约 98 s |
| 磁盘缓存 | 长会话条目涨到 8.6 GiB（31 个检查点），写入队列会饱和并丢弃条目 |

如果你对本地稠密模型的部署有其他加速想法，**欢迎在 [Issues](https://github.com/A-lucky-boy/llama.cpp-dspine-diskCache/issues) 提出，我会尝试实现**。不限于本仓库已有的方向——换 kernel、换量化、换调度、换缓存策略都可以。

为了让想法更快落地，如果能一并说明这几点会很有帮助：

- 具体改哪里：哪个 kernel、哪个参数、哪段代码路径
- 预期收益与依据：带宽模型、同类实现、论文，或你自己在其他项目上的实测数据
- 是否改变数值结果；如果改变，精度代价大概怎么估

这个仓库的取舍原则是**能用实测数据说话的优先**。`change.md` 与 `experienceHistory.md` 记录了每一轮尝试的结论、被否掉的方向和理由，提想法前可以先翻一眼，避免重复踩坑。

## License

MIT，与上游 llama.cpp 一致。详见 [LICENSE](LICENSE)。
