# 2026-11-16 13-02 交付说明（单用户磁盘提示缓存：第十三轮 —— v8 段表占位与 LZ77 尾部越界修复）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `5237861`（会话前 HEAD，第十二轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端 |
| 规划依据 | `betterModification.md` → `2026-11-16 13-02 - test1_5` |
| 交付性质 | **缺陷修复轮**（不启动新算法实验，规划 §一） |

本节修复新 codec 引入的两个确定性错误，并加固规划 §1.3 的防御边界。逐项细节见 `change.md` 的 `2026-11-16 14-20` 条目。

**两个问题都由源码确认存在**；问题 B 另有 AddressSanitizer 的独立确证。

---

## 1. 阻断问题 A：`none` 写出的 v8 文件与自身读端布局不符

### 1.1 缺陷

`write_entry()` 按 `codec_mode` 选版本号，但段表占位循环**无条件执行**：

```cpp
const size_t n_segments = 2 + 3 * ckpts.size();
const std::streamoff off_table = f.tellp();
for (size_t i = 0; i < n_segments; i++) {   // ← 未受 segmented 控制
```

只有 v9 回填该表，而 v8 读端不读段表、直接从 checkpoint 表之后读状态。因此 `none` 写出的文件在 checkpoint 表与 payload 之间多出 `28 × (2 + 3N)` 字节：**文件头声明的布局与实际字节不符**。此前几轮的 v8 恢复之所以通过，是因为那些文件都由缺陷引入**之前**的代码写出。

### 1.2 修复

预留与回填同置于**一个** `if (segmented)` 分支：

```cpp
std::streamoff off_table = 0;

if (segmented) {
    off_table = f.tellp();
    for (...) { /* 占位 */ }
}
```

`emit()`（段写入）两版本共用——它只负责把字节写出去；差异仅在段表记账。

### 1.3 验证

| 检查 | 结果 |
|---|---|
| `none` 新写 v8 → 独立进程重启（规划 §5 第 2 项） | `scan 1 entries, 0 dropped`；`source=disk`、`reused_disk=2102`、`restored=2102`；sha 与历轮一致 |
| v8 文件物理大小 | `564668394` —— **与修复前历轮 v8 逐字节相同**（缺陷下会多 224 字节） |
| 布局算术（解析真实 N=2 文件） | header 止于 8830；计算 payload 尾 = 实际大小 = `564668394`，**精确 MATCH** |

**新增布局夹具** `tools/server/tests/unit/test_disk_cache_layout.cpp`。

等待自然快照产生特定 checkpoint 数不是测试——那由模型与 prompt 决定。夹具按与写端相同的常量复现 `N = 0/1/2` 的头部算术，断言三条性质：

1. v8 的 payload 起点**等于** checkpoint 表终点（无缝隙）
2. v9 的 payload 起点比 v8 晚 `28×(2+3N)`
3. 版本必须从文件头取得——同一"配置"下 v8/v9 的起点必须不同

末项断言与**实测文件大小 564668394 精确对齐**，防止夹具算术自身漂移。

**夹具能抓到该缺陷**：把占位改回无条件执行后 **16 项失败**，并给出精确误差 `564668618 − 564668394 = 224`。

---

## 2. 阻断问题 B：LZ77 末尾 4 字节越界读取

### 2.1 缺陷

`lz_hash()` 读 4 字节，查找端守卫却用**匹配最小长度 3**：

```cpp
if (pos + LZ_MIN_MATCH <= n) {                  // 3
    int32_t cand = head[lz_hash(&srcp[pos])];   // 读 p[0..3]
```

`n - pos == 3` 时 `srcp[pos+3]` 越界。插入端用的是 `p + 4 <= n` —— **查找端比插入端弱一个字节**，因为两端用同一常量表达了两件不同的事。

### 2.2 修复

- 新增独立常量 `LZ_HASH_BYTES = 4`，注释写明它与 `LZ_MIN_MATCH` 是**两件事**，混用正是成因
- 查找端 `n - pos >= LZ_HASH_BYTES`；插入端同步改为 `n - p >= LZ_HASH_BYTES`
- `memcmp`、匹配扩展、back-reference 边界**未动**

### 2.3 AddressSanitizer 独立确证（规划 §1.2 第 5 条）

在**真实 `disk_cache_codec_encode()`** 上运行（非复制出的纯函数）：

把守卫退回修复前的形式后：

```
==1327636==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x100480020494
READ of size 1 at 0x100480020494 thread T0
SUMMARY: AddressSanitizer: heap-buffer-overflow
         server-disk-cache-codec.cpp:81 in `anonymous namespace'::lz_hash
