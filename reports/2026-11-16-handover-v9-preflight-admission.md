# 2026-11-16 10-20 交付说明（单用户磁盘提示缓存：第十一轮 —— 完整 KV 导出前的元数据准入）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `4decdf4`（会话前 HEAD，第十轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端（功能提交 `e004f71`） |
| 功能提交 | `e004f71`（全部代码与测试改动） |
| 规划依据 | `betterModification.md` → `2026-11-16 10-20 - test1_4` |
| 构建产物 | `0.9.0-dev (build 37, commit 4decdf4)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 交付性质 | **独立算法优化（准入提前）**（规划 §1） |

本轮实现规划 §3.1 的 Pre-export Materialization Admission：把磁盘层仅凭 token/media 元数据就能确定的判断，从"完整 KV 导出之后"提前到"导出之前"。逐项实现细节见 `change.md` 的 `2026-11-16 11-10` 条目；项目现状见 `projectSummary.md`。

---

## 1. 问题定位

规划 §2.1 指出的执行顺序可由源码直接确认，不是性能猜测：

| 步骤 | 位置 | 内容 |
|---|---|---|
| 1 | `server_prompt_cache::materialize()` | 调用 `llama_state_seq_get_size_ext` / `get_data_ext`，分配并导出完整 target+draft 状态 |
| 2 | `server_disk_cache::store()` | 此后才执行 `disk_cache_pack()`、完整身份去重、`inflight_check_locked()`、`covered_cheaply()`、队列容量检查 |

第 2 步的三项判断只依赖 token/media 元数据，却发生在第 1 步数百 MiB 的状态导出之后。

### 1.1 本轮必须保持的不变量（规划 §2.2）

- 前缀目录 `observed / eligible / persisted` 与真实 `reused_disk` 是不同概念，只有真实有效磁盘文件才有物理引用
- 已提交条目与 in-flight 条目都按**完整 token + media 三元组**判定"同一身份"，hash 只作索引键
- 长条目替代短条目复用现行媒体边界、最小长度、`DISK_CACHE_MAX_BYTES_PER_TOKEN` 规则，使用已提交文件的**真实 `size_file`**
- 隔离条目不参与可恢复覆盖、精确重复或 `persisted` 登记
- 预检查不私自跳过最终提交校验
- target/draft 端点按 position 比较；含媒体仍遵循 `media_state_unverified` 门禁

---

## 2. 实现

### 2.1 `preflight_materialize()`：只读的元数据准入（规划 §3.1）

```cpp
enum class preflight_result {
    export_needed, identical_committed, covered_by_committed,
    identical_inflight, hash_path_conflict, invalid_identity, cache_unavailable,
};

