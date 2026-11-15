# 两级提示缓存改造 — 当前状态报告

- 日期：2026-11-15
- 仓库：`llama.cpp-dspine-diskCache`，工作树 HEAD `eb76818`（`0.9.0-dev build 7`）
- 规划依据：`betterModification.md`
- 变更明细：`change.md`（`2026-11-15 18-18`、`18-35`、`18-49` 三条记录）
- 排查结论：`experienceHistory.md`

---

## 1. 结论摘要

1. **阶段 0（缺陷修复）已完成并经端到端验证**：重启后首个请求整段 prefill 的真因（`touch_entry()` 写偏移覆盖 `crc_drft`）已修复；packed 元数据补齐写侧 CRC 与读写绑定（格式 v7 → v8，清单 v2 → v3）。
2. **阶段 1（精确命中统计）已完成**：`--cache-stats` 与 `/metrics` 新增的 `llamacpp:cache_*` 系列可把每个请求的输入 token 拆成 `slot / RAM / 磁盘 / 真实 prefill` 四份互斥计数，并把"未命中"归因到具体拒绝原因。
3. **阶段 2（请求边界捕获）已完成**：`--cache-boundary-capture input|output|both` 在输入结束与生成结束两个安全点捕获完整快照，默认关闭；捕获只覆盖内存实际持有位置，不会声称未计算的 KV。
4. **阶段 3–7 未开始**。其中阶段 4（分块 KV）按规划要求必须先完成"序列状态可分块性"技术审查，未做审查前不得对 opaque state 做字节切片。
5. **未部署到生产**。`D:\AiHelper\llama.cpp` 仍是旧二进制，本次产物只落在仓库 `output/`。

---

## 2. 当前代码与产物状态

| 项 | 值 |
|---|---|
| 源码 HEAD | `eb76818`（`2026-11-15` 三组改动未提交） |
| 缓存条目格式 | **v8**：header 固定 80 B，`crc_main / crc_drft / crc_packed` 三校验 |
| 启动清单格式 | **v3**：每条记录含 `packed_crc`，整块记录另有 CRC |
| 构建 | CUDA shared，`sm_86-real;89-real`，MSVC 19.44.35226，437/437、0 FAILED |
| 产物 | `output/llama-server.exe` + `llama-server-impl.dll`（18:47 构建） |
| 测试脚本 | `test-run/probe.py`、`metrics.py`、`start_test_server.bat`、`run_case.bat` 等 |
| 测试日志 | `test-run/logs/`（20 份，约 536 KB，缓存副本已清理） |

本次被修改的文件：

| 文件 | 改动性质 |
|---|---|
| `common/common.h`、`common/arg.cpp` | 新增 `cache_stats`、`cache_boundary_capture` 参数与 `common_cache_boundary` 枚举 |
| `tools/server/server-common.h` | `server_metrics` 增加请求级 token 分解计数 |
| `tools/server/server-task.h` | `cache_reject_reason`、`prompt_cache_restore_info`、`disk_cache_counters`；`load()` 出参、`stats()`、`mirror_latest()` |
| `tools/server/server-task.cpp` | 磁盘层计数与拒绝原因、入队三态计数、Prometheus 输出 |
| `tools/server/server-context.cpp` | slot 归属信息、`prompt_capture()`、`capture_boundary()`、`add_cache_stats()`、三处调用点 |
| `README.md`、`README_EN.md` | 参数表、边界捕获章节、指标口径、§8 过时表述更正 |
| `change.md`、`experienceHistory.md` | 变更记录与结论沉淀 |

---

## 3. 规划阶段进度

