# 2026-11-14 17-01 - Change Record

## 变更概述

针对磁盘二级前缀缓存（`server_disk_cache`）的 9 项审查结论全部落地：KV 状态 CRC 完整性校验（含检查点）、`.tmp` 孤儿文件清理、命中同分时按 `last_used` 取舍、低收益恢复守卫、命中时间戳延迟批量持久化、启动清单（manifest）加速 `scan()`、`--cache-disk-ttl` 闲置过期、镜像写入共享内存（取消全量拷贝）、后台写入节流。条目文件格式版本 5 升级到 7，旧版本文件启动时按既有版本检查自动丢弃。

## 修改文件

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-14 16-31/tools/server/server-task.h
- 修改后路径：tools/server/server-task.h
- 修改位置：`server_prompt_data`、`server_disk_cache_entry`、`server_disk_cache_pending`、`server_disk_cache`、`server_prompt_cache::init_disk`
- 具体变更：`server_prompt_data::main/drft` 改为 `std::shared_ptr<common_state_data>`（RAM 与待写队列共享同一缓冲区）；`server_disk_cache_entry` 新增 `last_used_dirty`；`server_disk_cache_pending::main/drft` 改为 `std::shared_ptr<const common_state_data>`；新增 `disk_cache_manifest_meta` 结构；`server_disk_cache` 构造函数与 `init_disk` 增加 `ttl_seconds` 参数，新增 `ttl_seconds` 成员与 `sweep_expired/load_manifest/save_manifest` 方法声明
- 修改原因：磁盘缓存 9 项改进（#5/#6/#7/#8）的接口支撑

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-14 16-31/tools/server/server-task.cpp
- 修改后路径：tools/server/server-task.cpp
- 修改位置：匿名常量区、`disk_cache_pack`/`disk_cache_pack_ids`、`server_disk_cache` 构造/析构、`scan`、`load_manifest`、`save_manifest`、`write_entry`、`read_entry`、`load`、`store`、`mirror`、`sweep_expired`、`enforce_limit`、`worker`、`server_prompt_cache::init_disk`、`alloc`、RAM 命中路径
- 具体变更：
  1. `DISK_CACHE_VERSION` 5→7（v6 加 main/drft 的 CRC64-ECMA，v7 加每个检查点的 CRC）；header 固定段 56→72 字节
  2. `scan()` 删除所有 `*.tmp` 孤儿文件（原仅 `.bin.tmp`）
  3. `load()` 候选同分时取 `last_used` 较新者（`!found || lcp > best_lcp || (lcp == best_lcp && e.last_used > best.last_used)`）
  4. `load()` 新增恢复成本守卫：`e.size_file > lcp * 768KiB` 的条目跳过（KV 状态为流式 blob 无法切片读取，详见验证记录）
  5. `load()` 命中只更新内存 `last_used` 并置 `last_used_dirty`；去掉每次命中的 8 字节原地写；析构时仅对 dirty 条目批量 `touch_entry`
  6. 新增 `pc-index.dat` 启动清单：magic/version/sig + 每条目（文件名、hash、size_file、last_used、packed 元数据）；`scan()` 对大小匹配的清单记录免开文件直接信任，不一致回退完整校验；写入成功/淘汰/过期/读取失败删除后 `save_manifest()`
  7. 新增 `--cache-disk-ttl`（秒，0=不过期）：`sweep_expired()` 在 `enforce_limit()` 内先按闲置时间淘汰（启动扫描与每次写入后均触发）
  8. `mirror()` 由深拷贝 KV 状态改为 `shared_ptr` 共享；`store()` 移动 `shared_ptr`；`alloc()` 直接 `make_shared`；RAM 命中路径 `reset()` 释放（若镜像仍引用则延迟到写完）
  9. `worker()` 每写满 256 MiB 让出 10 ms（`DISK_CACHE_WRITE_THROTTLE_*`）
- 修改原因：磁盘缓存 9 项审查结论的全部实施

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-14 16-31/tools/server/server-context.cpp
- 修改后路径：tools/server/server-context.cpp
- 修改位置：`prompt_save`、`init_disk` 调用点
- 具体变更：`cur->data.main.data()` → `cur->data.main->data()`（`drft` 同）；`init_disk` 调用传入 `params_base.cache_disk_ttl`
- 修改原因：配合 `server_prompt_data` 共享指针化与 TTL 参数

### common/common.h

- 修改类型：修改
- 修改前路径：common/common.h
- 修改前备份：.backupChange/2026-11-14 16-31/common/common.h
- 修改后路径：common/common.h
- 修改位置：`common_params` 磁盘缓存参数区
- 具体变更：新增 `int32_t cache_disk_ttl = 0`（`--cache-disk-ttl`）
- 修改原因：TTL 过期功能的参数定义

### common/arg.cpp

- 修改类型：修改
- 修改前路径：common/arg.cpp
- 修改前备份：.backupChange/2026-11-14 16-31/common/arg.cpp
- 修改后路径：common/arg.cpp
- 修改位置：`--cache-disk-idle-flush` 选项之后
- 具体变更：新增 `--cache-disk-ttl N` 命令行选项（非负校验，环境变量 `LLAMA_ARG_CACHE_DISK_TTL`，server 示例）
- 修改原因：TTL 过期功能的命令行入口

### README_CN.md / README_EN.md

- 修改类型：修改
- 修改前路径：README_CN.md / README_EN.md
- 修改前备份：.backupChange/2026-11-14 16-31/README_CN.md、README_EN.md
- 修改后路径：README_CN.md / README_EN.md
- 修改位置：第 4 节参数调优表、第 7 节改动清单
- 具体变更：新增 `--cache-disk-ttl` 行；参数计数 4→5；`server_disk_cache` 特性描述补充 CRC 校验、manifest、TTL、写入节流、共享内存镜像
- 修改原因：文档与新参数同步

## 验证记录

- 已执行检查：
  1. MSVC 14.44 `cl /Zs /utf-8 /EHsc /std:c++17` 语法级编译 `tools/server/server-task.cpp`、`tools/server/server-context.cpp`、`common/arg.cpp` —— 0 错误（注意：vcvars64 在当前 shell 不可用，INCLUDE 为手工组装的 VC 14.44 + Windows SDK 10.0.26100.0；未做链接与运行）
  2. `grep` 全量核对 `data.main/drft`、`pending.main/drft` 无残留裸 vector 访问
  3. `git diff` 全文人工复核：写/读格式字段顺序一致（ckpt 元数据 40→64 字节/个）、manifest 读写字段顺序一致、守卫阈值无溢出（lcp×768KiB 在 64 位 size_t 内）
- 检查结果：通过（功能测试未执行——本机无 CUDA 运行环境，构建需部署机工具链）
- 遗留说明：条目文件格式 v5→v7 不向后兼容，旧条目启动时由既有版本检查自动丢弃并重新生成，属预期行为

# 2026-11-14 17-45 - Change Record (build & test follow-up)

## 变更概述

两项：① `build-cuda-shared.bat` 的工具链路径从旧机器（`C:\vs\buildTools`）更新到本机（`D:\Visual Studio\buildTools`）；② 修复磁盘缓存启动清单（manifest）v1 的缺陷——清单记录缺少 `size_main/size_drft`，经清单加载的条目在恢复时被 `read_entry` 的大小交叉校验拒绝，导致恢复必然失败、条目被误删。清单格式升级到 v2 并补全这两个字段；v1 清单启动时按版本检查自动拒绝并回退完整校验，可自愈。

## 修改文件

### build-cuda-shared.bat

