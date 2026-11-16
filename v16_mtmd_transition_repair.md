
---

# 2026-11-16 18-45 - test1_5

## 一、修复目标及规划前核验

**本节为现有缓存算法的最后阶段功能性崩溃修复与定向回归规划，不增加新的磁盘缓存、KV 编码、模型计算或性能优化算法。** 本轮必须优先排除单用户单 slot 下“纯文本缓存恢复后紧接着发送图像请求，服务器因 `GGML_ASSERT(has_mtmd)` 崩溃”的问题。原有已可用的三层缓存、完整 target/draft KV 恢复、隔离侧车、队列封锁和安全回退全部保留。

本规划生成前已先查验 GitHub 最新 `change.md` 的 `2026-11-16 18-20 - Change Record`，并检查 `2026-11-16-handover-v16-concurrency-lockdown.md`、`projectSummary.md`、当前 `server-context.cpp`、`server-common.cpp`、`server-task.cpp/.h`；再和原始 baseline 提交 `eb7681889f44304d2bd5b3e41572f5530af91a1d` 比较。核验时仓库 `A-lucky-boy/llama.cpp-dspine-diskCache` 的 main 文档 HEAD 为 `09f9ad72fa643f2f2047e72f0864ab804ceaeae1`，实际功能修复提交 `87c36508226728d191ed15625d60cff4f4671959`，构建报告称实际运行版本 build 56 / `ca07d839`。本轮之前的状态同步、队列取消和扫描失败清理已写入代码；尚无必要回滚这些已实现机制。

**已确认的故障与边界**：交付报告在真实模型上记录了纯文本 disk 恢复后发送图片触发 `tools/server/server-common.cpp::server_tokens::push_back_placeholder()` 中 `GGML_ASSERT(has_mtmd)`；同序列关闭磁盘层正常，且恢复至本轮修复前的二进制可复现同一断言。这是当前真实功能性错误，不是仅由指标推测出的性能风险。现有源码中该断言确实存在；`server-context.cpp` 的常规媒体追加和 mixed-batch 追加会调用 `push_back_placeholder()`。但**报告指向的 `server_disk_cache::load()` 构造 `server_tokens(restored_tokens, tokens_new.has_mtmd)` 本身并不足以证明最终根因**：该方法在接到媒体请求时已经使用当前请求的标志。应同时核对前一个文本请求的 disk 恢复如何影响后续 slot 保留与 RAM 迁移，不能仅按假设修改一行。

与 baseline 对照可见：`server_tokens` 的 `has_mtmd`、`push_back_placeholder` 断言、初始 `slot.prompt.tokens.has_mtmd = (mctx != nullptr)`、RAM 恢复的 `prompt = std::move(it_best->prompt)`、以及 Disk 恢复的 `server_tokens(restored_tokens, tokens_new.has_mtmd)` 都属于既有行为。当前缓存增强扩大了恢复与复用场景，使错误更容易被真实图文交替请求触发；不应把它错误归咎于已完成的磁盘隔离并发修复。

开始修改前，实验人员必须再次检查本地**最新**的 `change.md`、`projectSummary.md`、`git rev-parse HEAD`、`git status --short`、实际 `output/llama-server.exe` 构建版本，再与上述 baseline 逐函数对比。若当前工作树新增其他修复，先检查它是否解决此崩溃，避免重复改动。

## 二、明确错误位置与本轮修复对象

### 2.1 `has_mtmd` 的持有者与传播路径

按现有代码检查下列调用链：

1. `tools/server/server-context.cpp`：初始化 slot 时 `slot.prompt.tokens.has_mtmd = (mctx != nullptr)`；请求开始时的 `get_available_slot()`、`launch_slot_with_task()` 与 `update_slots()` 使用同一 slot 状态；普通图片追加会调用 `push_back_placeholder()`（当前约 5365、5573 行）。
2. `tools/server/server-common.cpp`：`server_tokens::push_back_placeholder()` 在图片/音频块分支要求 `has_mtmd == true`；`server_tokens::clear()` 只清空 token 和媒体映射，不改变 `has_mtmd`；`clone()` 复制旧标志。当前 token 容器也可能是纯文本、但对应多模态能力已启用的模型。
3. `tools/server/server-task.cpp`：Disk 恢复设置 `prompt.tokens = server_tokens(restored_tokens, tokens_new.has_mtmd)`，之后重建本次可共享 prefix 的媒体占位；RAM 恢复直接执行 `prompt = std::move(it_best->prompt)`，保留旧缓存对象的标志；直接 slot 重用没有强制在新请求开始前更新该标志。
4. `tools/server/server-common.cpp::server_tokens::get_common_prefix()`：当当前缓存 token 容器的 `has_mtmd == false` 时走纯 token 比较。它属于复用资格计算的一部分，不能在重新绑定媒体模式时忽略已有媒体占位的真实 chunk 身份。

