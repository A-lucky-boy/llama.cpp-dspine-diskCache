# 2026-11-15 23-20 交付说明（单用户磁盘提示缓存：第五轮缺陷修复）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `2799400`（会话前 HEAD） |
| 本次交付终点 | `059e267` |
| 代码修复提交 | `059e267` |
| 规划依据 | `betterModification.md` → `2026-11-15 22-40 - test1_2` |
| 构建产物 | `0.9.0-dev (build 23, commit 2799400)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | 缺陷修复，非新算法实验 |

本文档汇总第五轮修复的**实际改动、复现证据与未覆盖项**。逐项实现细节见 `change.md` 的 `2026-11-15 23-15` 条目；项目现状见 `projectSummary.md`。

---

## 1. 本轮修复范围

| 问题 | 规划章节 | 性质 | 状态 |
|---|---|---|---|
| A 保护指针在 `entries.erase()` 后失效 | §一.2 | 静态确认 + 端到端确认触发路径真实存在 | 已修 |
| B 回滚删除失败成为未记账孤儿文件 | §一.3 | 静态确认 | 已修 |
| C `plan.fits` 与物理兑现未分离 | §一.4 | 静态确认 + 端到端确认超额状态真实存在 | 已修 |

改动集中在三个函数与一个类型：

| 位置 | 改动 |
|---|---|
| `disk_cache_protected_paths`（新增别名） | `std::unordered_set<std::string>`，取代 `std::vector<const server_disk_cache_entry *>` |
| `is_protected()`（新增） | 按 `entry.path` 判定，不依赖地址 |
| `sweep_expired(protected_paths)` | TTL 判定前先跳过受保护条目 |
| `enforce_limit(protected_paths)` | 同一集合传给 TTL 清扫；LRU 每轮按路径重新判定 |
| `drop_prefixes(...)` | 返回值改为实际删除条数 |
| `worker()` 准入事务 | 回滚分支处理 `remove_file()` 结果；物理回收后复核 `total_size` |

---

## 2. 三个不变量的落实

### 2.1 保护身份不随容器移动

**配额事务须保护的条目，在整段进程内必须以稳定身份被识别。**

`entries` 是 `std::vector<server_disk_cache_entry>`。`enforce_limit()` 的第一件事是 `sweep_expired()`，其 `entries.erase(it)` 会把被删元素之后的所有元素前移。原实现把 `&*cur` 存进 `keep`，TTL 清扫删掉新条目前的条目后，这些地址即失效。

```cpp
// 之前：地址会失效
std::vector<const server_disk_cache_entry *> keep;
for (const auto & cur : entries) {
    if (cur.path == path) { keep.push_back(&cur); break; }
}
enforce_limit(keep);           // 内部先 sweep_expired()，可能 erase 掉 keep[0] 之前或之后

// 现在：路径副本，erase 不影响
disk_cache_protected_paths protected_paths = { path };
enforce_limit(protected_paths);  // sweep_expired(protected_paths) + LRU 按 path 判定
```

**端到端确认触发路径真实存在**（`test-run/logs/fix5-v1.log`，`--cache-disk-ttl 5`，`srv.sh` 固定参数）：

```text
行 32  worker: disk cache: stored 1385 tokens, 505.77 MiB
行 44  sweep_expire: dropping 'test-run/cache-v1/pc-736f...bin': expired after 7 s idle (505.77 MiB)
行 45  worker: disk cache: stored 2690 tokens, 569.49 MiB
行 57  sweep_expire: dropping 'test-run/cache-v1/pc-65e0...bin': expired after 7 s idle (569.49 MiB)
行 58  worker: disk cache: stored 4038 tokens, 635.30 MiB
指标   cache_disk_entries 1 / cache_disk_bytes 6.66161e+08 / quarantined 0
```

每一次准入 pass 内都发生了一次真实的 `entries.erase()`——问题 A 的触发条件在正常 TTL 使用下必然出现，新条目在两次 erase 后均正常留存。

### 2.2 回滚失败必须记账

**被拒绝写入的文件若无法删除，其字节仍占用磁盘，不得假装无事发生。**

```cpp
const disk_cache_remove_status rolled_back = remove_file(path);
counters.n_write_rejected++;