- 修改类型：修改
- 修改前路径：build-cuda-shared.bat
- 修改前备份：.backupChange/2026-11-14 17-20/build-cuda-shared.bat
- 修改后路径：build-cuda-shared.bat
- 修改位置：vcvars 调用与 `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER`/`CMAKE_CUDA_HOST_COMPILER`
- 具体变更：`C:\vs\buildTools\...` → `D:\Visual Studio\buildTools\...`（共 4 处路径）
- 修改原因：本机 VS Build Tools 安装位置变更，原脚本无法加载编译环境

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：无（同一会话内 17-01 记录的备份即为修改前基线，见 .backupChange/2026-11-14 16-31/）
- 修改后路径：tools/server/server-task.h
- 修改位置：`disk_cache_manifest_meta`
- 具体变更：新增 `size_main`、`size_drft` 两个 uint64 字段
- 修改原因：修复清单加载条目缺少 KV 负载大小、恢复时被交叉校验拒绝的缺陷（测试中发现）

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：无（同上，基线见 .backupChange/2026-11-14 16-31/）
- 修改后路径：tools/server/server-task.cpp
- 修改位置：`DISK_CACHE_INDEX_VERSION`、`scan()` 清单快速路径、`load_manifest`、`save_manifest`
- 具体变更：清单版本 1→2；记录与读写流程加入 `size_main/size_drft`；`scan()` 信任清单时回填 `e.size_main/e.size_drft`
- 修改原因：同上缺陷修复

## 验证记录

