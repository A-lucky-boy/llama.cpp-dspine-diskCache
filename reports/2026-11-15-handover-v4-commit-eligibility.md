# 2026-11-15 23-45 交付说明（单用户磁盘提示缓存：第六轮缺陷修复）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `c5d403d`（会话前 HEAD，第五轮交付说明提交） |
| 本次交付终点 | `1ab6528` |
| 代码修复提交 | `1ab6528` |
| 规划依据 | `betterModification.md` → `2026-11-15 23-17 - test1_2` |
| 构建产物 | `0.9.0-dev (build 23, commit c5d403d)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | 缺陷修复，非新算法实验 |

本文档汇总第六轮修复的**实际改动、复现证据与未覆盖项**。逐项实现细节见 `change.md` 的 `2026-11-15 23-40` 条目；项目现状见 `projectSummary.md`。

---

## 1. 本轮修复范围

规划只提出一个问题：**隔离条目可能被通知为 `persisted`**。

| 问题 | 规划章节 | 性质 | 状态 |
|---|---|---|---|
| 回滚失败登记的隔离条目触发 `notify_committed()`，前缀目录据此建立物理引用 | §二 | 静态确认 + 单测复现 + 端到端确认 | 已修 |

改动集中在两处：

| 位置 | 改动 |
|---|---|
| `disk_cache_commit_candidate` / `disk_cache_commit_eligible()`（新增） | 提交资格判定：路径存在 **且未隔离且 token 非空** |
| `worker()` 末尾提交查找 | 由"同路径存在"改为"存在可用条目"，仅资格成立才 `notify_committed()` |

### 1.1 缺陷的成因链

`worker()` 内有两个分支会把条目保留在 `entries` 里，但**只为记账、不代表可恢复**：

1. `!plan.fits` 且 `remove_file(path)` 失败 → `quarantine_entry(e, ...)` + `total_size += size_file` + `entries.push_back(...)`
2. `plan.fits` 但物理回收后仍超额、`superseded == 0`、回滚又失败 → 对已入 `entries` 的条目 `quarantine_entry(cur, ...)`

原先末尾的提交通知只判断 `e.path == path`，这两条路径下该条件为真，于是：

```
隔离条目 → notify_committed() → prefix_index::on_snapshot_committed() → persisted
```

而同时：

- `server_disk_cache::load()` 通过 `is_reusable_entry()` 排除该条目
- `server-context.cpp:1775` 的启动登记 `if (e.quarantined) continue;` 也跳过它

三者组合的结果是一个**运行期存在、无法恢复、重启后消失**的持久化引用。运行时与启动期对同一路径给出相反结论，是本轮要消除的错配。

---

## 2. 修复的落实

### 2.1 资格判定显式化并可测

```cpp
struct disk_cache_commit_candidate {
    std::string path;
    bool        quarantined = false;
    bool        has_tokens  = true;
};

bool disk_cache_commit_eligible(const std::vector<disk_cache_commit_candidate> & entries,
        const std::string & path);
