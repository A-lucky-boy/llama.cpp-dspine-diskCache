# 项目总体结构与现状（projectSummary）

- 仓库：`A-lucky-boy/llama.cpp-dspine-diskCache`（llama.cpp fork，含 DSpine 投机解码与两级提示缓存）
- 本文只描述**已落地**的结构、数据流、开关与边界；变更过程见 `change.md`，排查结论见 `experienceHistory.md`，规划见 `betterModification.md`
- 最近更新：2026-11-15（阶段 1/2/3 落地后）

---

## 1. 定位

在不改变模型数值计算与既有推理语义的前提下，把"会话完整状态快照 + 前缀 LCP 查找"的两级提示缓存，扩展成可观测、可按请求边界捕获、能识别公共前缀的缓存系统。所有新增机制默认关闭，关闭时走原始路径。

---

## 2. 代码结构

| 文件 | 职责 |
|---|---|
| `common/common.h`、`common/arg.cpp` | 全部缓存参数定义与命令行注册（`--cache-*`） |
| `tools/server/server-task.h/.cpp` | `server_prompt_cache`（RAM 层）、`server_disk_cache`（磁盘层）：条目格式 v8、CRC64、清单 v3、写入队列、恢复、LRU/TTL、写入抑制、统计计数器 |
| `tools/server/server-prefix-index.h/.cpp` | **新增**：单用户公共前缀目录（radix tree，元数据只读） |
| `tools/server/server-context.cpp` | 缓存初始化与配置签名、slot 选槽/保存/加载、请求边界事件（捕获与观察）、请求级统计汇总、`/metrics` 填充 |
| `tools/server/server-common.h` | `server_metrics`（请求级累计计数）、`lora_blocks_cache()` |
| `tools/server/tests/unit/test_cache_prefix_index.cpp` | **新增**：前缀目录的独立自检（不加载模型，可直接 cl 编译运行） |
| `src/llama-context.cpp` | 序列状态导出/恢复（`llama_state_seq_get/set_data_ext`），缓存层只调用、不改动 |

---

## 3. 数据流

```text
请求 → tokenize → 选槽（get_available_slot）
        ├─ slot 命中：按 token LCP 复用在位 KV
        ├─ RAM 层命中：恢复整段状态（server_prompt_cache::load）
        └─ 磁盘层命中：读条目 → 校验 → 恢复 → 从分歧点续算（server_disk_cache::load）
     → 真正 prefill / 生成
     → 输入边界：捕获快照（可选，--cache-boundary-capture）
                  + 发布前缀观察（可选，--cache-prefix-discovery）
     → 生成结束边界：捕获快照（可选，output 边界写盘）
     → 请求结束：按 n_past 结算 slot/RAM/disk/prefill 四类 token（--cache-stats 输出）

后台线程：写队列 → 原子写 pc-<hash>.bin → 提交清单 → 通知前缀目录登记引用
          LRU/TTL/去重/损坏删除 → 通知前缀目录撤销引用
```

要点：

- 状态捕获与序列化在**推理线程的安全点**完成，后台线程只做文件 IO。
- 磁盘条目只在**写入完成且清单提交后**才对读者与前缀目录可见。
- 前缀目录只提供元数据，不参与候选选择；`load()` 仍是唯一的恢复入口。

### 3.1 生命周期与资源不变量

