# 2026-11-16 10-10 交付说明（单用户磁盘提示缓存：第十轮 —— in-flight 去重改用完整身份）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `b4b831c`（会话前 HEAD，第九轮交付说明的推送状态补充） |
| 本次交付终点 | 分支 `main` 顶端（功能提交 `12075e4`，其后仅有本说明自身的文档提交） |
| 功能提交 | `12075e4`（全部代码与测试改动） |
| 规划依据 | `betterModification.md` → `2026-11-16 09-35 - test1_3` |
| 构建产物 | `0.9.0-dev (build 36, commit b4b831c)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | **正确性修复（身份判定一致性）**（规划 §1 明示不是新缓存算法实验） |

本轮处理规划 §1.2 指出的写队列 in-flight 去重缺陷：排队中的条目只按 64-bit hash 判定"是否已经在写"，而落盘条目一侧已按完整 token/media 身份判定，两条路径不一致。逐项实现细节见 `change.md` 的 `2026-11-16 10-10` 条目；项目现状见 `projectSummary.md`。

按规划 §1.1 的要求，先确认了缺陷来源：`std::unordered_set<uint64_t> inflight` 是**原始 baseline `eb76818` 就存在的**旧限制，不是近期引入；本轮修复只是让它与现行完整身份规则统一。

---

## 1. 缺陷定位与不变量

### 1.1 源码位置（与规划 §1.2 一致）

| 位置 | 内容 |
|---|---|
| `tools/server/server-task.h` | `server_disk_cache::inflight` 为 `std::unordered_set<uint64_t>` |
| `tools/server/server-task.cpp` | `store()`、`mirror()` 均执行 `if (inflight.count(hash))`，命中即计 `n_enqueue_dedup` 并返回 |
| `tools/server/server-task.cpp` | `worker()` 四条分支用 `inflight.erase(hash)` 释放占位 |

已提交条目的判定是**完整**的（`disk_cache_classify_duplicate()` 逐项核对 token 与媒体三元组）；等待写入或正在写入的条目**没有保存可比较的身份**。

### 1.2 不变量

```
hash 相同 + 完整身份相同  →  真正的 in-flight duplicate，合并
hash 相同 + 完整身份不同  →  同一路径的身份冲突，拒绝该次写入
```

旧行为把第二种情况也归入第一种，结果是**静默丢弃一个从未落盘的快照**，并在统计上记为"成功去重"。

按规划 §1.2，本轮不声称用户实际请求中已发生 hash 碰撞——这是由源码确认的逻辑不一致，用元数据夹具注入相同 hash 即可稳定重现。

---

## 2. 实现

### 2.1 inflight 保存拥有自身内存的完整身份（规划 §3.1）

```cpp
struct inflight_identity {
    llama_tokens              tokens;
    std::vector<uint32_t>     media_indices;
    std::vector<uint32_t>     media_token_counts;
    std::vector<std::string>  media_ids;

    disk_cache_prompt_identity view() const; // 非拥有视图，仅供比较
};

