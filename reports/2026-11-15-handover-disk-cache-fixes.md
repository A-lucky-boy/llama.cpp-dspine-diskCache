# 交付说明：磁盘提示缓存的修复批次（2026-11-15）

- 收件人：算法规划人员
- 仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`
- 本轮交付提交：见 §7「提交清单」；起始基线 `bc0c9c9`
- 配套阅读：`change.md`（`20-22` / `20-47` / `21-35` 三条记录）、`projectSummary.md`、`betterModification.md`（`20-31` 与 `20-53` 两节）

本文只回答三件事：**改了哪三批**、**哪些结论有实测支撑**、**哪些还没有**。不含性能预测。

---

## 1. 这批修复属于什么性质

三次规划轮（`20-31`、`20-53` 共两轮）都是**缺陷修复**，不是算法实验：不新增缓存策略、不改 KV 格式、不放宽成本阈值。共同特征是「写入侧的抑制/去重判断」与「读取侧的恢复前置条件」不一致，导致快照被错误保留、错误删除或错误复用。

三批修复的提交链：

```text
bc0c9c9  docs: delivery-for-experiment-planning            <- 进入本批之前的基线
0c289e7  fix: 隔离条目与可替代性判定                        <- 批次一
5627aed  fix: 双向媒体扫描、真实字节替代判断与路径所有权     <- 批次二
```

---

## 2. 批次一（`0c289e7`）：把「可替代」收敛成一个判定

修复前，四条路径各自判断「已有条目能否替代新条目」，且判据只依赖纯 token 前缀关系：

| 路径 | 修复前的判据 |
|---|---|
| `covered_cheaply()` | 纯 token LCP + 4096-token 尾部容忍 + 「带媒体的条目一律跳过」 |
| `worker()` 冗余检查 | 纯 token 前缀 |
| `drop_prefixes()` | 纯 token 前缀 + 「短条目的媒体是长条目的子集」 |
| `dedup_prefixes()` | 同上 |
| `store()`/`mirror()` 去重 | `hash` 相同 + `tokens.size()` 相同 |

而 `load()` 真正恢复前还要过：双向媒体身份、最小长度、`size_file > lcp × 768 KiB` 成本守卫。于是出现两个可复现的矛盾态：

1. **隔离条目既不可读、又被当作已覆盖**：`quarantined` 只在 `load()` 里被排除，其余三处都把它当成有效条目。
2. **长条目未必能替代短条目**：实测 1226 MiB / 1596 token = 786 KiB/token，超过 768 KiB 守卫，长条目对短请求是 `io_cost` 不可恢复，但短条目被它当作「已覆盖」删掉了。

**改动**：新增 `tools/server/server-disk-cache-match.{h,cpp}`（纯元数据，不读 KV、不碰 `llama_context`），六条规则供所有路径共用：

| 函数 | 判据 |
|---|---|
| `disk_cache_match_prefix` | token LCP + 双向媒体 + 媒体块内部边界钳制 |
| `disk_cache_cost_ok` | `file_bytes ≤ usable_lcp × 768 KiB`（常量唯一出处） |
| `disk_cache_identity_equal` | 逐 token + 逐媒体三元组全等 |
| `disk_cache_classify_duplicate` | hash 只作候选筛选，身份决定结果 |
| `disk_cache_suppresses_write` | 媒体兼容 + `≥ min_tokens` + 尾部在 4096 内 + 成本守卫 |
| `disk_cache_can_replace` | 严格前缀 + 整段媒体兼容 + 长条目按该长度恢复仍过守卫 |

另加 `is_reusable_entry()`（`!quarantined && !empty`），四条路径统一先过。

---

## 3. 批次二（`5627aed`）：三个由源码定位、其中一个实测复现

### 3.1 双向媒体匹配被短路 —— 已实测复现（真缺陷）

`disk_cache_match_prefix()` 原本写作：

```cpp
if (!media_side_ok(a, b, ...) || !media_side_ok(b, a, ...)) { ... }
```

`||` 短路：第一方向发现差异后，第二方向不执行，`clamp_to` 不保证是最早的媒体不一致位置。

**复现方式**：把该行临时改回上述形式重编译，运行新增的单测 `test_bidirectional_earliest_difference`：

```text
FAIL test_disk_cache_match.cpp:208: m.usable_lcp == 4 (12 vs 4)
```

构造：两侧 token id 完全相同（4..8 与 12..16 都是占位符），A 的媒体块声明在 12、B 在 4。旧写法返回 12，正确答案是 4 —— 可用前缀被高估了 8 个 token，而这正是 `load()` 用来选择恢复长度的量。

**修复**：两个方向各自返回最早差异，再取 `min()`，无条件执行两次。同时把媒体结构校验独立出来：三数组等长、下标严格递增、块长度非零、ID 非空、区间在范围内且互不重叠；不合格一律 `usable_lcp = 0`（fail-closed），不再因为「数组长度恰好相等」就信任内容。

### 3.2 用估算字节判断替代 —— 已改，只有单测覆盖

`worker()` 调 `drop_prefixes(..., bytes)`，其中 `bytes = pending.bytes` 是**写前估算**；`load()` 用的是 `e.size_file`。估算与实际在阈值附近时，可能删掉唯一经济可恢复的短快照。

**修复**：`drop_prefixes()` 形参改名 `committed_file_bytes`，只接受磁盘上已提交并进入 `entries` 的真实 `size_file`；写前估算仅用于排队与内存预算。顺序也调整为「确认新条目已在 `entries` 中」之后才允许删短快照。

**未做端到端边界复现**，原因见 §5。

### 3.3 hash 与路径所有权 —— 已改，并顺带发现第四个缺陷

文件名 = 内容 hash，所以 **hash 命中就是路径命中**。原实现有两处缺口：

- 精确去重只在 `hash` 与 `tokens.size()` 都相同时才分类，长度不同直接跳过；而 `write_entry()` 会先 `fs::remove(path)` 再 `fs::rename`。
- 同名 `quarantined` 条目仍在占用该路径。

**修复**：新增 `claim_path_locked(hash, path, wanted, what)`，在磁盘锁内解析目标路径：

| 路径状态 | 结果 |
|---|---|
| 无记录 | `free`，允许写入 |
| 有可用条目、身份全同 | `identical`，跳过写入 |
| 有可用条目、身份不同（含长度不同） | `conflict`，拒绝写入 + `n_hash_collisions` + `n_path_conflicts` |
| 有隔离条目 | 尝试 `remove_file()`：成功 → 扣减 `total_size`、撤销引用、移出 `entries`、`free`；失败 → `conflict` |

`write_entry()` 增加 `path_owned` 形参，不再无条件 `fs::remove(path)`。

**顺带发现的第四个缺陷（规划未提及）**：`store()` 先按 `hash` + 长度判 `identical`，对**已被隔离**的条目也计 `dedup`，于是同一个 prompt 会被永久判为「已存储」而再也无法落盘。首次端到端尝试就是这个现象（日志 `capture_boundary: captured = no`）。改为「隔离条目不算已存储」后，同一 prompt 再次写入被 worker 在锁内回收隔离条目并真实落盘（`stored 1385 tokens, 505.77 MiB`）。

---

## 4. 有实测支撑的结论

环境：双卡 4070TiS + 3090，`-sm layer -ts 38,62`，`-np 1 -c 16384/20000 --kv-unified`，Qwen3.8-27B-pi-Q4_K_M + DSpine b16 草稿，语料 155,535 字中文散文（1.44 字/token）。

| 结论 | 证据 |
|---|---|
| 双向短路是真缺陷，测试能抓到 | 临时回退后同一用例 12 vs 4；恢复后 12 组全通过 |
| 隔离条目不再阻止同 prompt 重缓存 | 修复前 `dedup` 误报；修复后 `stored 1385 tokens, 505.77 MiB`，`enqueue_dedup` 未增长 |
| 长条目不再无条件删短条目 | 短条目（505.77 MiB）与长条目（1059.80 MiB）并存；随后同请求 `source=disk, entry_tokens=1385, read_bytes=530337934` 从短条目恢复 |
| 启动期去重不误删 | 重启后 `2 entries after scan, 2 after dedup/trim` |
| 删除失败不虚扣配额 | `failed to remove ... being used by another process` → `quarantined ... keeping 505.77 MiB accounted for`；指标 `entries 2 / bytes 1.23549e+09（= 505.77 + 672.49 MiB）/ quarantined 1` |
| 句柄释放后账目恢复真实 | `dropping ... expired after 65 s idle` → `entries 1 / bytes 7.00719e+08 / quarantined 0` |
| 配额淘汰不抹除隔离条目账面 | `--cache-disk-max 600`：`entries 1 / bytes 5.30338e+08 / quarantined 1 / remove_failed 1` |
| 计算路径未被改动 | 标准回归 `1385/0`、`4/1381`、`215/1381` 与修复前逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`） |
| 重启后无同路径双记录 | `1 entries (manifest 1, header 0, stale index records 0)`，`1 after dedup/trim` |
| 闭合开关零影响 | prefix 日志 0 行、指标全 0、`prompt_n/cache_n` 与 sha 完全一致 |