| 不变量 | 实现方式 |
|---|---|
| 条目删除与记账只认文件系统事实 | 唯一删除原语 `remove_file()` 返回 `removed / already_missing / failed`；只有前两种才从 `total_size` 扣除 |
| 删除失败不得继续当可恢复条目 | 失败条目标记为 `quarantined`：仍计入配额、仍留在 `entries`，但 `load()` 的候选选择与 `enforce_limit()` 的淘汰对象都跳过它；前缀目录同步撤销其物理引用 |
| 配额不会因失败而无界失真 | 隔离条目按实际字节继续计入 `cache_disk_bytes`；句柄释放后下一次 sweep 重试并真实扣除 |
| 淘汰循环必然终止 | `evict_one()` 仅在真正移除节点时返回 true；`enforce_limit()` 跳过已隔离条目，无可删对象时记录一次并退出 |
| 提交/失效通知顺序 = 物理顺序 | 前缀目录回调**在磁盘锁内、与 `entries`/`total_size` 变更同一临界区**同步交付，不使用第二套异步通道；索引从不回调磁盘层，因此无锁反转 |
| 迟到的提交不能复活已删除的快照 | 磁盘层每次状态变更递增单调 `event_epoch`；索引为已撤销路径保留 tombstone，拒绝 epoch 不更新的提交 |
| 索引输入必须自洽 | media 三元组长度、区间、重叠、空 ID 任一不合法即整条观察被拒（`invalid_media`），不降级为"仅 token"路径；媒体身份 = 块 ID + token 数 |
| 只读查询不改变索引 | `candidates()` 只为已有身份解析编号，未知身份映射到不可匹配的哨兵值，不增长驻留表 |
| 索引内存有界且可回收 | `approx_bytes()` 覆盖节点、媒体表、快照表、tombstone、观察去重表；节点数与字节双约束，只淘汰无引用叶子 |
| 「可替代」只有一个定义 | `server-disk-cache-match.*` 的纯元数据判定：token 前缀 + 共享范围内双向媒体一致 + 独立边界不落在媒体块内部 + 该条目按此长度读盘满足 `768 KiB/token`。写入抑制（`covered_cheaply`）、后台冗余（`worker`）、长短前缀去重（`drop_prefixes`/`dedup_prefixes`）与 `load()` 的候选过滤共用它 |
| 隔离条目不参与任何替代判定 | `is_reusable_entry()` = 未隔离且非空；`covered_cheaply`、`store`/`mirror` 的精确去重、`worker` 冗余、两条去重路径与 `load()` 候选一律先过滤 |
| hash 不是身份 | hash 只用于挑候选，身份要求逐 token 与媒体三元组（位置/长度/ID）全等；token 长度不同同样属于冲突。同名条目身份不同 → 拒绝写入并计数，绝不覆盖原文件 |
| hash 命中就是路径命中 | 文件名 = 内容 hash，写入前在磁盘锁内解析该路径所有者（`claim_path_locked`）：记录里没有该路径才允许覆盖；同名隔离条目先真实回收、回收失败即明确拒绝；`write_entry()` 只在调用方给出所有权证明时才可能替换既有文件 |
| 媒体双向扫描 | `disk_cache_match_prefix()` 两方向独立求最早差异再取 `min`，不做短路；先做结构校验（三数组等长、下标严格递增、块非空且在范围内、ID 非空），不合格即按不可验证处理 |
| 成本守卫用真实字节 | 判定现存快照是否被长条目替代时只接受磁盘上已提交的 `size_file`；写前估算仅服务于排队与内存预算，不得决定既有文件生死 |
| 先准入、后替代 | `server-disk-cache-quota.*` 做只读配额试算：隔离条目按真实字节计入且不参与淘汰，新条目受保护；试算不通过即回滚新文件、保留原有快照。只有新条目存活过容量处理（`drop_prefixes(..., replacement_is_admitted=true)`）才允许删除被它替代的短快照 |
| 替代存活不变量 | 任何以"长快照 L 可替代短快照 S"为由删除 S 的事务，结束时 L 必须仍在 `entries` 中、未隔离、且文件存在；否则不删 S |

---

## 4. 缓存格式与完整性

| 项 | 现状 |
|---|---|
| 条目文件 | `pc-<hash>.bin`，格式 **v8**：固定头 80 B，含 `crc_main / crc_drft / crc_packed` |
| 启动清单 | `pc-index.dat`，格式 **v3**：每条记录含 `packed_crc`，整块记录另有 CRC |
| 校验 | CRC64-ECMA（slice-by-8）；packed 元数据有独立写侧 CRC，读侧在**任何** `set_data_ext` 之前校验 |
| 版本策略 | v8 与 v7 不互读；旧版本显式丢弃并打印原因（缓存是纯加速器，可安全重建） |
| 配置签名 | 目标/草稿模型路径 + `size@mtime`、K/V 类型、`n_ctx`、`n_parallel`、草稿 K/V 类型、`kv_unified`、可选 mmproj |
| 失败语义 | 写入前失败 → 干净回退候选；写入后失败 → 清空 slot 重新 prefill（不允许带损坏 KV 继续生成） |

---

## 5. 开关一览

| 开关 | 默认 | 作用 |
|---|---|---|
| `--cache-prompt` | 开 | 提示缓存总开关 |
| `--cache-ram N` | 8192 MiB | RAM 层预算（`0` 关闭） |
| `--cache-disk PATH` | 关 | 磁盘层目录 |
| `--cache-disk-max N` | 65536 MiB | 配额（`-1` 不限，`0` 禁用） |
| `--cache-disk-min-tokens N` | 2048 | 入库与恢复门槛 |
| `--cache-disk-idle-flush N` | 60 s | 空闲镜像 |
| `--cache-disk-ttl N` | 0 | 闲置过期 |
| `--cache-disk-queue-max N` | 24576 MiB | 待写队列预算 |
| `--cache-stats` | 关 | 逐请求 token 分解 + 拒绝原因 + `/metrics` 计数 |
| `--cache-boundary-capture` | none | 请求边界快照（`input` / `output` / `both`） |
| `--cache-prefix-discovery` | 关 | 公共前缀目录 |
| `--cache-prefix-min-tokens N` | 512 | 目录 `eligible` 长度门槛 |
| `--cache-prefix-observations N` | 2 | 目录 `eligible` 观察次数门槛 |
| `--cache-prefix-max-candidates N` | 256 | 目录节点上限 |
| `--cache-prefix-max-index-mib N` | 64 | 目录内存预算 |