preflight_result preflight_materialize(const server_tokens & prefix) const;
```

`const` 方法、同步、只读。实现要点（对应规划 §3.1 的七条）：

| 规划要求 | 实现 |
|---|---|
| 复用现有匹配逻辑 | 调 `disk_cache_pack()` / `disk_cache_hash()` / `disk_cache_classify_duplicate()` / `inflight_check_locked()` / `covered_cheaply()`，未新增任何匹配代码 |
| 身份在进锁前构造，判断全在临界区内 | 打包与 hash 在 `lock_guard` 之前；持锁期间只做元数据比对 |
| 持锁时不调 KV API / 不做文件 IO / 不做大分配 | 满足。探测对象 `server_disk_cache_pending` 只含 token 与媒体元数据，`main`/`drft` 保持空 |
| 只有 `is_reusable_entry()` 才作为重复或覆盖 | `committed_check_locked()` 内部判定；`covered_cheaply()` 自身跳过非可复用条目 |
| in-flight 按完整身份区分重复与冲突 | 复用 `inflight_check_locked()` |
| `covered_by_committed` 用与 `covered_cheaply()` 相同的门槛 | 就是调用 `covered_cheaply()` 本身，成本取自各条目真实 `size_file` |
| 不确定时返回 `export_needed` | 默认路径。不凭猜测的配额、未来容量或 CRC 阻断导出 |

`invalid_identity` 覆盖"打包失败"与"低于本层最小 token 数"——两者都意味着这次写入本就无法成立，无导出价值。

**未做**（规划 §3.1 第 7 条）：不写 `entries`、不改 `inflight`、不动 `queue_bytes`、不更新目录 `persisted`、不触碰 `event_epoch`。

### 2.2 判定顺序：先身份，后覆盖

这是本轮最重要的一个结论。`covered_cheaply()` 只回答"是否有更长条目能经济地服务这个前缀"，它**没有 hash 参数**——因此它无法区分"这是一个不同的 prompt 恰好是同 hash"与"这是一个可用覆盖"。

若先查覆盖再查身份，一个同 hash 但身份不同的条目会被报成 `covered_by_committed`（一次看起来无害的跳过），而实际语义是**路径冲突**。

preflight 的固定顺序：

```
1. committed_check_locked(hash, identity)   → identical / conflict / held / none
2. inflight_check_locked(hash, identity)    → duplicate / conflict / free
3. covered_cheaply(identity)                → 覆盖
4. export_needed
```

`test_preflight_conclusions()` 第 5 组用例把这一点固定下来：同一 committed 条目相对 `wanted` 同时满足"前缀覆盖为真"与"身份相同为假"。

### 2.3 `committed_check_locked()`：三处共用（规划 §3.1 第 1 条）

`store()` 与 `mirror()` 各有一份几乎相同的已提交条目循环，现抽为单一方法，preflight 也调用它：

```cpp
enum class committed_verdict { none, identical, conflict, held };
committed_verdict committed_check_locked(uint64_t hash,
        const disk_cache_prompt_identity & wanted, std::string * holder) const;
```

**顺带修正的语义**：原 `store()` 循环遇到 quarantined 条目时 `continue`（继续扫描同 hash 的其他条目），而抽出的初版写成 `return held` 会提前退出。源码注释明确"每个映射到该 hash 的条目共享同一路径"（`claim_path_locked()` 处），即同一 hash 下可能同时存在隔离条目与可用条目。已改为**隔离条目不终止扫描**，只有可复用条目决定结果；整轮只见到隔离条目才返回 `held`。

### 2.4 `disk_cache_decide_materialize_action()`：生产路径调用的决策函数（规划 §3.3）

规划 §3.3 明确要求"将本次物化状态动作决策抽为小型纯函数并由生产函数实际调用，避免单测只检查另一份相似实现"。输出四种动作：

| 动作 | 条件 |
|---|---|
| `wait_next_batch` | 视图未填满 K；或 target 未到 `pos_next(K)` |
| `export_state` | 视图足够、两处位置恰好等于 `pos_next(K)`、preflight 允许 |
| `skip_export` | 状态不可配对；或 preflight 判定无需导出 |
| `cancel_plan` | 未 armed / 已 done / task_id 不再匹配 |

**target 与 draft 必须分开判定**，而非直接映射 `disk_cache_capture_bounds` 的六个枚举：

- target 落后 ⇒ 抵达 K 的批次尚未解码，**等待**
- target **恰好**抵达 K 而 draft 不一致 ⇒ 本批就是抵达 K 的批次，draft 不会再有机会（下一批越过 K），**不成对，取消**

即 target 先判（落后→等待、越过→取消），再加"target 恰好在 K 时 draft 必须同样在 K"。

视图未填满是**最先**的判断且恒为等待——这正是第九轮曾被误改为硬失败的位置。

### 2.5 调用点（规划 §3.2）

`materialize_prefix()` 中，在既有的端点判定之后、`server_prompt_cache::materialize()` 之前：

```
验证 request_id / token 视图 / K / expected_position_end / target-draft 端点
  → 决策函数（preflight=export_needed）探测是否可捕获
      ├─ 不可捕获 → 直接按动作返回，preflight 不发起
      └─ 可捕获   → disk.preflight_materialize(prefix)   ← 元数据，无 context/文件/大分配
                     → 决策函数（带真实 preflight 结果）
                          ├─ wait_next_batch → return（计划保持 armed）
                          ├─ cancel_plan     → 丢弃计划
                          ├─ skip_export     → 终结本次可选物化
                          └─ export_state    → checkpoint 筛选 → materialize() → store()
