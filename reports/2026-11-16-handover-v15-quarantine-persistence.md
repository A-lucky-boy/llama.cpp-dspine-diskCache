# 2026-11-16 16-35 交付说明（单用户磁盘提示缓存：第十七轮 —— 隔离入口持久化、撤销失败实时隔离、fail-closed 封锁链 + 综合功能回归）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `2c8cbef`（会话前 HEAD，第十六轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端（功能提交 `af1c191`） |
| 功能提交 | `af1c191`（三项修复、附加 drop 修复、功能矩阵与注入脚本） |
| 构建产物 | `output\llama-server.exe` **build 53**（工作树 `af1c191`），MSVC 19.44.35226 |
| 规划依据 | `betterModification.md` → `2026-11-16 16-35 - test1_5` |
| 交付性质 | 两项状态一致性修复 + `quarantine_untrusted` 封锁链 + 综合功能回归（规划 §一） |

规划 §二 的三项源码缺陷均已确认并修复。故障注入过程中**另外发现并修复了一处规划未列出的同类缺陷**（扫描路径的删除失败）。逐项细节见 `change.md` 的 `2026-11-16 17-40` 条目。

---

## 1. 三项修复

### 1.1 错误 A：`quarantine_entry()` 只改内存

源码确认：

```cpp
void server_disk_cache::quarantine_entry(server_disk_cache_entry & e, const char * why) {
    if (e.quarantined) return;
    e.quarantined = true;   // 仅内存
}
```

不写侧车 → 重启后 `quarantined` 丢失 → `scan()` 的 manifest / header 分支只知道文件存在。

**修复**：返回 `disk_cache_commit_status`，顺序为**先写侧车、后标记**。失败时进入 fail-closed。共 5 处调用点（TTL 清理、超配额淘汰、回滚失败 ×2、扫描拒绝）共用。

### 1.2 错误 B：侧车撤销失败但运行时仍可命中

原实现在 `quarantine_paths_drop()` 失败时只打警告、不发 `notify_committed()`，但条目仍 `quarantined == false` → `load()` 仍恢复它，与重启扫描（依侧车排除）矛盾。

**修复**：撤销失败时立即置该条目 `quarantined`，`notify_invalidated()` 撤回物理引用，进入封锁。顺带修正日志语义——原 `stored ...` 把"字节已写盘"与"可恢复"混为一谈，现区分 `wrote ... but the entry is not usable`。

### 1.3 错误 C：`quarantine_untrusted` 只写不读

该标志在 3 处被置 true，**无任何生产路径读取**。

**修复**：`disable_disk_tier_locked()` 把它变成策略（置 `active=false` + 诊断），使既有的四处 `if (!active)` 门禁（`load`/`store`/`mirror`/`preflight_materialize`）立即生效。

### 1.4 附加发现：`scan()` 的 `drop()` 丢弃删除结果（规划未列出）

F01 注入时实测到：被 open 持有而无法删除的条目被判配置不符后 `drop()`，但 `drop()` 完全忽略 `ec_rm`：

```cpp
std::error_code ec_rm;
fs::remove(de.path(), ec_rm);   // 结果被丢弃
n_dropped++;
```

后果：文件**留在磁盘上且完全无记录**——不在 `entries`、不在索引、也未隔离。与错误 A 同源，方向更严重。

**修复**：只有确认文件消失才计数 dropped，否则建立隔离并计入真实占用。

---

## 2. 综合功能回归（真实模型 + 真实数据）

**关键前提**：`D:\AiHelper\llama.cpp\llama-server.exe` 是 **build 2 / commit 60f3891（11-15）**，早于本轮全部隔离修复且不识别 `--cache-stats`。测试统一使用 `output\llama-server.exe`（当次为 build 52，与功能提交同源；补文档后重建为 build 53）。

**另一关键前提**：`--cache-disk-idle-flush 60` 是磁盘层真正落盘的条件。先前用 `0` 配 `--cache-ram 25600` 时 RAM 永不驱逐，只写出索引、无 `.bin`。

### 2.1 按用户实际部署参数

`test-run/start_prod_params.bat` 复刻 `start-localModel-dspine.ps1`：`-c 262144`、`--cache-ram 25600`、`--cache-disk-max 204800`、`--cache-disk-min-tokens 2048`、`--cache-disk-idle-flush 60`、`--mmproj`、`--api-key sk-0`；仅把 `--cache-disk` 指向隔离目录、`--host` 改为 127.0.0.1。

### 2.2 多小说文本（T01–T03, T05, T07, T09）

`E:\小说` 自动发现 4 份合格文档（严格解码 utf-8-sig，无有损替换）：