磁盘层的删除、隔离与替代性判定不新增开关：它们是既有语义的修正。相关代码为 `server-disk-cache-match.h/.cpp`（纯元数据、可独立单测）与 `server_disk_cache` 的 `identity_of()/is_reusable_entry()/covered_cheaply()`。相关观测指标为 `cache_disk_bytes`（账面占用，含不可删除条目）、`cache_disk_quarantined`、`cache_disk_remove_failed_total`。

---

## 6. 统计口径

```text
P = R_slot + R_ram + R_disk + M          # 每个输入 token 只计入一类
candidate_lcp >= restored >= reused       # 依次被闸门削减
```

- `candidate_lcp`：候选共享前缀长度，**不是**收益。
- `restored`：条目实际恢复的长度。
- `reused_*`：`n_past` 最终认可、真正免掉 prefill 的 token 数。
- 前缀目录的 `observed / eligible / persisted` **一律不计入** `reused_*`；`persisted` 只表示存在精确对齐且校验通过的快照。

---

## 7. 能力矩阵（对照 `betterModification.md`）

| 阶段 | 内容 | 状态 |
|---|---|---|
| 0 | `touch_entry` 偏移修复、packed CRC、清单校验、先校验后恢复、候选回退 | 已落地 |
| 1 | 精确命中统计与事件可观测性 | 已落地 |
| 2 | 请求边界捕获（input/output） | 已落地 |
| 3 | 公共前缀识别与只读目录、物理引用关联与失效 | 已落地（本阶段） |
| 4 | 分块 KV 物理格式 | **未开始**，需先完成"序列状态可分块性"技术审查 |
| 5 | 增量写入、范围读取、成本感知查找 | 未开始 |
| 6 | 热点准入与分层淘汰 | 未开始 |
| 7 | 自动化回归与文档收尾 | 部分（README/change/projectSummary 已同步；`experiments/` 负载生成器未补） |

---

## 8. 现存边界

1. 面向**单用户、单实例、单 slot（`-np 1`）**；多 slot 并发未验证，前缀目录不做租户隔离。
2. 强制终止进程不触发析构，跨重启 LRU 依赖空闲镜像或淘汰落盘；前缀目录本身不持久化，重启后由已校验条目重建引用。
3. 恢复永远读整个条目文件；`lcp × 768 KiB` 的成本闸门会拒绝"命中比例过低"的长条目（实测出现过 `io_cost` 拒绝）。
4. 条目体积由检查点主导（该模型单状态约 151 MiB 固定开销，长会话检查点可达 31 个）。
5. 写入抑制 `DISK_CACHE_MAX_PREFIX_LAG_TOKENS=4096` 只挡小增量；超过仍会重写整条条目。
6. 前缀目录只观察输入边界；覆盖生成尾部的快照不把更短前缀标为 `persisted`（保守语义）。
7. 未部署到生产目录；`output/` 为当前构建产物。
8. 删除失败是常态而非异常：Windows 上杀软扫描或未释放句柄都会让 `fs::remove` 失败。此时条目被隔离、配额照实保留、引用撤销；句柄释放后由下一次 sweep/TTL 重试回收，不会静默少报占用。
9. 前缀目录不持久化：重启后由已校验磁盘条目重建引用（epoch 从 0 起算），不写额外索引文件；tombstone 只作用于进程生命周期。
10. 配额准入先于替代：新条目先通过只读配额试算才被接纳；试算不通过时回滚新文件并保留原有快照，不会出现"短快照与其替代者同时消失"。配额不足且无法回收时账面超额会如实保留并拒绝进一步不受控写入。
11. 写入需要路径所有权：内容寻址永不覆盖未解析的既有文件；同名条目身份冲突或隔离条目无法回收时，写入以明确失败结束（`cache_disk_path_conflict_total`），不会伪报 `dedup`。
11. 长条目未必能替代短条目：读盘成本守卫以「恢复该前缀要读多少字节」为判据，1 GiB 级条目对 1.4k token 请求就是 `io_cost`。因此短条目只在长条目确实经济可用时才被去重删除，否则两者共存（实测两者并存，短条目继续服务对应请求）。

---

## 9. 验证方式

```text
build-cuda-shared.bat                  # Windows/CUDA 构建，产物在 output\
test-run/probe.py <chars> <n> <label>  # 打请求并记录 prompt_n / cache_n / 输出 sha
test-run/metrics.py                    # 打印 cache_* 与 cache_prefix_* 指标
tools/server/tests/unit/test_cache_prefix_index.cpp   # 前缀目录自检（无需模型，见文件头编译命令）
```