| 规划阶段 | 状态 | 说明 |
|---|---|---|
| 阶段 0 缺陷修复 | **完成** | `touch_entry` 偏移；packed CRC + 读写绑定；版本化丢弃；读盘三态与先校验后恢复；候选回退（单次最多 4 个）；每条失败分支独立日志 |
| 阶段 1 统计与可观测性 | **完成** | 逐请求分解 + 拒绝原因 + 15 项指标；`candidate_lcp / restored / reused` 三者分离 |
| 阶段 2 请求边界捕获 | **完成** | `input` 只进 RAM、`output` 进 RAM 并排队写盘；复用既有去重与写入抑制 |
| 阶段 3 公共前缀识别与准入 | **未开始** | 需新增 `server-prefix-index.*`；必须先区分"被观察到的公共前缀"与"有可恢复状态的前缀" |
| 阶段 4 可增量持久化的分块 KV | **未开始（有前置阻断）** | 规划要求先审查 `llama_state_seq_get/set_data_ext` 是否提供"前 N token 的自洽状态"；目前证据是只保证整序列兼容，**不得切片** |
| 阶段 5 增量写入与成本感知查找 | **未开始** | 依赖阶段 4 的物理格式 |
| 阶段 6 热点准入与分层淘汰 | **未开始** | 依赖独立对象存储与稳定统计 |
| 阶段 7 回归与文档收尾 | **部分** | README / change / experienceHistory 已同步；`tools/server/tests/unit` 与 `experiments/` 负载生成器未补 |

---

## 4. 当前可用开关

| 参数 | 默认 | 作用 | 关闭时行为 |
|---|---|---|---|
| `--cache-disk PATH` | 关闭 | 磁盘二级缓存目录 | 仅 RAM 层 |
| `--cache-disk-max N` | 65536 MiB | 配额 | `0` 完全禁用 |
| `--cache-disk-min-tokens N` | 2048 | 入库与恢复的共同门槛 | — |
| `--cache-disk-idle-flush N` | 60 s | 空闲后镜像 RAM 条目 | `0` 只在淘汰时落盘 |
| `--cache-disk-ttl N` | 0 | 闲置过期 | 不过期 |
| `--cache-disk-queue-max N` | 24576 MiB | 待写队列预算 | `-1` 不限 |
| `--cache-stats` | **关闭** | 逐请求 `cache_stats:` 日志 + 指标口径 | 只累计计数，不打印 |
| `--cache-boundary-capture` | **none** | `input` / `output` / `both` 边界快照 | 完全走原来的淘汰/空闲镜像路径 |

---

## 5. 统计口径（不可混用的三个量）

```
P_i  = 请求 i 的输入 token 数
R_slot + R_ram + R_disk + M_i = P_i      # 互斥、合计等于输入长度
candidate_lcp >= restored >= reused        # 依次被闸门削减
H_disk = Σ R_disk / Σ P_disk_ok            # 分母只含查询过磁盘层的请求
```

- `candidate_lcp`：候选条目与请求共享的前缀长度，**不是**收益。
- `restored`：条目实际恢复的长度。
- `reused_*`：恢复后仍被 `n_past` 认可、真正免掉 prefill 的 token 数，会被检查点钳制与"末位 token 必须重算"削减。

实测一例：`candidate_lcp = 1385`、`restored = 1385`、`reused_disk = 1381` —— 差 4 个 token 正是检查点钳制的结果。把 `candidate_lcp` 当命中量会系统性高估。

---

## 6. 实测验证（双卡 4070TiS + 3090，`-ts 38,62`，语料 `E:\小说\正文.md`）

测试配置：`-np 1 -c 16384 -ctk/-ctv q4_0 --kv-unified`、Qwen3.8-27B-pi-Q4_K_M + DSpine 草稿、`--cache-ram 4096`、独立缓存目录、端口 18090。

| 场景 | 结果 | 日志 |
|---|---|---|
| 冷请求（1385 token） | `prefill = 1385`，`reject = no_prefix` | `logs/stats-run1.log` |
| 同请求重复 | `reused_slot = 1381`、`prefill = 4` | 同上 |
| 空闲镜像 | `mirrored 1 prompt(s)` → `stored 1392 tokens, 505.97 MiB` | 同上 |
| 强杀后重启 + 同请求 | `reused_disk = 1381`、`prefill = 4`、读盘 530,543,650 B / 436 ms | `logs/stats-run2.log` |
| `/metrics` 恒等式 | `2770 = reused_slot 1381 + reused_disk 1381 + prefill 8` | 同上 |
| 边界捕获（both）冷请求 | input 1385（仅 RAM）、output 1392 → 落盘 505.97 MiB | `logs/boundary-run2.log` |
| 追加请求 2000 → 2300 字符 | `reused_slot = 1381`、`prefill = 215`；output 镜像被写入抑制拦下（`skipping a 678.05 MiB mirror`） | 同上 |
| 强杀后重启 + 2300 字符 | `reused_disk = 1381`、`prefill = 215`、恢复 366 ms | `logs/boundary-run3.log` |
| 数值一致性 | 恢复谱系与全新缓存冷启动的 32 token 输出**逐字节相同**（sha256 `8e0f7474b632c91f`） | `logs/out-warm_restore.txt`、`out-cold_ref.txt` |
| `--cache-boundary-capture none` | `boundary =` 事件数为 0，路径与改造前一致 | `logs/cold-run.log` |
| 阶段 0 破坏用例 | 头部错位 / 单比特翻转 / 清单损坏 / v7 旧文件 / `.tmp` 残留 均安全回退 | `logs/case-*.log` |

