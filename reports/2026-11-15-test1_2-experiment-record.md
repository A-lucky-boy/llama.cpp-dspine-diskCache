# test1_2 实验记录 — 单用户公共前缀目录（阶段 3）

| 项 | 值 |
|---|---|
| 实验编号 | `test1_2`（`betterModification.md`） |
| 日期 | 2026-11-15 19:0x – 19:35 |
| 代码基线 | 起始 `eb76818`；阶段 1/2 提交 `01d5686`；本实验提交 `8325ae4` |
| 产物 | `output/llama-server.exe` / `llama-server-impl.dll`，`0.9.0-dev (build 8)` |
| 变更明细 | `change.md` 的 `2026-11-15 19-35` 记录 |
| 原始日志 | `test-run/logs/prefix-run{A,B,C,D}.log` |

本文件只记录**本次实验的判据、用例、实测数据、发现的问题与结论**；实现细节见 `change.md`，项目总体结构见 `projectSummary.md`。

---

## 1. 环境

| 项 | 值 |
|---|---|
| GPU | RTX 4070 Ti SUPER 16 GB (CUDA0) + RTX 3090 24 GB (CUDA1)，`-sm layer -ts 38,62` |
| 模型 | Qwen3.8-27B-pi-Q4_K_M + DSpine 草稿 `b16-Q4_K_M`（`--spec-type draft-dflash`、`n_max 6`） |
| 上下文 | `-np 1 -c 16384 --kv-unified -ctk/-ctv q4_0 -b 4096 -ub 1024` |
| 缓存 | `--cache-prompt --cache-ram 4096`，磁盘目录按用例独立，`--cache-disk-max 8192 --cache-disk-min-tokens 512 --cache-disk-idle-flush 3` |
| 目录 | `--cache-prefix-discovery --cache-prefix-min-tokens 256 --cache-prefix-observations 2` |
| 观测 | `--cache-stats --metrics -lv 3` |
| 语料 | `E:\小说\正文.md`（155,535 字中文散文，无重复段落）；`1.44 字/token` |

---

## 2. 待验证的问题与判据

实验前把待验证点写成可判定的判据，避免事后用结果反向定义目标：

| # | 问题 | 判据 |
|---|---|---|
| Q1 | 分叉请求的公共前缀能否被识别，且不依赖哈希等价 | `A+B`、`A+C`、`A+D` 的 `best_prefix` 等于 A 的长度；媒体身份不同时匹配在该媒体块起点截止 |
| Q2 | 同一请求的输入/输出边界是否只计一次 | 同一 `task id` 只产生一条观察，`requests_total` 等于请求数 |
| Q3 | `persisted` 是否只认可精确端点 | `A+B` 的快照不得使 `A` 变为 `persisted`；快照长度与节点前缀不等时节点保持 `observed/eligible` |
| Q4 | 物理快照消失后引用是否撤销 | 条目被删除后该前缀变为 `unavailable`，`invalidated_refs_total` 增加，`snapshots` 减少 |
| Q5 | 重启后能否重建引用 | 启动日志给出重建的快照数，且不写新的持久索引文件 |
| Q6 | 关闭开关是否零影响 | 无 prefix 日志、prefix 指标全 0、`prompt_n`/`cache_n` 与开启时逐一相同、输出文本 sha 相同 |
| Q7 | 目录候选是否会被误当成命中 | 目录候选与 `reused_*` 无任何耦合；被成本闸门拒绝的候选仍报 `reject=io_cost`、`reused=0` |

---

## 3. 用例与实测

### 3.1 运行 A：发现与分叉（`--cache-boundary-capture both`）

| 序 | 请求 | `prompt_n` | `prefix_tokens` | 候选数 | `best_prefix` | `best_state` | `persisted_prefix` | 日志行 |
|---|---|---|---|---|---|---|---|---|
| A1 | `corpus[:2000]` | 1385 | 1385 | 0 | 0 | observed | 0 | 26 |
| A2 | 同 A1 | 1385 | 1385 | 1 | 1385 | observed | 0 | 39 |
| A3 | `corpus[:2300]` | 1596 | 1596 | 1 | 1385 | **eligible** | 0 | 53 |
| A4 | `corpus[:5000]` | 3535 | 3535 | 2 | 1596 | observed | 0 | 67 |
| A5 | 同 A1 | 1385 | 1385 | 1 | 1385 | eligible | 0 | 81 |
| A6 | `corpus[5000:7000]` | 1317 | 1317 | 0 | 0 | observed | 0 | 114 |

