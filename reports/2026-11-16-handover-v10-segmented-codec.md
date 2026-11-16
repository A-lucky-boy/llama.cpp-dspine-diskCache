# 2026-11-16 12-20 交付说明（单用户磁盘提示缓存：第十二轮 —— 分段无损快照编码 + 导出调用直接计数）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `a85e66c`（会话前 HEAD，第十一轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端（功能提交 `1f15c1b`） |
| 功能提交 | `1f15c1b`（全部代码与测试改动） |
| 规划依据 | `betterModification.md` → `2026-11-16 11-25 - test1_5` |
| 构建产物 | `0.9.0-dev (build 41, commit 1f15c1b)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | **独立算法实验（物理存储格式）** + **上一轮欠账补齐**（规划 §6.1 第 3 项） |

本轮实现规划 §3 的 Segmented Lossless Snapshot Codec，并补上上一轮明确欠下的"导出 API 直接调用计数"验证。逐项实现细节见 `change.md` 的 `2026-11-16 12-20` 条目；项目现状见 `projectSummary.md`。

---

## 1. 欠账补齐：preflight 拦下的路径不进入导出 API

规划 §6.1 第 3 项：

> 为已存在的导出前预准入分支提供一次**直接的测试构建函数调用计数**：被 preflight 拦住的路径不得进入 `llama_state_seq_get_data_ext()`；不能只以累计导出日志作为证明。该检查不属于新算法变量。

上一轮我以 `exported_total` 由 2 降为 1 加日志作为证据，属**间接证据**——它能说明未走 export 分支，但不能直接证明 state API 未被调用。本轮补齐。

### 1.1 实现

在两处 state API 调用点计数，由宏启用，release 构建零开销：

```cpp
// server-task.cpp，文件作用域（定义早于使用它的 metrics 表）
#ifdef LLAMA_TEST_EXPORT_COUNTERS
std::atomic<uint64_t> g_test_state_size_calls { 0 };
std::atomic<uint64_t> g_test_state_data_calls { 0 };
#endif
```

`server_prompt_cache::materialize()` 内：

| 位置 | 计数 |
|---|---|
| `llama_state_seq_get_size_ext` 之前 | `g_test_state_size_calls++` |
| `llama_state_seq_get_data_ext` 之前 | `g_test_state_data_calls++` |

经 `/metrics` 暴露（同样在宏内）：

```
cache_prefix_materialize_test_state_data_calls_total
cache_prefix_materialize_test_state_size_calls_total
```

两者分开是有意的：`get_size_ext` 只查询大小、不分配，成本可忽略；`get_data_ext` 才是本轮要避免的完整拷贝。只统计后者会掩盖"preflight 省下了拷贝但仍在查询大小"这一情形。

### 1.2 实测（规划要求的那一类证据）

以 `-DLLAMA_TEST_EXPORT_COUNTERS` 独立构建（`build-testcounters`，配置与 release 完全一致，仅多该宏），跑 `A+B / A+C / A+D / A+E` 序列：

| 指标 | 值 |
|---|---|
| `preflight_export_allowed_total` | 1 |
| `preflight_skip_total` | 1 |
| `test_state_data_calls_total` | **1** |
| `test_state_size_calls_total` | **1** |

日志对应：

```
task 14: materialize = exported, prefix = 2102 tokens
task 23: materialize = preflight skip (covered by a committed entry), prefix = 2101 tokens
```

**结论**：被 preflight 拦下的 task 23 确实**未进入 `llama_state_seq_get_data_ext()`**，也未进入 `get_size_ext()`。这是计数直接给出的结论，不再依赖日志推断。上一轮该项未达规划要求，现已达到。

### 1.3 构建过程记录

测试计数构建首次链接失败（`LNK1104: 无法打开文件 output\ggml-cpu.dll`），原因是 release 构建的产物正被运行中的服务占用；停止服务后成功。另有一次编译期修正：计数器初版定义在匿名命名空间内（约 2121 行），而使用它们的 metrics 表在 1826 行——**声明晚于使用**（`C2065`）。已移到文件作用域并注明原因。

---

## 2. 段 codec 模块（规划 §3.1）

### 2.1 编码器选型：为什么不是 Zstandard

规划 §3.1**优先评估 Zstandard**，同时要求：

> 编码器必须显式声明固定版本/配置；不要依赖机器隐式安装的随机 DLL……如需集成依赖，必须固定版本、通过项目现有 CMake/构建脚本显式链接，并保证无编码器时原 v8 路径仍能构建。

实测勘查结果：

- 仓库 `vendor/` 下只有 `cpp-httplib`、`hash`、`miniaudio`、`nlohmann`、`sheredom`、`stb`，**无 zstd**
- 本机（含 MSVC 工具链目录）**无 `zstd.h` / `libzstd`**
- 网络可达 GitHub，但引入外部库会使**默认构建依赖网络获取**，与"无编码器时原 v8 路径仍能构建"直接冲突

因此**未集成外部压缩库**，改为实现一个紧凑的内置无损编码器。规划 §3.1 要求的"固定版本外部依赖"在本环境下不适用；`--cache-disk-zstd-level` 因此**未实现**（见 §5）。

### 2.2 内置 LZ77

令牌流格式（小端 varint，无填充）：

```
flags      byte   bit 0：本令牌带匹配
lit_len    varint 后随的字面量字节数
literals   raw    lit_len 字节
[ match ]  仅当 bit 0 置位：
  tag      byte   匹配长度 - 3（3..128 映射到 0..125）
  distance varint 回引距离，>= 1 且不超过已输出长度
