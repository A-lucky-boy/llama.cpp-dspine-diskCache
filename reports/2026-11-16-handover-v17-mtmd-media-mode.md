# 2026-11-16 18-45 交付说明（单用户磁盘提示缓存：第十九轮 —— has_mtmd 媒体模式不变量与崩溃修复）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `09f9ad7`（会话前 HEAD，第十八轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端 |
| 规划依据 | `betterModification.md` → `2026-11-16 18-45 - test1_5`；算法规划文档 `v16_mtmd_transition_repair.md` |
| 交付性质 | 单项功能性崩溃修复 + 定向回归（规划 §一、§七） |
| 功能提交 | `71dbd17`（`bind_media_mode`、同步点、媒体迁移单测与混合矩阵脚本） |
| 构建产物 | `output\llama-server.exe` **build 58**（commit `71dbd17`），MSVC 19.44.35226 |

**结论：崩溃已修复。** 上一轮记录的真实功能错误——"纯文本磁盘快照恢复后发送图片请求导致进程因 `GGML_ASSERT(has_mtmd)` 退出"——在本轮定位到真正的错源并修复。

---

## 1. 规划 §2.2 的告诫是对的：磁盘那一行不是根因

上一轮的交付报告把可疑点指向 `server_disk_cache::load()` 的 `server_tokens(restored_tokens, tokens_new.has_mtmd)`。规划明确要求"**没有证据不得断言 Disk 读取那一行就是唯一根因**"，并指定先加诊断定位"最后一次赋值"。

按此执行，在四个阶段加诊断后，崩溃序列的完整轨迹是：

```
tokenize_input_subprompt:  mctx=1 computed_has_mtmd=1 is_string=1
tokenize_input_subprompt:  returning TEXT tokens with has_mtmd=0 (2480 tokens)   ← 错源
disk restore:              tokens_new.has_mtmd=0 restored=2480
disk restore done:         prompt.tokens.has_mtmd=0 n_media=0                    ← 忠实传播
push_back_placeholder:     has_mtmd=0 n_tokens=46 n_media=0 chunk_type=1        ← 断言
```

**最后一次改变该标志的函数是分词器，不是磁盘恢复。** 磁盘那一行如实复制了上游已经错误的值。因此 `server-task.cpp` **一行未改**——这正是规划要求"仅修改可确定的失配点"的落实。

## 2. 根因

`has_mtmd` 被同时用于两种含义：

| 含义 | 取值依据 | 使用点 |
|---|---|---|
| **能力** | 服务器是否有 `mctx` | slot 初始化、`push_back_placeholder()` 断言、`pos_next()` / `keep_first()` / `get_common_prefix()` 的媒体分支 |
| **内容** | 本次请求是否含媒体 | 分词器对纯文本请求返回 `false` |

`tools/server/server-common.cpp`（诊断前 1186–1194 行）：

```cpp
const bool has_mtmd = mctx != nullptr;
if (json_prompt.is_string() || ...) {
    llama_tokens tmp = tokenize_mixed(...);
    return server_tokens(tmp, false);   // ← 上面算出的值被丢弃
}
```

于是"本次请求是纯文本"被写成了"这个容器不能承载媒体"。缓存增强只是**扩大了容器跨请求存活的范围**，把既有缺陷从"几乎不会发生"变成"图文交替必现"——**不是缓存并发修复引入的**。

## 3. 修复

新增 `server_tokens::bind_media_mode(bool server_has_mtmd)`，按容器内容分两种处理：

- **无媒体映射**：采纳服务器模式。这是纯元数据变更——token id 与已恢复的 KV 都未改变，**升级不需要重算任何东西**。
- **有媒体映射**：不翻转布尔，而是逐条校验 chunk 起点/长度/`n_pos`/占位一致性，且要求服务器确实支持媒体；任一不满足返回 `false`。

**同步点只有一处**：`launch_slot_with_task()` 尾部（`get_available_slot()` 选槽之后、交付 prefill 之前）。调用顺序实测为"选槽 → 恢复 → launch"，因此这一处同时覆盖：

| 路径 | 覆盖方式 |
|---|---|
| Slot 直接复用（`f_keep >= 0.5`，**不**调用 `prompt_load`） | 统一绑定 |
| RAM 恢复（`prompt = std::move(it_best->prompt)`） | 统一绑定 |
| Disk 恢复（`server_tokens(restored_tokens, tokens_new.has_mtmd)`） | 统一绑定 |