- 已执行检查（本机全量构建 + 部署实测）：
  1. `build-cuda-shared.bat` 全量编译 437 步 0 失败，66 个产物 staged 到 `output/`；`llama-server.exe --version` 正常运行且 `--cache-disk-ttl` 选项注册成功
  2. 部署到 `D:\AiHelper\llama.cpp\`（旧二进制哈希已记录于该目录 `SHA256SUMS.pre-diskcache9.txt`），启动脚本拉起成功，health OK
  3. 基础对话：中文问答、reasoning 格式正常
  4. 长前缀（5064 token）首次全量 prefill 3029 ms → 二次 RAM 命中仅 4 token / 699 ms → 空闲镜像落盘（841.61 MiB，5.4 s）→ 重启后磁盘恢复 5060 token / 466 ms（约 7×）；`pc-index.dat` 清单生成
  5. 图片理解：`1.png`（资源管理器截图）被正确描述，多模态链路正常
  6. `.tmp` 孤儿文件：制造 1 MiB 假 `.bin.tmp`，重启后被 `scan()` 清除并记日志
  7. CRC 校验：条目偏移 800 MB 处翻转 1 字节，恢复时被检查点 CRC 捕获（`checkpoint checksum mismatch`），条目删除并回退全量 prefill，无静默错误推理
  8. TTL（独立实例，ttl=120 s）：条目闲置 122 s 后被 `sweep_expired` 清除并记日志；存活的 RAM 会话在下次空闲镜像被重新落盘属预期行为
  9. 旧 v5 条目 4 个启动时按版本检查正确丢弃；v1 清单被 v2 构建拒绝并回退完整校验，自愈正常
- 检查结果：通过
- 未执行：64 位以外的架构、并发多 slot 压测（部署为 -np 1 串行场景）

# 2026-11-14 19-05 - Change Record (审查跟进小修包)

## 变更概述

磁盘缓存/解码路径联合审查结论的 6 项小修包一次性落地：P0-2 恢复失败静默错答（4 处 return true→false）、P0-3 缓存签名补维度（draft KV 类型 + kv_unified + mmproj）、P0-1 写队列 4GiB 硬上限改为 --cache-disk-queue-max（默认 24576 MiB）+ 条目数上限 8、P0-4 last_used 被 manifest 覆盖、speculative 启动日志补三字段、CRC64 单表→slice-by-8。另按审查意见实现 LoRA 运行时守卫：生效 LoRA（任一 scale≠0 且非纯 aLoRA）的 slot 绕过提示缓存（RAM+磁盘两层）。P0-1 为生产实锤：日志中 44042 tok/3594.13 MiB 成功落盘，而 46815/85295/89453/90979 tok 四条 "write queue is full" 被拒。

## 修改文件（修改前备份：.backupChange/2026-11-14 18-30/）

### tools/server/server-task.cpp

- 修改位置：匿名常量区、`disk_cache_crc64`、析构、`load()` 恢复路径、构造函数、`store()`/`mirror()`、`init_disk`
- 具体变更：① `DISK_CACHE_QUEUE_BYTES`(4GiB) → `DISK_CACHE_QUEUE_MAX_ENTRIES=8`，字节预算改由 `--cache-disk-queue-max` 控制（队列条目只持有 RAM 缓存缓冲的 shared_ptr，放宽无内存风险，真正要防的是慢盘无限积压）；② CRC64 改 slice-by-8（8×256 派生表，T7..T0 查找 + 尾部单表，校验值与单表版完全一致）；③ 析构在 touch_entry 之前先 `save_manifest()`（持锁）——原实现里清单快路径会用旧值覆盖关机写回的 last_used，跨重启 LRU 实际归零；④ `load()` 中 5 处 `return true` → `return false`（全部发生在 KV cell 已被条目数据替换之后、prompt.tokens 尚未更新之前，return true 会让 slot 带着错位 KV 继续推理——静默错答；return false 后上层 `prompt_clear()` 清槽重 prefill）。按触发条件排序：**文件损坏即可触发的两处**——prompt 元数据（packed）解码失败、media 元数据校验异常；**依赖配置的一处**——条目含 draft 状态但无 draft 上下文（前提 = 模型带真实 MTP 层（`n_layer_nextn > 0` 且 `router_layer < 0`，否则 MTP context 创建失败、服务启动即退出）+ 用 `--spec-type draft-mtp`（不带 -md）写过条目 + 之后切到 `--spec-type none`；`-md` 配置下不可达，签名含 draft 路径已保证。警示：`size_drft > 0` 只说明写入时 `ctx_dft` 存在——`state_seq_get_size` 无论载荷多少都至少写 ≥8 字节状态流头（io_magic 4 + seq_id 4，见 src/llama-context.cpp `state_seq_write_data`），不代表 draft 侧有实质 KV 体积，勿拿它反推 draft 状态大小）；target/draft 状态恢复失败两处（可能瞬时不删条目）；查找无果与 read_entry 失败两处保持 `return true` 不变；⑤ 构造函数与 `init_disk` 增加 `queue_max_mib` 参数；⑥ store/mirror 队列检查改用 `queue_bytes_max` + 条目数上限，store 超限 WRN 附带队列状态；⑦ 抽出 `drop_entry()`（删文件+摘索引+存清单，持锁调用——save_manifest 约定持锁），read_entry 失败路径复用，确定性失败三处（无 draft 上下文/元数据解码失败/media 校验异常）顺带删条目，防止「每请求一次整文件读+CRC 后失败」的循环；set_data 失败（可能瞬时）仅留日志保条目。审查定级为防御性/低优先，随本轮重编一并落地。另修：`remove` 的 `ec` 只在删除成功或文件不存在时才回退 `total_size`、失败时（Windows 共享冲突/Defender 占用）仅摘索引并记 WRN——原实现不查 `ec`，文件还在但配额已扣，enforce_limit 会低估用量、目录可静默超限至下次启动扫描；⑧ restored 汇总日志改 `%zu/%zu tokens`（实际命中 lcp / 条目长度——原实现打印条目长度，会让短前缀命中看起来像全量恢复，污染 bpt 测量阶梯）。持久判据（不依赖行号，供后续插项与回归断言）：`load()` 中第一个改写 ctx 状态的调用是 `llama_state_seq_set_data_ext(ctx_tgt, …)`——**它之前的任何 return 都是 `true`**（KV 与 slot.prompt.tokens 仍自洽）；**它之后的任何失败 return 都是 `false`**（KV 可能已被部分覆盖，必须由调用方 prompt_clear()）；**它之后唯一的 `true` 是函数末尾的成功 return**（回归断言建议用**序列签名**而非计数：`load()` 全函数 return 序列为 `T T T T | F F F F F T`——首个 set_data 边界前 4 个 true、边界后 5 个 false + 1 个 true 收尾。签名任一位被打乱即回归，比"计数==1"更抗编辑——单看"末尾必须是 T"抓不到在中段插入的"优雅降级"true）。断言作用域必须夹在 load() 函数体内——扫全文会数到 3 个 return true（load 收尾 + mirror() 两处），永远误报。将来在边界之后新增路径时返回值无自由度、只允许 false；可选的只有保留条目（瞬时失败，如 set_data 失败→不 drop）还是丢弃条目（确定性损坏，如 unpack/media/未来 B 校验→drop）；上边界之前新增路径仍可用 true（更优，不白丢上下文）。**B 落地后的预期签名**：`T T T T | F F F F F F T`（总 return 数 10→11，B 在边界后新增 1 个 F）——核对签名即可确认插入位置正确，无需 diff 行号
- 修改原因：上述 ①-⑥

### tools/server/server-task.h

- 修改位置：`server_disk_cache` 构造/init_disk 声明、成员区
- 具体变更：签名增加 `int32_t queue_max_mib`；新增成员 `size_t queue_bytes_max = 0`（0=不限）

### tools/server/server-context.cpp

- 修改位置：磁盘缓存签名与 `init_disk` 调用、`server_slot::prompt_save`/`prompt_load`、`get_available_slot`
- 具体变更：① 签名追加 `draft cache_type_k/cache_type_v`、`kv_unified`、`mmproj(path@size@mtime)`——draft KV 类型是独立参数（当前部署未传 -ctkd/-ctvd 即为 F16 默认），换类型/换视觉编码器后旧条目必须失效而非恢复不兼容 KV；② 新增辅助函数 `lora_blocks_cache()`（server-common.cpp/.h）：任一生效适配器 `scale != 0` 且非 aLoRA 即拦截——用 `!= 0` 是故意的，负 scale 同样修改模型权重但不在 `lora_get_enabled_ids` 的 `scale > 0` 范围内。守卫三处（函数名+条件锚点，不用行号）：`server_slot::prompt_save`（`lora_blocks_cache(lora)` → 返回 false，不入缓存）；`server_slot::prompt_load`（`lora_blocks_cache(lora)` → 返回 true，skip 语义保住 slot KV——LoRA 恒定部署保持增量续算，否则每请求全量 prefill）；`launch_slot_with_task` else 分支（`!are_lora_equal(slot.lora, params_base.lora_adapters) && lora_should_clear_cache(...)` → `slot.prompt.clear()`）——else 分支是覆盖面修复而非可选健壮性：相似度命中且 f_keep ≥ 0.5 时 prompt_load 不被调用（聊天式小步增长场景），一次性贴长文档走 LRU 选槽时才调用，只有 else 分支能拦住「直接复用旧 LoRA KV」；③ `get_available_slot` 对携带 per-request LoRA 的任务跳过缓存更新（slot.lora 在 launch_slot_with_task 才更新，需按任务配置预判）；prompt_load 失败的日志由 WRN 降为 TRC；启动时若全局 LoRA 生效输出 INFO「prompt cache effectively disabled…」；④ init_disk 传入 `cache_disk_queue_max_mib`。已记录限制：LoRA 部署下两级缓存对该 slot 整体失效（prompt_save 是 RAM 缓存 alloc() 的唯一调用方）；本部署未用 --lora，以上对空 lora 表是严格空操作
- 修改原因：签名维度补齐 + LoRA 静默错答防护（含负 scale 绕过修复）

### common/common.h

- 具体变更：新增 `int32_t cache_disk_queue_max_mib = 24576;`

### common/arg.cpp

- 具体变更：`--cache-disk-queue-max N`（-1=不限，0 拒绝；env `LLAMA_ARG_CACHE_DISK_QUEUE_MAX`；server 示例）

### common/speculative.cpp

- 修改位置：draft 实现构造函数启动日志
- 具体变更：新增一行 `causal_attn/selector_top_k/is_dflash2`——用于区分 lattice 走链路径与每行 151K 词表 CPU top-k 路径（性能画像完全不同）

### tools/server/server-common.h / server-common.cpp

- 修改位置：`lora utils` 区
- 具体变更：新增 `lora_blocks_cache()` 声明与实现（任一生效适配器 `scale != 0` 且非 aLoRA → true，见 server-context.cpp 改动②）

## 验证记录

- 已执行：① MSVC 环境增量构建 llama-server：66 步 0 错误（第一轮因常量定义替换遗漏报 C2065×2，已修）；② `llama-server.exe --version` 正常（0.9.0-dev build 2, commit 60f3891）；③ `--help` 确认 `--cache-disk-queue-max` 注册；④ CRC64 slice-by-8 与单表版 Python 等价验证（300 组随机缓冲 + CRC64-ECMA 标准向量 "123456789"→0x995DC9BBDF1939FA）；⑤ 第二、三轮增量构建（LoRA 成对修复、else 分支 + restored %zu/%zu + drop_entry 套件）均 0 错误；⑥ 第四轮增量构建（drop_entry ec 记账修复）0 错误。终包三方独立复核一致：`output/` 66 文件整组重铺，`llama-server.exe`/`llama-server-impl.dll`/`llama-common.dll` 与 build-cuda-shared/bin 侧 md5 逐字节相同（impl DLL 两侧同为 `aae4f61e…`，mtime 19:31:01 > 末次源码编辑 19:29:29，`find -newer output/llama-server-impl.dll` 在 server/common/src/include/ggml 源码面为空 = 源码不再领先产物）；二进制 canary 指纹全部命中：`could not remove`、`not reclaiming`（ec 修复独有）、`restored %zu/%zu tokens`、`skipping prompt cache load: active LoRA`、`skipping prompt cache save`、`entry has a draft state but there is no draft context, dropping it`。陈旧判据三条叠用（源码 md5 区分 touch/真改、max(源码 mtime)≤产物 mtime 抓改了没编、产物 md5==已铺+canary 抓编了没铺）；注意判陈旧必须以 `output/llama-server-impl.dll` 为基准——server 源码经静态库 server-context 链入 impl DLL（tools/server/CMakeLists.txt:6-59），`llama-common.dll` 不随 server 改动重编是结构必然
- 未执行（需部署机重启服务）：功能测试。首次部署后建议观察：启动 INFO 的 `causal_attn=... selector_top_k=... is_dflash2=...` 三字段；旧缓存条目被签名检查级联删除（大量 dropping ... different model or context configuration 属预期，**签名不匹配删除不可恢复，升级前如需保留旧缓存请先备份目录**）；长会话落盘不再出现 write queue is full

## 遗留说明与下一步

- 恢复失败条目分层：确定性损坏（元数据解码失败/media 不一致）已删条目；set_data_ext 失败视为瞬时、保留条目（存在每请求重试成本，观察后可加失败计数阈值）
- `read_entry` 把「打不开文件」「截断」「CRC 不匹配」返回同一个 false → 失败分支会把**暂时不可读的有效条目**也删掉（Defender 实时扫描、可移动盘掉线重连等场景，一次删几 GB）。下轮修法定稿（三态判据）：`read_entry` 加 `bool & unreadable` 出参（默认 false = 删，保证新增早退不破坏既有删除语义）；判定用 `e.size_file` 与实际 `file_size` 比对——**相符 → I/O 故障（文件完好）→ 保留**；**不符 → 截断 → 删**；`stat` 失败 → 保留 + WRN（可叠加 per-entry 连续失败计数，2~3 次后无条件删，防"stat 长期失败但 open 能开"的低概率组合）。注意 `e.size_file` 的可信来源只有清单快路径（manifest 的 `size_file == fsize` 校验）与 `store()` 写入设值，头部交叉校验比的是 size_main/size_drft，别在注释里引用后者。失败路径多一次 stat 可忽略
- **P1**（最终权威规格，单一副本，取代此前所有散落描述；**冻结基准**：`tools/server/server-task.cpp` md5 `1dea2460939c2863e1110c7d09856e7d` / 134502 B / mtime 2026-11-14 19:29:29——md5 变化即视为规格需重核、行号一律重算，mtime 不可靠已实证 19:24:52 空写）条目 packed（token+media 元数据）无 CRC 守卫。**本轮只记录、未落地**（曾实现后按冻结裁定回退，非编译失败——第三轮构建 BUILD_OK）。`#1-#6` 验收结论不受影响
  **落地位置**：`load()` 内，`unpack_ids` 成功之后、`try` 之前（当前 `:3066`）。与 unpack 失败同属「packed 不可信」，不放进 try，避免被读成归 catch 管
  **L2 条数一致性（O(1)）**：`restored_tokens.size() != best.tokens.size()` → WRN（`packed metadata token count mismatch: restored=%zu expected=%zu lcp=%zu`）+ `drop_entry` + return false。覆盖面**仅清单快速路径有意义**（best.tokens 来自清单 blob、restored_tokens 来自文件 blob）。全量路径**在无 TOCTOU 时**两者同源解码、此检查空转——`best.tokens` 是**扫描时刻**的解码、`restored_tokens` 是**请求时刻**的解码，「同源」只在同一时间点成立；`:2206` 快速路径只要求 `size_file == fsize`、不比 packed，文件在 `scan()` 与 `read_entry()` 之间被外部改动（另一实例共用同一 `--cache-disk` 目录、清理脚本/杀毒、跨启动残留）时两份解码会不一致，**L2 仍可触发**。**并发删除不属于内容改动源**：候选遍历在锁内（:2915）、`read_entry` 在锁外（:3021）、`drop_entry` 内部加锁（:2748），并发删除只能落在 read_entry 窗口——但删除不改内容（先删后 open → 打开失败走 :3023/:3025；先 open 后删 → Windows 共享冲突文件仍在 / POSIX unlink 后句柄仍指原数据，两种交错内容均不变），不产生 L2/L3 意义上的不一致；能造成内容变化的是**同一路径被重写成不同内容**，归入外部改动类。成本 O(1)。必要性依据：`read_entry` 只比 size_main/size_drft/n_ckpt（`:2594`），不比 packed_size、不比 packed 哈希；且 size_main 按 n_pad=256 分桶（llama-kv-cache.cpp:1322），`:2594` 从原理上看不见计数差异
  **L3 内容一致性（std::mismatch，一次遍历）**：`end1 = restored_tokens.begin() + (ptrdiff_t) best_lcp`（**L2 通过后其上界才安全**）；`mm = std::mismatch(begin, end1, tokens.begin())`；`mm.first != end1` → WRN（`cached token ids differ from the prompt prefix at %zu/%zu (%zu%%), restored=%zu expected=%zu lcp=%zu`，带 diff_at 绝对值 + 百分比）+ drop + return false。**比较域必须是局部 tokens**（disk_cache_pack 产出、media 已折叠），**不能用入参 tokens_new**（media 区间逐 token 展开，直接比会错位、带图必误删）。**顺序不可颠倒**：直接拿 best_lcp 当 restored_tokens 上界会在长度不足时越界（`:3082` 只做向下截断、从不补齐），UB 先于检测发生。**`best_lcp ≥ 1` 恒成立**——`:2989` 排除 lcp==0，且三个 entries.push_back 点（scan 快路 `:2210/:2220`、全量 `:2298/:2301`、worker `:3378/:3388/:3392`）的 size_file 均来自真实文件大小（>0），除法无需保护。B 注释措辞定为「best_lcp ≥ 1 由上面的字节/token 闸门保证；此处的 0 保护只是结构性防御，当前永不触发」——不要写成「min_tokens 可以放开所以 0 保护有用」（与 :2989 冲突）
  **收益三条（按强弱排序）**：③（最强，**正确性 + 永久化**）消除「同长度同条数 id 段损坏」的完全静默——n_tokens 与 media 段不变、只改 ids 若干值时，:1945（长度）/:1949（media 段起始偏移）/:1962-1973（表头/完全消费）/:3069（media 语义比对，media 确实没变）/:3082/:3091 每一步都通过、**全程无 WRN**。关键机制（post-scan 全覆盖）：`e.tokens` 是**索引时刻**的解码——post-scan 下 `best_lcp` 锚在**真值**上。请求在损坏位置 `j` 持真值时 lcp 越过 `j`，L3 在 `j` 处抓到 restored≠tokens（`I'[j]≠I[j]`）；请求恰好持坏值 `I'[j]` 时 `best_lcp` 锚在真值 `e.tokens` 上必停在 `j`（`e.tokens[j]=I[j]≠I'[j]`），`:3082` 截掉 → **无害**。故 post-scan 窗口「catch 或无害」全覆盖，**B 完整闭合，连「请求恰好匹配坏值」的巧合也自动无害**。pre-scan 损坏（索引前已坏）不在覆盖面：两份拷贝同源一致、L2/L3 恒不触发，其唯一危险子集是「索引前已损坏 + 请求在损坏位置匹配坏值」（lcp 越过损坏点、错位 KV 被 prefill），须由 v8 的 packed 校验和覆盖。**洗白与永久化**：若 post-scan 损坏未经 B 拦截被装入 slot，会经两条写入口固化——`store()`（`:3129`，pack 在 `:3140`）与 `mirror()`（`:3199`，pack 在 `:3220`，`:3225` 有独立 min_tokens 检查），两者都是「ids 取 `state.prompt.tokens`、state 取 buffer/ctx」；固化成的新条目 packed=改写值、KV=真值——清单解码与文件解码同源同值 ⇒ L2 过；L3 比的是「条目 vs 请求」、请求匹配条目 ids 即通过 ⇒ L3 也不触发。**所有检查只比对 packed 的两次解码、或 packed 与请求，从不比对 packed 与 KV 的对应关系**——对未来匹配其 ids 的请求**永久**输出错位 KV。①补 CRC 分区盲区——写路径只对 main/drft/各 checkpoint 校验，packed 无校验和，read_entry 亦不校验其哈希；②（最弱）消除 token 段**条数**损坏的误归因——原本只有 media 那条 WRN 会响，归因指向 media、真凶在 token 段。**media 段**损坏不归 B 管——manifest 侧由 `:2217-2218` 的重算哈希自校验（哈希覆盖 tokens + media），文件侧由 `:3069` 语义比对兜底；落到 :3069 时归因本身正确（media 确实不一致），代价只是白付一次 KV 写入。现有 catch 文案无需改动（它服务「真换了图」）。**L2 管条数（写错→误归因）、L3 管内容（写错→完全静默并被永久化），两者都不能省**
  **已知限制（根因 + 正交分工 + 两条出路，均不实现）**：条目完整性有两个维度——packed 自洽（L2/L3 管）与 **packed↔KV 对应（今天无人校验）**。CRC 只各自覆盖 payload，从设计上允许「packed 说一套、KV 存另一套」。**分界点在「损坏是否早于索引」**——L2/L3 比的是同一份 packed 的两次解码：**post-scan** 损坏（索引后文件被改）：`e.tokens`=真值、`restored_tokens`=损坏值，`j < best_lcp` 时 L3 必抓、`j ≥ best_lcp` 被 :3082 截掉无害，**B 落地后该窗口完整闭合**；**pre-scan** 损坏（索引时就坏）：两份拷贝同源一致、**恒不触发**——静默错误。故 **B（L2/L3）与 v8 是两个正交项、不可互相替代**：B 覆盖 post-scan、是该窗口唯一闸门；v8（packed 加第三个 CRC 字段）覆盖 pre-scan、是唯一手段。洗白路径有**两条写入口**——`store()`（`:3140`）与 `mirror()`（`:3220`，RAM 缓存空闲落盘）都用 `disk_cache_pack(state.prompt.tokens, …)` 配 `state.data`，性质相同：slot 里 ids 与 KV 已错位时会被原样固化成 packed↔KV 不对应的新条目。出路按优先级：①**根治（v8）**——给 `packed` 加第三个 CRC 字段，把「packed↔文件其余部分」的对应关系纳入写路径的原子保证（唯一能同时挡住首犯与洗白的机制）；②**缓解**——`server_prompt` 加 `restored_from_disk` 标记，`prompt_save` 对「带标记且本会话未被 prefill 重算」的 prompt 跳过落盘（挡住洗白传播，挡不住首犯）。**覆盖面区间与平级关系**：**B 覆盖 `(索引, 恢复]`，v8 校验和覆盖 `(-∞, 索引]`，互不替代、在记录里平级**——没有 v8，pre-scan 损坏永远无门；没有 B，post-scan 损坏只能靠运气；不是「先做 B、以后顺手加 CRC」。结构性结论：任何 packed-vs-packed 检查（L2/L3）对「索引前已损坏」的条目恒失效——两份拷贝都来自同一份坏字节；只有写路径上算好、随条目存下的 packed 校验和（v8 第三 CRC 字段）能在读取时判定「这份 packed 与当初写下的不是同一份」
  **盲区分工**：无 media 条目 → L2 结构性不可触发（`:1973` 要求 offset == packed.size() 完全消费，改小 n_tokens 必留残余字节），唯一闸门 L3；有 media 且 n_tokens 改小后恰好自洽 → L2 可能触发，**L2 是 KV 改写前唯一闸门**（`:3069` media 交叉校验是第二道，但位于 set_data 之后：拦得住也白付，KV 已写入、prompt_clear()+全量 prefill 逃不掉）。「恰好自洽」条件：`n_media=ids[N-k]`、`media_indices[0]=ids[N-k+1]`、`media_token_counts[0]=ids[N-k+2]`、`strlen=ids[N-k+3]` 四个真实文本 token 的 id 构成合法表头（`:1950` 上界、`:1962` 单调与范围），且 index 落在某条 NULL 占位符连续区内（**不要求等于 chunk 起点**，窗口 `[chunk_start, chunk_end - count]`）。窄但不空
  **独立既存问题**：restored_tokens 偏短时赋给 prompt.tokens 的长度短于 best_lcp，load() 尾部「prompt.tokens 恰为 best_lcp」不变量当场破坏——`n_past` 变小 → `seq_rm(id, p0, -1)` 从更小 p0 截尾 → 刚恢复的 KV 尾部被丢弃重算（「恢复」静默退化为部分恢复+部分重算）；命中日志的两个参数均非 restored_tokens.size()，此退化在现有日志下完全不可观测
  **验收**：返回点签名 `T T T T F F F F F T` → `T T T T F F F F F F T`（总 return 10→11，新 F 第 5 位）；求 E 用括号配平，T/F 判定必须对**剥注释后的行**分类。canary：`token count mismatch`=1（静态存在性、非运行期指标——无 media 原位改写下运行期永不触发）、`token sequence mismatch`=1（**唯一有运行期观测意义的信号**；diff_at 分诊两层：0/非 0 → 清单/文件错位；是否紧邻 media chunk 边界——`restored_media_indices`（:3057）与 diff_at 同域——size 等长是常量不能当判据。方向语义（须写进 L3 注释）：`diff_at == idx-1`（紧邻 chunk 起点之前）更像 chunk 起点/长度在写入侧漂移，`diff_at == idx+1`（刚进入 chunk）更像占位符被改写——布尔「near media」只回答第一问，无法指导方向；整数百分比会把 `diff_at=5/lcp=34000` 压成 0%，**判零必须读 diff_at 绝对值**）、`expected=%zu lcp=%zu`=2（两处共享标签串，顺序统一才成立，掉到 1 即标签被改乱）。三条在源码与当前 DLL 均为 0，可用；裸词 mismatch 已有 6 处，不可作 canary。排查经验：现场出现 media mismatch 而请求的 media 从未变过 → 优先怀疑 packed token 段被改，而非 media 逻辑
