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

---

## 9. 验证方式

```text
build-cuda-shared.bat                  # Windows/CUDA 构建，产物在 output\
test-run/probe.py <chars> <n> <label>  # 打请求并记录 prompt_n / cache_n / 输出 sha
test-run/metrics.py                    # 打印 cache_* 与 cache_prefix_* 指标
tools/server/tests/unit/test_cache_prefix_index.cpp   # 前缀目录自检（无需模型，见文件头编译命令）
```