```

`flags` 字节使两种令牌形态**无歧义**：纯字面量的收尾令牌不带任何匹配字段，正是流的结束方式。解码器在输出达到声明长度时停止，故截断或被填充的流既不会失控，也不会被误判为完整。

选型理由：payload 是不透明的 KV 状态，冗余度未知且可能为零，编码器的职责是**永不成为写入失败的原因**且**小到可完整审计**。因此：

- 输出不小于输入时**回退为原样存储**，由调用方比较大小决定
- 单段最坏膨胀由 `disk_cache_codec_bound()` 界定
- 纯函数、无全局状态、确定性（单测固定）
- 参数为 `const void *` + 长度，因为载体是两种容器（KV 状态是 `common_state_data`，checkpoint spec 是普通字节向量）

### 2.3 每段独立：为什么不做单一流（规划 §3.1）

段集合为 `main`、`drft`、以及每个 checkpoint 的 `spec`/`tgt`/`dft`，共 `2 + 3N` 段，**各自独立编码**。

`read_entry()` 只恢复一个 checkpoint。若把全部 checkpoint 串联进同一压缩流：

1. 取出目标 checkpoint 必须解压它之前的所有 checkpoint —— 直接摧毁现有的按需读取能力
2. 一个损坏字节会使整个文件失效，而不是单段

对 `main` 与 `drft` 同样分别编码，不将两个模型上下文混成不可独立校验的流。

### 2.4 压缩后不小于原段则原样存

允许同一快照内混合编码段与原样段。实测中这正是常态（见 §4.2）。

---

## 3. v9 文件格式（规划 §3.2）

### 3.1 关键设计：保留 v8 全部前置字段，段表追加在尾部

```
magic(8) | version(4) | sig_len(4) | sig | packed_size(4) | packed
| size_main(8) | size_drft(8) | n_ckpt(4) | created(8) | last_used(8)
| crc_main(8) | crc_drft(8) | crc_packed(8)
| checkpoint 表 (每项 44 字节)
| [v9 only] 段表 (每项 28 字节)   ← 追加
| payload
```

段表每项固定宽度：`raw_len(8) + stored_len(8) + codec(4) + encoded_crc(8)`，读端可先算表大小。

**为什么必须追加而非重排**：`touch_entry()` 就地重写 `last_used`，其偏移为

```cpp
const std::streamoff off_last_used = DISK_CACHE_HDR_LAST_USED_OFFSET + sig_len + packed_size;
```

其中 `DISK_CACHE_HDR_LAST_USED_OFFSET` 有 `static_assert(== 48)` 保护，由前置字段布局决定。若为段表重排字段，该偏移会落偏，**时间戳写入将覆盖 CRC**，把每个被 touch 的条目标记为损坏。追加到尾部使两版本共用同一偏移，`touch_entry()` 与 v8 相比**零改动**。

### 3.2 写入数据流

```text
pending（RAM 持有原始 payload）
  → 段表占位写入（n_segments × 28 字节）
  → 逐段：编码到复用的单个 scratch 缓冲 → 写入 → 记录段描述
  → seekp 回填段表
  → flush / 原子提交（原路径）