规划 §2.3.4 要求"不能只修 Disk 分支"——单点同步天然满足，且避免三处各写一遍造成漂移。

`push_back_placeholder()` 的断言**未被改动**（规划 §2.3.5 禁止以压制断言掩盖不一致）；`bind_media_mode()` 返回 `false` 时由调用方丢弃旧状态走普通 prefill，即规划 §五.4 要求的"只导致保守清理，绝不导致进程级断言退出"。

## 4. 回归结果

### 4.1 崩溃序列（本轮核心）

| 步骤 | 结果 |
|---|---|
| 文本磁盘恢复 | `source=disk`，`reused_disk=2476` |
| **1.png**（原崩溃点） | 正常，描述正确 |
| 2.png | 正常，答案不同 |
| 1.png 再次 | 正常，sha 与首次一致 |

**断言 0 次，进程存活，回退计数 0**——纯元数据升级已足够，没有丢弃任何已恢复的 KV。

### 4.2 混合矩阵（规划 §6.3）

```
A(disk,2476) -> A(slot,2476) -> 1.png -> 2.png -> A(ram,2476)
B(disk,2251) -> 2.png(ram,3282) -> 1.png(ram,597) -> B(ram,2251)
A -> B -> C(disk,2457) -> A
1.png -> 1.png -> 2.png
1.png 可复现 True / 2.png 可复现 True / 1.png != 2.png True
```

每行满足 `prompt = reused_slot + reused_ram + reused_disk + prefill`。

**混合 prompt**（文本 + 图 + 文本于同一请求，占位在 token 串中间）：两次运行 `cached=2091`，输出 sha 完全一致，断言 0。

### 4.3 其它

| 用例 | 结果 |
|---|---|
| 十套独立单测 | 全部通过（含新增 media transition） |
| 媒体迁移单测（10 组） | 通过；**反向验证：退回修复前行为后 6 项失败** |
| **Cache-off 对照** | 通过。关闭磁盘层后同样序列正常，sha 一致——排除通用媒体模型缺陷 |

## 5. 未完成的检查

| 检查项 | 状态 | 原因 |
|---|---|---|
| RAM 恢复的**定向**构造 | **部分执行** | 矩阵中出现 `source=ram`，但未构造"文本移入 RAM 后发图"的定向用例——条目的分层时机由内部预算决定，无可控手段指定 |
| **Disk-only 对照**（降低/关闭 RAM 以隔离 Slot/RAM） | **未执行** | 实测 `--cache-ram 0` 会使 `--cache-disk` 整体失效（日志明确要求 RAM 缓存），无法在保留磁盘层的前提下关闭 RAM |
| 边界：`n_past = 0`、媒体在文本**之前**、多媒体 chunk | **未单独构造** | 已覆盖"媒体在文本之后"与"文本-图-文本" |
| 图片的**磁盘**路径身份（A 落盘 → 重启 → B） | **未执行** | 实测图片请求不产生磁盘条目（`media_state_unverified` 门禁保持关闭），该形状在本机配置下无法建立；图片身份验证在 **RAM/Slot 层**完成，报告中如实区分 |
| 有媒体映射容器的 `bind_media_mode` 校验分支 | **未单测** | 构造需真实 mtmd chunk；已在测试文件中标注而**未伪称覆盖** |
| 队列异常注入、精确崩溃窗口、`touch_entry` | **仍未验证** | 与前几轮相同，未因本轮修复而改写 |

## 6. 范围与合规

- 未改 `src/llama-context.cpp` 的 seq state API、DSpine 投机解码 kernel、磁盘 v8/v9 文件、manifest v3、侧车 CRC、写入配额、in-flight 身份、主动前缀捕获算法
- 保持 `std::atomic<bool> active`、唯一 `disable_disk_tier_locked()`、禁用后队列取消逻辑不动
- 未开启未验证的媒体状态导出；`media_state_unverified` 门禁不变
- 测试全程使用隔离缓存目录，**未写入 `E:\llama.cpp.cache`**；未修改 `E:\小说` 与桌面两张 PNG
- 未上传小说原文与图片二进制
- 诊断代码在定位完成后**全部移除**，正式构建不含诊断输出
