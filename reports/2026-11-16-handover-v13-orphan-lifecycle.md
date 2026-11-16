# 2026-11-16 15-25 交付说明（单用户磁盘提示缓存：第十五轮 —— 路径状态三态、隔离跨重启、孤儿来源区分）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `980ab84`（会话前 HEAD，第十四轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端 |
| 规划依据 | `betterModification.md` → `2026-11-16 15-25 - test1_5` |
| 交付性质 | **缺陷修复轮**（规划 §一：不启动新的压缩算法实验） |

三处逻辑错误均已由源码确认存在并已修复。B（隔离跨重启丢失）另有真实端到端证据。逐项细节见 `change.md` 的 `2026-11-16 16-20` 条目。

---

## 1. 缺陷 A：`exists()` 报错被当作"路径不存在"

### 1.1 缺陷

```cpp
std::error_code ec_final;
if (fs::exists(path, ec_final) && !ec_final) {
    result.outcome = disk_cache_write_outcome::failed_orphaned;
}
return result;   // 默认 failed_clean
```

`fs::exists` 返回 **false 并设置错误码**时条件为假、分支跳过，函数返回 `failed_clean`——**把"查询失败"当成了"文件不存在"**。规划 §二.1 指出这一条"可以仅凭当前源码的布尔表达式直接确认"——确认如此。

`!path_owned` 分支同类：`fs::exists(path, ec_exists)` 报错时 `if` 为假，代码继续走到 `rename()`，**在路径所有权未被证明的情况下覆写**。

### 1.2 修复

`disk_cache_path_state` 三态：`confirmed_absent` / `confirmed_present` / `unknown`。

- rename 失败后：`present && !ec` → present；`ec` → unknown；否则 absent。**只有 absent 返回 `failed_clean`**
- `!path_owned` 报错时**拒绝继续**，丢弃 tmp 并以 unknown 返回——所有权证明是覆写安全的前提
- 新增 `drop_tmp()` 统一四处 tmp 清理，删除失败时记录

---

## 2. 缺陷 B：隔离状态跨重启丢失

### 2.1 缺陷

`disk_cache_manifest_meta` **没有 `quarantined` 字段**；`save_manifest()` 把隔离条目当普通记录写出；`scan()` 重建时**从不设置** `quarantined`（全文件只有一处读取，无写入）。隔离资格在重启后消失——文件被重建为可用条目。

### 2.2 修复：独立侧车

新增 `pc-quarantine.dat`，每行一个条目文件名：

- **不放进 manifest**：manifest v3 是要求保持兼容的格式，其记录描述*可用*快照；加字段会让旧读者看到不认识的记录
- 原子落盘（tmp + rename）
- **在 `scan()` 之前读取**，否则条目已登记完毕
- **隔离检查置于 manifest 与 header 两个分支之前**——两者都无法把隔离文件重新判为可用：manifest 缺字段，header 只能证明"这是一份快照"，不能证明"这个路径曾被成功提交"
- **不对称规则**：存在从不解除隔离，只有确认文件消失才解除
- 被隔离字节**仍然计数**

### 2.3 验证：真实端到端

构造带隔离标记的真实目录（538.51 MiB 文件 + 索引 + 侧车）后启动：

```
1 path(s) were left quarantined by a previous run; they stay unusable until their files are confirmed gone
keeping 'test-run/cache-q\pc-bf495e2785d51ca5.bin' quarantined (538.51 MiB)
cache_scan_end: 1 entries (manifest 0, header 0, stale index records 0), 1 kept quarantined, 0 quarantine cleared
cache_stats: reused_disk = 0, restored = 0, prefill = 2496, reject = no_prefix
```

文件存在、索引列出它、**仍被拒绝恢复**。修复前它会被重建为可用条目。

**反向验证（隔离应被解除）**：删除文件后启动：

```
cache_scan_end: 0 entries ..., 0 kept quarantined, 1 quarantine cleared
```

侧车文件本身被移除，条目不再出现。

---

## 3. 缺陷 C：孤儿来源区分与未知占用

### 3.1 缺陷

`written_bytes` 来自**新写临时文件**的 `tellp()`。rename 失败后最终路径上残存的可能是更早的另一个文件，该计数描述的却是本次临时文件——对"从未成功 rename 的既有路径"，它甚至不是可靠下限。原代码却把它当作 `orphan.size_file` 记账。