- spec state 长度不符静默返回已核实（speculative.cpp 两处 `set_state`：先精确比 `data.size()`、不等即 `return`、不写任何字节、无日志）——跨 spec-type 恢复是**静默空操作，后果无害**（等效无该状态，不是损坏、也不是错误）；KV 层（llama-kv-cache.cpp:2740/2774）是严格校验会失败，checkpoint data_spec 随 KV 层走故整体安全；不改
- 可选：spec types 位图拼进缓存签名（一个 %d）可让「无 draft 上下文」整类失效消失 + checkpoint data_spec 不跨 spec-type 误命中；代价是再清一次缓存（与 draft 类型入签名同性质）。**必须排在本轮测量之后**：签名变更会清空现有条目，而取数（-lv 4 逐条 lcp、-lv 5 的 restored a/b）需要条目在测量期间保持有效。顺序：先取数定稿 P1-4/bpt → 再决定加不加
- drop_entry 持锁重写整份 manifest：单次可忽略；批量失败场景（配置变化致大量条目同时判失败）会阻塞 worker，届时改阈值批量 drop + 一次 save_manifest
- queue-max 调参：受 `--cache-ram` 约束（pending 只引用 RAM 条目缓冲）；多并发大 prompt 按 单条目MiB×并发 估算（本部署 12.5GiB×1）；出现 "write queue is full" WRN 再调
- 不变量 + 设计债（非 bug）：**`best_lcp ≥ 1` 恒成立，与 `--cache-disk-min-tokens` 取值无关**——`lcp == 0` 被恢复闸门排除（`:2989`：`e.size_file > 0 × 768 KiB = 0` 恒成立 → continue）。结构性证据：`entries` 不可能存在 `size_file == 0` 的条目，三个入库点全部用真实文件大小——scan 快路 `:2210/:2220`（`e.size_file = fsize`）、scan 全量 `:2298/:2301`（同）、worker `:3378 if (ok) { … :3388 e.size_file = size_file … :3392 push }`（write_entry 成功才入库）。`best_lcp` 只在选中候选后写入，永远是 0 以外的值。由此 L3 的除零分支连前提都不存在（装饰性写法），B 注释措辞定为「`best_lcp ≥ 1` 由上面的字节/token 闸门保证；此处的 0 保护只是结构性防御，当前永不触发」。**两个闸门域不同，勿把 min_tokens 当读侧保护**：load 侧 `:2989`（约束每次恢复的字节/token 比）；store 侧 `:3134`（约束入库长度）——`min_tokens = 0` 只松动 store（极短 prompt 也占配额），load 侧下界由 `:2989` 给出且强得多：`lcp ≥ ceil(size_file / 768 KiB)`（`DISK_CACHE_MAX_BYTES_PER_TOKEN = 768 KiB`，:1798）——生产实测 3594 MiB 条目 ⇒ `ceil(3594×1024/768) = 4792` token。设计债一句带过：入库阈值与恢复阈值共用一个旋钮；不需要 `if (best_lcp == 0) return true;`（死代码）
- 设计层观察（只记不实现）：`prompt_save` 信任 `prompt.tokens` 与 ctx 状态一致，而该假设**无校验点**；彻底闭合不可能（无法从 KV 反推 token 序列）。唯一可行的闭合方向：「进入 `prompt.tokens` 的 id 必须来自可信来源」——L2/L3 必须在赋值之前跑（当前 B 设计正是如此）。可选增强：给「来自磁盘恢复的 prompt」打标记、逐出时不写回缓存，防洗白二次传播
- 条目体积与 dense/hybrid 结构待下次启动加 `-lv 5 --log-file` 取数（`restored N/M tokens (a + b bytes)` 定 main/draft 拆分、`llama_kv_cache` 行数判结构）——不再引用未验证数字；量级注记：34k token 全量 prefill 下界 ~20s 级 vs 磁盘恢复亚秒级（数百倍差）；测量方法：`-lv 4` slot print_timings 的 prompt eval time（同 prompt 第二次请求）
- 部署脚本 A/B 项（与本修包分离，逐项验证）：`--cache-ram 25600`、`--spec-draft-n-max 15`、`-b 4096 -ub 1024`、`--spec-type none` 对照（`-sm row` 对照已于 20-05 实测否决，不再列入）。缓存盘迁移 NVMe 与草稿 KV 降阶两项按部署决策不纳入考虑
- 第二阶段（需设计评审）：镜像增量/分段条目、恢复按层偏移切片+异步预取、自适应 n_max、`--spec-draft-swa`、Windows 写路径 Win32 化（实测写 156-180 MB/s）、`/metrics` 可观测性