单测：`test_disk_cache_match.cpp` 12 组、`test_cache_prefix_index.cpp` 13 组，均不加载模型即可运行。

---

## 5. 没有实测支撑的部分（请按此判断可信度）

| 项 | 状态与原因 |
|---|---|
| 3.2 的端到端边界 | **只有单测**。要构造「估算过阈值而真实不过」，需让条目成本恰好落在 768 KiB/token 附近；本机条目体积由检查点主导（1.4K token 就 505 MiB），可控构造只能靠人为改序列化内容，不是真实输入。单测覆盖等值 / +1 B / +1 KiB 与 `can_replace` 的两种结果；代码层已确保只传 `size_file` |
| 真实 hash 文件名冲突 | **只有单测**。64 位 FNV 碰撞无法在合理时间内构造；端到端 `hash_collision_total` 与 `path_conflict_total` 均为 0 |
| 媒体相关分支 | **只有单测**。测试机是纯文本 prompt，未启用 mmproj |
| `clamp_candidates()` 的钳制分支 | 媒体身份含 token 数且输入校验通过后，该分支实际不可达，属防御性路径 |
| `enforce_limit()` 立即淘汰新写入条目时的短快照保护 | 逻辑上已按「先确认在 `entries` 再删」处理，但未构造到该时序的用例 |
| 多 slot、取消/中断、Linux 构建 | 本轮范围外，未验证 |