```

第 81 行正是 `lz_hash` 里的 `p[3]`。修复后 ASan 静默通过。这让问题 B 不止于源码推断。

**新增用例** `test_hash_window_tail()`：长度 0–16 × 4 种尾部内容的真实编码器 round-trip，缓冲区**恰为所测长度**（`shrink_to_fit()`，使越界无处藏在冗余容量里）；覆盖规划点名的 `n=4/pos=1`、`n=5/pos=2`、扩展循环触及缓冲末端、以及完全没有合法哈希位置的短输入。

---

## 3. 防御边界加固（规划 §1.3）

| 项 | 状态 | 说明 |
|---|---|---|
| decoder 拒绝非规范尾随字节 | **已修** | 原 `return out == want;` 不检查输入耗尽。改为 `out == want && pos == src_len`；配套让编码器在循环恰于 match 处结束时不再写空字面量令牌（那会留一个解码端永不消费的字节） |
| decoder 拒绝未知 flags | **已修** | 原只测 `bit 0`。翻转 `0x01` 的高位后 `bit 0` 仍置位 → 流照常解析且产出**相同输出**。这不是假设：既有损坏用例实测到的就是这个。现拒绝 `~LZ_FLAG_MATCH` 的任何置位 |
| `read_entry()` seek 成功判断 | **已修** | 跳过未选中 checkpoint 的 `seekg` **未检查成功**；越界 seek 会让后续读取从错误偏移开始，且错位若落在匹配字节上仍可能通过 CRC。现检查 `!f` 判 corrupt |
| 段表上界 / CRC 顺序 | **已复核未改** | 上界在分配前；长度一致性逐项校验；全部校验在 `set_data_ext()` 之前 |
| `fs::file_size` 错误码 | **已修** | `size_out` 原直接取值而**不检查 `ec_size`**——失败时调用方会拿未核实的大小作 quota 与 `persisted` 依据。现失败即报错、删除刚提交的文件并返回 false |

新增 `test_non_canonical_stream_rejected()`（尾随字节 / 未知 flags / 短字面量流）与 `test_boundary_windows_encode_and_decode()`。

---

## 4. 兼容性回归

| 用例 | 结果 |
|---|---|
| `none` 新写 v8 → 重启 | 通过 |
| `lz` 新写 v9 → 重启 | `read_bytes = 391647888`，sha 一致 |
| v9 逐段解析 | version 9、段表间隔 224、逐段累加 = 实际大小 **MATCH**、saved=38980、encoded=3/8 —— **与修复前完全一致**，证明修复 A 未影响 v9 |
| v8 配置进程读 v9 文件 | 通过 |
| **v9 配置进程读修复后的 v8 文件** | 通过（修复 A 的交叉验证） |
| 六套独立单测 | 全通过 |

---

## 5. 未完成的检查（明确记录）

| 检查项 | 状态 | 原因 |
|---|---|---|
| **`touch_entry()` 在真实 v8/v9 上触发（规划 §5 第 6 项）** | **未完成** | `touch_entry()` 只在 `server_disk_cache` **析构**中、经 `save_manifest` 后的 `last_used_dirty` 分支调用，运行期无其他可达路径。本环境下进程由非交互工具环境启动、无可附加的控制台：`GenerateConsoleCtrlEvent(CTRL_C_EVENT)` 无法送达（`AttachConsole` 报成功但事件不生效），`taskkill`（无 `/F`）对控制台程序报"只能强制终止"，bash 的 `kill` 直接终止进程使析构不跑。**未能真正触发 touch**。替代证据仅静态：`last_used` 偏移在 v8/v9 实测**均为 8670**，且两次运行后**非时间戳字节 hash 未变**（v8 `3f6bdc4ff38f9fc3`、v9 `1df84c699a850aec`）——但**这不等于验证过 touch 本身** |
| N=1 的真实文件 | 夹具覆盖算术 | 夹具做 `N=0/1/2`；真实文件只观察到 `N=2`（两次运行都产出 2 个 checkpoint） |
| 真实文件上的损坏注入 | 未做 | 合成流用例已有（截断 / 错误段长 / 未知 codec / CRC 不符），但未在真实 564 MB 文件上人为破坏后验证拒绝与回退 |
| `fs::file_size` 失败注入 | 未做 | 错误检查已实现，未构造实际失败 |
| `encode_us` / `decode_us` | 仍未测 | 与上一轮相同 |
| 配额 / TTL 淘汰、句柄占用后回收、强杀重启 | 未执行 | 与前几轮相同 |
| 媒体输入端到端 | 未执行 | 测试机无多模态模型；`media_state_unverified` 门禁未变 |

---

## 6. 上一轮为何没发现

问题 A 自 `1f15c1b` 起就存在，通过了上一轮的全部验证。原因明确：**上一轮只测了 v9 路径**——v9 写入、v9 恢复、以及"v8 配置进程读 v9 文件"，却没有测"`none` 进程写出 v8 文件、再读回"。

`none` 是**默认值**，也是本轮之前所有 v8 恢复所依赖的路径。这些恢复都通过，只是因为那些文件由缺陷引入之前的代码写出。这一点已写入 `change.md` 作为流程记录。