---

## 7. 已知边界与风险

1. **未部署**：生产目录 `D:\AiHelper\llama.cpp` 仍是旧二进制；上线前需备份 `E:\llama.cpp.cache`（格式 v8 会显式丢弃 v7 条目重建，属预期）。
2. **强杀不落 `last_used`**：`touch_entry()` 只在析构里执行，`Stop-Process -Force` 不触发；跨重启 LRU 顺序退化为创建时间。
3. **`-np 1` 之外未验证**：所有实测均为单 slot；多 slot 并发下的边界捕获与缓存竞争未测。
4. **中断路径未测**：超时/取消时 `input` 快照保留在 RAM 的行为只做了设计保证，未构造用例。
5. **捕获有代价**：每次捕获序列化整个状态（本机 1.4K token ≈ 0.2 s / 506 MiB），所以默认关闭。
6. **条目体积由检查点主导**：该模型单状态约 151 MiB 固定开销，长会话检查点可涨到 31 个、条目达 GiB 级；写入抑制只挡小增量（>4096 token 仍重写）。
7. **Linux/POSIX 未构建**：本次未引入平台相关 API，但未实际编译。
8. **格式版本是硬边界**：v8 与 v7 不互读，升级即重建缓存。

---

## 8. 下一步建议

1. **先完成阶段 4 的前置审查**（只读工作，不改代码）：读 `src/llama-context.cpp` 的状态导出/恢复实现，确认是否存在"导出前 N token 自洽状态"的正式 API。结论为否时，分块 KV 不应实施，应保留阶段 3 的"独立公共前缀完整快照"方案。
2. **再做阶段 3**：新增 `server-prefix-index.*`，用 token id + media 身份建前缀目录，明确区分 `observed / eligible / materializing / persisted / unavailable`，只有存在物理快照的前缀才允许计为命中。
3. **补齐回归**：把本次的 8 类场景（冷启动、重复、多轮追加、分叉共享、重启、`none` 回归、破坏回退、数值一致性）落成 `tools/server/tests/unit` 与 `experiments/` 可复现脚本，避免每次靠手工驱动。
4. **再考虑部署**：先备份缓存目录，再用 `output/` 覆盖 `D:\AiHelper\llama.cpp`，观察 `cache_scan_*` 与 `cache_stats:` 两类日志各一轮，确认无 `reject` 异常集中在单一原因上。

---

## 9. 复现步骤

```bat
rem 1) 构建（产物在 output\）
build-cuda-shared.bat

rem 2) 启动测试实例（双卡、独立缓存目录、统计 + 边界捕获）
output\llama-server.exe -m D:\AiHelper\llama.cpp\localModel\Qwen3.8-27B-pi-Q4_K_M.gguf ^
  -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fa on -fit off -np 1 -c 16384 ^
  -ctk q4_0 -ctv q4_0 -t 8 -tb 16 --backend-sampling ^
  --spec-type draft-dflash --spec-draft-model D:\AiHelper\llama.cpp\localModel\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf ^
  --spec-draft-n-max 6 --spec-draft-n-min 0 --spec-draft-device CUDA0 --spec-draft-ngl all ^
  --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
  --kv-unified --cache-prompt --cache-ram 4096 ^
  --cache-disk test-run\cache --cache-disk-max 8192 --cache-disk-min-tokens 512 --cache-disk-idle-flush 3 ^
  --cache-stats --cache-boundary-capture both --metrics ^
  -b 4096 -ub 1024 -lv 3 --offline --host 127.0.0.1 --port 18090

rem 3) 打请求并看指标（在项目根目录执行）
python test-run\probe.py 2000 8 cold
python test-run\probe.py 2000 8 append ext:2300
python test-run\metrics.py
```