std::unordered_map<uint64_t, inflight_identity> inflight;
```

关键点：值**拥有自己的内存**，不指向 `queue` 元素。worker 在写完成之前就执行了 `pop_front()`，因此指向 deque 的指针恰好在最需要判定的时候失效——规划 §3.1 明确禁止这种引用。同时不保存 target/draft 的 KV buffer，只存 token id 与媒体块身份，容量由写队列的条目上限（`DISK_CACHE_QUEUE_MAX_ENTRIES = 8`）约束。

### 2.2 store() 与 mirror() 共用同一判定（规划 §3.2）

新增三个私有方法，均在 `mtx` 保护下：

| 方法 | 职责 |
|---|---|
| `inflight_check_locked(hash, wanted)` | 返回 `inflight_verdict{free, duplicate, conflict}` |
| `inflight_add_locked(hash, pending)` | 从 pending 自身数组拷出身份 |
| `inflight_erase_locked(hash)` | 释放占位 |

判定内部调用**现有** `disk_cache_classify_duplicate(queued, wanted, same_hash=true)`：

```cpp
if (disk_cache_classify_duplicate(queued, wanted, true) == disk_cache_duplicate::identical) {
    return inflight_verdict::duplicate;
}
return inflight_verdict::conflict;
```

按规划 §2 的要求，**没有**另造第二份身份语义——队列侧与落盘侧对"同一 prompt"的定义因此由同一个函数决定。

两处调用点由二分支改为三分支：

```
duplicate → n_enqueue_dedup++，跳过（原语义不变）
conflict  → n_hash_collisions++ / n_path_conflicts++，记录明确错误并拒绝该次写入
free      → 继续原有 covered_cheaply()、队列预算与入队
```

`conflict` 分支不覆盖已有的 `pc-<hash>.bin`，也不改动先到的写任务，符合规划 §3.2 与 §5.3。

未改动 `covered_cheaply()`、配额准入、提交资格、隔离生命周期与前缀目录通知条件（规划 §3.2 末段）。

### 2.3 占位生命周期（规划 §3.3）

| 阶段 | 处理 |
|---|---|
| 已入队 | `inflight_add_locked()` 与 `queue.push_back()` 在同一 `lock_guard` 内完成，无"已入队但尚未登记"窗口 |
| worker 已出队 | 表中项独立于 `queue`，`pop_front()` 不影响可见性 |
| 释放 | 四处释放点原本就在锁内并伴随 `queue_bytes` 减记，统一改用 `inflight_erase_locked()` |

**成功路径原子性已核实**：`worker()` 提交成功分支的 `inflight_erase_locked(hash)` 与 `entries.push_back()` 位于**同一个** `std::lock_guard<std::mutex> lock(mtx);` 作用域内，前台不会观察到"既不在 inflight、也不在 entries"的中间态，满足规划 §3.3 末段要求。

**新增异常回滚**：`inflight_add_locked()` 与 `queue.push_back()` 都可能抛 `bad_alloc`。若先插占位再入队失败，会留下"hash 被永久占用但没有写任务"的孤儿状态——之后同一路径的每次写入都会被回以"already queued"而永不落盘。现改为两者作为整体回滚：

```cpp
try { inflight_add_locked(hash, pending); }
catch (...) { queue_bytes -= std::min(queue_bytes, queued_bytes); throw; }

try { queue.push_back(std::move(pending)); }
catch (...) { inflight_erase_locked(hash);
              queue_bytes -= std::min(queue_bytes, queued_bytes); throw; }
```

`queued_bytes` 在 `std::move` 之前取出，避免读已移出对象。

### 2.4 与主动前缀物化的兼容（规划 §3.4）

未修改物化何时在 prefill 安全点执行、如何求 `expected_position_end`、是否跳过多模态输入，也未改动物化计数。仅让其进入写队列时的"排队中是否已有相同快照"判断更准确。相关回归见 §3.4。

---

## 3. 验证

### 3.1 缺陷复现（构造性）

按规划 §6.2 要求，**直接注入相同 hash** 而非寻找真实 64-bit 碰撞。把 `disk_cache_classify_duplicate()` 临时改回 hash-only 语义（`same_hash` 即 `identical`）后，单测出现 **12 项失败**，其中本轮新增的 6 项全部命中：

```
FAIL test_disk_cache_match.cpp:559-560  不同 token 长度（双向）
FAIL test_disk_cache_match.cpp:568      同长度不同 token
FAIL test_disk_cache_match.cpp:577      同 token 不同 media id
FAIL test_disk_cache_match.cpp:586      同 media id 不同起点
FAIL test_disk_cache_match.cpp:614      结构非法身份
```

恢复修复版后全部通过。这证明新增用例确实钉住了该缺陷，而非仅仅通过。

### 3.2 新增单测

| 用例 | 覆盖 |
|---|---|
| `test_inflight_identity_verdict()`（8 组） | 完全相同→duplicate；不同长度 / 同长不同 token / 同 token 不同 media id / 同 media id 不同起点 / 结构非法→conflict；hash 不同→不参与分类；含媒体但完全一致→duplicate |
| `test_inflight_lifecycle()`（8 组） | 入队即可见；worker 出队后仍可见；不同身份被拒且首写者身份未变；相同身份合并；各终态释放后同一路径可重新入队；两个无关 hash 独立跟踪 |

`test_inflight_lifecycle()` 使用生产路径同一个 `disk_cache_classify_duplicate()`，不是第二份规则，符合规划 §2「单测与生产路径共用真正的完整身份判定」。

### 3.3 端到端（规划 §6.4）

| 检查 | 结果 |
|---|---|
| 四套独立单测 | 通过：`match` / `quota` / `prefix-index` / `prefix-materialize` |
| 突进链路（`--cache-prefix-materialize`） | `scheduled 2 / exported 2 / cache_disk_entries 1 / persisted_refs 1`；`hash_collision 0`、`path_conflict 0` |
| 重启后磁盘恢复 | `source = disk`、`reused_disk = 2102`、`restored = 2102`、`read_bytes = 564668394` 与文件大小精确一致 |
| 开启/关闭态一致性 | **8 个请求 sha 逐字节一致**；关闭态三个 materialize 指标与 `cache_disk_hash_collision_total` 全为 0、`cache_disk_entries 0` |
| 恒等式 | `2504 = 0+0+0+2504`、`2468 = 1476+0+0+992`、`2506 = 1476+0+0+1030`、`2540 = 1478+0+0+1062` |

### 3.4 既有物化语义未被影响

按规划 §3.4 要求核对：`slot.prompt.tokens.size() < K` 的中间批次仍为「**等待下一批**」而非置 `done`（上一轮修正的语义保持）；`K == n_past`（`scheduled at n_past`）与 `K == input_end` 两个安全点在本轮端到端中均正常触发并成功导出，见 §3.3 的 `exported 2`。

### 3.5 `enqueue_dedup` 来源定位

本轮唯一一次 `enqueue_dedup` 来自：

```
store: disk cache: 2101 tokens are covered up to a 4096-token tail by a stored entry,
       skipping a 703.82 MiB write
