# 2026-11-15 交付说明（单用户磁盘提示缓存：四轮缺陷修复）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `3ba52dc`（会话前 HEAD） |
| 本次交付终点 | `d3c3591`（已推送 `origin/main`） |
| 代码修复提交 | `0c289e7`、`5627aed`、`0e46af4` |
| 构建产物 | `0.9.0-dev (build 21, commit 0e46af4)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | 缺陷修复，非新算法实验 |

本文档汇总本次四轮修复的**实际改动、复现证据与未覆盖项**，供规划人员做源码审查。实现细节见 `change.md` 对应条目；项目现状见 `projectSummary.md`。

---

## 1. 修复范围与提交对应关系

| 轮次 | 规划章节 | 提交 | 修复的问题 |
|---|---|---|---|
| 一 | `20-31 - test1_2` | `0c289e7` | 隔离条目仍阻止有效写入；长条目未必能替代短条目（成本守卫）；hash + token 数不足以证明同一条目 |
| 二 | `20-53 - test1_2` | `5627aed` | 双向媒体匹配被 `\|\|` 短路；替代判断用估算字节而非真实 `size_file`；hash 命中即路径命中（路径所有权） |
| 三 | `21-55 - test1_2` | `0e46af4` | 先删替代对象、后淘汰新条目 → 短快照与其替代者同时消失 |
| — | — | `d3c3591` | 测试 harness：固定参数启动脚本（防重启参数漂移） |

**新增模块**（均为纯元数据，不取锁、不碰文件、可独立单测）：

| 文件 | 职责 |
|---|---|
| `tools/server/server-disk-cache-match.{h,cpp}` | 静态等价与成本判定：token LCP + **双向**媒体身份 + 媒体块边界钳制 + `768 KiB/token` 成本守卫 |
| `tools/server/server-disk-cache-quota.{h,cpp}` | 只读配额规划：隔离条目按真实字节计入且不可淘汰，受保护条目不可淘汰，返回 `fits` / `evict_order` |

---

## 2. 三个核心不变量的落实

### 2.1 替代存活不变量

**任何以“长快照 L 可替代短快照 S”为由删除 S 的事务，结束时 L 必须仍在 `entries` 中、未隔离、且文件存在。**

落实方式：`drop_prefixes()` 增加 `replacement_is_admitted` 门禁。`worker()` 的顺序改为

```text
write_entry() 成功 → 真实 size_file
  → 锁内构造规划输入（新条目 protected）
  → disk_cache_plan_quota()
      ├─ !fits → remove_file(path) 回滚，计 write_rejected，不动任何既有条目
      └─ fits  → 入 entries → enforce_limit(keep)  ← 新条目在保护集内
                  → 确认仍存活后，才以 replacement_is_admitted=true 调 drop_prefixes()
```

**端到端证据**（`test-run/logs/fix4-a8.log`，两步均用 `srv.sh` 保证参数一致）：

```text
行 32  worker: disk cache: stored 2557 tokens, 562.99 MiB          (短快照 S)
行 44  drop_prefixe: dropping '...pc-bdaf909dfb266b6f.bin':
       prefix of a stored prompt, 2557 tokens, 562.99 MiB          (S 被替代删除)
行 45  worker: disk cache: stored 7516 tokens, 1146.81 MiB         (L 存活)
指标   cache_disk_entries 1 / cache_disk_bytes 1.20252e+09
```

同参数重启（`fix4-a8restart.log`）：`cache_scan_end: 1 entries ... 0 dropped`——L 完整保留。随后请求 3600 字符（S 已不存在）：

```text
cache_stats: prompt = 2557, reused_slot = 0, reused_ram = 0, reused_disk = 2553, prefill = 4,
             source = disk, candidate_lcp = 2557, restored = 2557, entry_tokens = 7516,
             read_bytes = 1202519898, restore_us = 673467, reject = none