```

**preflight 只在状态确实可捕获时才发起**——它的意义是省下导出，对不可捕获的状态发问毫无意义。

**竞态**：preflight 与 `store()` 之间存在时间窗口。`store()` 与 worker 继续执行原有完整判定，作为最终权威检查；未因 preflight 返回 `export_needed` 而绕过任何一步。

### 2.6 指标（规划 §3.4）

```
cache_prefix_materialize_preflight_skip_total            // 被省下的导出
cache_prefix_materialize_preflight_export_allowed_total  // 放行的导出
```

两者都不是缓存命中，不计入 `enqueue_dedup`、`restored`、`reused_disk`。`skipped` 语义不变，仍专指"状态无法配对"；被 preflight 拦下的请求不再落入 `enqueue_dedup`（该值表示真正走到磁盘入队判定的路径）。

---

## 3. 验证

### 3.1 preflight 端到端生效（规划 §7.4）

`test-run/logs/fix11-p1.log`，`A+B / A+C / A+D` 序列后接第四请求：

```
task 14: materialize = scheduled, prefix = 2102 tokens, n_past = 1476
         materialize = exported, prefix = 2102 tokens
task 22: materialize = scheduled, prefix = 2101 tokens, n_past = 1478
         materialize = preflight skip (covered by a committed entry), prefix = 2101 tokens