| 文档 | 字符 | 3500 字 → token |
|---|---|---|
| 正文.md | 155535 | 2480 |
| 语录.md | 11683 | 2255 |
| 核心规则.md | 4366 | 2461 |
| 金属设定.md | 3525 | — |

跨文档公共 token 前缀 **0**，即四份内容确不相同。

**T03（本轮最核心）跨重启磁盘命中**：

写盘 3 条（558 / 548 / 559 MiB），重启后**按不同于写入的顺序**请求：

| 文档 | source | reused_disk | restored | sha |
|---|---|---|---|---|
| 核心规则.md | `disk` | 2457 | 2461 | `6b84e67f1928d399` |
| 正文.md | `disk` | 2476 | 2480 | `ae40d196680b87a3` |
| 语录.md | `disk` | 2251 | 2255 | `0d9780ae1c8c4710` |

三条全部真实磁盘恢复，sha 与预热一致。

**T05 前缀分叉**的关键细节：字符前缀 3000 字，但 `LCP(shared, X) = 2101` 而 `len(shared) = 2102`——**字符相同不等于 token 相同**（BPE 边界）。实测 `X-again` 复用 **2101**，与真实 LCP 一致，未越过分叉点。

**T01/T02/T09**：同输入同输出；A1=A2、B1=B2 无串扰；中间改一字符后 LCP=1208，恢复原文重新 `source=disk` 命中。

### 2.3 双图片（M01/M02/M05）

`mmproj-Qwen3.8-27B-pi-BF16.gguf` **兼容**，视觉路径可用（1.png 被正确描述为"文件资源管理器窗口，含以 handover/日期命名的 MD 文件列表"）。两张图 SHA 与尺寸均不同。

**M05（媒体身份隔离，本轮最重要）**：重启后先发 2.png → `cached=3282` 命中磁盘条目；再发 1.png → **`cached=0`**，未从 2.png 状态复用；2.png 再次请求 sha 一致。

M02：1.png 可复现，2.png 答案不同——无跨图状态泄漏。

### 2.4 故障注入（F01 / F04）

注入方式无代码：**另一进程持有条目文件句柄**（Windows 上持有中的文件无法删除，见 `test-run/hold_delete_probe.cpp`）。

**F01 删除失败 → 持久隔离**：

```
rejected ... but its file is still present; keeping it quarantined
rejected during the startup scan, keeping 548.27 MiB accounted for and excluding it from lookups
cache_scan_end: 1 entries ..., 1 kept quarantined, 548.27 MiB
```

侧车（magic `LLDSCQ01`, 51 bytes）记入该名；**重启后仍 `1 path(s) were left quarantined`、`1 kept quarantined`**。

修复前同一场景：文件被移出账本与索引，却**留在磁盘上无任何记录**。

**F04 真实删除 → 解除隔离**：`1 quarantine cleared`，侧车移除，占用释放。

---

## 3. 回归

九套独立单测全通过（codec / layout / lifecycle / match / quota / prefix-index / materialize / ASan codec / fs replace）。

---

## 4. 未完成的检查

| 检查项 | 状态 | 原因 |
|---|---|---|
| **F02 / F03**（`save_quarantine` 写入或替换失败的注入） | **未执行** | F01 的"持有条目文件"只让**条目**删除失败，不影响侧车路径。未找到不引入生产构建故障开关的实现方式 |
| **F05–F07**（四个精确中断点的异常结束） | **未执行** | `taskkill /F` 无法选择进程停止位置。**写前标记与提交撤销的崩溃恢复语义仍只有代码推理，没有运行复现** |
| T06 / T08 / T10 / T11 / T12 | 未单独执行 | T08 成本门禁与 T11 配置失配**在 F01 中意外观察到**（`different model or context configuration` 触发 drop），但未按 `768 KiB/token` 独立定量核对 |
| M03 / M04 / M06 / M07 / M08 | 未执行 | M08 的 `media_state_unverified` 门禁在日志中未出现，**未能确认主动捕获门禁对媒体请求的实际拦截行为** |
| `touch_entry()` 端到端 | **仍未验证** | 析构路径，本环境送不达 `CTRL_C_EVENT`，连续三轮相同 |

---

## 5. 范围与合规

- 未改模型权重、tokenizer、DSpine 投机解码、GPU kernel、完整 KV state API、v8/v9 文件头、manifest v3、CRC64
- 未改 `768 KiB/token` 成本守卫与 `--cache-disk-min-tokens` 默认门槛
- 测试全部使用隔离缓存目录，**未读写生产缓存 `E:\llama.cpp.cache`**，未改动 `E:\小说` 原文
- 未把小说原文或图片二进制写入仓库或报告；`.jsonl` 已加入忽略规则
- 未在 Release 构建加入可被外部请求触发的破坏性测试功能
