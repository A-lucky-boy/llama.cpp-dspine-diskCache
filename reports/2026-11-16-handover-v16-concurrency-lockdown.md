# 2026-11-16 17-36 交付说明（单用户磁盘提示缓存：第十八轮 —— active 原子化、封锁入口统一、队列取消、扫描清理口径统一）

| 项 | 值 |
|---|---|
| 仓库 | `A-lucky-boy/llama.cpp-dspine-diskCache` |
| 本次交付起点 | `689aaba`（会话前 HEAD，第十七轮交付说明） |
| 本次交付终点 | 分支 `main` 顶端 |
| 规划依据 | `betterModification.md` → `2026-11-16 17-36 - test1_5` |
| 交付性质 | 同步 / 封锁 / 文件生命周期修复 + 回归（规划 §一，不启动新的缓存算法） |
| 功能提交 | `87c3650`（四项修复、新增生命周期用例、测试运行脚本） |
| 构建产物 | `output\llama-server.exe` build 54（工作树含本轮全部修改），MSVC 19.44.35226 |

规划 §一 列出的 4 条全部由源码确认。实现过程中还暴露并修正了规划未列出的一处辅助函数类型/命名空间问题（§1.5）。

**回归中另外发现一个既有的严重崩溃**，经在改动前二进制上复现确认**与本轮改动无关**（§5）。

---

## 1. 四项修复

### 1.1 `active` 数据竞争

`tools/server/server-task.h:1065` 原为 `bool active = true;`。后台 worker 持锁写 `false`，而前台 `load`/`store`/`mirror`/`preflight_materialize` 及 `flush_to_disk`/`mirror_latest` **不持同一把锁读取**——C++ 数据竞争。规划指出 baseline 中 disable 主要在单线程启动阶段，本轮新增的**运行时** fail-closed 写使它成为实际风险，判断成立。

改为 `std::atomic<bool>`，禁用 release、读取 acquire。转移单向，故不需 compare-exchange。

覆盖 4 处入口门禁 + 2 处 `server_prompt_cache` 无锁读取。并按规划 §3.1"acquire 只证明读取那一刻"在 **worker 内两处**复检（取出后一处、**磁盘锁内**决定点一处，后者权威）。

### 1.2 两处只写 `quarantine_untrusted` 不置 `active`

worker 中"写前标记添加失败"与"孤儿补充标记失败"直接 `quarantine_untrusted = true`，**不联动 `active = false`**——记录了疑虑却没据此行动。

改为调用 `disable_disk_tier_locked()`。构造函数中同类两句也统一到同一入口。修复后全文件只剩 **1 处**写该变量（`disable_disk_tier_locked()` 内部）。

### 1.3 禁用后队列仍被执行

`cv.wait` 谓词只看 `stop_flag || !queue.empty()`。任务 A 触发 fail-closed 后，已排队的 B 仍会被取出并 `write_entry()`。

新增 `cancel_pending_locked()`（归还 `queue_bytes` 与 in-flight 占位）并在取出后判断禁用即取消；已开始写盘的按既有失败/隔离流程收口；`stop_flag` 响应不受影响。

### 1.4 `scan()` 的 `file_size` 失败分支

```cpp
if (ec_size) {
    std::error_code ec_rm;
    fs::remove(de.path(), ec_rm);   // ← 结果被丢弃
    continue;                        // ← 路径可能仍存在
}
```

抽出共享 `quarantine_unremovable()`，`drop()` 与本分支共用：确认消失才忽略；删除失败或状态未知则持久隔离并计入占用；测不到大小用 `size_unknown` **不当作 0**。规划 §3.3.6 明确建议的"统一辅助函数"照此实现。

### 1.5 规划未列出

`disk_cache_remove_and_confirm()` 首次实现时定义落在匿名命名空间、声明在全局（`C2665`/`C2668`），且调用点传 `fs::path` 而形参为 `std::string`。已连同 `disk_cache_path_state` 迁入 `server-disk-cache-quarantine.{h,cpp}`——同属"路径当前状态"问题，且放这里才能**独立单测**（`server-task.h` 需要整包服务端头文件）。

---

## 2. 回归

| 用例 | 结果 |
|---|---|
| 九套独立单测（新增 `run_unit_tests.bat`） | 全通过 |
| 新增 7 组用例（删除确认 4 + 原子标志 3） | 通过；**反向验证：退回旧逻辑后 2 项失败** |
| F04 型故障注入（持有文件 → 扫描拒绝 → 删除失败） | `keeping it quarantined`、侧车记入、`559.26 MiB` 计入；删除文件后重启 `1 quarantine cleared`、侧车移除 |
| 文本跨重启磁盘恢复（三篇真实小说，按生产参数） | 乱序请求全部 `source=disk`：2457 / 2476 / 2251，sha 与预热一致 |
| 图片请求 | **被既有崩溃阻断**，见 §5 |