另外，`betterModification.md` 中「静态发现」的表述是准确的：3.1 是我实测确认的，3.2 与 3.3 是静态定位后直接修复（3.3 另有第二个实测缺陷），不存在「静态发现被当成实测复现」的情况。

---

## 6. 当前状态与不变量

| 项 | 值 |
|---|---|
| 缓存条目格式 | **v8**：header 80 B，`crc_main / crc_drft / crc_packed` |
| 启动清单 | **v3**：每条记录含 `packed_crc`，整块另有 CRC |
| 成本守卫 | `768 KiB/token`，未改 |
| 写入抑制尾部容忍 | 4096 token，未改 |
| 新增开关 | 无 |
| 新增指标 | `cache_disk_hash_collision_total`、`cache_disk_path_conflict_total`（另 `cache_disk_bytes`、`cache_disk_quarantined`、`cache_disk_remove_failed_total` 为批次一引入） |
| 通知通道 | 仍是磁盘锁内同步 + 单调 `event_epoch`，无第二条通道 |

对规划有影响的三条不变量：

1. **「可替代」只有一个定义**，写入抑制、后台冗余、两条去重路径与 `load()` 候选过滤共用它；后续若要调整，必须改同一个函数。
2. **成本守卫决定既有文件生死时必须用真实字节**；估算值只服务排队与内存预算。
3. **内容寻址永不覆盖未解析的既有文件**；同名冲突或隔离回收失败一律明确失败，不伪报 `dedup`。

---

## 7. 提交清单与推送状态

- 远程：`git@github.com:A-lucky-boy/llama.cpp-dspine-diskCache.git`，分支 `main`
- 本轮推送：`6f9b0de..5bd7c84`，已于 2026-11-15 21:50 完成
- 工作树在推送后为空（无未提交改动）

```text
5bd7c84  docs(test-run): 补充 MODEL_DIR 环境变量说明
55b2aac  chore(test-run): 提交复现脚本并隔离本地产物
5627aed  fix: 双向媒体扫描、真实字节替代判断与路径所有权
0c289e7  fix: 隔离条目与可替代性判定
6f9b0de  docs: 可替代性判定修复的变更说明
750f547  docs: 更新 test1_2 修复轮规划（20-31）
bc0c9c9  基线（进入本批之前）
```

本批新增文件：

```text
tools/server/server-disk-cache-match.h
tools/server/server-disk-cache-match.cpp
tools/server/tests/unit/test_disk_cache_match.cpp
test-run/README.md            （测试脚本说明）
reports/2026-11-15-handover-disk-cache-fixes.md   （本文）
```

`test-run/` 只提交**脚本**（9 个文件，共约 4 KB）；原始日志（`logs/*.log`）、模型续写文本（`logs/out-*.txt`）、缓存目录（`cache-*/`，每个 0.5–1.5 GiB）与临时 exe 均已加入 `.gitignore`，不入库。

两点需要说明的处置：

1. `logs/out-*.txt` 是模型基于本地中文语料续写出的文本片段，属第三方作品衍生内容，**不应上传公开仓库**，因此单独加入忽略规则，而不是连带 `test-run/` 一起忽略。
2. 测试脚本原先写死了本机路径（仓库目录、模型目录、语料路径）。改用环境变量 `MODEL_DIR` / `LLAMA_TEST_CORPUS` / `LLAMA_TEST_PORT`，`ROOT` 由脚本自身位置推导，默认 `MODEL_DIR=<repo>\models`。

---

## 8. 建议的下一步（不含性能判断）

1. **先补 3.2 的可控构造能力**：如果算法规划需要验证「真实字节 vs 估算」的边界行为，需要一个能在受控条件下产出指定体积条目的入口（例如把检查点数量固定为已知值），否则这条只能靠单测。
2. **媒体路径需要一次带 mmproj 的实测**：双向匹配与结构校验目前只有单测，建议用真实图片/音频 prompt 跑一轮冷启动 + 复用 + 重启。
3. **`enforce_limit()` 与新写入的时序**：建议构造「写入立即被配额淘汰」的用例，确认短快照保护在真实时序下成立。
4. 阶段 4（分块 KV）的前置审查仍未做；本批修复不涉及状态导出，不改变该结论。