# 2026-11-14 20-05 - Deployment A/B record（配置级实测，未改代码）

## `-sm row` 可行性结论：不可行（决定性）

测试变体（`-sm row -ts 40,60`，其余与生产配置一致）启动即失败：

```
llama_model_load: error loading model: device CUDA0 does not support split buffers
```

本 fork 的 CUDA 后端未实现 split buffer（与上游 llama.cpp 一致），按行张量切分在 CUDA 设备上不可用——双卡并行化此路不通，消费卡 P2P 问题都未到达。双卡只剩 `-sm layer` 一条路，优化空间仅为切分比例。

## `-sm layer` 切分比例实测（生产配置其余不变：n-max 15 / ub 1024 / cache-ram 25600 / draft KV q8_0）

基准方法：bench-req*.json（重复技术段落构成 ~6K / ~36K / ~117K token prompt），`max_tokens=128`，第二次请求命中缓存以纯解码计时，取 server 日志 print_timing。测试脚本与请求体保留在 `D:\AiHelper\llama.cpp\bench-*`。

| 切分 | ~6K ctx | ~36K ctx | ~117K ctx | 备注 |
|---|---|---|---|---|
| 38,62（生产现值） | **49.9 t/s** | 39.4 t/s | **34.3 t/s** | 带宽模型最优点 ~35/65，38/62 已贴近 |
| 30,70 | 40.5 t/s | — | — | 慢 19%（该轮接受率偏低，见下） |
| 45,55 | 41.9 t/s | **42.6 t/s** | — | 36K 处略快（+8%，接近噪声） |