```

放在 `server-disk-cache-quota.*`，因为它与配额规划同属纯元数据判定：不取锁、不碰文件、不涉及 `llama_context`，因此可以脱离模型与服务单测。

`worker()` 调用的是同一个函数，避免"单测验证的逻辑与运行时实际使用的逻辑不是同一份"。

### 2.2 `worker()` 的三种结局被区分

| 最终状态 | `entries` | 提交通知 |
|---|---|---|
| 新快照成功入库且有效 | 非隔离条目 | **发送** |
| `!plan.fits`、回滚成功 | 不包含新条目 | 不发送 |
| `!plan.fits`、回滚失败 | 包含隔离条目 | 不发送 + 明确诊断 |
| 物理回收后超额、无替代删除、回滚成功 | 不包含新条目 | 不发送 |
| 同上但回滚失败 | 包含隔离条目 | 不发送 |

路径存在但无可用条目时打印：

```
disk cache: '<path>' stays accounted for but is not usable, not publishing a persisted reference
```

把"仅有账面占用"与"已提交快照"在日志层面也区分开，避免以后有人再按路径存在性推断状态。

### 2.3 没有过度排除

规划 §4.2 特别提醒：新条目已替代短快照、虽存在不可回收的超额字节但自身未隔离时，**仍属有效缓存条目**，不能把所有超配额条目排除。

资格判定只看 `quarantined` 与 `has_tokens`，不看 `total_size`。单测场景 7 专门钉住这一点（`pc-stuck.bin` 隔离 + `pc-new.bin` 有效 → 后者仍应发送），标准回归中 `persisted_refs 1` 也确认有效路径未被误伤。

---

## 3. 测试覆盖

### 3.1 缺陷复现（单测）

把 `disk_cache_commit_eligible()` 临时改回旧的"仅按路径存在"语义后重编译，同一组用例有 3 项失败：

```
FAIL tools\server\tests\unit\test_disk_cache_quota.cpp:808: !disk_cache_commit_eligible(entries, "pc-aaa.bin")
FAIL tools\server\tests\unit\test_disk_cache_quota.cpp:817: !disk_cache_commit_eligible(entries, "pc-aaa.bin")
FAIL tools\server\tests\unit\test_disk_cache_quota.cpp:825: !disk_cache_commit_eligible(entries, "pc-aaa.bin")
```

三个失败分别对应：`!plan.fits` 回滚失败、配额不足回滚失败、token 为空。恢复修复版本后全部通过。**这是本轮缺陷的确定性复现证据。**

### 3.2 新增单测（8 组）

| 场景 | 期望 |
|---|---|
| 1 有效入库 | 可提交 |
| 2 回滚成功、路径消失 | 不可提交 |
| 3 `!plan.fits` 回滚失败（隔离） | 不可提交 |
| 4 配额不足回滚失败（隔离） | 不可提交 |
| 5 token 为空 | 不可提交 |
| 6 别的路径被隔离 | 本路径仍可提交 |
| 7 因无关隔离文件整体超额但自身有效 | 仍可提交（防止过度排除） |
| 8 同路径多条记录、其中有可用的 | 可提交 |

### 3.3 三套磁盘层单测

```
all disk-cache match checks passed
all disk-cache quota checks passed      （含本轮新增 8 组）
all prefix-index checks passed
```

### 3.4 端到端（`--cache-prefix-discovery` 开启，`-np 1`，句柄占用）

指标数值取自运行时 `--metrics` 端点的 `/metrics` 抓取（`test-run/metrics.py`），不在服务日志中；日志证据为对应的状态迁移行。

| 状态 | `cache_prefix_persisted_refs` | `cache_prefix_snapshots` | `cache_prefix_unavailable` | 日志/日志证据 |
|---|---|---|---|---|
| 有效条目基线 | 1 | 1 | 0 | `stored 1385 tokens, 505.77 MiB` |
| 隔离之后 | **0** | **0** | 1 | `quarantined ... keeping 505.77 MiB accounted for and excluding it from lookups` |
| 重启之后 | 0 | 0 | 0 | `prefix discovery: ... 0 snapshot(s) registered` |

证据文件：`test-run/logs/fix6-p1.log`（隔离）、`test-run/logs/fix6-p1r.log`（重启）、`test-run/logs/fix6-reg.log`（标准回归）。

基线为 1 是关键：证明修复没有把有效路径一并排除。隔离后降为 0、且重启后启动登记同样为 0，两者一致——这正是本轮要消除的错配。

同一事务还顺带触发了上一轮的配额复核路径：

```
over the 1400.00 MiB limit but no removable entry left (1565.57 MiB used)
quota still exceeded after the admission pass (165.57 MiB over), rolling back '...pc-3a888c45f86e70ff.bin' because no superseded snapshot was deleted
```

磁盘目录最终只剩隔离文件（530,337,934 B），未出现孤立文件。

### 3.5 标准回归

```
1385/0、4/1381、215/1381        —— 与上一轮逐项相同
输出 sha fd7c5bda…、03e0e517…   —— 与上一轮相同
cache_disk_write_rejected_total 0、cache_prefix_nodes 3
cache_prefix_persisted_refs 1、cache_prefix_snapshots 1
```

---

## 4. 新增接口

| 名称 | 位置 | 含义 |
|---|---|---|
| `disk_cache_commit_candidate` | `server-disk-cache-quota.h` | 一个路径在事务结束时的对外可见状态：`path` / `quarantined` / `has_tokens` |
| `disk_cache_commit_eligible(entries, path)` | 同上 | 唯一提交资格判定：存在 + 未隔离 + token 非空 |

未新增指标，未新增通知通道，`notify_committed` / `notify_invalidated` 的锁内同步语义与单调 `event_epoch` 不变。

---

## 5. 未覆盖项与已知残留

**以下三项未取得端到端证据，只有单测与静态核对。**

| 未覆盖项 | 原因 |
|---|---|
| `!plan.fits` 且回滚删除失败的分支 | 需要新条目单体在配额内、回收全部可回收对象后仍放不下，**且**回滚瞬间被外部进程独占。本机条目体积由检查点主导（1.4K token 即 505 MiB），任意两条相加就超过任何可设的 MiB 级配额；也无法在 `remove_file(path)` 调用瞬间注入占用。覆盖方式：单测场景 3/4 + 静态核对 |
| "同名快照先失效、后回滚失败"的时序 | 需要同一路径先 `notify_invalidated` 再在同一事务内回滚失败，构造依赖前者。规划 §4.3 的要求由提交资格判定在源头保证（该路径根本进不了 `notify_committed`），但**未单独构造时序用例证明** |
| 规划 §4.4 建议的"测试用删除故障注入钩子" | **本轮未实现**。它会往生产源码引入测试专用路径（编译期开关或运行期钩子），而现有单测已能在纯元数据层覆盖全部五个事务结局、端到端也用真实句柄覆盖了 TTL/LRU 与配额复核两条路径。是否值得为它留钩子，建议下一轮单独决策，本轮不擅自扩大改动面 |

其余范围外项：真实媒体 prompt（测试机为纯文本）、多 slot（本轮明确 `-np 1`）、Linux/POSIX 构建、请求取消/中断路径。

**覆盖强度自评**：本轮缺陷的直接证据是单测的"旧语义 3 项失败"与端到端"`persisted_refs` 由 1 降为 0、重启后一致"。但两个回滚失败分支本身仍未在真实运行中触发过——它们依赖"单体在配额内却仍放不下"的极端条件。若规划认为该分支必须取得动态证据，可行的办法是给 `disk_cache_plan_quota()` 加一个仅测试构建可用的强制 `fits = false` 注入点，而不是依赖文件系统故障注入。

---

## 6. 审查建议

1. 优先核对 `worker()` 末尾的提交查找：确认资格判定的唯一来源是 `is_reusable_entry()` / `disk_cache_commit_eligible()`，不存在遗留的 `e.path == path` 单独判定。
2. 确认 `disk_cache_commit_eligible()` 与 `server-context.cpp:1775` 的启动登记判据一致（后者为 `if (e.quarantined) continue;`）。前者多了 `has_tokens` 一项——这是有意为之：无 token 的条目也无法参与身份比较，但请审查确认这不会与实际可能出现的合法条目冲突。
3. 本轮未改 `server-disk-cache-quota.*` 的准入策略与 `enforce_limit()` / `sweep_expired()`，只新增了一个纯判定函数；若审查认为资格判定应放在 `server-disk-cache-match.*`（语义更接近"静态等价判定"）而非 quota 模块，位置可调整。
4. 规划 §4.4 的故障注入钩子是否实施，详见 §5 第三行，需要规划决策。

---

## 7. 推送状态

| 项 | 值 |
|---|---|
| 远端 | `git@github.com:A-lucky-boy/llama.cpp-dspine-diskCache.git` |
| 分支 | `main` → `origin/main` |
| 本次推送提交 | `1ab6528`（`c5d403d..1ab6528`） |
| 推送结果 | 成功 |

`test-run/` 下仅提交复现脚本（`srv.sh`、`probe.py`、`metrics.py` 等），运行产物（`cache-*/`、`*.log`、`*.exe`、`hold*.py`、`bt*.bat`）由 `.gitignore` 排除，不随本次推送发布。工作树在提交后为干净状态。
