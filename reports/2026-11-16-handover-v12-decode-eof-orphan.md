# 2026-11-16 14-03 交付说明（单用户磁盘提示缓存：第十四轮 —— 解码收尾、孤儿文件账本、文件末端范围校验）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `3e4bd1f`（会话前 HEAD，第十三轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端 |
| 规划依据 | `betterModification.md` → `2026-11-16 14-03 - test1_5` |
| 交付性质 | **缺陷修复轮**（规划 §一：不启动新的存储优化算法实验） |

三处问题均由源码确认存在并已修复。逐项细节见 `change.md` 的 `2026-11-16 15-10` 条目。

---

## 1. 问题 A：纯字面量收尾分支绕过尾随字节检查

### 1.1 缺陷

解码器有两个停止点，完整消费检查只做在其中一个：

```cpp
if ((flags & LZ_FLAG_MATCH) == 0) {
    return out == want;                    // ← 只查输出长度
}
...
return out == want && pos == src_len;      // ← 主循环收尾，两个条件都查
```

因此一个**以纯字面量令牌收尾**的有效流，可被追加任意字节并同步改大 `stored_len` 而仍被接受。

### 1.2 为何此前未覆盖

既有尾随字节用例的语料是全零。用探针实测编码器的收尾形态：

```
zeros(4096)          enc=129  last=match      ← 已受保护的那条路径
pattern(4096)        enc=135  last=match
tail_literal(100)    enc=  8  last=literal    ← 未被覆盖的分支
```

全零语料**以匹配令牌收尾**，所以旧用例从未触及字面量分支。

### 1.3 修复

抽出共享纯函数，两个收尾点都调用：

```cpp
inline bool is_complete_stream(size_t out, size_t want, size_t pos, size_t src_len) {
    return out == want && pos == src_len;
}
```

### 1.4 验证

新增 `test_literal_only_tail_rejects_padding()`。语料为「`run` 个 `A` 后接一个无法匹配的 `Z`」——该构造确以字面量收尾，**用例先断言这一点**，避免语料漂移后测试悄悄改测另一分支。三组长度（20 / 100 / 4096）各测四件事：干净流 round-trip 成功、追加 1 字节失败、追加 3 字节失败、追加「伪装成空字面量令牌」的两字节失败。

另加 `test_both_endings_use_one_rule()`，用同套断言包住匹配收尾，使两条分支受同一规则约束。

**反向验证**：退回 `return out == want;` 后 **9 项失败**，全部落在追加字节断言上；恢复后通过。

---

## 2. 问题 B：`file_size` 失败且清理失败会留下未登记文件

### 2.1 缺陷

文件先 `rename` 到最终路径、**之后**才查大小。原实现查询失败后：

```cpp
std::error_code ec_rm;
fs::remove(path, ec_rm);
return false;          // ← 未检查 ec_rm，也不看文件是否真的消失
```

worker 的失败分支只清 `queue_bytes` 与 in-flight 占位，**不知道最终路径可能仍有文件**。于是它不进 `entries`、不计 `total_size`，却占着路径；下次同 hash 写入会在 `claim_path_locked()` 遇到一个账本上没有的主人。

### 2.2 修复

`write_entry()` 改为返回结构化结果：

```cpp
enum class disk_cache_write_outcome {
    committed,        // 已落盘、已测大小、可入账
    failed_clean,     // 未产出，或残留已确认移除
    failed_orphaned,  // 最终路径仍存在，或其状态无法确认
};

struct disk_cache_write_result {
    disk_cache_write_outcome outcome;
    std::string path;         // 总是有效，调用方不必重算
    size_t size_file = 0;     // 仅 committed 时是实测值
    size_t written_bytes = 0; // flush 后 tellp，仅作下限与交叉核对
};
```

- 每个退出点给出准确结果。rename 失败时额外做 `fs::exists` 检查——最终路径若确实存在则升级为 `failed_orphaned`，因为先前的崩溃写入可能留下东西
- `file_size` 失败后：`fs::remove` 之后**直接查 `fs::exists`** 来确认，而不是相信 remove 的返回值。仍在或状态不可确认 → `failed_orphaned`
- `flush` 成功后记录 `tellp()` 作为 `written_bytes`
- worker 在锁内为该路径建立 `quarantined` 条目

**关于估算值**：真实大小拿不到时唯一可用数字是 `written_bytes`，它只是下限（缺目录项与块填充）。所以用它记账、把不确定性写进日志，**不伪装成实测值**。`quarantined` 标记是这一步的保障——隔离条目被 `is_reusable_entry()` 排除，永远不会被当作有效快照查找、去重或发布。

新增指标 `cache_disk_write_failed_total` / `cache_disk_write_orphaned_total`。

### 2.3 路径占用（规划 §2.2.5）：未改动

`claim_path_locked()` 已有逻辑正是所要求的——遇到非可复用（隔离）条目时调用 `remove_file()`，**仅在确认删除成功**后才释放路径与账本，否则返回 `conflict`。下次同 hash 写入因此会走路径所有权检查，不会覆盖未确认的残留。本轮无需改动。

### 2.4 验证状态

代码路径已实现，**故障注入未执行** —— 见 §4。

---

## 3. 问题 C：`seekg()` 成功不足以证明未越过 EOF

### 3.1 缺陷

C++ 标准下文件流可定位到 EOF 之后而保持 `good()`，短缺只在真正读取时暴露。未选中的 checkpoint **只被 seek 跳过、从不读取**，所以越界的段长度不会被发现。

### 3.2 修复

在段表解析之后、分配 target/draft 缓冲之前建立真实文件末端与布局上界：