判读：per-round 耗时三个比例在 63-80ms 内差异 ≤6%（6K→117K 仅 69→90ms，KV 随上下文增长的影响被投机批量摊薄）；t/s 表观差异主要来自 **draft 接受率的运行间波动**（本合成 prompt 上 0.11-0.18，远低于生产 0.48）。**结论：38/62 维持不动，切分比例不是有效杠杆。**

## 附带发现（比切分重要）

1. **生产「120K 上下文 10 t/s」未被复现**：~117K 合成上下文实测 34.3 t/s。若生产仍观测到 10 t/s，差异来自负载特性（长 reasoning 生成/真实更长上下文/图文混合/旧配置），建议在生产实例加 `-lv 4` 用真实流量抓一轮 print_timing 定位。
2. **磁盘缓存工作正常且已被实测依赖**：36K prompt 跨重启恢复（"cold"实为磁盘恢复 6.7s）；条目 41278 tok/1785.87 MiB（43.3 KiB/tok，含检查点与草稿）与 41293 tok/1197.53 MiB（29 KiB/tok）两条并存，体量在旧二进制 4GiB 队列内无压力。
3. 本次实测运行的是 **17:35 旧二进制**（修复包尚未部署）；修复包（含 CRC slice-by-8、24GiB 队列）部署后磁盘恢复与落盘还会再快。
4. bench 期间接受率 0.11-0.18（合成重复文本），生产 0.48：draft 对内容敏感，任何解码 A/B 必须同学接受率下比较 per-round 耗时。

# 2026-11-15 02-34 - Change Record

## 变更概述

依据 D1–D4 四组解码实验（记录见 `experiments/RESULTS.md` 与 `experienceHistory.md` §4.1）调整生产部署参数：投机草稿长度 15→6、草稿模型 CUDA1→CUDA0。同步修正 README 中因此过时的放置/长度描述，以及 30/70、38/62、45/55 切分判据里错误的带宽基数。项目 C++ 源码未改动。

## 修改文件

### D:\AiHelper\llama.cpp\start-localModel-dspine.ps1

- 修改类型：修改
- 修改前路径：D:\AiHelper\llama.cpp\start-localModel-dspine.ps1
- 修改前备份：.backupChange/2026-11-15 02-32/deploy/start-localModel-dspine.ps1
- 修改后路径：D:\AiHelper\llama.cpp\start-localModel-dspine.ps1
- 修改位置：`$Arguments` 数组中投机解码段（`--spec-draft-n-max`、`--spec-draft-device`）及其上方注释
- 具体变更：`--spec-draft-n-max` 由 `15` 改为 `6`；`--spec-draft-device` 由 `CUDA1` 改为 `CUDA0`；原"CUDA1 is selected for the DSpine draft context"注释改为说明草稿移至 CUDA0 以避免与 62% 目标层争带宽
- 修改原因：实测 per-round 耗时 59.9/68.6/80.1 ms → 54.9/60.2/73.3 ms，端到端 6K/33K/90K 解码 39.7/46.9/35.3 → 43.9/48.7/37.3 t/s；同时 CUDA1 占用 20.4 GiB → 18.3 GiB。两项均不在配置签名内，改动后 `pc-index.dat` 未触发重写，3039.23 MiB 既有磁盘条目保留

### README_CN.md

- 修改类型：修改
- 修改前路径：README_CN.md
- 修改前备份：.backupChange/2026-11-15 02-34/README_CN.md
- 修改后路径：README_CN.md
- 修改位置：§2.1 模型与显存放置表（草稿模型放置、草稿长度两行）、§2.1 末段切分判据
- 具体变更：`--spec-draft-device CUDA1` → `CUDA0`；`--spec-draft-n-max 15` → `6`；带宽最优点由"约 35/65"更正为"按 672 GB/s 与 936 GB/s 计算，最优约 42/58"
- 修改原因：与部署脚本保持一致；原 35/65 基于 4070 Ti SUPER 的 504 GB/s，该卡实际为 672 GB/s（256-bit × 21 Gbps）