核心不变量：**任一服务端请求开始向 slot 的 prompt token 容器追加媒体块之前，容器必须使用与当前 server 实际 `mctx` 能力一致的媒体模式；已经恢复的媒体 chunk 占位必须仍与当前请求的 token/media 身份对应。** `has_mtmd` 不能被误用为“这次输入已经含有媒体块”，它也参与“模型是否允许媒体 token 操作”的能力判断。

### 2.2 先定位最后一次赋值，再做最小修复

在 Debug / 测试构建增加不包含原始内容的临时诊断：请求 ID、slot ID、`mctx != nullptr`、本次 `task.tokens.has_mtmd`、`slot.prompt.tokens.has_mtmd`、`slot.prompt.tokens.n_media_chunks()`、前缀长度、`n_past`、Slot/RAM/Disk 来源，以及下面四个阶段的标志：

- A. 请求入槽前（`get_available_slot()` 选择后）；
- B. RAM 或 Disk 恢复完成后；
- C. `launch_slot_with_task()` 及 `update_slots()` 真正追加媒体 token 前；
- D. `push_back_placeholder()` 调用入口。

将此诊断用于现有崩溃序列，精确找到哪一次赋值或 move 导致 `has_mtmd` 不符合模型能力；没有证据不得断言 Disk 读取那一行就是唯一根因。

### 2.3 将请求切换与缓存恢复统一到可测试的媒体模式不变量

修改策略以**最小且完整覆盖三条缓存路径**为原则：

1. 在 `server_slot` 管理请求与缓存的边界提供一个受控、可测试的媒体模式同步逻辑（函数名按项目风格选定）。模型实际支持 mtmd 时，纯文本 prefix 的 token 容器也必须能安全进入媒体追加路径；模型不支持 mtmd 时绝不接收含媒体 chunk 的请求或缓存状态。
2. 在 `get_available_slot()` 使用旧 prompt 计算 LCP 前、`prompt_load()` 恢复 Slot/RAM/Disk 之后、以及 `launch_slot_with_task()` 将 token 交给 prefill 之前，确保容器的模式和媒体映射一致。根据真正的故障轨迹选择**最少的必需调用点**，避免无谓重复重建 token 和媒体映射。
3. 对纯文本缓存 prefix 的模式升级应是安全的**元数据转换**，不重新计算已恢复的 text KV；若缓存中已有 `LLAMA_TOKEN_NULL` 或媒体映射，不得仅把 bool 翻转，而应校验 chunk 开始位置、长度、ID 和 token 占位，确认与当前请求一致。
4. RAM 缓存 `prompt = std::move(it_best->prompt)` 后应遵守与 Disk 恢复相同的媒体模式约束；直接 Slot 复用/截断时亦然。不能只修 Disk 分支，导致 RAM 或 slot 在相同顺序下继续崩溃。
5. 针对真实不兼容或无法安全重新绑定的状态，使用既有 `prompt_clear()`、KV 序列清理与普通 prefill 回退，确保不会保留不匹配的 KV 或媒体占位；不得把 `GGML_ASSERT(has_mtmd)` 改为无条件跳过，更不能以压制断言来掩盖数据结构不一致。
6. 同步检查 `server_tokens::clear()/keep_first()/clone()/get_common_prefix()/append_media_placeholders_from()` 的实际语义，尤其在文本→图片、图片→文本、图片 A→图片 B 转换时；只在必要位置调整，不为本轮重写所有 token 处理函数。
7. 现有多模态主动中途前缀物化仍受 `media_state_unverified` 门禁限制；此次只修复正常多模态推理、Slot/RAM/Disk 恢复和合法媒体身份匹配，不借机开启未验证的媒体状态导出。

## 三、预期修改文件

