# 2026-11-16 15-22 交付说明（单用户磁盘提示缓存：第十六轮 —— 侧车可校验格式、写前标记、启动 fail-closed）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `03d806c`（会话前 HEAD，第十五轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端（功能提交 `d91d5fc`） |
| 功能提交 | `d91d5fc`（侧车格式、写前标记、fail-closed、替换语义修复） |
| 构建产物 | `0.9.0-dev (build 51, commit d91d5fc)`，MSVC 19.44.35226，sm_86-real + sm_89-real |
| 规划依据 | `betterModification.md` → `2026-11-16 15-22 - test1_5` |
| 交付性质 | **事务一致性修复轮**（规划 §一：不启动新的存储算法实验） |

规划 §二列出的 1–5 条均由源码确认。第 6 条（Windows 替换语义）我实测后发现**规划的前提不完整**，并据此修掉了一个上一轮引入的真实缺陷。逐项细节见 `change.md` 的 `2026-11-16 17-10` 条目。

---

## 1. 侧车格式：无法区分"缺了一个名字"与"从来没有"

**缺陷**：原格式无 magic、无版本、无记录数、无校验。截断的文件只是返回更少名字：

```cpp
if (!line.empty() && line.size() <= 256) out.insert(line);
```

而缺一个名字就意味着**本应被排除的路径被登记为可用快照**。

**修复**：新格式 `magic | version | count | { len, name }* | crc64`，读取严格校验 magic/version、整份 body 的 CRC、每条长度、名字形状（必须是 `pc-<16 hex>.bin`）、**不得重复**、记录必须恰好耗尽 body。

旧格式仅在**每行都是合法条目名**且**以换行结束**时可原子升级；末尾无换行视为可能写到一半 → 拒绝。

**如实记录的不可追溯限制**：旧格式无法自证完整，升级前已丢失的名字无法从字节恢复。迁移能保证的是*可见地*截断或含非法内容的旧文件被拒绝，而不是被读成"短一点的名单"。

**格式独立成模块** `server-disk-cache-quarantine.{h,cpp}`：它有自己的正确性规则，而这些规则需要在**字节层面**被测——读取端在损坏记录上的行为正是格式的全部意义。

---

## 2. 写前标记（事务顺序）

**缺陷**：原顺序是"写文件 → rename 可见 → 失败后登记"。rename 之后到 manifest 之间有准入、配额、通知多个步骤。在那些窗口崩溃会留下**无记录的文件**，而下次启动的 header 校验会把它登记为普通快照——header 只能证明"这是一份快照"，**不能证明"这个路径曾被成功提交"**。

**修复**：标记提前到最终路径可见之前。

```text
持锁确认所有权与身份
  → 侧车登记并确认持久化
      ├─ 失败：拒绝写入，不动最终路径，entries 不变
      └─ 成功：写 tmp → rename → 取 file_size → 准入/配额/manifest
              → 事务确认提交且条目可用 → 才撤销标记
```

最坏情况变成"为一个成功写入留了条标记"，由提交路径或下次启动按"文件不存在"清除。**标记在危险方向上永不错误。**

实测确认：一次干净的 v8 写入（564668394 字节）后**没有任何侧车残留**——标记被正确撤销。

---

## 3. 启动 fail-closed

`load_quarantine()` 返回 `absent` / `valid` / `invalid` / `io_failure`，构造函数显式检查。后两者：

```
active = false，禁用磁盘层，保留目录与文件，不执行 scan
```

实测三种损坏模式：

| 损坏方式 | 结果 |
|---|---|
| 截断（尾记录与校验和不全） | `cannot be trusted`，**scan performed: 0** |
| 错 magic | `no recognised format`，**scan performed: 0** |
| 校验和不符 | `cannot be trusted`，**scan performed: 0** |

三种情况下 `active=false`、`reused_disk=0`、**写盘 0 次**、目录与损坏侧车**逐字节保留**。推理正常（sha 一致）。