---

## 3. 未完成的检查

| 检查项 | 状态 | 原因 |
|---|---|---|
| **队列取消的端到端注入**（两条任务、第一条触发禁用） | **未执行** | 需在**生产 Release 构建**中让侧车写入失败，而规划 §八明确禁止把破坏性开关带入 Release。`cancel_pending_locked()` 与两处复检**只有代码实现与静态推理，没有运行证据**。本轮最大缺口 |
| **worker 正在写文件时触发关闭** | **未执行** | 同上 |
| **`file_size` 失败本身的触发** | **未执行** | 实测：Windows 上（a）持句柄占用会让 `remove` 失败但 `file_size` **仍读到大小**；（b）拒绝读取的 ACL 也不影响 `file_size`（`GetFileAttributesEx` 不打开文件）。两种手段都构造不出"存在但测不到大小"，该分支**触发本身**无动态证据。删除失败但文件存在的情形已由 F04 型注入真实验证 |
| `exists` 自身失败 | 未构造 | — |
| 残留文件在 manifest 损坏时的行为 | 仅验证 manifest 0（header 路径） | — |
| TSAN / 线程检查器 | **未执行** | 本机未配置；规划 §七"如有可用线程检查器则补充"未满足 |
| `touch_entry()` 端到端 | **仍未验证** | 析构路径，本环境送不达 `CTRL_C_EVENT`，连续四轮相同 |

---

## 4. 范围确认

- 未改 v8/v9 KV 文件、manifest v3、侧车 CRC 格式、`llama_state_seq_get/set_data_ext`、完整 token/media 身份、`--cache-disk-codec` 默认值、`768 KiB/token` 闸门、RAM/Slot 优先级
- 未新增缓存算法、KV 分块、压缩优化、投机解码改动、主动媒体写盘或多 slot
- 图片请求不产生磁盘条目，`media_state_unverified` 门禁保持不变
- 测试均用独立 scratch 目录；`E:\llama.cpp.cache`、`E:\小说`、桌面两张 PNG **未被修改**

---

## 5. 发现的既有崩溃（与本轮改动无关）

**现象**：**先恢复纯文本磁盘快照，紧接着发图片** → 崩溃：

```
... source=disk, reused_disk=2476
C:\...\tools\server\server-common.cpp:732: GGML_ASSERT(has_mtmd) failed
```

**归因过程**（不停留在推断）：

1. 崩溃点由 `server-context.cpp:5573` → `push_back_placeholder()`
2. `git blame`：相关行出自 **`60f38918`（11-14）**，早于本轮全部工作
3. 崩溃那次运行中本轮新增的禁用路径**一次都没触发**
4. 对照：`--cache-disk-max 0` 关闭磁盘层后，"文本→图片"与"文本→图片→文本→图片"**全部正常**
5. **决定性对照**：`git stash` 暂存本轮改动、重新构建（二进制验证 `has my fix: False`），在**改动前二进制**上重放同一序列——**同样崩溃，同一断言**

**结论：既有缺陷，非本轮引入。**

**可疑机制**：

```cpp
prompt.tokens = server_tokens(restored_tokens, tokens_new.has_mtmd);
prompt.tokens.append_media_placeholders_from(tokens_new);
```

`has_mtmd` 取自**本次请求**（图片为 true），而磁盘条目来自**纯文本**请求、不含媒体块——无媒体的 prompt 被标记 `has_mtmd=true`，后续媒体占位插入走到断言失败路径。

**对本轮的影响**：规划 §七要求"三篇小说交替跨重启恢复 **+** 1.png/2.png 媒体身份隔离"。**文本部分已完成并通过**；**媒体身份隔离部分被此既有崩溃阻断，未能执行**。

**为何未在本轮修复**：它不在规划 §二"预期修改文件"内，修复需改动 `server-context.cpp` / `server-common.cpp` 的 `has_mtmd` 传播语义——超出本轮四项范围。规划 §十二虽要求"发现明确错误应先修实际位置"，但同时也限定本轮只处理"当前确定的同步/封锁/文件生命周期问题"，且明确"不修改完整 KV state API"。**本轮未改动这两处**，如实记录为待办，供下一轮规划决定。