if (rolled_back == disk_cache_remove_status::failed) {
    quarantine_entry(e, "write rejected but the rollback failed");
    total_size += size_file;      // 真实字节入账
    entries.push_back(std::move(e));   // 但 quarantined=true，排除出全部复用路径
    counters.n_rollback_failed++;
}
```

已确认该路径与既有隔离语义一致：`is_reusable_entry()` 为 false 的条目不参与 `load()` 候选、写入抑制、`worker()` 冗余判断、精确去重与 `persisted` 引用；`claim_path_locked()` 仍负责在后续写入需要该路径时尝试真实回收。

### 2.3 计划可行不等于已兑现

**`plan.fits` 只说明"假定候选都能删除"时的可行性；物理删除可能失败。**

规划 §一.4 要求区分两种情形，落实如下：

```cpp
const size_t superseded = drop_prefixes(..., /*replacement_is_admitted=*/admitted);

if (limit_size > 0 && total_size > limit_size) {      // 物理回收后复核
    if (superseded == 0) {
        // 本次没删任何被替代快照 → 超额只因删除失败 → 回滚新条目
        // 回滚若又失败 → 按隔离条目登记
    } else {
        // 已删除只有新条目能替代的快照 → 保留真实超额账本，拒绝进一步不受控写入
    }
}
```

`drop_prefixes()` 的返回值为"实际删除条数"，正是区分这两条分支的依据。

**端到端确认超额状态真实可达**（`test-run/logs/fix5-t1.log`，`--cache-disk-max 1400`，Python 持有 `.bin` 句柄）：

```text
行 45  quarantine_e: '...pc-736f...bin' expired but could not be removed,
       keeping 505.77 MiB accounted for and excluding it from lookups