```

**这是最强的证据**：一个原本由 S 服务的请求，在 S 被删除后完全由 L 恢复（读取 1146.81 MiB 换掉 2553 token 的 prefill），说明“L 可替代 S”的静态判定与真实 `load()` 行为一致。

### 2.2 真实占用不变量

`total_size` 始终包含所有未成功删除的物理条目。证据链：

- `remove_file()` 是唯一删除原语，返回 `removed / already_missing / failed`，只有前两者扣减
- 删除失败 → `quarantined`：仍计入 `cache_disk_bytes`，但不再参与查找与淘汰
- `fix4-a2.log`：`quarantined ... keeping 505.77 MiB accounted for` → `entries 2 / bytes 1.64161e+09（505.77 + 1059.80 MiB）/ quarantined 1`
- `fix3-w7.log`：`over quota but could not be removed, keeping 505.77 MiB` → 长条目（330.03 MiB）被配额淘汰，隔离条目的账面字节未被抹除

### 2.3 无重复路径不变量

文件名 = 内容 hash，因此 **hash 命中即路径命中**。`claim_path_locked()` 在磁盘锁内解析所有权：

| 路径状态 | 处理 |
|---|---|
| 无记录 | `free`，允许写入 |
| 可用且身份全同 | `identical`，跳过写入（计 dedup） |
| 可用但身份不同（**含 token 长度不同**） | `conflict`，拒绝写入并计数，不覆盖文件 |
| 被隔离 | 先真实回收（`remove_file` 成功才释放路径并调整账本）；失败则 `conflict` |

`write_entry()` 的 `path_owned` 形参保证：没有所有权证明时绝不替换既有文件。

---

## 3. 过程中发现并修复的实现缺陷

以下是修复过程中**测试先失败、后修正**的项，如实列出：

| # | 缺陷 | 发现方式 | 修正 |
|---|---|---|---|
| 1 | 媒体身份只用块 ID，同 ID 不同块长会误判为共享 | 单测首轮 2 项失败 | 身份键改为 `id#count` |
| 2 | `on_snapshot_committed()` 先容量检查再挂引用，预算为 1 时把即将被引用的节点先淘汰 | 单测 | 改为先挂引用再容量检查 |
| 3 | 同名**隔离**条目且身份相同时被误判 dedup，导致该 prompt 永久无法重缓存 | 端到端首次尝试（`capture_boundary: captured = no`） | 隔离条目不算“已存储”，交由 worker 在写盘时解析路径 |
| 4 | 规划器把 `limit == 0` 当成“淘汰到 0”，不限量场景仍清空缓存 | 单测首轮 7 项断言失败 | `limit == 0` 即不限量，直接返回 `fits = true` |
| 5 | 双向媒体匹配用 `\|\|` 短路（**规划已指出，并经验证确认**） | 单测：把代码临时改回旧写法，同用例返回 12，正确值 4 | 两方向独立求最早差异再取 `min` |

第 5 项是规划文档预判的风险，我做了对照复现：临时恢复旧写法后同一用例立即失败（`m.usable_lcp == 4 (12 vs 4)`），恢复修复版本后通过。

---

## 4. 测试覆盖

三套纯 CPU 单测（不加载模型，`cl` 直接编译；因服务端测试目录是需运行服务端的 pytest 套件，未接入 CMake）：

| 测试文件 | 断言数 | 状态 |
|---|---|---|
| `tools/server/tests/unit/test_cache_prefix_index.cpp` | 37 | 通过 |
| `tools/server/tests/unit/test_disk_cache_match.cpp` | 63 | 通过 |
| `tools/server/tests/unit/test_disk_cache_quota.cpp` | 38 | 通过 |

`test_disk_cache_quota.cpp` 含**旧/新顺序对照模拟**：同一组模拟文件（Q 600 MiB 不可删 / S 200 MiB / L 300 MiB，limit 800 MiB）下，旧顺序得到 `S=false, L=false`（两者都消失），新顺序得到 `S=true, L=false`（回滚 L、保留 S）。这直接钉住了第三轮修复的目标。

标准回归（`test-run/logs/final-reg.log`，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）：

| 请求 | `prompt_n` | `cache_n` | 输出 sha |
|---|---|---|---|
| 2000 字符 | 1385 | 0 | `fd7c5bda91fa1d60` |
| 同请求重复 | 4 | 1381 | `fd7c5bda91fa1d60` |
| `ext:2300` | 215 | 1381 | `03e0e517e9ca99c0` |
| 再次重复 | 4 | 1381 | `fd7c5bda91fa1d60` |