### 3.2 修复

`disk_cache_write_origin` 区分 `none` / `this_attempt` / `preexisting_unverified`，加 `size_unknown` 标记。新增纯函数 `disk_cache_orphan_chargeable()`，**worker 直接调用**（不是复制规则）：

| 失败方式 | origin | size_unknown | 可记账 |
|---|---|---|---|
| rename 失败、路径存在/未知 | `preexisting_unverified` | 是（`written_bytes` 清零） | 否 |
| rename 成功、`file_size` 失败 | `this_attempt` | 是 | 否 |
| 提交成功 | `this_attempt` | 否 | 用实测 `size_file` |

### 3.3 未知占用必须阻塞准入

`size_file = 0` 会被配额读成"不占空间"。`disk_cache_plan_quota()` 现在在**任何**情况下都不因未知路径报告 `fits`，**包括 `limit == 0`（无限配额）的提前返回**——无限容量仍是对容量的声明。未知路径**永不**成为淘汰候选。

新增指标 `cache_disk_unknown_size_paths`。

### 3.4 验证

新增 `test_disk_cache_lifecycle.cpp`，11 组用例驱动生产函数：

- 未知占用阻塞准入（有限配额下 countable 算术明明通过）
- 同一形状去掉未知标记则正常准入（证明不是恒假）
- 未测量与实测零字节的**区别**：`total` 都是 0，但只有前者使计划不可信
- **无限配额下同样阻塞**
- 移除未知路径后准入恢复（门禁针对未知路径，不是"缓存已满"）
- 未知路径永不在淘汰序列中
- 隔离条目不得作为物理引用、隔离持有者不能作为替代依据

**反向验证**：退回配额 gate 后 **5 项失败**，覆盖有限与无限两条路径。

---

## 4. 回归结果

| 用例 | 结果 |
|---|---|
| 八套独立单测（含 ASan 版 codec） | 全通过 |
| 新单测反向验证 | 退回 gate 后 5 项失败 |
| 正常 v8 新写 + 跨进程重启 | 564668394 字节、**无侧车产生**（无隔离时不生成）；`0 kept quarantined`；`reused_disk=2102`、sha 一致 |
| 隔离跨重启（真实目录） | 拒绝恢复，`prefill = 2496` |
| 隔离在文件消失后解除 | `1 quarantine cleared`，侧车移除 |

---

## 5. 未完成的检查

| 检查项 | 状态 | 原因 |
|---|---|---|
| **文件系统故障注入（规划 §八第 2/3/4 项）** | **未执行** | 需要可控地让 `rename` 失败且 `exists` 返回 false 带错误码、或让 `file_size`/`remove` 失败。本机无 fault-injection 框架，Windows 上难以对该路径制造稳定的权限/共享冲突而不影响其它 I/O。因此 `failed_clean` 与 `failed_orphaned` 的**生成**只有代码实现与单元级算术验证，**没有从真实失败中产生过**。这是本轮最大缺口 |
| 侧车自身部分写入 | 未测试 | 格式的失败模式（截断 → 名字更少 → 当作普通条目由既有校验重新把关）已在注释说明，但未实际构造截断侧车 |
| `touch_entry()` 端到端 | **仍未验证** | 同上轮：只在析构路径调用，本环境无法向非交互启动的进程送达 `CTRL_C_EVENT` |
| 故障后同 hash 重写走 `claim_path_locked()` 回收 | 逻辑未改且已被既有分支覆盖 | 未在真实故障后构造 |
| 媒体输入端到端 | 未执行 | 测试机无多模态模型；`media_state_unverified` 门禁未变 |

---

## 6. 范围确认

- 未改 `src/llama-context.cpp` 的 `state_seq_get/set_data_ext` 协议、模型参数、DSpine 解码流程（规划 §五）
- 未改 v8/v9 已持久化 payload、CRC、媒体主动捕获门禁、token 复用统计
- **manifest v3 格式未变更**；隔离信息走独立侧车，旧清单/旧缓存扫描方式不受影响
- `claim_path_locked()` 仍协调路径所有权，隔离只收紧错误路径的写入权限
- 未对任何隔离路径调用 `notify_committed()`
- `--cache-disk-codec` 默认 `none`、`768 KiB/token` 守卫、4096-token 抑制、preflight、完整身份匹配、in-flight 保护均未改动