行 46  enforce_limi: over the 1400.00 MiB limit but no removable entry left (1565.57 MiB used)
行 47  worker: disk cache: stored 6233 tokens, 1059.80 MiB
指标   cache_disk_entries 2 / cache_disk_bytes 1.64161e+09 / quarantined 1
```

隔离条目的账面字节保留、受保护的新条目未被淘汰、超额如实记录——未出现"删掉唯一替代者来修平配额"。

---

## 3. 测试覆盖

| 测试 | 类型 | 结果 |
|---|---|---|
| `test_disk_cache_quota.cpp`（19 组用例、83 处断言） | 纯 CPU 单测，不加载模型 | 通过 |
| `test_disk_cache_match.cpp`（85 处断言） | 纯 CPU 单测 | 通过（本轮未改动） |
| `test_cache_prefix_index.cpp` | 纯 CPU 单测 | 通过（本轮未改动） |
| `fix5-v1.log` | 真实模型端到端，TTL 介入准入 | 通过 |
| `fix5-t1.log` / `fix5-t3.log` | 真实模型端到端，句柄占用 + 配额 | 通过 |
| `fix5-reg.log` | 标准回归（双卡、both 捕获、发现开启） | 通过 |
| `fix5-restart.log` | 强杀重启 | 通过 |

**本轮新增的单测**（针对规划 §四.1 / §四.2 的定点触发要求）：

| 用例 | 断言内容 |
|---|---|
| `test_protection_identity_survives_prior_erase` | 受保护条目位于被 TTL 删除条目**之后**；先建保护集、再 erase、后判定，路径键版本必须保住它 |
| `test_protection_survives_eviction` / `_repeated_eviction` | 连续淘汰下受保护条目始终不被选中 |
| `test_protected_never_lru_victim` | 受保护条目即使 `last_used` 最旧也不被淘汰 |
| `test_quarantined_and_protected_together` | 隔离 + 受保护共存时 `fits == false`、`overshoot` 正确、两者都不进候选 |
| `test_rejected_write_rollback` | 回滚成功不计账面字节；回滚失败必须按隔离登记并计入 `total_size`；两者都不算成功写入 |

`sim_erase_by_path()` 按 `std::vector::erase()` 语义逐个前移尾部元素再收缩，用于真实复现"erase 导致元素移动"，而非仅标记删除。

**回归数值**（`fix5-reg.log`，与上一轮逐项相同）：

```text
1385/0、4/1381、215/1381
输出 sha：fd7c5bda…、03e0e517…（与上一轮一致）
write_rejected 0 / path_conflict 0 / cache_prefix_nodes 3
重启：1 entries after dedup/trim、prefix discovery 1 snapshot(s) registered
```

---

## 4. 新增指标

| 指标 | 含义 |
|---|---|
| `llamacpp:cache_disk_rollback_failed_total` | 回滚失败、被迫以隔离条目登记的写入数 |
| `llamacpp:cache_disk_quota_unreclaimed_total` | 因物理删除失败而未能真实回收配额的准入事务数 |

两者与已有的 `cache_disk_write_rejected_total`、`cache_disk_quarantined` 配合，可区分"配额拒绝"、"回滚失败"、"真实超额"三种状态，不会互相冒充 `dedup`。

---

## 5. 未覆盖项与已知残留

**以下三项未取得端到端证据，只有单测与静态核对。**

| 未覆盖项 | 原因 |
|---|---|
| 问题 B / §2.3 的 `plan.fits == false` 分支 | 需要新条目单体在配额内、且回收全部可回收对象后仍放不下。本机条目体积由检查点主导（1.4K token 即 505 MiB），任意两条相加就超过任何可设的 MiB 级配额，真实模型上无法构造该组合。覆盖方式：单测 `test_rejected_write_rollback`、`test_nothing_reclaimable`、`test_limit_below_entry_size` + 代码静态核对 |
| 回滚失败的端到端注入 | 需要"写入刚完成、临时句柄已关闭、随即被外部进程独占"的时序。本机只能稳定构造 TTL/LRU 阶段的句柄占用，无法在 `remove_file(path)` 调用的瞬间注入占用 |
| 问题 A 的"修复前会失败"对照 | 无法在真实服务上运行带失效指针的版本再比对——读失效指针是未定义行为，结果不确定。单测中路径键与指针键两套序列并列记录，**仅对路径键版本断言**；指针版本不断言 |

其余范围外项：真实媒体 prompt（测试机为纯文本）、多 slot（本轮明确 `-np 1`）、Linux/POSIX 构建、请求取消/中断路径。

**方法上的说明**：我在单测里写过一版"指针失效"对照实现，但它按 `vector` 语义只在 erase 时标记 `alive`、没有真正移动元素存储，因此**不能**作为缺陷复现证据。后改为 `sim_erase_by_path()` 真实前移元素，但指针版本依然不断言。若审查需要该缺陷的确定性证明，建议单独做一个带 `-D_ITERATOR_DEBUG_LEVEL=2`（MSVC 迭代器/指针调试）的构建，让失效访问直接触发断言——这比在单测里模拟更可靠。

---

## 6. 审查建议

1. 优先看 `worker()` 准入事务的三段顺序（试算 → 回收 → 复核），确认 `superseded` 的两种取值分支与规划 §一.4 的两种情形一致。
2. 核对 `enforce_limit()` / `sweep_expired()` 是否只通过 `is_protected()` 判定保护，不存在遗留的地址比较。
3. `quarantine_entry()` 现有 6 处调用（`2504` 定义；`3360` 扫描期、`4098` TTL、`4203` 配额、`4385` 回滚失败、`4452` 回滚失败且超额）。本轮新增 `4385`、`4452` 两处，均在磁盘锁内；`4385` 在同一分支内做 `total_size += size_file`，`4452` 不重复调整——该条目已入账，此处只需从可用集合排除。请核对两处的账本一致性。
4. 本轮未改 `server-disk-cache-quota.*` 的准入策略本身，只改了调用方对结果的采信方式；若审查认为规划器语义也该扩展（例如把"预计无法删除"提前纳入规划），那属于下一轮。

---

## 7. 推送状态

| 项 | 值 |
|---|---|
| 远端 | `git@github.com:A-lucky-boy/llama.cpp-dspine-diskCache.git` |
| 分支 | `main` → `origin/main` |
| 本次推送提交 | `059e267` |

`test-run/` 下仅提交复现脚本（`srv.sh`、`probe.py`、`metrics.py` 等），运行产物（`cache-*/`、`*.log`、`*.exe`、`hold*.py`）由 `.gitignore` 排除，不随本次推送发布。