- A2 相对 A1 的共享前缀 1385 被识别，观察数为 1（不含本请求），故仍为 `observed`；A3 再次共享同一前缀后转为 `eligible`。**Q1 前半成立**。
- A6 与 A1..A5 无共同起始 token，候选为 0，未出现"假共享"。
- A4 的 `best_prefix=1596` 且 `best_state=observed`（该节点仅由 A3 建立、观察数为 1），与状态定义一致。

指标快照（A 结束时）：

```text
cache_prefix_requests_total 6      cache_prefix_candidates_total 5
cache_prefix_nodes 5               cache_prefix_tokens 4859
cache_prefix_bytes 39730           cache_prefix_persisted_refs 1
cache_prefix_snapshots 1           cache_prefix_invalidated_refs_total 0
cache_prefix_evicted_total 0       cache_prefix_rejected_total 0
```

> `observed_total` 当时显示 0，属计数器语义缺陷（新节点直接以 `observed` 状态创建，无"迁移"事件），见 §5.3；该字段在运行 B 之前已修正。

### 3.2 运行 B：`input` 单边界下的精确对齐与失效

配置改为 `--cache-boundary-capture input`（此时输入边界也写盘），其余不变。

| 序 | 请求 | `prompt_n` | 候选数 | `best_prefix` | `best_state` | `persisted_prefix` | 关键日志 |
|---|---|---|---|---|---|---|---|
| B1 | `corpus[:2000]` | 1385 | 0 | 0 | observed | 0 | 行 26；随后 `stored 1385 tokens, 505.77 MiB`（行 34） |
| B2 | 同 B1 | 1385 | 1 | 1385 | **persisted** | **1385** | 行 40 |
| B3 | `corpus[:2300]` | 1596 | 1 | 1385 | persisted | 1385 | 行 54 |
| B4 | `corpus[:9000]` | 6233 | 2 | 1596 | observed | 1385 | 行 76；`reused_slot=1592, prefill=4641` |

B4 之后（同一日志）：

```text
行 84  disk cache: dropping 'pc-736f067aa72f365b.bin': prefix of a stored prompt, 1385 tokens, 505.77 MiB
行 85  disk cache: stored 6233 tokens, 1225.98 MiB in 1194.63 ms
```

指标（B 结束时）：

```text
cache_prefix_requests_total 4      cache_prefix_observed_total 3
cache_prefix_eligible_total 1      cache_prefix_invalidated_refs_total 1
cache_prefix_nodes 3               cache_prefix_tokens 6233
cache_prefix_bytes 50442           cache_prefix_persisted_refs 1
cache_prefix_snapshots 1           cache_prefix_unavailable 1
```

- **Q3 成立**：B1 的输入快照长度 1385 与节点前缀完全相等，B2 得到 `persisted`；相比之下运行 A 的 1392-token 输出快照（含 7 个生成 token）没有让 1385 变成 `persisted`（A2/A3/A5 的 `persisted_prefix` 均为 0）。
- **Q4 成立**：B4 写入 6233-token 条目后，`drop_prefixes` 删除 1385 条目，目录在同一动作里撤销引用（`invalidated=1`、`unavailable=1`），新条目成为唯一引用。

### 3.3 运行 D：强杀重启后重建

复用运行 B 的缓存目录（此时 1 个 6233-token 条目）。

```text
行 18  prefix discovery: enabled, min 256 tokens, min 2 observations, at most 256 nodes / 64 MiB, 1 snapshot(s) registered
行 24  cache_stats: prompt = 1596, reused_slot = 0, reused_ram = 0, reused_disk = 0, prefill = 1596,
       source = slot, candidate_lcp = 1596, restored = 0, entry_tokens = 0, read_bytes = 0, restore_us = 0, reject = io_cost
```

指标：`nodes 2 / tokens 6233 / persisted_refs 1 / snapshots 1 / observed_total 2`。