```

**写入阶段只访问 `pending` 已持有的不可变数据**，不接触 live `llama_context`（规划 §3.3 第 1 条）。

**内存有界**：复用**单个** scratch 缓冲，峰值额外内存是**一段**而非一份快照。规划 §3.3 第 2 条禁止"先压缩到内存再整体写入"的巨量分配；另一种朴素做法——先把所有段编码进内存再写——会额外持有一份完整快照（本例 538 MiB）。故采用占位回填。

**`pending.bytes` 语义未变**：仍按**原始状态内存大小**计队列占用，未因磁盘可压缩而低报（规划 §3.3 第 3 条）。配额与读盘成本仍用提交后的真实 `size_file`。

**失败清理**：编码不引入新的失败点（输出总是有效，最坏是原样）。写入失败沿用既有 `.tmp` + 清理路径，不存在"只有部分段的 v9 文件"被提交的可能（规划 §3.3 第 5 条）。

### 3.3 版本由文件头决定，不由配置决定

规划 §3.2 要求"新版本读取代码不得把 v8 字节解释成 v9 描述"，并对称地要求"不因默认写入策略关闭而无法读取已经存在的 v9 文件"。

实现：`read_entry()` 与 `scan()` 都接受 `8` 与 `9`，`segmented` 由 header 里的 version 推导：

```cpp
const bool segmented = version == DISK_CACHE_VERSION_SEGMENTED;
```

**v8 路径把每个段都描述为 verbatim**（`raw_len == stored_len`、codec `raw`），因此读写**共用同一套段描述代码路径**，v8 的 payload 语义与旧实现逐字节等价。

### 3.4 manifest v3 未改动

规划 §3.2 要求"manifest 仍优先使用 v3……只要启动扫描与读盘可无歧义识别版本且所有原索引不变量成立，就不需要为本实验擅自升级"。核查结果：manifest 记录原始 main/draft 长度、文件物理大小、packed CRC 与身份，**均与 payload 编码无关**，无需增补。`load_manifest()` / `save_manifest()` 未改动。

---

## 4. 读取、校验与恢复（规划 §3.4）

### 4.1 校验全部发生在 `set_data_ext` 之前

规划 §3.4 第 1 条要求"**不得先恢复 KV 再做校验**"。执行顺序：

| 顺序 | 检查 |
|---|---|
| 1 | header 版本（8 或 9）与配置签名 |
| 2 | `packed_size` 上界（`1<<28`） |
| 3 | `size_main` / `size_drft` 与索引记录一致 |
| 4 | `crc_packed`（身份元数据）——决定 KV 属于哪个 prompt |
| 5 | 段表每项：`raw_len` / `stored_len` 是否超界 |
| 6 | 段表 raw 长度与 header 的 state 大小、checkpoint 表逐项一致 |
| 7 | 读段时：编码段的 `encoded_crc` |
| 8 | 解码：长度必须**恰好**等于声明 raw 长度 |
| 9 | `crc_main` / `crc_drft`（**解压后**字节） |
| 10 | checkpoint 三 CRC（**解压后**字节） |

任何一步失败即返回 `corrupt`，该候选被跳过并沿用既有安全回退（另有候选则重试，否则走普通 prefill）。**没有任何字节越过这一步到达 `llama_state_seq_set_data_ext`。**

### 4.2 边界检查在分配之前（规划 §3.4 第 2 条）

段表读取后立即校验，不做任何分配：

```cpp
if (raw_len > DISK_CACHE_SEGMENT_RAW_MAX ||
        stored_len > DISK_CACHE_SEGMENT_COMPRESSED_MAX) {
    → corrupt
}
```

`DISK_CACHE_SEGMENT_RAW_MAX = 1 TiB`，远高于任何合法段（完整 target 状态），但有限。解码器内部另有独立上界（`disk_cache_codec_bound()`），且解码前先 `resize` 失败即拒。规划点名的是"本机输入/缓存文件结构正确性"，非网络攻击场景。

### 4.3 未选中的 checkpoint 只 seek 跳过（规划 §3.4 第 3 条）

```cpp
if ((int) i != want) {
    for (int k = 0; k < 3; k++) {
        f.seekg((std::streamoff) seg_table[2 + 3*i + k].stored_len, std::ios::cur);
    }
    continue;
}
```

按**存储长度**跳跃（而非原始长度），不读、不解压。checkpoint 选择规则（`pos_max <= lcp` 的最后一个，或 `pos_min == 0` 的第一个）**完全未改**。

### 4.4 `read_bytes` 报告真实物理读取

规划 §3.4 第 4 条要求"`read_bytes` 统计**本次真正从磁盘读取的**压缩字节和相关元数据，与 `file_bytes` 的完整物理文件大小区分"。

**交付前自查发现此处曾未达标，已修正。** 修正前 `load()` 使用 `info.read_bytes += best.size_file`（条目文件总大小），而 `size_file` **包含未被读取的 checkpoint 段**（那些段只被 `seek` 跳过）。我原先写的 `stored_read` 只喂给一条 `SRV_TRC` 日志，未进入任何指标——且该行是 TRACE 级，在 `-lv 3` 下**从未输出**，即我在报告里引用了一个从未被观察到的日志行。

修正：

- `server_disk_cache_payload` 新增 `uint64_t stored_bytes`
- `read_entry()` 以 `hdr_end` 初始化 `stored_read`（header 与段表属读取过程的一部分），逐段累加 `d.stored_len`，结束时写入 `out.stored_bytes`
- `load()` 改为 `info.read_bytes += payload.stored_bytes > 0 ? payload.stored_bytes : best.size_file;`

**修正后实测**（`fix12-v9fr.log`，`-lv 5`，v9 文件，2 个 checkpoint）：

```
read_entry: disk cache: read 373.50 MiB stored (373.53 MiB decoded) of 538.47 MiB, checkpoints 1/2, segmented
add_cache_st: reused_disk = 2102, source = disk, read_bytes = 391647888
```

| 量 | 值 |
|---|---|
| 物理读取（修正后 `read_bytes`） | 373.50 MiB / 391647888 B |
| 解压后 | 373.53 MiB |
| 文件总大小（修正前误报为此值） | 538.47 MiB |

**修正前高估 44%**——差异正来自未选中的 checkpoint 0（其 tgt 段 156894364 B 被跳过）。修正后 `read_bytes` 与 trace 的 stored 值自洽，且与 `file_bytes` 明确区分。

`restored` / `reused_disk` 仍按真实 token 复用计算，未改。

### 4.5 成本守卫用真实 `size_file`

`disk_cache_cost_ok()` 与 `768 KiB/token` 门槛未改，输入仍是提交后的实际物理文件大小（压缩后的）。规划 §3.4 第 5 条要求的"不因压缩实验调整此门槛"已遵守。

---

## 5. 控制变量与可观测性（规划 §3.5）

### 5.1 新增参数

```
--cache-disk-codec none|lz    （默认 none）
```

- `none`：写 v8，布局与恢复路径与之前完全一致
- `lz`：写 v9，每段独立编码

未知取值**在启动时抛错**而非静默降级（`arg.cpp` 的 setter 与 `server-context.cpp` 的解析双重拒绝），因为格式写错不是日志能挽回的事。与已有缓存参数风格一致，含 `LLAMA_ARG_CACHE_DISK_CODEC` 环境变量。

**未实现** `--cache-disk-zstd-level`：zstd 未集成（见 §2.1）。

### 5.2 观察口径

| 规划要求 | 状态 |
|---|---|
| `read_bytes` 区分物理/解码 | **已实现**（§4.4），trace 同时打印两者 |
| 文件格式版本 | **已实现**：启动日志打印 `format v9`；trace 打印 `segmented` 标记 |
| `cache_disk_bytes` 代表实际物理占用 | **未变**（取 `size_file`） |
| `raw_bytes` / `stored_bytes` | **未加为指标**，但已由直接解析段表得出（见 §6.7） |
| `codec_bytes_read` | **未加为独立指标**（`read_bytes` 已是物理量，§4.4） |
| `encode_us` / `decode_us` | **未加** |
| `codec_fallback_raw_segments` | **未加为指标**，但已由段表解析得出（5/8 回退，见 §6.7） |
| 诊断值不影响 `reused_disk`/`restored`/`candidate_lcp` | 已遵守 |

未加的四项属**实际缺口**，见 §7。

---

## 6. 验证

### 6.1 v9 写入（规划 §6.3）

`test-run/logs/fix12-v9.log`，`--cache-disk-codec lz`：

```
load_model: disk cache: writing new entries with per-segment 'lz' encoding (format v9);
            existing files keep the format their header records