### README_EN.md

- 修改类型：修改
- 修改前路径：README_EN.md
- 修改前备份：.backupChange/2026-11-15 02-34/README_EN.md
- 修改后路径：README_EN.md
- 修改位置：§2.1 placement 表（Draft placement、Draft length 两行）、§2.1 末段
- 具体变更：与 README_CN.md 同义同步
- 修改原因：中英两份描述必须一致

### experienceHistory.md

- 修改类型：修改
- 修改前路径：experienceHistory.md
- 修改前备份：.backupChange/2026-11-15 02-34/experienceHistory.md
- 修改后路径：experienceHistory.md
- 修改位置：§0 结论速览表、§4.1（新增）、§5 开放项列表
- 具体变更：速览表新增单卡 3090、`--spec-type none`、`n-max 15→6`、草稿卡位、`-ub 2048` 五行结论；新增 §4.1 记录 D1–D4 三档上下文数据、per_round 口径、投机盈亏平衡点与 36 ms 固定开销；§5 移除已完成的 `n-max` 与 `--spec-type none` 对照，补入 `p_min` 与 per_round 开销压缩方向
- 修改原因：避免后续重复试错；原 §5 把"投机净收益未量化"列为开放项，现已量化

## 验证记录

- 已执行检查：`stop-localModel.ps1` + `start-localModel-dspine.ps1` 重启；`curl /health`；`grep n_max= localModel-server-error.log`；`nvidia-smi` 显存对比；`/completion` 探针；磁盘缓存启动日志
- 检查结果：通过。日志出现 `- n_max=6, n_min=0, p_min=0.00`；`/health` 返回 ok；CUDA0 13051→14101 MiB（+草稿 1.05 GiB）、CUDA1 20163→18049 MiB（-2.05 GiB）；6K 上下文探针 50.3 t/s / 19.89 ms/token，`draft_n=266 accepted=81`；启动日志 `1 entries, 3039.23 MiB`，无 `dropping ... different configuration`
- 备份说明：`start-localModel-dspine.ps1` 为改动前复制，md5 与原件一致。三个 `.md` 的备份因先编辑后备份，改为按本记录的替换内容反向还原生成，并用「重放前向替换后 md5 与当前文件一致」校验；还原结果字节数 13109 / 14482 / 12697 与改动前的目录列表完全吻合

# 2026-11-15 03-16 - Change Record

## 变更概述

为评估 opt 树（llama.cpp-opt-opt-main）的 MMSQ / 融合 norm / q4 KV FA 能否提升解码速度，编译该树并做对照实验。该树在 Windows 上无法编译：`server-sleep-cache.h` 无条件包含 POSIX 头文件。本次仅加条件编译守卫使其降级为空实现，不改变任何算法路径。项目自身 C++ 源码未改动；部署脚本未改动。

## 修改文件

### llama.cpp-opt-opt-main/tools/server/server-sleep-cache.h

- 修改类型：修改
- 修改前路径：llama.cpp-opt-opt-main/tools/server/server-sleep-cache.h
- 修改前备份：.backupChange/2026-11-15 02-55/llama.cpp-opt-opt-main/tools/server/server-sleep-cache.h
- 修改后路径：llama.cpp-opt-opt-main/tools/server/server-sleep-cache.h
- 修改位置：文件头部 include 区之后的 namespace 入口，以及文件末尾
- 具体变更：新增 `#if defined(_WIN32)` 分支，提供 `server_sleep_cache::save/restore` 的模板空实现（返回 0）；把 `<fcntl.h> <sys/mman.h> <sys/stat.h> <unistd.h>` 与原有实现移入 `#else` 分支；文件末尾补 `#endif // _WIN32`
- 修改原因：原文件在 MSVC 下报 `fatal error C1083: 无法打开包括文件: "sys/mman.h"`，整棵树编译失败。sleep cache 是可选功能（仅 `LLAMA_SLEEP_CACHE_DIR` 设置时生效），Windows 下降级为空实现不影响其余路径

## 验证记录

- 已执行检查：`build-opt-cuda.bat` 完整编译；N=8 ubatch 口径 A/B；无投机解码三档上下文；greedy 输出一致性对比；恢复生产部署
- 检查结果：通过。编译产出 66 个文件（`output-opt/`）；N=8 矩阵乘 MMSQ 关闭 173.2 t/s、开启 277.9 t/s（1.60×）；N=1 时 MMSQ 慢 2%–5%；生产已恢复为 `n_max=6`，显存 14101/18049 MiB，`/health` ok
- 备注：`llama.cpp-opt-opt-main` 为被参照的独立目录，不在本仓库版本管理内，其 `build-opt/`、`output-opt/` 为编译产物，未清理

# 2026-11-15 03-47 - Change Record

## 变更概述

磁盘缓存写入放大治理：当存量条目已与待写提示共享除末尾一小段外的全部前缀时，跳过本次落盘。目的只有一个——减少机械盘上的重写量。不改变任何计算路径，缓存中存的是与原来完全相同的那份 KV 字节，因此对模型输出无影响。

## 修改文件

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 03-30/tools/server/server-task.cpp
- 修改后路径：tools/server/server-task.cpp
- 修改位置：常量区（`DISK_CACHE_MAX_BYTES_PER_TOKEN` 之后）、`server_disk_cache::covered_cheaply()`（新增，`drop_prefixes` 之前）、`store()` 与 `mirror()` 的 dedup 之后
- 具体变更：新增常量 `DISK_CACHE_MAX_PREFIX_LAG_TOKENS = 4096`；新增 `covered_cheaply(const llama_tokens & ids)`，在 `entries` 中查找满足 `lcp(e.tokens, ids) + 4096 >= ids.size()` 且不含 media 占位符的条目；`store()` 命中时打印 `covered up to a N-token tail ... skipping a X MiB write` 并直接返回，`mirror()` 同理返回 false
- 修改原因：`store()`/`mirror()` 原先只按 hash 精确去重，会话每增长一轮就重写一份全量条目；实测 60K token 条目 1.66–2.74 GiB，在 150 MiB/s 的缓存盘上每次约 4–8 s，而重算被跳过的尾段仅需约 3 s。判据按 lcp 而非严格 token 前缀：追加文本会让切点附近的 token 重新合并，严格前缀判断在实际语料上一次都没命中

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 03-30/tools/server/server-task.h
- 修改后路径：tools/server/server-task.h
- 修改位置：`server_disk_cache` 私有段，`drop_prefixes` 声明之后
- 具体变更：新增 `bool covered_cheaply(const llama_tokens & ids) const;`，注明需持 `mtx`
- 修改原因：配合上述实现

### README_CN.md / README_EN.md

- 修改类型：修改
- 修改前路径：README_CN.md、README_EN.md
- 修改前备份：.backupChange/2026-11-15 03-47/README_CN.md、.backupChange/2026-11-15 03-47/README_EN.md
- 修改后路径：README_CN.md、README_EN.md
- 修改位置：§3.4 失效、淘汰与容量
- 具体变更：各新增一条写入抑制规则、阈值来源与日志标记说明
- 修改原因：该策略属可观测的缓存行为，文档需与实际一致

### experienceHistory.md

- 修改类型：修改
- 修改前路径：experienceHistory.md
- 修改前备份：.backupChange/2026-11-15 03-47/experienceHistory.md
- 修改后路径：experienceHistory.md
- 修改位置：§0 结论速览表
- 具体变更：新增两行——`--n-ctx-checkpoints 32→8` 无收益（实测条目仅 1–3 个检查点）；前缀覆盖跳过重写有效
- 修改原因：此前误判检查点数量是条目体积来源，需留下实测否证