```

即 `covered_cheaply()`，**不是** in-flight 判定。单请求串行场景下 in-flight 冲突未自然触发——这与规划 §6.2 的要求一致（用元数据夹具注入，而非等待真实碰撞）。

---

## 4. 未执行的检查（明确记录）

按规划 §6.3 与 §7 的清单，以下**未执行**，不得视为通过：

1. **故障注入清单（规划 §6.3）全部未执行**：`worker` 正常写盘、`write_entry` 失败、路径冲突、已被已有条目覆盖、quota 拒绝、回滚失败、worker 停止——七个分支的占位释放已**逐一静态核实**（四处 `inflight_erase_locked` 均在锁内并伴随 `queue_bytes` 减记），但**没有在真实服务上人为触发过**。
2. **入队分配失败的异常回滚只做静态核对**，未注入 `bad_alloc` 验证。
3. **`inflight` 表的并发行为未验证**：本机 `-np 1` 单请求串行，两个请求同时进入 `store()` 的情形未构造。
4. **`mirror()` 路径的运行验证未执行**：本轮未开启 idle flush（`--cache-disk-idle-flush 0`），其 in-flight 判定改动只有单测覆盖。
5. **媒体输入未端到端验证**：测试机无多模态模型；in-flight 的媒体分类由元数据用例覆盖；`media_state_unverified` 主动捕获门禁保持原样，未在真实媒体请求下验证。
6. **context shift / 取消 / decode 重试**：未构造。

---

## 5. 本轮无自查发现的回归

与第九轮不同，本轮实现过程中未发现需要回退的自身错误。本轮只进行了一次 CUDA 构建（`b44.log`，`EXIT=0`），三分支判定与异常回滚在首次构建后即通过全部单测与端到端检查，无需回退重来。

需要注意的是，这项结论的强度受 §4 限制：故障注入与并发场景未执行，因此"未发现回归"只覆盖了纯文本单请求串行路径与元数据用例。

---

## 6. 边界确认

- 文件命名 `pc-<hash>.bin`、磁盘格式 v8、manifest v3、CRC64、配置签名与媒体身份规则**均未改动**
- 未新增 CLI 参数；默认配置与已有开关含义不变
- 单用户、本机、`-np 1`；未加入鉴权、多租户或多 slot 调度
- 未启动分块 KV、压缩、差量写入或范围恢复等新算法（规划 §5.7）
- 媒体主动写盘门禁不变，未借本轮扩大未验证媒体 KV 捕获范围（规划 §5.6）