| 文件 | 本次修改职责 |
|---|---|
| `tools/server/server-context.cpp` | 修复请求入槽、slot 再利用、恢复后进入 prefill 的媒体模式同步；在调用 `push_back_placeholder()` 前保证正确 `has_mtmd` 与映射状态，不改变模型前向或 target/draft 调度 |
| `tools/server/server-task.cpp` | 审核 Disk 构造与 RAM `std::move(it_best->prompt)` 的媒体模式传播；仅修改可确定的失配点，保留完整状态恢复、CRC 与真实复用统计 |
| `tools/server/server-common.h/.cpp` | 如需要，增加最小的媒体模式安全绑定/校验辅助，使用真正的 `server_tokens` 生产实现；不修改文件序列化格式与模型状态 API |
| `tools/server/tests/unit/test_cache_media_transition.cpp`（新增或等效） | 直接测量生产复用的媒体模式同步/身份校验函数，覆盖文本/图片切换、`clear`、`keep_first`、`clone`、恢复的不同来源以及非法映射回退 |
| `test-run/cache_media_matrix.py` | 扩展文本 Disk 命中后立即请求 `1.png` / `2.png` 的崩溃回归、重复图片、换图、反向顺序和同图回归 |
| `test-run/cache_functional_matrix.py` | 增加跨小说 A/B/C 恢复后插入图片再回小说的完整交替请求记录；校验真实 `source`、`reused_*`、sha 与请求成功结束 |
| `test-run/start_prod_params.bat`、`test-run/run_unit_tests.bat` | 只做必要测试脚本调整，维持真实 `-np 1` 与固定模型/mmproj/上下文签名，不加入 Release 故障开关 |
| `change.md` | 由实际实施者追加定位证据、修改文件、调用链和真实执行的测试记录 |
| `projectSummary.md` | 修复实施并验证后才更新总体支持范围和已知限制 |

本轮不修改 `src/llama-context.cpp` 的 seq state API、DSpine 投机解码 GPU kernel、磁盘文件 v8/v9、manifest v3、侧车 CRC 结构、写入配额、in-flight 身份与主动前缀捕获算法。

## 四、预期数据流变更

### 4.1 文本缓存恢复后发送媒体请求

```text
请求 A：长纯文本 → 原有 Slot/RAM/Disk 缓存与正常生成
      → 恢复的 prompt.tokens 保留正确 token 身份与 KV 位置
      → 下一请求 B：图像请求，获取 input_tokens 与 server mctx 能力
      → 在计算共用 prefix、恢复缓存和进入 prefill 的合法边界同步 media mode
          ├─ 旧 prefix 为可安全继承的纯文本：维持已恢复 KV，允许追加图像块
          └─ 状态映射与媒体 chunk 无法验证：安全清理旧状态并重新 prefill
      → 正常媒体 chunk 编码与 push_back_placeholder
      → target/draft 正常执行 → 返回响应
```

### 4.2 图片 A 与图片 B 的缓存身份

```text
已编码图片 A 的 prefix → 将 chunk ID、token 范围与位置作为身份的一部分
      → 请求相同图片 A：只在相同媒体身份和合法位置下复用
      → 请求图片 B：若媒体身份不同，只能复用真正相同的文本 prefix
      → 重新编码不同的图片块，不从图片 A 的 KV 读取 B 的媒体内容
```

### 4.3 训练/推理流水线影响

- **训练**：不涉及训练，不改变权重、参数和训练 checkpoint。
- **推理**：只修复 prompt token 容器元数据及其与 slot 缓存来源的衔接；不改变图像编码器输出、生成算法、模型缓存物理文件和正常情况下的采样。
- **磁盘缓存**：保持跨重启真实恢复，`reused_disk` 只计真实接受的 KV token；异常回退只做普通 prefill，不伪报 Disk hit。
- **部署**：仅针对本机、单用户、`-np 1` 的文本/图像混合请求；不增加公网、多用户或多 slot 能力。

## 五、与当前修改的兼容性

1. 保持 `std::atomic<bool> active`、唯一 `disable_disk_tier_locked()` 和禁用后队列取消逻辑；不得将媒体状态修复混进磁盘层异步事务。
2. 保持可校验 `pc-quarantine.dat` 与 file_size/remove 失败隔离，不重写 v8/v9 恢复数据格式。
3. RAM / Disk / Slot 的 token 复用均按真实共同前缀和媒体身份；媒体位置 `n_pos` 与 token 数量不得混淆，`server_tokens::pos_next()` 在多模态输入仍为权威换算。
4. 保持原输出和训练无关配置；媒体状态异常只能导致保守清理或显式请求错误，绝不导致进程级断言退出。
5. `--cache-disk-codec` 默认 `none`；`--cache-disk-idle-flush`、缓存最小 token 门槛、输出采样参数和 `-c` 在各组对照中固定，避免把配置签名变化当成缓存错误。
6. 测试只读 `E:\\小说` 内符合要求的 `.md`、`.txt`、`.text`、`.jsonl` 之外的普通本地文本（仅当格式可安全严格解码），并使用 `C:\\Users\\Administrator\\Desktop\\1.png` 与 `2.png`；输出只记录文件名、输入 SHA、token 数、缓存路径、生成输出摘要和错误信息，不上传原文与原图。使用**全新隔离测试缓存目录**，严禁写入 `E:\\llama.cpp.cache` 正式缓存目录。

## 六、验证检查清单

### 6.1 可定位性和构建