## 验证记录

- 已执行检查：`build-cuda-shared.bat` 增量构建；四步增量写入测试（同一语料逐步加长）；跨重启恢复测试；连续三次同请求测试；部署 MD5 校验；恢复生产
- 检查结果：通过。构建 0 FAILED；写入计数 1→1→1→2，即 54203→54910（+707）与 54910→55905（+1702）被跳过，55905→62193（+7988，超 4096）正常落盘；日志出现 `skipping a 1906.98 MiB mirror` 与 `skipping a 2159.39 MiB mirror`；重启后对 50508 token 请求恢复 50504 token、仅 prefill 4 token，恢复路径未受影响；生产已恢复（`n_max=6`，显存 14101/18049 MiB，`/health` ok）
- 遗留观察：存在两条前缀关系条目时，重启后的首个请求有概率整段重算（疑似启动期 `dedup_prefixes` 与首次查询并发），第二次起正常。与本次改动无关，未处理

# 2026-11-15 11-50 - Change Record

## 变更概述

发布准备：构建产物默认写入 `output/`，据此重整中英 README，补齐 `.gitignore` 使仓库可直接公开。项目源码逻辑未改动；本次仅涉及构建配置、忽略规则与文档。

## 修改文件

### CMakeLists.txt

- 修改类型：修改
- 修改前路径：CMakeLists.txt
- 修改前备份：.backupChange/2026-11-15 11-50/CMakeLists.txt
- 修改后路径：CMakeLists.txt
- 修改位置：`CMAKE_MODULE_PATH` 之后的产物目录设置
- 具体变更：`CMAKE_RUNTIME_OUTPUT_DIRECTORY` 与 `CMAKE_LIBRARY_OUTPUT_DIRECTORY` 由 `${CMAKE_BINARY_DIR}/bin` 改为 `${CMAKE_SOURCE_DIR}/output`
- 修改原因：`output/` 是部署布局，构建后需再拷一次；直接作为产物目录后部署只需整目录覆盖。`ggml/CMakeLists.txt` 仅在变量未设置时才写默认值，因此子目录继承该设置

### build-cuda-shared.bat

- 修改类型：修改
- 修改前路径：build-cuda-shared.bat
- 修改前备份：.backupChange/2026-11-15 11-50/build-cuda-shared.bat
- 修改后路径：build-cuda-shared.bat
- 修改位置：`SRC` 路径推导、末尾的产物铺装段
- 具体变更：`SRC` 改为按脚本自身位置推导（`%~dp0`），不再硬编码绝对路径；删除「清空并整组重铺 output/」的拷贝段；改为仅在缺失时补拷 4 个 CUDA runtime DLL
- 修改原因：产物已直接生成到 `output/`，重铺既多余又会让 ninja 认为输出被删而反复重链；硬编码路径使脚本无法在别处复用

### .gitignore

- 修改类型：修改
- 修改前路径：.gitignore
- 修改前备份：.backupChange/2026-11-15 11-50/.gitignore
- 修改后路径：.gitignore
- 修改位置：文件末尾新增 Deployment output / Local pre-change backups 两段
- 具体变更：新增 `/output/*` 与 `!/output/.gitkeep`、`*.pdb`、`*.ilk`、`*.exp`、`*.lib`、`!/build-cuda-shared.bat`、`/.backupChange/`
- 修改原因：`output/` 需在仓库中保留空目录；原有的 `*.bat` 通用规则会把唯一的构建脚本一起隐藏；`.backupChange/` 是本地改动备份，不应入库

### README_CN.md / README_EN.md

- 修改类型：修改
- 修改前路径：README_CN.md、README_EN.md
- 修改前备份：.backupChange/2026-11-15 11-50/README_CN.md、.backupChange/2026-11-15 11-50/README_EN.md
- 修改后路径：README_CN.md、README_EN.md
- 修改位置：全文重组
- 具体变更：改为「特性 → 环境与依赖 → 构建 → 运行 → 放置与上下文 → 解码性能 → 两级缓存（6.1–6.7）→ 可观测性 → 行为边界 → 实现位置 → 实验记录」；修正特性表中「草稿模型整体置于 CUDA1」为 CUDA0；解码性能表换成本次实测的 43.9/48.7/37.3 t/s 与每轮耗时、接受率；新增 §6.7 条目构成（main/drft/检查点三段体积与 151 MiB 固定开销）；删除「32 个检查点导致条目偏大」的错误表述；运行段改为给出完整参数而非指向仓库外的脚本
- 修改原因：原文档的解码数据来自 n_max=15 的旧配置，草稿卡位描述与实际部署相反，检查点数量结论已被实测否证；README 需与当前实现一致后才可公开

## 验证记录

- 已执行检查：删除 `build-cuda-shared/` 与 `output/` 内容后执行 `build-cuda-shared.bat` 全量构建；统计产物数量；`output/llama-server.exe --version`；`git check-ignore` 校验忽略规则；核对 README 引用的 9 个路径
- 检查结果：通过。0 个 FAILED，`output/` 直接得到 47 个 `.exe` 与 19 个 `.dll`，`--version` 输出 `0.9.0-dev (build 2, commit 60f3891)`；`/backupChange` 与 `build-cuda-shared/` 被正确忽略，`build-cuda-shared.bat`、`output/.gitkeep`、`experiments/bench_decode.py` 均可入库；README 引用的路径全部存在
- 备注：首次在旧构建目录上增量重链时出现 `LNK2001 __std_regex_transform_primary_char` 等 STL 符号缺失，删除整个构建目录全量重编后消失，属跨工具链的陈旧对象，非源码问题

# 2026-11-15 12-57 - Change Record

## 变更概述

GitHub 仓库首页不渲染任何 README：仓库中只有 `README_CN.md` 与 `README_EN.md`，而首页只识别精确匹配的 `README.md / README.rst / README.txt / README`（大小写不敏感），带下划线后缀的 `README_CN.md` 不在此列。将中文版重命名为 `README.md` 作为首页，英文版保留，两处交叉链接同步更新。

## 修改文件

### README_CN.md → README.md

- 修改类型：重命名（内容未改）
- 修改前路径：README_CN.md
- 修改前备份：.backupChange/2026-11-15 12-57/README_CN.md
- 修改后路径：README.md
- 修改原因：GitHub 仅把精确命名的 README 渲染为仓库首页
- 备注：本文档历史条目中的 `README_CN.md` 是对当时文件的记录，未回改

### README_EN.md

- 修改类型：修改
- 修改前路径：README_EN.md
- 修改前备份：.backupChange/2026-11-15 12-57/README_EN.md
- 修改后路径：README_EN.md
- 修改位置：文件首部语言切换行
- 具体变更：`[简体中文](README_CN.md)` → `[简体中文](README.md)`
- 修改原因：中文版已改名，原链接会 404

### experienceHistory.md

- 修改类型：修改
- 修改前路径：experienceHistory.md
- 修改前备份：.backupChange/2026-11-15 12-57/experienceHistory.md
- 修改后路径：experienceHistory.md
- 修改位置：文首说明段
- 具体变更：`README_CN.md` → `README.md`
- 修改原因：同上

## 验证记录

- 已执行检查：`git ls-files | grep -i readme`；全仓检索 `README_CN` 残留引用；`git mv` 后确认重命名被记录为 R 而非删除+新增
- 检查结果：通过。仓库现有 `README.md` 与 `README_EN.md` 两个文件；除 `change.md` 的历史条目外无残留引用