- **Q5 成立**：目录由已校验条目重建（1 个引用、2 个节点），未产生任何新的持久文件。
- **Q7 成立**：`candidate_lcp=1596` 但 `restored=0 / reused_disk=0 / reject=io_cost`——1225.98 MiB ÷ 1596 token = 786 KiB/token，超过 `768 KiB/token` 成本闸门。候选前缀没有被记成命中，且既有成本守卫行为未变。

### 3.4 运行 C：关闭开关的对照

同参数、独立缓存目录、`--no-cache-prefix-discovery`。

| 序 | 请求 | `prompt_n` | `cache_n` | 与运行 A 对应项 |
|---|---|---|---|---|
| C1 | `corpus[:2000]` | 1385 | 0 | A1 相同 |
| C2 | 同 C1 | 4 | 1381 | A2 相同 |
| C3 | `corpus[:2300]` | 215 | 1381 | A3 相同 |

- `prefix_tokens` 日志行数 **0**；全部 `cache_prefix_*` 指标为 **0**。
- 输出文本 sha 与运行 A 完全相同：`fd7c5bda91fa1d60`（2000 字符）、`03e0e517e9ca99c0`（2300 字符）。
- **Q6 成立**。

### 3.5 独立自检（不加载模型）

`tools/server/tests/unit/test_cache_prefix_index.cpp`，7 组检查：分叉发现、请求 ID 去重、媒体身份隔离、媒体块内候选钳制、快照精确对齐与撤销、容量淘汰保护被引用节点、关闭态空目录。

在最终源码（`8325ae4`）上重新编译运行：`all prefix-index checks passed`（exit 0）。

---

## 4. 数据口径

```text
P = R_slot + R_ram + R_disk + M          # 每个输入 token 只计入一类
candidate_lcp >= restored >= reused
```

- 目录的 `observed / eligible / persisted` **不属于上面任何一项**，也不得与之相加。
- 本次最有说服力的一行数据是运行 D：`candidate_lcp=1596 / restored=0 / reused_disk=0`。三者分离使"看起来共享了 1596 token"与"实际省掉 0 个 token"同时可见。

---

## 5. 过程中发现并修复的问题

### 5.1 Trie 在边内部分歧时不报告共享前缀（自检首先抓到）

首轮自检 3 项失败：`A+C` 相对边 `A+B` 得到 0（应为 10），媒体隔离用例得到 0（应为 8）。原因：`walk()` 在边内部发生分歧时只 `break`，而该共享前缀没有任何节点承载。

修复：`walk()` 额外返回分歧位置 `partial` 与"该边的观察数"，候选列表补入这一**合成候选**（状态只能是 `observed/eligible`，永不 `persisted`）。修复后 7 组全通过。

### 5.2 在输出边界发布观察导致断言崩溃

首版在两处输出边界也调用观察，触发 `server-common.cpp:856 GGML_ASSERT(n <= tokens.size())`：`n_covered=1392` 含生成 token，而 `task->tokens` 只有 1385。

修复：只观察输入边界（`slot.task->tokens` 就是用户输入），并把截断长度再钳制到 `task->tokens.size()`；覆盖生成尾部的快照改由磁盘提交事件登记。**这也是 Q2 的实现方式**。

### 5.3 `observed_total` 计数器恒为 0

`refresh_state()` 只在状态迁移时累加，而新节点直接以 `observed` 创建、没有迁移事件。

修复：改为在节点创建处累加 `observed` 事件，`eligible` 仍按迁移累加。修正后运行 B 显示 `observed_total=3`，与节点数一致。

### 5.4 `input` 单边界配置下不写盘

原本只有 `output` 边界写盘，导致"只开 input 捕获"时目录永远拿不到持久引用。修复：`--cache-boundary-capture input` 时输入边界也写盘；`both` 仍由 output 负责落盘，避免写入后立刻被更长的条目取代。

---

## 6. 结论

**本次可以证实的**