1. `f.seekg(0, std::ios::end)` 取真实末端；`tellg() < 0` 或 `file_end < payload_start` → `io_error`，不假设长度正确
2. **受检加法**累加各段 `stored_len`（`total > UINT64_MAX - n` 即溢出 → `corrupt`）
3. 要求 `payload_bytes == file_end - payload_start`：格式在 payload 之后没有任何内容
4. 与索引记录的 `size_file` 交叉核对
5. 逐段累计偏移，确认每段区间落在 payload 内
6. 全部通过后才 `seekg(payload_start)` 复位并开始读取

v8 与 v9 走同一路径（v8 把每段描述为 verbatim），语义一致。未选中 checkpoint 仍只 seek 跳过，**未因校验而把全部 checkpoint 读入内存**。

### 3.3 验证：真实截断文件（本轮最强证据）

用真实写出的 v8 文件（2 个 checkpoint）截断尾部 1 MiB。解析出的段布局证明截断点**精确落在最后一个、且未被选中**的 checkpoint 的 `dft` 段内：

```
ck0.tgt  [218668946, 375563310)   选中
ck0.dft  [375563310, 391657766)   选中
ck1.tgt  [391657766, 548552130)   ← 仅 seek 跳过，从不读取
ck1.dft  [548552130, 564668394)   ← 截断点 563619818 落在此段内
段表累计末端 = 564668394 = 真实 EOF（完整文件时精确一致）
```

恢复选中 ck0、跳过 ck1，**因此没有任何读取触及越界位置**——旧检查必然放行。实测：

```
read_entry: '...pc-bf495e2785d51ca5.bin' segment table accounts for
            564659564 bytes but the payload holds 563610988
cache_stats: reused_disk = 0, restored = 0, prefill = 2496, reject = checksum_error
```

**在 `set_data_ext()` 之前拒绝**，完整 prefill 回退，未恢复任何状态。

另测**单字节截断**（`564668394 → 564668393`）：`accounts for 564659564 but the payload holds 564659563`，同样被拒。

### 3.4 夹具扩展

`test_disk_cache_layout.cpp` 增加与 `read_entry()` 同构的 `check_segment_range()`：精确匹配接受、截断/超长拒绝、**溢出拒绝**（`{UINT64_MAX, 1}`）、payload 起点越过 EOF 拒绝、越界段拒绝。

并用**真实快照的段长**验证：v9 的 `stored` 比 `raw` 少 38980 字节（逐段 24842 + 7070 + 7070），确认校验必须用 stored 而非 raw，否则每个编码文件都会被误拒。

---

## 4. 未完成的检查（明确记录）

| 检查项 | 状态 | 原因 |
|---|---|---|
| **`file_size` / `remove` 失败的定向故障注入（规划 §2.2.6、§六第 5/6 项）** | **未执行** | 需要在测试构建中可控地让 `fs::file_size` 或 `fs::remove` 失败。本机无 fault-injection 框架，Windows 上难以对该路径制造稳定的权限/占用错误而不影响其它 I/O。因此 **`failed_orphaned` 分支、隔离记账、"删除失败后再写同 hash 走路径所有权"这三项只有代码实现，没有运行证据**。这是本轮最大的缺口 |
| **`touch_entry()` 端到端触发（规划 §六）** | **仍未验证** | 同上轮：只在析构路径调用，本环境无法向非交互启动的进程送达 `CTRL_C_EVENT`。规划明确要求"在尚未真正触发前继续明确标为未验证，不用静态偏移断言代替运行证据"——故**标记为未验证** |
| `write_entry` 各 outcome 分支的运行触发 | 仅正常提交路径 | 构建覆盖了类型与调用完整性，但 `failed_clean` / `failed_orphaned` 未在运行中走到 |
| 多 checkpoint 越界组合 | 部分 | 只测了"未选中的最后一个 checkpoint 越界"（规划点名的场景）。选中越界、部分越界未单独构造 |
| 媒体输入端到端 | 未执行 | 测试机无多模态模型；`media_state_unverified` 门禁未变 |

---

## 5. 回归结果

| 用例 | 结果 |
|---|---|
| 七套独立单测（含 ASan 版 codec） | 全通过，ASan 静默 |
| 问题 A 反向验证 | 退回缺陷后 9 项失败，恢复后通过 |
| `none` 新写 v8 → 跨进程重启 | `scan 1 entries, 0 dropped`；`reused_disk=2102`、`restored=2102`；sha 一致；`read 373.53 MiB stored (373.53 MiB decoded)` —— **新增 EOF 校验未误拒合法 v8** |
| `lz` 新写 v9 → 跨进程重启 | `read_bytes = 391647888`、sha 一致；`read 373.50 MiB stored (373.53 MiB decoded), segmented` —— 校验正确使用 stored 长度 |
| 真实 1 MiB 截断 | 拒绝，`set_data_ext()` 之前，完整回退 |
| 真实 1 字节截断 | 拒绝 |

---

## 6. 范围确认

- 未更换 LZ77 编码语法、未升级 v9 版本号、未改变原样段语义（规划 §2.1.4）
- 未新增压缩器、checkpoint 去重、KV 物理分块或增量持久化（规划 §一）
- `--cache-disk-codec` 默认 `none`、manifest v3、CRC64、`768 KiB/token` 守卫、4096-token 写入抑制、preflight、完整 token/media 身份、in-flight 保护、同步通知**均未改动**
- `seekg` 跳过仍保留按需读取（规划 §2.3.3）
- 文件系统异常未被当作缓存命中，未修改 model state，未重算 prompt，未削弱配额（规划 §2.2 末段）
- 回滚失败路径不触发 `notify_committed()`