---

## 4. 实测发现：规划 §二第 6 条的前提不完整

规划要求验证 Windows 上 `fs::rename` 能否替换已存在目标，并提示"即便单次新建侧车成功，也不能据此断定后续可以覆盖旧侧车"。

我写了探针（现为 `test-run/fs_replace_probe.cpp`，长期回归检查）。第一版**通过**——但真实迁移路径**失败**：

```
failed to replace the quarantine record 'test-run/cache-leg/pc-quarantine.dat'
(Access is denied.)
```

分离变量后定位到确切条件：

```
replace existing   : ok
create target      : ok
replace while open : Access is denied.    ← 目标被打开时
replace after close: ok
```

**Windows 上 rename 到仍被打开的目标会以 `Access is denied` 失败。** 而 `load_quarantine()` 在同一个函数里先读该文件、再经迁移路径写回——**读取流仍然活着**，所以每次升级都失败。这是我上一轮引入的缺陷。

修复：把读取的 `std::ifstream` 限定在独立作用域，在可能写入之前关闭句柄。修复后迁移成功：侧车由 24 字节逐行格式升级为 51 字节新格式（magic `LLDSCQ01`）。

**规划的前提方向猜错了**：真正的障碍不是"重命名不支持替换"，而是"读取方持有自己的目标"。仅按字面实现（改用 `ReplaceFileW`）会掩盖真正原因。规划的**实测要求**因此是有价值的——它逼出了这条。

---

## 5. 回归结果

| 用例 | 结果 |
|---|---|
| 九套独立测试（含新增 fs 替换探针） | 全通过 |
| 侧车格式单测（27 组） | 通过；**反向验证**：不校验 CRC 后 2 项失败、不校验名字形状后 1 项失败 |
| 正常 v8 写入 | 564668394 字节，**无侧车残留** |
| 旧格式迁移（真实文件） | 24 → 51 字节，条目保持隔离 |
| 三种损坏侧车 | 全部 fail-closed，`scan performed: 0` |
| fail-closed 下推理 | sha 一致，`reused_disk=0`，0 次写盘，目录未改动 |

---

## 6. 未完成的检查

| 检查项 | 状态 | 原因 |
|---|---|---|
| **规划 §七第 6 项：四个精确中断点的异常结束** | **未执行** | 需要在「写前标记成功→rename 前 / rename 后未进 manifest / manifest 事务中断 / commit 后侧车清理前」四处可控结束进程。本机无相应注入点，`taskkill /F` 只能杀进程、无法选位置。**写前标记的崩溃恢复语义只有代码推理与静态顺序，没有运行复现**。本轮最大缺口 |
| **侧车写入/替换失败的故障注入** | **未执行** | 无法在受控条件下让 `save_quarantine` 的写入或 replace 失败。§1.2 的失败分支（拒绝写入、撤销失败放回名字）**只有代码实现，没有运行证据** |
| `touch_entry()` 端到端 | **仍未验证** | 析构路径，本环境送不达 `CTRL_C_EVENT`，与前两轮相同 |
| `n_size_unknown_paths` 在侧车失败后的语义 | 未单独构造 | — |
| 媒体输入端到端 | 未执行 | 测试机无多模态模型；`media_state_unverified` 门禁未变 |

---

## 7. 范围确认

- 未改 v8/v9 payload、manifest v3、CRC64、完整 token/media 身份、原 target/draft 恢复语义
- 未改 `768 KiB/token` 守卫、4096-token 抑制、in-flight 完整身份判定、preflight
- 只有完成实际物理提交的可复用条目才发 `notify_committed()`；**撤销标记失败时不发送通知**
- `pc-quarantine.dat` 仍是唯一持久隔离依据，未新增第二套索引
- 未改 `src/llama-context.cpp` 的 state 协议、模型参数、DSpine 解码流程
- 未新增压缩器、KV 分块、checkpoint 去重；配额算法未改