worker: disk cache: stored 2102 tokens, 538.47 MiB in 13161.45 ms
```

文件头逐字节核对（`od -t x1`）：

```
4c4c4453 43303031 09000000 ...     →  "LLDSC001" + version = 9
```

**物理大小对比**（同一内容、同一请求序列）：

| 格式 | 文件大小（字节） |
|---|---|
| v8（历轮） | 564668394 |
| v9 | 564629638 |
| 差 | **-38756**（-0.007%） |

压缩比极小，**符合预期**：KV 状态近乎不可压缩，绝大多数段按"不缩小则原样存"落为 verbatim。

### 6.2 v9 跨重启真实恢复（规划 §6.3）

`fix12-v9r.log`：

```
scan: cache_scan_end: 1 entries (manifest 1, header 0, stale index records 0), 0 dropped, 538.47 MiB
load_model: prefix discovery: enabled, ..., 1 snapshot(s) registered
add_cache_st: prompt = 2496, reused_slot = 0, reused_ram = 0, reused_disk = 2102,
              prefill = 394, source = disk, candidate_lcp = 2102, restored = 2102,
              entry_tokens = 2102, read_bytes = 564629638   ← 与 v9 文件实际大小一致
```

- `0 dropped`：v9 未被当作损坏或旧格式丢弃
- `read_bytes` 等于**物理文件大小**，非解压后长度
- 恒等式 `0 + 0 + 2102 + 394 = 2496` 成立
- 输出 sha `34cf653dceec290d` 与历次 v8 恢复**完全一致**

### 6.3 混合版本双向可读（规划 §3.2）

规划要求"不因默认写入策略关闭而无法读取已经存在的 v9 文件"。构造：**v9 文件 + `--cache-disk-codec none` 进程**（`fix12-mix.log`）：

```
scan: cache_scan_end: 1 entries (manifest 1, ...), 0 dropped, 538.47 MiB
add_cache_st: reused_disk = 2102, source = disk, read_bytes = 564629638
输出 sha = 34cf653dceec290d   ← 与 v9 进程一致
```

**通过**：写 v8 的进程正确识别并恢复了已有 v9 文件。反向（v9 进程读 v8 文件）在历轮全部 v8 恢复中已等价覆盖——那些恢复都由含 v9 代码的构建完成。

### 6.4 推理输出未受影响

- v9 模式 4 请求 sha：`f283d36a836120c4` / `452bdcadc89d353f` / `ad661337d3aa8896` / `b9febb26949711a2`
- 与关闭编码（默认 `none`）、与历轮 v8 全部一致
- 默认关闭态 4 请求 sha 同上一轮，逐字节一致

### 6.5 单测

新增 `test_disk_cache_codec.cpp`（20 组）：

| 类别 | 用例 |
|---|---|
| 基本语料 | 空段、短段（0..8）、不可压缩、可压缩、高重复模式、长游程（512 KiB 单字节 + 1000×500 循环） |
| 段独立性 | 单快照内 5 段混合编码/原样；损坏 B 段不影响 A 段解码 |
| **拒绝路径（7 类）** | 长度不符、raw 长度不符、声明长度超上界、未知 codec、截断流、匹配越过输出、回引到空输出、字面量越界、空编码段 |
| 属性 | 编码确定性、上界内、模式名解析（含 `nullptr` 与空串拒绝） |

**全部一次通过。**

五套独立单测结果：

```
all disk-cache codec checks passed
all disk-cache match checks passed
all disk-cache quota checks passed
all prefix-index checks passed
all prefix-materialize checks passed
```

### 6.6 构建

release `build-cuda-shared.bat` 本轮 5 次，最终 `EXIT=0`、`0 FAILED`。测试计数构建 1 次成功。期间三次失败均是我自己的问题，已如实记录：

1. Python 脚本里的 `\n` 被 shell 二次转义，字面换行进入源码（`C2001` / `C2146`）
2. `common_state_data` 与 `std::vector<char>` 类型不匹配（`C2664`），改为 `const void*` + 长度接口并让 `load_segment` 泛化
3. 测试计数变量定义在使用点之后（`C2065`）

### 6.7 逐段构成与真实 checkpoint（规划 §3.5、§6.4）

交付前以 `-lv 5` 运行并**实际触发了 2 个 checkpoint**（历轮从未触发），首次获得真实模型证据。直接解析 `test-run/cache-v9f/pc-bf495e2785d51ca5.bin` 的段表：

```
version 9, sig_len 206, size_main 195689284, size_drft 22970832, n_ckpt 2
  ckpt0: n_tokens=1476 pos=[1475,1475] spec=0 tgt=156894364 dft=16094456
  ckpt1: n_tokens=1478 pos=[1477,1477] spec=0 tgt=156894364 dft=16116264

  seg 0 main       raw= 195689284 stored= 195689284 codec=0 raw
  seg 1 drft       raw=  22970832 stored=  22945990 codec=1 ENCODED
  seg 2 ck0.spec   raw=         0 stored=         0 codec=0 raw
  seg 3 ck0.tgt    raw= 156894364 stored= 156894364 codec=0 raw
  seg 4 ck0.dft    raw=  16094456 stored=  16087386 codec=1 ENCODED
  seg 5 ck1.spec   raw=         0 stored=         0 codec=0 raw
  seg 6 ck1.tgt    raw= 156894364 stored= 156894364 codec=0 raw
  seg 7 ck1.dft    raw=  16116264 stored=  16109196 codec=1 ENCODED

  TOTAL raw=564659564 stored=564620584 saved=38980 encoded=3/8