与上一轮逐项相同，输出 sha 相同；`write_rejected 0 / path_conflict 0 / hash_collision 0`。

---

## 5. 未覆盖项与已知残留

**如实列出，供审查时判断风险：**

1. **`write_rejected` 分支的端到端触发未构造**。需要新条目单体在配额内、但回收全部可回收对象后仍放不下。本机条目体积由检查点主导（1.4K token 即 505 MiB），构造该组合需人为缩放条目体积，不属于真实输入。该分支仅有单测（`test_nothing_reclaimable`、`test_limit_below_entry_size`）与静态核对。

2. **真实媒体 prompt 未验证**。测试机为纯文本，媒体身份分歧、块内部边界、不同块长等分支全部由单测覆盖。**若规划后续要进入阶段 4（分块 KV），建议先补一组带 mmproj 的真实输入验证媒体路径**。

3. **跨进程 hash 文件名冲突未构造**。64 位 hash 碰撞无法在合理时间内构造，`claim_path_locked` 的 `conflict` 分支只有单测（`classify_duplicate` 的不同长度/不同媒体分支）。端到端仅确认计数为 0。

4. **`sweep_expired()` 与配额事务的协作是间接的**。当前实现中 `enforce_limit()` 首行调用 `sweep_expired()`，此时保护集尚未传入，但被隔离的文件本身就不会被删除，因此不会出现“先在配额事务内删掉受保护条目”。这一点是**推理**，不是构造验证。

5. **`enforce_limit()` 首次扫描可能清空所有内容**：扫描时会调用 `enforce_limit()`，若缓存总量超过上限，全部条目会被淘汰（这是设计行为，非缺陷）。审查时请注意这与本轮的准入保护不冲突。

6. **未执行**：多 slot（`-np > 1`）、多用户、Linux/POSIX 构建、中断/取消路径、长时间内存压力。本轮范围明确排除。

---

## 6. 一条测试方法上的教训（建议写入下一次规划）

我在第三轮曾把“替代路径端到端未复现”记为**不可构造**，理由是“能过 mirror 抑制的长条目必然超成本守卫”。**这个结论是错的**：

- mirror 抑制阈值 = `S_token + 4096`
- 成本守卫要求 `L_size ≤ S_token × 768 KiB`
- 两者能否同时满足取决于 **S 的长度**，因为守卫是绝对值而上限随 S 线性增长

按本机实测比率（约 0.17–0.21 MiB/token）：

| S (token) | 守卫上限 | L 最小长度 | L 预估体积 | 可构造 |
|---|---|---|---|---|
| 1385 | 1039 MiB | 5481 | 1168 MiB | 否 |
| **2557** | **1875 MiB** | **6553** | **1406 MiB** | **是** |
| 4000 | 3000 MiB | 8096 | 1726 MiB | 是 |

用 S=2557（3600 字符）、L=7516（11000 字符）构造成功。**教训：当结论是“不可构造”时，应当先做参数化可行性分析，而不是凭单点实测外推。** 这条经验已写入 `test-run/README.md`。

---

## 7. 审查建议

规划人员复审时建议优先核对：

1. `worker()` 的三段式顺序（准入 → 回收 → 替代）是否与 §2.1 的不变量严格对应，特别是 `admitted` 的计算是否基于 `entries` 的实际查找而非假设。
2. `drop_prefixes(replacement_is_admitted=false)` 的早退分支是否真的不产生任何副作用（当前只统计计数并打印 TRC）。
3. `enforce_limit(keep)` 的保护集语义是否覆盖所有“本次事务不可失去”的条目（目前只有新条目；`keep_quarantined` 的情况由隔离条目天然不可淘汰覆盖）。
4. `quota_items_locked()` 的 `path_storage` 指针稳定性（先 `reserve`，再逐项 `push_back`）在条目数变化后是否仍成立。
5. §5 未覆盖项中，哪些必须在本轮补齐、哪些可以留给后续阶段。