exported_total            1   ← 上一轮同样序列为 2
preflight_skip_total      1
preflight_export_allowed  1
cache_disk_entries        1
persisted_refs            1
```

task 22 在上一轮会导出并排队一次 **703.82 MiB** 写入（被 `covered_cheaply` 在 store 之后挡下），本轮在导出之前即被拦住。

### 3.2 推理输出未受影响

开启态 4 请求与关闭态 4 请求输出 sha 逐字节一致：

```
f283d36a836120c4 / 9bceec30889f8bda / ad661337d3aa8896 / b9febb26949711a2
```

关闭态 `scheduled` / `exported` / `skipped` / `preflight_skip` / `preflight_allowed` 五项全 0、`cache_disk_entries 0`。

### 3.3 重启后真实磁盘恢复

`fix11-p1r.log`：`cache_scan_end: 1 entries, 538.51 MiB`、`1 snapshot(s) registered`；恢复 `source = disk, reused_disk = 2102, restored = 2102, read_bytes = 564668394`（与文件大小一致）；恒等式 `0 + 0 + 2102 + 394 = 2496` 成立。恒等式另抽查 4 组，全部成立。

### 3.4 新增单测

| 用例 | 组数 | 覆盖 |
|---|---|---|
| `test_materialize_action()` | 24 | 未 armed / 已 done / task 不匹配、视图未填满（含 0）、target 落后（含负值）、draft 落后而 target 已到 K、两处恰好 `pos_next(K)`（含位置≠token 数）、target/draft 越过、preflight 三种拒绝、视图检查优先于 preflight、四动作名互异 |
| `test_preflight_conclusions()` | 7 | 精确重复、成本合格覆盖、**成本守卫拒绝不得报覆盖**、无关前缀、同 hash 不同身份（含"身份与覆盖是独立问题"）、低于最小长度、媒体 id 不同 |

### 3.5 四套独立单测

`all disk-cache match checks passed`、`all disk-cache quota checks passed`、`all prefix-index checks passed`、`all prefix-materialize checks passed`。

---

## 4. 本轮自查发现的两处实现缺陷

与第十轮不同，本轮实现过程中测试**两次抓到**问题，均如实记录：

### 4.1 `draft_short` 的归类错误

初版把 `disk_cache_capture_bounds::draft_short` / `draft_missing` 与 `target_short` 一并归入 `wait_next_batch`，即把它们当作"还没到"。

测试用例 `mk(..., target=100, has_draft=true, draft=99)` 断言应为 `skip_export` 而失败。分析后确认测试是对的：**target 恰好抵达 K 时，本批就是抵达 K 的批次**，draft 在本次捕获内不会再有追赶机会（下一批越过 K 了）。这是不成对，不是等待。修正为按 target/draft 分别判定（见 §2.4）。

### 4.2 `suppresses_write` 与身份是两个独立问题

`test_preflight_conclusions()` 第 5 组最初断言"同 hash 不同身份"应同时不构成覆盖，实测失败——`disk_cache_suppresses_write()` 返回 true，因为该条目确实是 `wanted` 的更长前缀。

这澄清了一个我原先想错的地方：`covered_cheaply()` 没有 hash 参数，它回答的是"能否经济地服务"，与"是否同一 prompt"无关。因此 preflight **必须先查身份、后查覆盖**，否则冲突会被报成"无害的覆盖跳过"。已把结论写入 §2.2、实现注释与测试注释。

### 4.3 构建过程

两次 CUDA 构建（`b45.log` / `b46.log`）均 `EXIT=0`。一次编译期修正：`preflight_materialize()` 声明为 `const`，但其初版尝试写磁盘层 `counters.n_preflight_checked++`。计数因此移到调用方（context 侧 `counters_materialize`），磁盘层该项计数一并删除——它既与 const 冲突，也与 context 侧的统计语义重叠。

---

## 5. 未执行的检查（明确记录）

规划 §7.2、§7.3 的部分条目未能执行，按规划要求如实记录，不作为已完成项：

| 检查项 | 状态 | 原因 |
|---|---|---|
| **导出 API 调用计数核验** | **未实施** | 规划 §7.4 要求"仅测试构建加入轻量函数调用计数，**严禁把日志是否出现当成唯一证据**"。本轮未添加该计数器。现有证据（`exported_total` 由 2 降为 1 + preflight 日志）属**间接证据**：它能说明未走 export 分支，但不能直接证明 `llama_state_seq_get_data_ext` 未被调用。**本条未达规划要求** |
| `identical_committed` 端到端 | 仅单测 | 本机 `-np 1` 串行，同一前缀的重复请求在 slot 复用阶段（`reused_slot`）即被拦下，未走到 preflight |
| `identical_inflight` 端到端 | 仅单测 | 同上，需两个请求同时在途 |
| `hash_path_conflict` 端到端 | 仅单测 | 需真实 hash 碰撞；规划本身即要求用元数据夹具构造，不依赖自然碰撞 |
| `invalid_identity` / `cache_unavailable` | 仅单测 | 运行中未触发 |
| 内存分配失败 / worker 停止 | 未注入 | 规划 §7.3 列出的异常分支 |
| 媒体输入端到端 | 未执行 | 测试机无多模态模型。`media_state_unverified` 门禁未变；门禁（规划阶段）与 preflight（捕获阶段）的相对顺序未在真实媒体请求下验证 |
| context shift / 取消 / decode 重试 | 仅单测 | `cancel_plan` 分支单测覆盖，运行中未构造 |
| `store()` 权威检查未被绕过 | 静态核实 | 已确认 preflight 无任何写操作且 `store()` 未因 preflight 结果提前返回；未做注入式验证 |

---

## 6. 边界确认

- 文件命名 `pc-<hash>.bin`、磁盘格式 v8、manifest v3、CRC64、配置签名与媒体身份规则**均未改动**
- `768 KiB/token` 成本守卫、4096-token 写入抑制、`--cache-disk-min-tokens`、媒体边界与 checkpoint 筛选语义**均未改动**
- 未新增 CLI 参数；默认配置与已有开关含义不变
- 单用户、本机、`-np 1`；未加入鉴权、多租户或多 slot 调度（规划 §6.8）
- 未启动分块 KV、压缩、差量写入或范围恢复等新算法（规划 §6）
- 媒体主动写盘门禁保持原样，未借本轮扩大未验证媒体 KV 捕获范围（规划 §6.7）
- `preflight` 结果**未缓存**为任何长期全局准入表；每次捕获前重新询问（规划 §6.3）
- 返回值 `preflight_result` 为纯枚举，不带出任何指向 `entries` 或 `inflight` 容器的裸指针（规划 §6.4）