```

**8 段中 3 段被编码**（`drft` 与两个 checkpoint 的 `dft`），`main` 与全部 checkpoint `tgt` 完全不可压缩并原样存储。这给出了规划 §3.5 所需的 `raw_bytes` / `stored_bytes` / 回退段数的**部分数据**（虽未做成指标）。

**据此首次获得证据的条目**（此前只有 codec 层元数据用例）：

1. **多 checkpoint 独立编码** — 文件含 2 个 checkpoint、各 3 段共 6 段
2. **未选段的 seek 跳过** — 恢复选中 checkpoint 1，跳过 checkpoint 0 的全部段
3. **只恢复一个 checkpoint** — trace `checkpoints 1/2`
4. **checkpoint 三 CRC 在解压后字节上校验** — 恢复成功、输出 sha 与历轮一致

**仍未获得证据**：恢复**中间** checkpoint 需 ≥3 个 checkpoint，未构造。（恢复最后一个的情形本轮已覆盖——选中的即索引 1。）

**说明**：`drft`/`dft` 可压缩而 `main`/`tgt` 不可压缩这一规律，**只有一个快照、一个模型组合**作为样本，不足以支撑关于模型类型的结论，需要多快照才能判断是否稳定。

---

## 7. 未执行的检查与已知缺口（明确记录）

按规划要求如实记录，**不作为已完成项**：

| 检查项 | 状态 | 原因 |
|---|---|---|
| **Zstandard 集成** | **未做** | 仓库与本地均无 zstd；引入需网络获取，破坏"无编码器仍可构建"（规划 §3.1）。改以内置 LZ77 实现 |
| **`--cache-disk-zstd-level`** | **未实现** | 同上 |
| **压缩收益系统评估** | **部分完成** | `raw_bytes` / `stored_bytes` / 回退段数已由直接解析段表得出（§6.7：8 段中 3 段编码、总节省 38980 B、5/8 回退），但**均未加为指标**。`encode_us` / `decode_us` **完全未测**。样本只有 **1 个真实快照**，块大小敏感性未测 |
| **checkpoint 段真实模型验证** | **已做**（见 §6.7） | 交付前 `-lv 5` 运行实际触发 2 个 checkpoint，首次获得真实模型证据：多 checkpoint 独立编码、未选段 seek 跳过、只恢复一个 checkpoint、checkpoint 三 CRC 均已在真实快照上验证。**仍缺**：恢复**中间** checkpoint（需 ≥3 个，未构造） |
| **损坏文件的实际注入** | **未做** | 截断、段长溢出、codec 不识别、CRC 不一致四类在 codec 单测中覆盖（合成流），但**未在真实 564 MB 文件上人为破坏后验证拒绝恢复并安全回退**（规划 §6.2、§6.4） |
| **`touch_entry()` 对 v9 的实测** | **未做** | v9 保留了 `last_used` 偏移（有 `static_assert` 保护），但**未在 v9 文件上实际触发 touch 并核对 CRC 未被覆盖**（规划 §6.2） |
| **解压后 payload 与 `get_data_ext()` 输出的逐字节比对** | **未做** | 规划 §6.3 要求"不得仅比较最终模型输出"。我用的是**解压后字节的 CRC 校验**（强于只比输出），但**不等于**逐字节 dump 比对 |
| **v8 文件由新版进程恢复** | 等价覆盖 | 未单独构造；但历轮全部 v8 恢复都由含 v9 代码的构建完成，且 §6.3 已单独验证 v9 文件被 v8 配置进程恢复 |
| **配额/TTL 淘汰路径** | **未执行** | 规划 §6.3 列出 |
| **Windows 句柄占用后回收** | **未执行** | 规划 §6.3 列出 |
| **强杀重启** | **未执行** | 规划 §6.3 列出（仅做了正常重启） |
| **媒体输入端到端** | **未执行** | 测试机无多模态模型。规划 §5 要求"在没有真实媒体环境时不得宣称其新格式兼容性已由端到端验证"——**未宣称**。`media_state_unverified` 主动捕获门禁未变 |
| **正常重复 / 公共前缀分叉的对照** | 部分 | 公共前缀分叉已覆盖（A+B/A+C/A+D 序列）；正常重复未单独构造 |

---

## 8. 边界确认

- 文件命名 `pc-<hash>.bin` 未变；同一身份的去重仍由既有 `committed_check_locked()` / `inflight_check_locked()` / `covered_cheaply()` / 配额事务负责，**不因格式变更而覆盖已有文件**（规划 §3.2）
- **不对 v8 目录做原位迁移**；对照使用独立目录（规划 §3.2 末段）
- manifest **仍为 v3**，未升级、未重新解释任何字段（规划 §3.2）
- `CRC64` 明确绑定**原始解压后字节**；编码片段另有封装 CRC，但**未以"压缩流有效"替代原始状态完整性校验**（规划 §3.3 第 4 条）
- 写入仍走临时路径完整写入、flush 并原子提交；**未绕过** `claim_path_locked()`、回滚隔离、`event_epoch` 或 `notify_committed()`（规划 §3.3 第 6 条）
- `preflight_materialize()`、`committed_check_locked()`、`inflight_check_locked()`、`covered_cheaply()`、`disk_cache_can_replace()`、配额事务与同步提交通知**均未改身份与状态语义**（规划 §5）
- `server_tokens::pos_next(K)` 与位置端点判定未改；媒体主动捕获仍在 `media_state_unverified` 门禁内（规划 §5）
- **不进行 token 范围 KV 切割**；未从 opaque state 内推断 token 边界，不支持只恢复某个 token 子区间（规划 §1）
- **未因低成本读盘而绕过 checkpoint、CRC 或 target/draft 完整恢复校验**（规划 §6.4）
- 未新增多用户、公网 API、安全鉴权或多 slot 调度（规划 §5）
- 边界算术与段描述为纯函数，单测与生产路径共用同一实现（未新增单测专用逻辑）