1. 分叉共享前缀可被识别，匹配是 token 与媒体身份的精确比较，未使用哈希等价假设（Q1）。
2. 观察按请求 ID 去重，输入边界为唯一观察来源（Q2）。
3. `persisted` 只认精确端点；`A+B` 的快照不会把 `A` 标为 persisted（Q3，运行 A 与自检双重覆盖）。
4. 条目删除会立即撤销引用并保留 `unavailable` 记录；重启从已校验条目重建（Q4、Q5）。
5. 关闭开关零影响：日志、指标、复用统计、输出文本全部一致（Q6）。
6. 目录候选不会进入命中统计，成本闸门拒绝仍如实上报（Q7）。

**本次不能证实的**

1. **复用收益**：目录本身不改变复用行为，因此本次没有、也不能给出命中率或性能提升数字。真实收益取决于后续是否用它驱动"按公共前缀捕获"。
2. 多 slot（`-np > 1`）下的并发正确性、目录锁竞争：未测。
3. 长跑内存：只验证了 3–5 节点的规模（`bytes` 约 40–50 KB），未做长时间压力。
4. 中断/超时路径的观察与引用清理：本机用例均为正常完成。

---

## 7. 未覆盖与风险

| 项 | 状态 |
|---|---|
| 阶段 4（分块 KV） | 未进入；须先完成"序列状态可分块性"只读审查，禁止 opaque state 字节切片 |
| 阶段 5–7 | 未进入 |
| 多 slot / 并发 | 未验证 |
| 中断、超时、断流 | 未构造用例 |
| Linux/POSIX 构建 | 未执行（本次未引入平台相关 API） |
| 部署 | 未部署，`output/` 为当前产物 |

---

## 8. 复现步骤

```bat
rem 1) 构建
build-cuda-shared.bat

rem 2) 运行 A（发现 + both 捕获）
output\llama-server.exe -m <target.gguf> -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fa on -fit off ^
  -np 1 -c 16384 -ctk q4_0 -ctv q4_0 -t 8 -tb 16 --backend-sampling ^
  --spec-type draft-dflash --spec-draft-model <draft.gguf> --spec-draft-n-max 6 --spec-draft-n-min 0 ^
  --spec-draft-device CUDA0 --spec-draft-ngl all --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
  --kv-unified --cache-prompt --cache-ram 4096 ^
  --cache-disk test-run\cache-prefixA --cache-disk-max 8192 --cache-disk-min-tokens 512 --cache-disk-idle-flush 3 ^
  --cache-stats --cache-boundary-capture both ^
  --cache-prefix-discovery --cache-prefix-min-tokens 256 --cache-prefix-observations 2 ^
  --metrics -b 4096 -ub 1024 -lv 3 --offline --host 127.0.0.1 --port 18090

rem 3) 打请求（项目根目录）
python test-run\probe.py 2000 8 A1
python test-run\probe.py 2000 8 A2
python test-run\probe.py 2000 8 A3 ext:2300
python test-run\probe.py 5000:7000 8 A6
python test-run\metrics.py

rem 4) 运行 C（对照）：换成 --no-cache-prefix-discovery 与独立 --cache-disk 目录
rem 5) 运行 B/D（引用对齐与重建）：--cache-boundary-capture input，强杀后复用同一目录重启

rem 6) 独立自检（无需模型，命令见文件头）
cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS ^
  /I tools\server /I tools\mtmd /I . /I common /I vendor /I include /I ggml\include ^
  /Fe:test-run\prefix_index_test.exe ^
  tools\server\tests\unit\test_cache_prefix_index.cpp tools\server\server-prefix-index.cpp
test-run\prefix_index_test.exe
```

---

## 9. 证据索引

| 文件 | 内容 |
|---|---|
| `test-run/logs/prefix-runA.log` | 发现与分叉：行 18 启动、26/39/53/67/81/114 观察行 |
| `test-run/logs/prefix-runB.log` | 精确对齐与失效：行 34 写入、40 `persisted`、76 B4 观察、84 删除旧条目、85 写入新条目 |
| `test-run/logs/prefix-runC.log` | 关闭开关对照：`prefix_tokens` 0 行，23/35/48 为三次请求的分解 |
| `test-run/logs/prefix-runD.log` | 重启重建：行 18 注册 1 个引用、24 `io_cost` 拒绝 |
| `change.md` | `2026-11-15 19-35` 记录（文件级变更与验证） |