- [ ] 首先核实最新 `change.md`、实际源码 HEAD、baseline、二进制 commit 字符串与模型/mmproj 配置；不存在旧 `D:\\AiHelper\\llama.cpp\\llama-server.exe` 冒充新 `output\\llama-server.exe` 的问题。
- [ ] 用**未修复二进制**在隔离目录重现“小说文本 Disk 恢复 → 1.png”，确认触发点；输出两端 `has_mtmd`、`mctx`、media chunk 数、来源与 `n_past`，并标记最后一次标志状态变化的真实函数。
- [ ] 修复后同样输入不触发断言，不把错误由崩溃改为死循环、空 batch 或错位 KV；需要完成请求并产生正常多模态响应。
- [ ] 对应 `server_tokens` 状态转换的纯单测直接调用生产函数，而不测试复制的布尔逻辑。

### 6.2 不同缓存来源的分离验证

- [ ] **Slot**：同一实例里先送文本、再送 1.png、2.png、再送文本，保持 `-np 1`，验证媒体模式与已有 KV 状态一致。
- [ ] **RAM**：启用 RAM 缓存，构造旧文本移入 RAM 后的图像请求，核查确实出现 `source=ram`，而不单凭请求重复推测 RAM hit。
- [ ] **Disk**：使用现有 `E:\\小说` 中足够长的文本，预热生成物理 `pc-*.bin`，重启同配置确认 `source=disk` 与 `reused_disk > 0`，紧接 1.png 和 2.png，验证正常完成。
- [ ] **Disk-only 对照**：可用测试隔离配置降低/关闭 RAM，隔离 Slot 与 RAM 影响；如因此无法生成磁盘快照，先按原生产配置建立条目并固定签名再进行对照。不得把 `cache_n` 等于某值直接写成 Disk 命中。
- [ ] **Cache-off 对照**：不使用磁盘缓存的同模型/mmproj 请求必须也能正常完成，用于排除通用媒体模型错误。

### 6.3 文本/媒体混合与身份正确性

- [ ] 小说 A → 小说 B → 小说 C → 小说 A：记录实际 `reused_slot/reused_ram/reused_disk/prefill` 与输出摘要，保持跨小说无串扰。
- [ ] 小说 A 真实 Disk 恢复 → 1.png → 2.png → 小说 A；再执行 小说 B Disk 恢复 → 2.png → 1.png；两组均须正常结束。
- [ ] 1.png → 1.png、2.png → 2.png：相同媒体身份正确，若无合法缓存不应虚报命中。
- [ ] 1.png → 2.png → 1.png：不同图片仅可共享确切的文本前缀，不能复用不相同的图片状态；最终返回第一张图片时仍能正确描述。
- [ ] 同一图文混合 prompt 的重复请求，及相同文本搭配不同图片的请求；检查媒体 ID、`LLAMA_TOKEN_NULL` chunk 起点、位置端点与 `has_mtmd` 全部对齐。
- [ ] `n_past = 0`、有相同文本 prefix、媒体在文本之前、文本之后、多个媒体 chunk（模型支持时）等边界；即使完全没有可复用前缀，也不得因为遗留的 `has_mtmd=false` 崩溃。
- [ ] 关闭主动中途多模态前缀物化的门禁继续有效；不能将新增可运行的媒体请求误当成“已验证主动写入多模态 prefix KV”。

### 6.4 崩溃回归与既有机制

- [ ] 运行现有九套独立单测，以及本轮媒体状态迁移单测。
- [ ] 复核文本跨重启 Disk 命中保持正常，长文本恢复与相同模型的生成输出摘要一致。
- [ ] `prompt = reused_slot + reused_ram + reused_disk + prefill` 的 token 计数分解保持一致；前缀目录的 `eligible/persisted` 不冒充实际恢复。
- [ ] 不改动隔离侧车、manifest 或工作线程，验证 `active` 禁用仍阻止新写入与新恢复，已有入队任务仍按原取消逻辑处理。
- [ ] 保留当前未执行的队列异常注入、精准崩溃窗口、`touch_entry` 等为**未验证范围**，不得因本次媒体崩溃修复而改写为已通过。
- [ ] 提交实际改动到 GitHub，并在 `change.md` 记录真实源码问题、已运行的用例、原样日志位置和仍未覆盖的测试；完成后同步 `projectSummary.md` 的已证实能力边界。

## 七、实施记录边界

本节只记录“文本缓存恢复后图片请求崩溃”这一单项修复的目标、文件、数据流、对推理/训练影响、与现有修改的兼容性和验证清单。任何尚未实际执行的请求都不可以写成已通过，不填写预测的 AP、tokens/s、写盘缩减数据、早停要求、计算报告或最终判定规则，不把未来新的算法实验并入本节。
