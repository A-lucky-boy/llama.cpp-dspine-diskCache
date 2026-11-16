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

# 2026-11-15 18-18 - Change Record

## 变更概述

按 `betterModification.md` 的阶段 0 实施：定位并修复「重启后首个请求偶发全量重算」的真实根因，把 token/media 元数据的校验纳入写路径，并补齐启动与读盘失败路径的可观测性。不涉及任何新缓存格式（分块/增量持久化）、不改变模型计算、不改变查找策略与 slot 生命周期语义。

**已定位的根因（问题 A）**：`server_disk_cache::touch_entry()` 计算 `last_used` 写入偏移时使用 `DISK_CACHE_HEADER_FIXED - 8 + sig_len + packed_size`。v7 头部固定部分为 72 字节，`last_used` 实际位于 `48 + sig + packed`，而该公式得到 `64 + sig + packed`，正好落在 **`crc_drft`** 上（`crc_main` 在 56，`crc_drft` 在 64）。因此每次优雅退出时，凡是被命中过（`last_used_dirty`）的条目都会把 8 字节的 unix 时间戳写进草稿状态的 CRC 字段。下次启动的首个匹配请求在 `read_entry()` 里做 `disk_cache_crc64(drft) != crc_drft` 判定必然失败，日志打 `failed to read ... dropping it`，条目被删除并整段重新 prefill；该请求结束又会把状态重新写入 RAM 缓存，于是「第二个请求正常」。这也解释了为何该现象只在重启后出现、且与被用过的条目相关。

**未完全定位的部分（如实记录）**：部署日志 `D:\AiHelper\llama.cpp\localModel-server-error.log` 中还出现两类不同签名：

- 第 4046 行：`load: disk cache: failed to read 'pc-ff2770755480dcb4.bin', dropping it`，其前无 `read_entry` 告警，说明命中的是当时无日志输出的静默失败分支（头部/长度/截断类）。
- 第 4129 行：`read_entry: disk cache: checkpoint checksum mismatch in 'pc-ff2770755480dcb4.bin'`，即草稿状态 CRC 正常但某个检查点 CRC 不符。

两者都不由上述偏移错误解释（偏移错误必然先触发 payload CRC 分支）。可能来源是同一版本号下不同构建产物的检查点表布局差异，或写入侧与读侧对检查点表理解不一致；现有日志无法定论，故本次不做猜测性修复，只把每条失败路径改为输出具体原因（见下），使下一次复现即可定位。当前 `E:\llama.cpp.cache` 中 26 个条目的头部 CRC 字段均为正常 64 位值、与 `pc-index.dat` 逐字段一致（见验证记录），说明损坏条目已被删除而不在目录中残留。

**规划要点对照**：阶段 0 的「问题 B（packed 元数据写侧校验）」已实施：v8 头部新增 `crc_packed`，写侧计算、读侧在**任何** `llama_state_seq_set_data_ext` 之前校验；清单升级 v3，记录 packed 的 CRC 并对整块记录加校验和；旧版本条目显式丢弃并说明原因。规划中阶段 1 及以后（统计、边界捕获、公共前缀识别、分块 KV、成本感知查找、热点淘汰）本次**未实施**，因为它们要求阶段 0 结论先行确认，且需要新缓存格式。

## 修改文件

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 18-11/tools/server/server-task.cpp
- 修改后路径：tools/server/server-task.cpp
- 修改位置：`server_disk_cache` 条目文件布局注释、常量区、`disk_cache_crc64` 重载、构造函数、`scan()`、`load_manifest()`、`save_manifest()`、`write_entry()`、`read_entry()`、`touch_entry()`、`load()`、`worker()`
- 具体变更：
  - 布局注释补全 `crc_main / crc_drft / crc_packed` 三行与检查点表的三个 CRC 字段（原注释漏写前两项、检查点表缺 crc 字段）。
  - `DISK_CACHE_VERSION` 7 → 8；新增 `DISK_CACHE_HDR_PREFIX / HDR_BEFORE_LAST / HDR_AFTER_LAST / HDR_LAST_USED_OFFSET` 与 `DISK_CACHE_HEADER_FIXED`（80），并加两条 `static_assert`（固定头 80 字节、`last_used` 相对偏移 48）。
  - `DISK_CACHE_INDEX_VERSION` 2 → 3，新增常量 `DISK_CACHE_MAX_LOAD_ATTEMPTS = 4`。
  - 新增 `disk_cache_crc64(const std::vector<char> &)` 重载：packed 元数据以 char 容器存放，原模板只匹配 `value_type == uint8_t`，直接调用无法编译。
  - `touch_entry()`：偏移改为 `DISK_CACHE_HDR_LAST_USED_OFFSET + sig_len + packed_size`，即根因修复。
  - `write_entry()`：写头部前计算 `crc_packed`，写在 `crc_drft` 之后。
  - `read_entry()`：签名改为 `disk_cache_read_status read_entry(const server_disk_cache_entry &, size_t lcp, server_disk_cache_payload &)`，按 `ok / corrupt / io_error` 三态返回；校验顺序改为「头部 → packed 存储 CRC → 与索引 CRC 绑定 → 解包 → 与索引 token/media 逐项比对 → 检查点表 → 载荷 CRC」，全部在返回前完成，不再在 ctx 已被改写之后才发现元数据损坏；每条失败分支单独打印原因（原来是静默 `return false`）。
  - `load()`：候选选择抽为 `pick()` lambda（持锁内完成选择，读数仍不持锁）；新增最多 4 次候选回退循环——某条目读盘失败时丢弃（corrupt）或保留（io_error）并换下一个候选，而不是直接整段 prefill；media 元数据一致性检查与「有无草稿上下文」判断前移到任何 `set_data_ext` 之前，失败时走干净回退（`return true`），只有真正写坏过 KV 才 `return false`。
  - `scan()`：新增 `cache_scan_begin` / `cache_scan_manifest` / `cache_scan_end` 事件与计数（manifest 命中/回退、按头解析、陈旧索引记录、丢弃、临时文件清理）；版本不符时打印 `entry format v7 is older than v8, the cache will be rebuilt` 而非笼统 `bad header`；按头解析时同时校验 packed CRC，坏条目在启动阶段即被丢弃。
  - `load_manifest()`：改为 magic/version/sig → `body_size` → `body` → `body_crc64` 的结构，整块记录校验失败即回退全量扫描并说明原因；每条记录新增 `packed_crc`。
  - `save_manifest()`：先序列化记录块再计算并写入 CRC，避免先写头再写记录时出现计数与实际记录不一致；记录新增 `packed_crc`。
  - 构造函数：去重/配额修剪后打印 `cache_index_ready`（扫描后条目数、去重后条目数、占用），把初始化完成屏障显式化。
  - `worker()`：新建条目的 `packed_crc` 与写入文件头的值一致，保证索引与文件绑定。
- 修改原因：`betterModification.md` §2.1（问题 A 定位与修复）、§2.2（packed 与 KV 配对完整性、版本化丢弃策略、失败返回契约）、§2.3（阶段 0 验证清单所需的启动/查找/恢复日志）。

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 18-11/tools/server/server-task.h
- 修改后路径：tools/server/server-task.h
- 修改位置：`server_disk_cache_entry`、`disk_cache_manifest_meta`、`server_disk_cache` 私有段
- 具体变更：`server_disk_cache_entry` 新增 `uint64_t packed_crc`；新增 `enum class disk_cache_read_status { ok, corrupt, io_error }` 与 `struct server_disk_cache_payload`（packed、tokens、media 三元组、main/drft、checkpoints）；`disk_cache_manifest_meta` 新增 `packed_crc`；`read_entry` 声明改为返回三态并填充 payload。
- 修改原因：承载「先校验后恢复」的中间结果，避免校验失败时已改写 ctx；区分确定性损坏与瞬时读失败。

### README.md / README_EN.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md
- 修改前备份：无（改动仅文档，回退以 git 历史为准）
- 修改后路径：README.md、README_EN.md
- 修改位置：§6.2 条目格式与完整性、§6.3 命中与恢复
- 具体变更：条目格式版本 7 → 8、清单版本 2 → 3；校验和一行补 packed 元数据；新增「格式版本是硬边界」段落说明 v7 会被显式丢弃重建、清单 packed CRC 与条目头绑定；§6.3 补「先校验后恢复」顺序、候选回退、失败分层（确定性损坏删除 / 瞬时失败保留）。
- 修改原因：文档必须与实际二进制行为一致。

### change.md

- 修改类型：修改（追加）
- 修改前路径：change.md
- 修改前备份：无（按规范只追加、不改写历史）
- 修改后路径：change.md
- 修改位置：文末
- 具体变更：追加本记录。
- 修改原因：记录已实施变更。

### experienceHistory.md

- 修改类型：修改
- 修改前路径：experienceHistory.md
- 修改前备份：.backupChange/2026-11-15 18-18/experienceHistory.md
- 修改后路径：experienceHistory.md
- 修改位置：§0 结论速览表、§2 新增 2.6 小节
- 具体变更：新增两行结论（`touch_entry` 偏移错误已修复；重启首请求读盘失败已在部署日志中实证）；新增 2.6 记录该 bug 的证明方法（公式推导 + 独立复现程序 + 部署日志）与「读盘失败 ≠ 缓存不存在」的排查要点。
- 修改原因：避免后续再把它当成「并发竞态」重复排查。

## 验证记录

- 已执行检查：单 TU 编译 `ninja tools/server/CMakeFiles/server-context.dir/server-task.cpp.obj`（MSVC 19.44.35226，/W3）；全量 CUDA 构建（sm_86-real、GGML_CUDA=ON、BUILD_SHARED_LIBS=ON、LLAMA_CURL=OFF）；`output/llama-server.exe --version`
- 检查结果：通过。单 TU 编译 1/1 成功、无告警；全量构建 436/436、`0 FAILED`，产出 `output/llama-server.exe` 与 `output/llama-server-impl.dll`；`--version` 输出 `0.9.0-dev (build 7, commit eb76818)`

- 已执行检查：独立布局复现程序（`.tmp_layout_check.cpp`，模拟 v7/v8 头部写入 + 两种 `touch_entry` 偏移）
- 检查结果：通过，复现并证明根因。v7 头（固定 72）：`last_used` 实际偏移 178、`crc_main` 186、`crc_drft` 194，旧公式得 194 → 命中 `crc_drft`；写入后实测 `last_used` 未变、`crc_main` 完好、`crc_drft` 由 `0xd00dfeedd00dfeed` 变为 `0x00000000671db480`（时间戳）。新公式得 178 → 命中 `last_used`，两个 CRC 均不变。v8 头（固定 80）下旧公式还会命中新增的 `crc_packed`

- 已执行检查：解析 `E:\llama.cpp.cache` 现有 26 个条目头部（magic/version/sig/packed/size_main/size_drft/CRC）并与 `pc-index.dat`（v2，26 条记录）逐字段比对
- 检查结果：通过。26/26 条目 `size_file / size_main / size_drft / packed` 与索引记录完全一致，0 条陈旧记录；所有条目的 `crc_main / crc_drft` 均为正常 64 位值，无被时间戳覆盖的痕迹——与「被破坏的条目在首次读取时即被删除、不会留在目录中」一致

- 已执行检查：检索部署日志 `localModel-server-error.log`（42 次启动）中的磁盘缓存事件
- 检查结果：症状已实证。第 4042 行启动 `1 entries, 841.61 MiB`，第 4046 行首个请求 `failed to read ... dropping it`，随后 `prompt processing, n_tokens = 5060, t = 3.17 s`（整段 prefill）；同一条目在第 4129 行另一次运行中报 `checkpoint checksum mismatch`。两类签名不同，后者不由偏移错误解释，已作为未定位项记录

- 已执行检查：`git status` 确认变更范围；`.tmp_*` 临时文件与 `build-check/` 构建目录清理
- 检查结果：通过。仅 `tools/server/server-task.cpp`、`tools/server/server-task.h`、`README.md`、`README_EN.md`、`change.md`、`experienceHistory.md` 被修改；`output/` 被 .gitignore 忽略

- 未执行的检查及原因：
  - 端到端模型验证（完全重复 / 分叉前缀 / 短前缀 / 长前缀 / 多媒体 / 重启首请求 / 单比特破坏 / 断电 `.tmp` 残留）需要真实模型与 GPU：生产实例正在 `D:\AiHelper\llama.cpp` 运行（占 14101/18049 MiB 显存），本机加载第二份 27B 模型有 OOM 风险，且未经授权不应改动生产部署。
  - 跨重启 `-lv 4` 观察 `restored N/M tokens`：同上。
  - Linux/POSIX 构建：本机为 Windows/MSVC 环境，未执行；本次改动未引入平台相关 API（仅 `std::filesystem` / `std::fstream`，与既有代码一致）。
  - 上游 baseline 逐行 diff：仓库未配置上游 remote，`0.9.0-dev commit 60f3891` 无法在本环境解析为完整 SHA，因此未做也无法声称完成逐行源码 diff；本次仅与本仓库 HEAD `eb768189f44304d2bd5b3e41572f5530af91a1d` 对比。

# 2026-11-15 18-35 - Change Record（运行时验证，无代码变更）

## 变更概述

对 `2026-11-15 18-18` 那条记录实施的改动做端到端实测验证，测试语料为 `E:\小说\正文.md`（155535 字中文散文，无重复段落，避免合成文本把命中率抬高）。本次**不改任何源码**，只追加验证事实，并更正上一条记录中「端到端模型验证未执行」的表述——该表述在当时是准确的（改动刚完成、生产实例状态未知），现在已实际执行。

测试环境：`output/llama-server.exe`（本次构建，v8 格式）、Qwen3.8-27B-pi-Q4_K_M + DSpine 草稿模型、`-c 16384`、`-ctk/-ctv q4_0`、`-np 1 --kv-unified`、独立缓存目录、端口 18090。测试脚本与日志保留在 `test-run/`（脚本 6 个、日志 6 份，共约 488 KB；8 GB 缓存副本已删除）。

## 修改文件

### test-run/（新增，未入库）

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：test-run/start_test_server.bat、test-run/run_case.bat、test-run/req.py、test-run/prep_cases.py、test-run/drive_case.sh、test-run/drive_two.sh、test-run/logs/*.log
- 修改位置：不适用
- 具体变更：测试实例启动脚本、请求脚本、破坏场景构造脚本、逐场景驱动脚本与运行日志。`test-run/` 未被 .gitignore 覆盖，也未提交。
- 修改原因：使阶段 0 的验证可复现。

## 验证记录

- 已执行检查：冷启动 → 首次请求 → 空闲镜像落盘 → 强杀 → 重启 → 同请求（`logs/run1.log`、`logs/run2.log`）
- 检查结果：通过。冷请求 `prompt_n=8243, cache_n=0`（4.71 s prefill）；落盘 `stored 8246 tokens, 839.64 MiB`；强杀后重启扫描 `manifest hit, 1 record(s)`、`cache_index_ready: 1 entries`；同一请求 `prompt_n=4, cache_n=8239`，`read_entry: read 614.99 MiB of 839.64 MiB, checkpoints 1/2`。即**重启后首个请求恢复 8239 token、只 prefill 4 token**

- 已执行检查：把时间戳写进真实条目头部三处偏移，分别用当前二进制读取（`logs/case-*.log`）
- 检查结果：通过，且与诊断完全一致：
  - 写在 `48+sig+packed`（修复后的 `touch_entry` 偏移，`case-correct`）：`cache_n=8239`、`prompt_n=4`，恢复成功
  - 写在 `64+sig+packed`（v7 下旧公式命中的 `crc_drft`，`case-v7off`）：`read_entry: payload checksum mismatch ... dropping it` → `dropping the corrupt entry` → 再次查找 `0 entries` → `prompt_n=8243, cache_n=0`，**整段重算 4.69 s**。这就是生产日志第 4046 行的现象，复现条件确认
  - 写在 `72+sig+packed`（v8 下旧公式会命中的 `crc_packed`，`case-v8off`）：`prompt metadata checksum mismatch` → 丢弃 → 整段重算

- 已执行检查：单比特破坏（`logs/case-packedflip.log`、`logs/case-mainflip.log`）
- 检查结果：通过。packed 中部翻一位 → `prompt metadata checksum mismatch in ... dropping it`；main 载荷中部翻一位 → `payload checksum mismatch in ... dropping it`。两种都先丢弃再全量 prefill，没有出现错误复用或错误输出

- 已执行检查：清单损坏（`logs/case-indexcorrupt.log`）
- 检查结果：通过。`index checksum mismatch, falling back to a full scan` → `cache_scan_manifest: miss` → `cache_scan_end: 1 entries (header 1)` → 恢复 `cache_n=8239`。清单损坏不阻断命中，只是退化为逐文件头校验

- 已执行检查：v7 旧格式文件 + 断电残留 `.tmp`（`logs/case-v7file.log`）
- 检查结果：通过。`dropping 'pc-0000000000000001.bin': entry format v7 is older than v8, the cache will be rebuilt`；`cache_scan_end: ... 1 dropped, 1 temp removed`。旧格式被显式丢弃并说明原因，不当作损坏、也不按新格式解析

- 已执行检查：两条前缀关系条目 + 重启后请求短前缀（`logs/two-run1.log`、`logs/two-run2.log`）
- 检查结果：通过。run1 中短请求冷 prefill `prompt_n=2844`，长请求恢复 `cache_n=2840` 后补 5403；两条都落盘（短条目 2847 token / 577.09 MiB、长条目 8246 token / 1187.31 MiB）。短条目不是长条目的严格 token 前缀（`lcp=2843 < 2847`，追加文本在切点处重新合并），故 `drop_prefixes`/`dedup_prefixes` 不去重，这符合设计。重启后短请求：`2 entries` 中选中 `lcp=2844` 的短条目，`cache_n=2840, prompt_n=4`，**重启后首个请求未重算**

- 已执行检查：`git status`；临时文件清理
- 检查结果：通过。源码变更仍是 `tools/server/server-task.cpp`、`tools/server/server-task.h`；文档变更 `README.md`、`README_EN.md`、`change.md`、`experienceHistory.md`；`build-check/`（488 MB）与全部 `.tmp_*` 已删除；`test-run/` 内 8 GB 缓存副本已删除，仅留脚本与日志

- 未执行的检查及原因：
  - `input`/`output` 边界捕获、统计口径、公共前缀识别、分块 KV、成本感知查找：对应规划阶段 1–5，本次按规划顺序**未实施**，无可验证对象。
  - 候选回退到「另一个条目」的分支：`load()` 的多次尝试逻辑被执行（丢弃后重新 `lookup ... 0 entries`），但未构造出「首个候选损坏、第二个候选可用」的用例；该场景在阶段 1 统计落地后应补测。
  - Linux/POSIX 构建与多 slot 并发：本机为 Windows 单实例，未执行；改动未引入平台相关 API。

### 补充（触发条件与上一条记录的边界）

本记录不修改上一条记录的任何内容，只补充两点事实边界：

1. `touch_entry()` 只在 `~server_disk_cache()` 里调用，因此上一条记录描述的崩溃链条**只在前一轮优雅退出时成立**。部署脚本 `stop-localModel.ps1` 使用 `Stop-Process -Force`（等价 TerminateProcess），析构函数不执行，`touch_entry()` 与收尾的 `save_manifest()` 都不会跑；用这种方式重启不触发该 bug。本次实测确认：强杀前后 `pc-index.dat` 的 mtime 不变。
2. 因此，上一条记录中「这也解释了为何该现象只在重启后出现」应理解为「解释了优雅退出后重启首请求重算」这一条路径。生产日志里的其余两类签名（`checkpoint checksum mismatch`、无告警的静默读失败）仍未定位，结论保持为未定位，不做推断性归因。

该 bug 的修复与实测结论不因此改变：修复后的偏移经真实条目验证可正常恢复（`case-correct`，`cache_n=8239`），而按旧公式写入同一字段会导致 `payload checksum mismatch` 并整段重算（`case-v7off`，`cache_n=0`）。

# 2026-11-15 18-49 - Change Record（阶段 1 + 阶段 2）

## 变更概述

按 `betterModification.md` 的实施顺序（阶段 0 已完成并记录于 `2026-11-15 18-18` / `18-35`），本次实施 **阶段 1（精确命中统计与事件可观测性）** 与 **阶段 2（请求边界捕获）**。两阶段均为新增开关、默认关闭；关闭时全部走原有路径，缓存文件格式、查找策略、写入抑制、slot 生命周期与模型计算均未改变。

阶段 1 把「没有命中」拆成可区分的事实：请求是否查询过磁盘层、候选前缀多长、被哪一道闸门拒绝、恢复后有多少 token 真正免掉 prefill。关键点是**以真实免除的 prefill 为准**：`candidate_lcp`（候选共享前缀）、`restored`（条目恢复长度）与 `reused_*`（`n_past` 最终认可）三者分开统计，实测分别为 1385 / 1385 / 1381，差值来自检查点钳制与「末位 token 必须重算」守卫。

阶段 2 在「RAM 淘汰落盘」与「空闲镜像」之外增加两个请求边界事件：`input`（最后一个输入 token 的 KV 算完、即将采样）与 `output`（生成正常结束）。捕获只覆盖内存实际持有位置（以 `llama_memory_seq_pos_max` 为上界），因此条目不会声称一段没算过的前缀。

## 修改文件

### common/common.h

- 修改类型：修改
- 修改前路径：common/common.h
- 修改前备份：.backupChange/2026-11-15 18-32/common/common.h
- 修改后路径：common/common.h
- 修改位置：`common_cache_boundary` 枚举（新增，置于 `common_params` 前）、`common_params` 的 server 参数段
- 具体变更：新增 `enum common_cache_boundary { NONE, INPUT = 1<<0, OUTPUT = 1<<1 }`；新增 `bool cache_stats`（默认 false）与 `int32_t cache_boundary_capture`（默认 `COMMON_CACHE_BOUNDARY_NONE`）
- 修改原因：承载阶段 1 的统计开关与阶段 2 的边界选择

### common/arg.cpp

- 修改类型：修改
- 修改前路径：common/arg.cpp
- 修改前备份：.backupChange/2026-11-15 18-32/common/arg.cpp
- 修改后路径：common/arg.cpp
- 修改位置：`--cache-disk-queue-max` 之后
- 具体变更：新增 `--cache-stats / --no-cache-stats`（env `LLAMA_ARG_CACHE_STATS`）与 `--cache-boundary-capture {none,input,output,both}`（env `LLAMA_ARG_CACHE_BOUNDARY_CAPTURE`）；非法取值抛 `invalid_argument`
- 修改原因：两个功能都必须显式开启

### tools/server/server-common.h

- 修改类型：修改
- 修改前路径：tools/server/server-common.h
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/server-common.h
- 修改后路径：tools/server/server-common.h
- 修改位置：`server_metrics`
- 具体变更：新增 `n_cache_req_prompt`、`n_cache_req_prompt_disk_ok`、`n_cache_reused_slot/ram/disk`、`n_cache_prefill`
- 修改原因：请求级 token 来源分解的累计计数器（单线程主循环更新，可随 metrics 任务直接复制）

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/server-task.h
- 修改后路径：tools/server/server-task.h
- 修改位置：`/metrics` 结果结构之前新增三个类型；`server_disk_cache`、`server_prompt_cache` 声明
- 具体变更：新增 `enum class cache_reject_reason`（`none / no_prefix / too_short / media_mismatch / io_cost / checksum_error / read_error / restore_failed / signature_mismatch`）与 `cache_reject_reason_name()`；新增 `prompt_cache_restore_info`（来源层、`entry_tokens`、`candidate_lcp`、`lcp`、`read_bytes`、`elapsed_us`、拒绝原因）与 `disk_cache_counters`；`server_task_result_metrics` 增加 `disk_cache_counters cache_disk`；`server_disk_cache::load` 增加 `prompt_cache_restore_info &` 形参；新增 `server_disk_cache::stats()` 与成员 `counters`；`server_prompt_cache::load` 增加同一形参；新增 `server_prompt_cache::mirror_latest()`
- 修改原因：统计口径需要值对象承载；三个类型必须位于 `server_task_result_metrics` 之前才能作为其成员

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/server-task.cpp
- 修改后路径：tools/server/server-task.cpp
- 修改位置：`cache_reject_reason_name()`（新增，匿名命名空间之前）、`scan()`、`server_disk_cache::stats()`（新增）、`load()`、`store()`、`mirror()`、`worker()`、`server_prompt_cache::load()`、`mirror_latest()`（新增）、`server_task_result_metrics::to_metrics()`
- 具体变更：
  - `scan()`：清单不可信时 `counters.n_manifest_fallback++`
  - `load()`：新增 `info` 出参与 `note_reject()`；逐候选记录「最佳 lcp + 该候选被拒原因」，媒体钳制单独标记，`lcp < min_tokens` 时按是否被媒体钳制区分 `media_mismatch` / `no_prefix`；`io_cost`、`checksum_error`、`read_error`、`restore_failed`、`signature_mismatch` 各有独立计数；成功恢复时填 `src / lcp / entry_tokens / read_bytes / elapsed_us` 并累加 `n_read_bytes`、`n_restore_us`
  - `store()` / `mirror()`：入队结果分三态计数（`queued` / `dedup` / `rejected`），判定顺序不变
  - `worker()`：写入成功累加 `n_write_bytes`
  - `stats()`：加锁采样 `entries`、`queue` 大小与队列字节
  - `server_prompt_cache::load()`：RAM 恢复成功时填 `src=ram`、`entry_tokens`、`candidate_lcp=lcp`、`elapsed_us`；恢复失败填 `restore_failed`；未命中转交磁盘层时透传 `info`
  - `mirror_latest()`：对刚插入 RAM 的条目调用 `disk->mirror()`，去重/限额/队列预算全部沿用
  - `to_metrics()`：新增 15 个 `llamacpp:cache_*` counter 与 3 个 gauge，并按 reason 输出 labeled counter（跳过 `none`）
- 修改原因：阶段 1 的分解统计与观测口径；阶段 2 的落盘入口

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/server-context.cpp
- 修改后路径：tools/server/server-context.cpp
- 修改位置：`server_slot`（新增 `cache_restore`、`boundary_captured` 与 `prompt_capture()`）、`prompt_load()`、`get_available_slot()`、`capture_boundary()`（新增）、`add_cache_stats()`（新增）、`SLOT_STATE_DONE_PROMPT → GENERATING` 分支、两处 `process_token()` 停止分支、`SERVER_TASK_TYPE_METRICS` 分支
- 具体变更：
  - `prompt_cache.load()` 调用传入 `cache_restore`；`get_available_slot()` 在分配任务前把 `cache_restore` 与 `boundary_captured` 归零，避免跨请求串味
  - `add_cache_stats()`：在 `n_past` 定案处按 `cache_restore.src` 把复用 token 计入 slot/ram/disk 之一，差额计入 prefill；`src == disk` 或 `reject != none` 时把该请求计入磁盘命中率分母；`--cache-stats` 开启时输出一行 `cache_stats:`
  - `prompt_capture()`：与 `prompt_save()` 同构，但接受给定的（已截断的）快照；`to_disk` 时以与空闲镜像相同的预算（RAM 上限的 1/4，1.5–6 GiB）调用 `mirror_latest()`
  - `capture_boundary()`：按 `--cache-boundary-capture` 位掩码、任务类型（COMPLETION）、`cache_prompt`、`is_stateless()`、LoRA 守卫过滤；用 `llama_memory_seq_pos_max` 求实际覆盖的 token 数并截断快照，丢弃超出前缀的检查点；`input` 只进 RAM，`output` 才排队写盘
  - 三处调用点：`DONE_PROMPT` 转 `GENERATING` 前（input）、两处 `!process_token()` 停止分支（output）
  - metrics 任务：`res->cache_disk = prompt_cache->disk->stats()`
- 修改原因：阶段 1 的请求级归属与阶段 2 的边界捕获

### README.md / README_EN.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md
- 修改前备份：.backupChange/2026-11-15 18-32/README.md、README_EN.md
- 修改后路径：README.md、README_EN.md
- 修改位置：§6.6 参数表、新增 §6.7「请求边界捕获」（原 6.7 顺延为 6.8）、§7 日志表、新增 §7.1 指标表、§8 行为边界
- 具体变更：补两个新参数；说明边界捕获的触发点、写入目标、截断规则、input 为何不落盘与代价；列出全部 `llamacpp:cache_*` 指标、三条口径（candidate / restored / reused）不可混用及恒等式；把「packed 元数据没有独立 CRC」这一过时表述更正为 v8 的事实
- 修改原因：文档必须与实际二进制行为一致

### change.md

- 修改类型：修改（追加）
- 修改前路径：change.md
- 修改前备份：无（按规范只追加）
- 修改后路径：change.md
- 修改位置：文末
- 具体变更：追加本记录
- 修改原因：记录已实施变更

## 验证记录

- 已执行检查：全量 CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real，GGML_CUDA=ON，BUILD_SHARED_LIBS=ON），1 次全量 + 2 次增量
- 检查结果：通过，`0 FAILED`；`output/llama-server.exe --version` 输出 `0.9.0-dev (build 7, commit eb76818)`；`--help` 可见两个新参数

- 已执行检查：双卡（`-dev CUDA0,CUDA1 -sm layer -ts 38,62`）冷请求 → 重复请求（`test-run/logs/stats-run1.log`，测试语料 `E:\小说\正文.md`，`--cache-stats`，边界捕获取默认 none）
- 检查结果：通过。冷请求 `cache_stats: prompt = 1385, reused_slot = 0, prefill = 1385, source = slot, reject = no_prefix`；重复请求 `reused_slot = 1381, prefill = 4`；空闲镜像随即 `stored 1392 tokens, 505.97 MiB`。两例恒等式 `prompt = reused + prefill` 成立

- 已执行检查：强杀 → 重启同目录 → 同一请求（`test-run/logs/stats-run2.log`）
- 检查结果：通过。`cache_stats: prompt = 1385, reused_disk = 1381, prefill = 4, source = disk, candidate_lcp = 1385, restored = 1385, entry_tokens = 1392, read_bytes = 530543650, restore_us = 436478`；`/metrics`：`cache_request_prompt_tokens_total 2770 = reused_slot 1381 + reused_disk 1381 + prefill 8`

- 已执行检查：首次 `/metrics` 抓取（`--cache-stats --metrics`）
- 检查结果：**发现问题并修复**。首版 `cache_disk_bytes_read_total` 恒为 0——`load()` 只填了 `info.read_bytes`，漏加 `counters.n_read_bytes`；同时 `n_lookup_prompt_tokens`、`n_reused_tokens` 为无出口的死字段，`cache_request_prompt_tokens_disk_ok_total` 的分母错误地只统计「磁盘命中」的请求。修正为：读字节入计数器、删除两个死字段、分母改为「查询过磁盘层（命中或被拒）」的请求。修复后实测 `cache_disk_bytes_read_total = 5.30544e+08`、`cache_request_prompt_tokens_disk_ok_total = 1596 = prompt`，与日志一致

- 已执行检查：`--cache-boundary-capture both` 冷请求（`test-run/logs/boundary-run2.log`）
- 检查结果：通过。`boundary = input, captured = yes, tokens = 1385, pos_max = 1384`（仅入 RAM，无写盘日志）→ `boundary = output, captured = yes, tokens = 1392, pos_max = 1391` → `stored 1392 tokens, 505.97 MiB`

- 已执行检查：追加请求 `corpus[:2000] → corpus[:2300]`（同一日志）
- 检查结果：通过。`reused_slot = 1381, prefill = 215`；input 捕获 1596、output 捕获 1603；output 的镜像被既有写入抑制拦下：`1603 tokens are covered up to a 4096-token tail by a stored entry, skipping a 678.05 MiB mirror`，说明边界捕获没有绕过 `covered_cheaply()`

- 已执行检查：强杀 → 重启 → 请求 `corpus[:2300]`（`test-run/logs/boundary-run3.log`）
- 检查结果：通过。`reused_disk = 1381, prefill = 215, candidate_lcp = 1385, restore_us = 365928`；`/metrics`：`cache_disk_bytes_read_total 5.30544e+08`、`cache_disk_restore_seconds_total 0.365928`、`cache_disk_lookups_total 1`、全部 `cache_disk_reject_total` 为 0。1385 → 1381 的差值正是检查点钳制，验证了不能用 `candidate_lcp` 当命中量

- 已执行检查：数值一致性——恢复谱系 vs 冷启动（`test-run/logs/out-warm_restore.txt`、`test-run/logs/out-cold_ref.txt`）
- 检查结果：通过。同一 `corpus[:2300]`、`n_predict = 32`、temperature 0：走过「磁盘恢复 + 边界捕获」谱系的输出与全新缓存冷启动的输出**逐字节相同**（sha256 前 16 位均为 `8e0f7474b632c91f`）

- 已执行检查：`--cache-boundary-capture none` 回归（`test-run/logs/cold-run.log`）
- 检查结果：通过。`grep -c "boundary ="` 为 0，行为与原路径一致；冷请求 `prefill = 1596`、`reject = no_prefix`、`cache_disk_manifest_fallback_total 1`（首次启动无清单）

- 已执行检查：`git status`；临时产物清理
- 检查结果：通过。`test-run/` 下三个 500 MiB 级缓存目录与 `build-cuda-shared/build*.log` 已删除，仅保留脚本与 500 KB 级日志；无 `.tmp_*` 残留

- 未执行的检查及原因：
  - 阶段 3–7（公共前缀目录、分块 KV、成本感知查找、热点淘汰、回归套件）：按 `betterModification.md` 的顺序尚未进入，无验证对象；阶段 4 要求的「状态可分块性」技术审查也仍未做
  - 中断（超时/取消）路径下 input 边界保留行为：本机测试均为短请求，未构造超时中断用例
  - 多 slot 并发（`-np > 1`）：生产与测试均为 `-np 1`，未验证
  - Linux/POSIX 构建：本机为 Windows/MSVC，未执行；本次改动未引入平台相关 API

# 2026-11-15 19-35 - Change Record（阶段 3：公共前缀目录）

## 变更概述

按 `betterModification.md` 的 `test1_2` 实施阶段 3：新增**单用户、只读、常驻内存**的公共前缀目录（`server-prefix-index`），并为它接上观察事件、与既有 v8 磁盘快照的物理引用关联/失效，以及独立指标。本次**不改变**磁盘条目格式、清单格式、查找策略、恢复顺序、写入抑制与模型计算；不引入任何区间 KV 切片。

目录把三个概念分开：`observed`（观察到共享前缀）、`eligible`（长度/频次达标，值得为它捕获状态）、`persisted`（存在**精确对齐**且校验通过的磁盘快照）。三者**都不计入** `reused_slot/ram/disk`；真实复用仍只由 `server_disk_cache::load()` 判定。

### 实施前核查（`change.md` §2.1 要求）

- 起始 HEAD：`eb7681889f44304d2bd5b3e41572f5530af91a1d`；工作树含 `2026-11-15 18-18 / 18-35 / 18-49` 三条记录对应的未提交改动
- 先建立可回退提交：`01d5686 feat(disk-cache): 请求级命中统计与请求边界捕获（阶段 1/2）`，此后本阶段的改动均相对该提交
- 记录到的源码哈希（md5，本阶段改动前）：`server-task.cpp 5848fcf3…`、`server-context.cpp 68b50f73…`、`server-task.h a8dae66b…`、`server-common.h 6d5f6693…`、`common/common.h 9cccaa03…`、`common/arg.cpp e30d4d8f…`
- 构建产物：`output/llama-server-impl.dll` `2026-11-15 18:47:25`，`--version` 为 `0.9.0-dev (build 7, commit eb76818)`
- 逐项复核既有改动：CRC/长度校验先于状态恢复；恢复失败清空 slot；target/draft、KV 类型、mmproj、LoRA、media 守卫均未改动；`--cache-boundary-capture none` 与新开关关闭时路径不变（本阶段实测见下）。未发现需要先修复的阻断性缺陷

## 修改文件

### tools/server/server-prefix-index.h / .cpp

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/server-prefix-index.h、tools/server/server-prefix-index.cpp
- 修改位置：新模块全文
- 具体变更：
  - 路径压缩 radix tree，边键为 `(token id, 媒体身份)`；媒体身份用**字符串驻留编号**（`media_intern`），因此比较是精确比较，不以 64 位哈希作为等价证明
  - `observe()`：先算候选再插入（不与自身比较），按 `request_id` 去重（环形表，4096 条），插入后对路径节点累加 `observations`、更新 `last_observed` 并刷新状态
  - 候选包含"落在边内部的共享前缀"（`walk()` 返回 `partial` 与所属边的观察数），并钳制到媒体块起点，避免给出媒体块内部的非法边界
  - 状态机 `observed / eligible / persisted / unavailable`；`refresh_state()` 只在状态迁移时累加 `n_eligible_events`，`observed` 事件在节点创建时累加
  - `on_snapshot_committed()`：仅在快照 token 序列与某节点前缀**完全相等**时挂载引用（`A+B` 的快照不会把 `A` 标为 `persisted`）；`on_snapshot_invalidated()` 撤销引用并标 `unavailable`
  - 容量：`max_candidates` / `max_index_bytes` 联合约束，`evict_one()` 只淘汰**无引用**的叶子；死节点过半时 `compact()` 重建索引并重映射 parent/children/snapshot_ref
- 修改原因：`test1_2` §三、§四

### tools/server/CMakeLists.txt

- 修改类型：修改
- 修改前路径：tools/server/CMakeLists.txt
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/CMakeLists.txt（同批备份）
- 修改后路径：tools/server/CMakeLists.txt
- 修改位置：`server-context` 静态库源文件列表
- 具体变更：加入 `server-prefix-index.cpp` / `.h`
- 修改原因：注册新模块

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/server-task.h
- 修改后路径：tools/server/server-task.h
- 修改位置：头文件包含、`server_task_result_metrics`、`server_disk_cache` 公有段
- 具体变更：引入 `server-prefix-index.h` 与 `<functional>`；`server_task_result_metrics` 增加 `prefix_index_stats cache_prefix`；`server_disk_cache` 增加 `entries_snapshot()`、`on_entry_committed`、`on_entry_invalidated`
- 修改原因：目录需要只读访问已提交条目并在提交/失效时得到通知

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/server-task.cpp
- 修改后路径：tools/server/server-task.cpp
- 修改位置：`drop_entry()`、`sweep_expired()`、`drop_prefixes()`、`enforce_limit()`、`worker()`、新增 `entries_snapshot()`、`server_task_result_metrics::to_metrics()`
- 具体变更：
  - `worker()`：写入成功且 `save_manifest()` 之后，按路径在 `entries` 中确认该条目仍然存在（可能刚被 `drop_prefixes` 或 `enforce_limit` 删除），再在锁外触发 `on_entry_committed`
  - `drop_entry()`：用析构守卫在**释放锁之后**触发 `on_entry_invalidated`，避免在持锁状态下回调
  - `sweep_expired()` / `drop_prefixes()` / `enforce_limit()`：删除后触发 `on_entry_invalidated`
  - `to_metrics()`：新增 7 个 `llamacpp:cache_prefix_*` counter 与 6 个 gauge，并注明不得与 `reused_*` 相加
- 修改原因：`test1_2` §4.3 的引用一致性要求

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 18-32/tools/server/server-context.cpp
- 修改后路径：tools/server/server-context.cpp
- 修改位置：析构函数、`load_model` 的缓存初始化、新增 `observe_prefix()`、`SLOT_STATE_DONE_PROMPT → GENERATING` 分支、`SERVER_TASK_TYPE_METRICS` 分支、`capture_boundary()` 的写盘判据
- 具体变更：
  - 配置签名的构造从 `if (!cache_disk_path.empty())` 内提升到外层并存入 `cache_config_sig`，磁盘层与目录共用同一签名
  - `--cache-prefix-discovery` 开启时创建目录：`min_tokens / min_observations / max_candidates / max_index_mib` 来自参数；把 `entries_snapshot()` 中已存在的条目登记为 `persisted` 引用；挂上提交/失效回调；启动打印 `prefix discovery: enabled, ...`
  - `observe_prefix()`：在输入边界发布**不可变**观察（`slot.task->tokens` 截断到 `llama_memory_seq_pos_max` 覆盖的 token，且不超过 task token 数），携带 media 三元组；`--cache-stats` 下输出 `cache_stats: prefix_tokens / prefix_candidates / best_prefix / best_state / persisted_prefix`
  - 只在**输入边界**发布观察：输出边界含生成 token，`task->tokens` 不含它们，覆盖生成尾部的快照由 `on_snapshot_committed` 登记，避免同一请求两次观察
  - `capture_boundary()`：`input` 边界在**仅有 input 捕获**时也写盘（此时不再有更长的 output 条目会覆盖它），`both` 仍由 output 负责落盘
  - 析构时先清空磁盘层回调，避免目录先于磁盘层析构造成悬空回调
  - `/metrics` 任务填充 `res->cache_prefix`
- 修改原因：`test1_2` §三、§五

### common/common.h、common/arg.cpp

- 修改类型：修改
- 修改前路径：common/common.h、common/arg.cpp
- 修改前备份：.backupChange/2026-11-15 18-32/common/common.h、common/arg.cpp
- 修改后路径：同前
- 修改位置：`common_params` 缓存参数段；`--cache-boundary-capture` 之后
- 具体变更：新增 `cache_prefix_discovery`、`cache_prefix_min_tokens`、`cache_prefix_observations`、`cache_prefix_max_candidates`、`cache_prefix_max_index_mib` 及对应 5 个命令行选项（含 env 变量），非法取值抛 `invalid_argument`
- 修改原因：目录的开关与候选门槛

### tools/server/tests/unit/test_cache_prefix_index.cpp

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/tests/unit/test_cache_prefix_index.cpp
- 修改位置：新文件全文（`main()` + 7 组检查）
- 具体变更：覆盖分叉发现（`A+B/A+C/A+D`）、请求 ID 去重、媒体身份隔离、媒体块内部候选钳制、快照精确对齐与撤销、容量淘汰保护带引用的节点、开关关闭时的空目录。文件头给出独立编译命令；**未**接入 CMake 测试目标（该目录为需要运行服务的 pytest 套件）
- 修改原因：`test1_2` §8.2 / §8.3 的纯元数据检查

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改 / 新增
- 修改前路径：README.md、README_EN.md；projectSummary.md 不存在
- 修改前备份：.backupChange/2026-11-15 18-32/README.md、README_EN.md
- 修改后路径：README.md、README_EN.md、projectSummary.md（新增）
- 修改位置：§6.6 参数表、新增 §6.9、§7 日志表、§7.1 指标表、§8 行为边界；projectSummary.md 全文
- 具体变更：说明"发现 ≠ 可恢复 ≠ 实际命中"、四种状态、精确端点语义、失效规则、容量与单用户边界；列出新指标；projectSummary 汇总当前结构、数据流、格式、开关、口径与阶段进度
- 修改原因：`test1_2` §三 的文档交付

### change.md

- 修改类型：修改（追加）
- 修改前路径：change.md
- 修改前备份：无（按规范只追加）
- 修改后路径：change.md
- 修改位置：文末
- 具体变更：追加本记录
- 修改原因：记录实际实施与验证

## 验证记录

- 已执行检查：全量/增量 CUDA 构建共 5 次（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real）
- 检查结果：通过，`0 FAILED`。`--version` 为 `0.9.0-dev (build 8, commit 01d5686)`；`--help` 可见 5 个新选项

- 已执行检查：编译并运行 `tools/server/tests/unit/test_cache_prefix_index.cpp`（cl 直接编译，不加载模型）
- 检查结果：**先失败后通过**。首次运行 3 项失败，定位为：`walk()` 在边内部发生分歧时只 `break`，不报告"边内部的共享前缀"，导致 `A+C` 相对边 `A+B` 只得到 0 而不是 10。修复为 `walk()` 额外返回 `partial`（分歧位置）与所属边的观察数，并在候选列表中加入该合成候选（状态只能是 `observed/eligible`，永不为 `persisted`）。修复后 7 组检查全部通过（`all prefix-index checks passed`）

- 已执行检查：`--cache-prefix-discovery --cache-boundary-capture both` 下的发现序列（`test-run/logs/prefix-runA.log`，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）
- 检查结果：通过。`corpus[:2000]`（1385 token）首请求 0 候选；同前缀第二请求 `prefix_candidates=1, best_prefix=1385, best_state=observed`；`corpus[:2300]`（1596 token）`best_prefix=1385, best_state=eligible`（观察数达 2）；从 5000 字符偏移取的 1317-token 请求 `prefix_candidates=0`（无错误共享）。指标：`requests 6 / candidates 5 / nodes 5 / tokens 4859 / bytes 39730 / persisted_refs 1 / snapshots 1`

- 已执行检查：`--cache-boundary-capture input` 下的精确对齐与失效（`logs/prefix-runB.log`、`cache-prefixB`）
- 检查结果：通过。B1 写入 1385-token 输入快照（505.77 MiB）；B2 `best_prefix=1385, best_state=persisted, persisted_prefix=1385`；B4（6233 token，与存量条目相差 4848 > 4096）写入新条目并触发 `drop_prefixes` 删除 1385 条目 → 指标 `invalidated_refs_total=1 / snapshots=1 / unavailable=1`，目录同步撤销引用

- 已执行检查：强杀重启后引用重建（`logs/prefix-runD.log`，复用 `cache-prefixB`）
- 检查结果：通过。启动日志 `prefix discovery: enabled, ... 1 snapshot(s) registered`，目录由已校验条目重建，未写任何新的索引文件；1596-token 请求得到 `candidate_lcp=1596, restored=0, reused_disk=0, reject=io_cost`（1225.98 MiB ÷ 1596 = 786 KiB/token 超过 `768 KiB/token` 成本闸门）——说明候选前缀没有被当作命中，且既有成本守卫不受影响

- 已执行检查：关闭开关的回归对照（`logs/prefix-runC.log`，同参数、独立缓存目录）
- 检查结果：通过。`prefix_tokens` 日志行数 0，全部 `cache_prefix_*` 指标为 0；三个请求的 `prompt_n / cache_n` 与开启时逐一相同（1385 / 0；4 / 1381；215 / 1381），生成文本 sha 相同（`fd7c5bda…`、`03e0e517…`）

- 已执行检查：`git status`、临时文件清理
- 检查结果：通过。`test-run/` 三个缓存目录（505 MiB / 1.2 GiB / 505 MiB）与 `build-cuda-shared/build*.log`、临时编译脚本、测试 exe 已删除，`test-run/` 仅剩 610 KB 脚本与日志

- 已执行检查：构建期崩溃排查
- 检查结果：已修复。首版在输出边界发布观察时 `server_tokens::keep_first()` 触发 `GGML_ASSERT(n <= tokens.size())`——`n_covered`（1392）含生成 token，超出 `task->tokens`（1385）。修复为只在输入边界发布观察，并把截断长度钳制到 `task->tokens.size()`

- 未执行的检查及原因：
  - 阶段 4–7：未进入。阶段 4 的"状态可分块性"审查仍未做（需要读 `src/llama-context.cpp` 的序列状态布局）
  - 多 slot（`-np > 1`）、并发请求：生产与测试均为 `-np 1`
  - 中断/超时/断流路径的观察与引用清理：本机测试均为正常完成
  - Linux/POSIX 构建：本机为 Windows/MSVC；本次未引入平台相关 API
  - 目录长期内存与 GC 压力：仅以 5 节点的小规模验证，未做长跑统计

# 2026-11-15 20-22 - Change Record（test1_2 修复轮：终止性 / 删除账本 / 引用时序 / 索引输入）

## 变更概述

按 `betterModification.md` 的 `2026-11-15 19-51 - test1_2` 修复轮要求，处理五类已确认缺陷（A 容量控制不终止、B 删除失败仍扣配额、C 提交/失效通知顺序、D 索引输入完整性与资源控制、E 媒体钳制后的引用不一致）。本轮**不改变**磁盘条目 v8、清单 v3、KV 恢复路径、采样与 `n_past` 计算，不新增缓存开关；所有修改集中在元数据有效性、删除记账与通知顺序。

基线：`8325ae4`（上一轮阶段 3），本轮结束时 HEAD 为本记录对应的提交。

## 修改文件

### tools/server/server-prefix-index.h

- 修改类型：修改
- 修改前路径：tools/server/server-prefix-index.h
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-prefix-index.h
- 修改后路径：tools/server/server-prefix-index.h
- 修改位置：`prefix_input_status`、`prefix_observation_result`、`prefix_index_stats`、私有成员与 helper 声明
- 具体变更：
  - 新增 `prefix_input_status { ok, invalid_media, capacity }` 与其 `_name()`；`observe()` 改为返回 `prefix_observation_result{status, candidates}`
  - `to_units()` 改为返回 `bool` 并接受 `allow_intern`；新增 `media_key(id, token_count)`（媒体身份 = 块 ID + token 数）、`lookup_media()`（只读）、`intern_media()` 改为 const（表为 mutable 追加式）
  - `on_snapshot_committed/invalidated` 增加 `uint64_t epoch`；新增 tombstone 表（上限 512）与 `remember_tombstone/tombstone_epoch`
  - `evict_one()` 改为返回 bool；新增 `over_limits()`、`state_for()`、`alive_nodes()`、`clamp_candidates()`
  - `prefix_index_stats` 新增 `n_media_ids / n_input_rejected`，并明确 `n_rejected` 为"每次观察至多一次"的容量拒绝
- 修改原因：规划 §一 的 A/D/E 与 §三 第 1、4 步

### tools/server/server-prefix-index.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-prefix-index.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-prefix-index.cpp
- 修改后路径：tools/server/server-prefix-index.cpp
- 修改位置：全文相关函数
- 具体变更：
  - **A 终止性**：`note_capacity()` 改为 `while (over_limits()) { if (!evict_one()) { n_rejected++; return; } }`；`evict_one()` 只在真实移除节点时返回 true，内部不再增加拒绝计数——旧实现把"拒绝"当作进展，遇到全部叶子都被物理引用保护时会无限循环
  - **D 输入校验**：媒体三元组长度不一致、空 ID、零长度、越界、与前一块重叠，任一成立即整条观察被拒（`invalid_media`），不再降级为"仅 token 路径"；拒绝不创建节点、不消耗 request id
  - **D 只读性**：`candidates()` 走 `allow_intern=false`，未知身份映射为 `k_media_unknown`（永不与已存单元相等），驻留表不增长
  - **D 预算**：观察前按 `approx_bytes() + Σ(新身份字节)` 预判，超预算直接返回 `capacity`，不产生副作用；`approx_bytes()` 覆盖节点、媒体名与 map、快照表、tombstone、观察去重表
  - **D 媒体身份**：身份键改为 `id#count`，同一 ID 不同 token 数的块不再互相匹配（由单元测试发现）
  - **E 钳制一致性**：`clamp_candidates()` 在缩短候选后清空 `snapshot_path` 并按新长度重算 `state`，被钳制的前缀不再声称与快照精确对应
  - **C 顺序**：`on_snapshot_committed()` 先挂载引用再调用 `note_capacity()`/`compact()`——旧顺序可能先把即将被引用的节点淘汰掉（由单元测试发现）；`on_snapshot_invalidated()` 对未知路径也记录 tombstone，并拒绝 `epoch` 不更新的提交
- 修改原因：规划 §一 A/C/D/E，§三 第 1、3、4 步

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.h
- 修改后路径：tools/server/server-task.h
- 修改位置：`disk_cache_remove_status`、`server_disk_cache_entry`、`disk_cache_counters`、回调声明、私有 helper 与 `event_epoch`
- 具体变更：新增删除结果枚举与 `_name()`；条目增加 `quarantined`；计数器增加 `n_remove_failed / n_quarantined / n_bytes`；回调签名加 `uint64_t epoch` 并注明"在磁盘锁内交付、只有一条通道"；新增 `remove_file/quarantine_entry/notify_committed/notify_invalidated` 声明与 `event_epoch`
- 修改原因：规划 §一 B/C、§二

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：tools/server/server-task.cpp
- 修改位置：`remove_file()`（新增）、`quarantine_entry()`（新增）、`notify_*()`（新增）、`drop_entry()`、`sweep_expired()`、`enforce_limit()`、`drop_prefixes()`、`dedup_prefixes()`、`worker()`、`load()` 的候选循环、`stats()`、`to_metrics()`
- 具体变更：
  - **B 统一删除**：新增唯一删除原语 `remove_file()`，返回 `removed / already_missing / failed`，失败时记录 OS 原因并累加 `n_remove_failed`；四条清理路径全部改为结果驱动，只有前两种才 `total_size -=`，失败时不再虚假扣减
  - **B 隔离**：失败条目 `quarantined = true`，继续计入 `cache_disk_bytes`，但 `load()` 候选循环与 `enforce_limit()` 的淘汰对象选择都跳过它；`drop_prefixes()/dedup_prefixes()` 删除失败时**保留**短条目（文件仍在、仍可恢复，不通知失效）
  - **B 有界淘汰**：`enforce_limit()` 不再用 `min_element` 反复选中同一失败对象，改为"跳过已隔离条目"的选择，无可删对象时输出一次超额告警并退出
  - **C 顺序**：所有提交/失效通知移入磁盘锁内的同一临界区，`notify_*()` 统一分配 `++event_epoch`；删除了旧的"锁外通知 + 无锁查找"路径
  - **指标**：新增 `cache_disk_bytes`（账面占用）、`cache_disk_quarantined`、`cache_disk_remove_failed_total`
- 修改原因：规划 §一 B/C，§三 第 2、3 步

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-context.cpp
- 修改后路径：tools/server/server-context.cpp
- 修改位置：目录初始化处的回调注册与启动登记、`observe_prefix()`
- 具体变更：启动登记跳过 `quarantined` 条目并以 `epoch = 0` 注册；回调 lambda 增加 `epoch` 形参并注明交付语义；`observe_prefix()` 适配 `prefix_observation_result`，被拒时在 `--cache-stats` 下打印 `prefix observation refused (<reason>)`
- 修改原因：规划 §一 C，§三 第 3 步

### tools/server/tests/unit/test_cache_prefix_index.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_cache_prefix_index.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/tests/unit/test_cache_prefix_index.cpp
- 修改后路径：tools/server/tests/unit/test_cache_prefix_index.cpp
- 修改位置：全文
- 具体变更：适配新 API（`observe()` 返回结构体、快照事件带 epoch）；新增 6 组测试——只读媒体查询（未知身份不增长驻留表且不误共享）、非法媒体（长度不一致/重叠/越界/空 ID）、媒体预算拒绝、事件逆序（迟到提交被拒）、容量终止性（全引用保护时函数返回且拒绝计数有界）、压缩后引用仍可解析（`compact()` 重映射）、媒体块边界性质（候选不得落在块内部、非精确端点不得保留引用）
- 修改原因：规划 §二（tests/unit）与 §六 检查清单

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（README 沿用上一轮备份；projectSummary.md 在备份目录中）
- 修改后路径：同前
- 修改位置：指标表、行为边界；projectSummary 新增 §3.1 生命周期与资源不变量、§5 指标说明、§8 边界条目
- 具体变更：补充三个磁盘指标与"删除失败不扣配额"的语义；projectSummary 写入删除/引用/索引输入的不变量表
- 修改原因：规划 §二（projectSummary.md）、§六.3

## 验证记录

- 已执行检查：CUDA 构建 3 次（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real）
- 检查结果：通过，`0 FAILED`；`--version` 为 `0.9.0-dev (build 10)`（构建时的 HEAD 为 `342fd7d`，本轮提交为 `57d9fce`）

- 已执行检查：独立自检 `tools/server/tests/unit/test_cache_prefix_index.cpp`（cl 直接编译，不加载模型）13 组
- 检查结果：**先失败后通过**，且失败暴露了两个真实缺陷：
  - 媒体身份只用块 ID，导致"同一 ID、不同 token 数"的两个块互相匹配（相等 token 数才应相等）→ 身份改为 `id#count`
  - `on_snapshot_committed()` 先 `note_capacity()` 再挂引用，容量预算为 1 时会把即将被引用的节点先淘汰掉，导致 `persisted` 永远为 0 → 改为先挂引用再执行容量检查
  修正后 13 组全部通过（`all prefix-index checks passed`）

- 已执行检查：删除失败注入（`test-run/logs/del-run.log`，`--cache-disk-ttl 2`，用 Python 持有 `.bin` 句柄使 `fs::remove` 失败）
- 检查结果：通过。
  - `remove_file: failed to remove '...pc-746c343f7e6504e6.bin': The process cannot access the file because it is being used by another process.`
  - `quarantine_entry: '...' expired but could not be removed, keeping 505.97 MiB accounted for and excluding it from lookups`
  - 指标：`cache_disk_entries 2 / cache_disk_bytes 1.64203e+09（= 505.97 + 1059.99 MiB）/ cache_disk_quarantined 1 / cache_disk_remove_failed_total 1`，配额未被虚假扣减
  - 释放句柄后下一次写入触发 sweep：`dropping '...pc-746c...': expired after 65 s idle (505.97 MiB)`，随后 `cache_disk_entries 1 / cache_disk_bytes 7.00719e+08 / quarantine 0`，账目恢复到真实占用

- 已执行检查：前缀目录在删除失败下的引用一致性（同一次运行）
- 检查结果：通过。指标 `cache_prefix_invalidated_refs_total 2 / cache_prefix_unavailable 2 / snapshots 1`——被隔离与被删除的条目都在同一时刻撤回了引用，最终只剩存活条目的引用

- 已执行检查：修复后回归（`test-run/logs/fix-run1.log`，发现开启，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）
- 检查结果：通过。`F1 prompt_n=1385 cache_n=0`、`F2 4/1381`、`F3 215/1381`，与修复前逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`）；发现结果不变（0 → 1 候选 `best_prefix=1385 observed` → `eligible`）；`cache_prefix_rejected_total 0`、`n_input_rejected` 未增长

- 已执行检查：关闭开关对照（`logs/fix-run2.log`，`--no-cache-prefix-discovery`）
- 检查结果：通过。`prefix_tokens` 日志 0 行、启动无 `prefix discovery` 行、全部 `cache_prefix_*` 为 0；三次请求的 `prompt_n/cache_n` 与 sha 与开启时逐一相同

- 已执行检查：强杀重启（`logs/fix-run3.log`）
- 检查结果：通过。`prefix discovery: enabled, ... 1 snapshot(s) registered`；`cache_prefix_nodes 2 / persisted_refs 1 / snapshots 1`，未新增磁盘索引文件

- 已执行检查：`git status`、临时文件清理
- 检查结果：通过。三个测试缓存目录（约 3.2 GiB）、临时编译脚本、测试 exe、构建日志与根目录 `.obj` 已删除；`test-run/` 仅剩 692 KB 脚本与日志

- 未执行的检查及原因：
  - **磁盘层的提交/失效交错复现**：本轮把通知改为"磁盘锁内同一临界区同步交付"，该交错在实现上已不可能发生；验证方式为 (a) 静态证明（索引从不回调磁盘层 → 无锁反转、无锁外窗口）与 (b) 索引侧的确定性逆序用例（`test_event_order`）。没有构造"真实磁盘层迟到提交"的端到端用例，因为构造它需要人为注入非同步通道
  - **`clamp_candidates()` 的端到端触发**：媒体身份含 token 数且输入经过区间/重叠校验后，节点边界不可能落在同一媒体块内部，因此该钳制在当前实现中是防御性路径，只能由单元测试的"边界性质"断言覆盖
  - 多 slot（`-np > 1`）、多用户：本轮明确不实现
  - Linux/POSIX 构建：未执行；本次改动未引入平台相关 API（`std::filesystem::remove` 与既有代码一致）

# 2026-11-15 20-47 - Change Record（test1_2 修复轮：隔离条目与可替代性判定）

## 变更概述

按 `betterModification.md` 的 `2026-11-15 20-31 - test1_2` 修复轮要求，修正磁盘缓存**写入抑制/冗余判断/前缀去重**三处与真实恢复条件不一致的判定。基线 HEAD `bc0c9c9`（`--version` 显示 `0.9.0-dev (build 13)`），上一修复提交 `57d9fce`。

三个已确认缺陷：

- **A 隔离条目仍阻止有效写入**：`quarantined` 只在 `load()` 的候选循环里被排除，`covered_cheaply()`、`store()`/`mirror()` 的精确去重、`worker()` 的冗余判断都没有过滤。结果同一条目可以"加载时不可用"却"写入时被认为已覆盖"。
- **B 长条目未必能替代短条目**：`worker()`/`drop_prefixes()`/`dedup_prefixes()`/`covered_cheaply()` 只按纯 token 前缀关系判断冗余，忽略 `load()` 恢复前还要过的媒体身份、最小长度与 `size_file > lcp × 768 KiB` 成本守卫。真实案例：1226 MiB / 1596 token = 786 KiB/token，长条目对短请求是 `io_cost` 不可恢复，但短条目却被它当作"已覆盖"删掉。
- **C hash + token 数不是身份**：`e.hash == hash && e.tokens.size() == …` 直接当"已保存"，既不看隔离状态也不核对 token/media 全量身份。

本轮不改变 v8 条目格式、v3 清单、CRC 覆盖、配置签名、`load()` 的校验与恢复顺序、`768 KiB/token` 与 4096-token lag 两个常量、采样与 `n_past` 计算；不新增开关、不新增第二个通知通道。

## 修改文件

### tools/server/server-disk-cache-match.h / .cpp

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/server-disk-cache-match.h、tools/server/server-disk-cache-match.cpp
- 修改位置：新模块全文
- 具体变更：
  - `disk_cache_prompt_identity`：非拥有指针形式的 token + media 三元组视图，附 `file_bytes`
  - `disk_cache_match_prefix()`：三段判定——token LCP、**双向**媒体比较（A 的块必须在 B 同位置存在且长度/ID 相同，反之亦然）、媒体块内部边界钳制；媒体元数据数组长度不一致时返回 `usable_lcp = 0`（保守）
  - `disk_cache_cost_ok()`：`file_bytes <= usable_lcp × DISK_CACHE_MAX_BYTES_PER_TOKEN`，常量从 `server-task.cpp` 的匿名命名空间移到这里，成为唯一出处
  - `disk_cache_identity_equal()`：逐 token + 逐媒体三元组全等
  - `disk_cache_classify_duplicate()`：hash/长度命中只作为候选筛选，身份不等一律返回 `none`（调用方据此拒绝写入，而不是判为去重或覆盖文件）
  - `disk_cache_suppresses_write()`：媒体可兼容 + 共享长度 ≥ min_tokens + 尾部在 `max_prefix_lag_tokens` 内 + 成本守卫通过
  - `disk_cache_can_replace()`：`shorter` 是 `longer` 的严格前缀、整个前缀媒体可兼容、且 `longer` 按该长度恢复仍满足成本守卫
- 修改原因：规划 §1.3 / §1.4 / §3.1。判定与 `load()` 的确定性前置过滤一致，但不替代文件级校验

### tools/server/CMakeLists.txt

- 修改类型：修改
- 修改前路径：tools/server/CMakeLists.txt
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/CMakeLists.txt（同批备份）
- 修改后路径：同名文件
- 修改位置：`server-context` 源列表
- 具体变更：加入 `server-disk-cache-match.cpp/.h`
- 修改原因：注册新模块

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.h
- 修改后路径：同名文件
- 修改位置：包含、`disk_cache_counters`、`server_disk_cache` 私有段
- 具体变更：引入 `server-disk-cache-match.h`；新增 `n_hash_collisions`；`covered_cheaply()` 形参改为 `const disk_cache_prompt_identity &`；新增 `identity_of()` 与 `is_reusable_entry()`；`drop_prefixes()` 增加 `pending_bytes`
- 修改原因：把"候选是否有效/是否可替代"从各处内联判断收敛为单一接口

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：匿名命名空间常量、`identity_of()`/`is_reusable_entry()`（新增）、`covered_cheaply()`、`load()` 候选循环、`store()`、`mirror()`、`worker()`、`drop_prefixes()`、`dedup_prefixes()`、`to_metrics()`
- 具体变更：
  - **A**：`is_reusable_entry()` = `!quarantined && !tokens.empty()`；`covered_cheaply`、`store`/`mirror` 的去重循环、`worker` 的冗余循环、`drop_prefixes`/`dedup_prefixes`、`load` 候选统一先过这个判断
  - **A 记账保持**：隔离条目仍留在 `entries` 与 `total_size`，不调用 `remove_file()`、不提前扣减、不恢复 `persisted`；被排除后新写入走原队列/预算路径（队列满时仍是 `n_enqueue_rejected`，不再伪称去重）
  - **B**：`covered_cheaply()` 的判据换成 `disk_cache_suppresses_write()`（token+双向媒体+lag+成本守卫），删除了原先"带媒体的条目一律不参与"的粗糙跳过；`worker()` 的"更长就跳过"换成 `disk_cache_can_replace()`；`drop_prefixes()`/`dedup_prefixes()` 只在长条目确实可替代时才删除短条目，删除仍走 `remove_file()` + 成功后扣减 + `notify_invalidated`
  - **B**：`drop_prefixes()` 新增 `pending_bytes` 形参，用待写状态体积近似待写文件体积参与成本守卫（文件头与 packed 部分相差 KiB 量级，相对 MiB 可忽略）
  - **C**：`store`/`mirror` 的精确去重改为 `disk_cache_classify_duplicate()`；hash 冲突时 `n_hash_collisions++` 并 `SRV_ERR` 拒绝写入，不覆盖原文件
  - `load()` 的内联 LCP + 三次媒体循环 + 内部钳制替换为 `disk_cache_match_prefix()`，行为等价（同一套规则），候选语义不变
  - 删除已无引用的 `disk_cache_lcp()`；`DISK_CACHE_MAX_BYTES_PER_TOKEN` 改用头文件中的定义
  - 指标新增 `cache_disk_hash_collision_total`
- 修改原因：规划 §1.2–§1.4、§3.1–§3.2

### tools/server/tests/unit/test_disk_cache_match.cpp

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/tests/unit/test_disk_cache_match.cpp
- 修改位置：新文件（9 组检查）
- 具体变更：纯 token 前缀；媒体身份（不同 ID、相同 ID 不同块长、一侧无媒体）；块内部边界钳制；成本守卫边界（含 1226 MiB / 1596 token 与 506 MiB / 1596 token 两个真实量级）；`can_replace` 的经济性/前缀性/最小长度；`can_replace` 的媒体分歧；`identity_equal`；`classify_duplicate` 的冲突分支；`suppresses_write` 的 lag、成本与媒体分歧
- 修改原因：规划 §5.3/§5.4 要求可独立验证；该模块不依赖模型即可运行

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：指标表、§8 行为边界；projectSummary §3.1 不变量表、§5 说明、§8 边界
- 具体变更：补 `cache_disk_hash_collision_total`；写明"可替代"的单一定义、"hash 不是身份"、"长条目未必能替代短条目（两者并存）"
- 修改原因：规划 §二、§六

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real）
- 检查结果：通过，`0 FAILED`；`--version` 为 `0.9.0-dev (build 13, commit bc0c9c9)`

- 已执行检查：`tools/server/tests/unit/test_disk_cache_match.cpp`（cl 直接编译，不加载模型）
- 检查结果：**先失败后通过**。首轮 2 项失败，均为用例构造问题而非实现错误：(1) 边界钳制用例的请求在前缀外已因 token 不同而分歧，"媒体钳制"没有真正生效，改为保留占位 token 的切片；(2) 媒体分歧用例的请求只有 500 token，落在 4096-token 容忍区间内，抑制本就成立，改为 6000 token 后夹具符合预期。修正后 9 组全部通过（`all disk-cache match checks passed`）

- 已执行检查：隔离条目不再抑制写入（`test-run/logs/fix2-e1c.log`，`--cache-disk-ttl 2`，Python 持有 `.bin` 句柄）
- 检查结果：通过。
  - S1 写入 1385-token 条目（505.77 MiB）；S2（802 token）触发 `enforce_limit → sweep_expired`，删除失败并隔离：`failed to remove ... being used by another process` + `quarantined ... keeping 505.77 MiB accounted for and excluding it from lookups`
  - S3（1456 token，与隔离条目共享 1385）在修复前会被 `covered_cheaply` 判为"已覆盖"而丢弃；本次实测 `stored 1456 tokens, 672.49 MiB`
  - 指标：`cache_disk_entries 2 / cache_disk_bytes 1.23549e+09（= 505.77 + 672.49 MiB）/ cache_disk_quarantined 1 / enqueue_queued 3 / enqueue_dedup 0 / enqueue_rejected 0`——隔离条目仍占配额，新写入既没被伪称去重也没有突破预算

- 已执行检查：短条目不被不可替代的长条目删除（`test-run/logs/fix2-e2.log`）
- 检查结果：通过。先写 1385-token 条目（505.77 MiB），再写 6233-token 条目（1059.80 MiB）；日志中**没有** `prefix of a stored prompt` 删除记录，两个文件并存（530,337,934 B 与 1,111,276,454 B）。修复前该短条目会因纯 token 前缀关系被删除

- 已执行检查：短条目仍然可服务（同上日志）
- 检查结果：通过。随后同一 2000 字符请求：`source = disk, candidate_lcp = 1385, restored = 1385, entry_tokens = 1385, reused_disk = 1381, read_bytes = 530337934`——恢复自短条目（长条目 1059.80 MiB / 1385 = 783 KiB/token，超出 768 KiB 守卫）

- 已执行检查：启动期去重不误删（`test-run/logs/fix2-startup.log`，重启于上一条的缓存目录）
- 检查结果：通过。`2 entries after scan, 2 after dedup/trim`，两个文件都在；修复前启动期 `dedup_prefixes()` 会按纯前缀关系删掉短条目

- 已执行检查：标准回归（`test-run/logs/fix2-reg.log`，发现开启 + both 捕获，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）
- 检查结果：通过。`1385/0`、`4/1381`、`215/1381` 与上一轮逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`）；合法写入抑制仍生效：`1603 tokens are covered up to a 4096-token tail by a stored entry, skipping a 678.05 MiB mirror`；`cache_disk_hash_collision_total 0`

- 已执行检查：`git status`、临时产物清理
- 检查结果：通过。测试缓存目录（约 4 GiB）、句柄脚本、临时编译脚本/可执行文件、构建日志与根目录 `.obj` 均已删除；`test-run/` 仅保留脚本与日志

- 未执行的检查及原因：
  - **媒体路径的端到端验证**：测试机加载的是纯文本 prompt，未启用 mmproj 输入，媒体分歧、块内部边界、不同块长等分支只由单元测试覆盖
  - **真实 hash 冲突的端到端构造**：64 位 FNV 冲突无法在合理时间内构造，`classify_duplicate` 的冲突分支只有单元测试；端到端仅确认计数为 0
  - 多 slot（`-np > 1`）与多用户：本轮明确不实现
  - Linux/POSIX 构建：未执行；本轮未引入平台相关 API
  - 取消/中断路径：未构造用例

# 2026-11-15 21-35 - Change Record（test1_2 修复轮：双向媒体、真实字节、路径所有权）

## 变更概述

按 `betterModification.md` 的 `2026-11-15 20-53 - test1_2` 修复轮要求，修正三处可由源码直接定位的风险。基线 HEAD `6f9b0de`（上一轮提交 `0c289e7` 为算法改动）。

- **A 双向媒体匹配被短路**：`disk_cache_match_prefix()` 用 `!media_side_ok(a,b) || !media_side_ok(b,a)`，C++ 短路语义下第一方向发现差异时第二方向不执行，因此 `clamp_to` 不保证是最早的媒体不一致位置。
- **B 删除短快照时用估算字节**：`worker()` 调 `drop_prefixes(..., bytes)`，其中 `bytes = pending.bytes` 是写前估算；`load()` 的成本守卫用的是 `e.size_file`。估算与实际在阈值附近时可能删掉唯一经济可恢复的短快照。
- **C hash 相同但 token 长度不同跳过检查**：精确去重只在 `e.hash == hash && e.tokens.size() == …` 时才分类，长度不同直接跳过；而 `write_entry()` 的文件名由 hash 决定并先 `fs::remove(path)` 再 `fs::rename`。另外同名 `quarantined` 条目仍在占路径，新写入可能与其争用同一文件名。

本轮不改变 v8 条目格式、v3 清单、CRC 覆盖、配置签名、`load()` 的校验与恢复顺序、`768 KiB/token` 与 4096-token lag 常量、采样与 `n_past`；不新增开关、不新增第二通知通道、不引入新文件命名协议。

## 修改文件

### tools/server/server-disk-cache-match.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-disk-cache-match.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-disk-cache-match.cpp（上一轮版本）
- 修改后路径：同名文件
- 修改位置：匿名命名空间的 `media_structurally_ok` / `media_side_earliest_difference`、`disk_cache_match_prefix()`、`disk_cache_identity_equal()`、`disk_cache_classify_duplicate()`
- 具体变更：
  - **A**：`media_side_earliest_difference()` 取代原 `media_side_ok()`，一次调用只负责一个方向并返回该方向的最早差异（或 `limit` 表示一致）。`disk_cache_match_prefix()` 无条件求两个方向，再取 `std::min`，不再有短路
  - **A 结构校验**：`media_structurally_ok()` 除三数组等长外，还要求下标**严格递增**（顺带拒绝重复与乱序）、块长度非零、ID 非空、区间在 token 数以内且互不重叠；任一不成立时 `usable_lcp = 0`（fail-closed）
  - **C**：`disk_cache_identity_equal()` 先校验两侧描述结构，不可验证时直接返回 false；新增 `disk_cache_media_valid()` 对外暴露该判断
  - **C**：`disk_cache_classify_duplicate()` 的形参由 `same_hash_and_length` 改为 `same_hash`，文档说明长度不同也属冲突
- 修改原因：规划 §一 A、§一 C、§三.1

### tools/server/server-disk-cache-match.h

- 修改类型：修改
- 修改前路径：tools/server/server-disk-cache-match.h
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-disk-cache-match.h
- 修改后路径：同名文件
- 修改位置：`disk_cache_media_valid()`（新增）、`disk_cache_classify_duplicate()` 形参说明、`disk_cache_can_replace()` 的 `file_bytes` 语义说明
- 具体变更：导出结构校验函数；明确 `same_hash` 语义（含长度不同）；明确 `file_bytes` 必须是已提交的真实大小
- 修改原因：调用方需要区分"未验证描述"与"验证后不相等"

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.h
- 修改后路径：同名文件
- 修改位置：`disk_cache_path_owner`（新增）、`disk_cache_counters`、`write_entry()` 声明、`drop_prefixes()` 形参、私有 helper
- 具体变更：新增 `disk_cache_path_owner { free, identical, conflict }` 与 `_name()`；计数器新增 `n_path_conflicts`；`write_entry()` 增加 `path_owned` 形参；`drop_prefixes()` 形参改名 `committed_file_bytes` 并写明来源要求；新增 `claim_path_locked()` 声明与 `path_owner_scratch` 成员
- 修改原因：规划 §一 C、§三.3

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：`disk_cache_path_owner_name()`（新增）、`claim_path_locked()`（新增）、`write_entry()`、`worker()`、`store()`、`mirror()`、`drop_prefixes()`、`to_metrics()`
- 具体变更：
  - **C 路径所有权**：新增 `claim_path_locked(hash, path, wanted, what)`，在磁盘锁内解析目标路径：无记录 → `free`；有可用且身份全同的记录 → `identical`（写盘可跳过）；有可用但身份不同 → `conflict`（`n_hash_collisions` + `n_path_conflicts`，拒绝写入）；有隔离记录 → 尝试 `remove_file()`，成功则扣减 `total_size`、`notify_invalidated`、从 `entries` 移除并 `save_manifest()`，然后视为 `free`；失败则 `conflict`（`n_path_conflicts`）
  - **C worker**：写盘前先调用 `claim_path_locked`；`identical` 按已有语义跳过（计 dedup），`conflict` 计 `n_enqueue_rejected` 并丢弃本次写入；只有 `free` 才继续写
  - **C write_entry**：新增 `path_owned` 形参。为 false 且目标已存在 → 拒绝写入并删除临时文件；为 true 时 `rename` 失败才回退到"删除后重试"（此时残留只可能是无 `entries` 记录的孤儿），不再无条件 `fs::remove(path)`
  - **B**：`worker()` 改为在确认新条目已进入 `entries` 之后才调 `drop_prefixes(..., size_file)`，传入的是从磁盘读回的真实 `size_file`（写前估算只用于队列/内存预算）；`drop_prefixes()` 形参更名 `committed_file_bytes`
  - **C store/mirror**：循环改为对所有 `e.hash == hash` 的条目分类；身份全同**且记录可用**才计 `dedup`；身份不同计哈希冲突并拒绝；隔离条目不算"已存储"（否则同 prompt 会被永久判为重复），交由 worker 在写盘时解析路径
  - 指标新增 `cache_disk_path_conflict_total`
- 修改原因：规划 §一 B、§一 C、§三.2、§三.3

### tools/server/tests/unit/test_disk_cache_match.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_disk_cache_match.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/tests/unit/test_disk_cache_match.cpp
- 修改后路径：同名文件
- 修改位置：新增 `test_bidirectional_earliest_difference()`、`test_media_structural_validity()`、`test_cost_boundary()`；扩充 `test_media_identity()`、`test_classify_duplicate()`
- 具体变更：新增两侧差异位置不同的确定性用例（A 的块在 12、B 的块在 4，两方向答案都是 4）并验证参数交换一致；多媒体块下"最早分歧优先"；结构校验的 8 类非法输入（数组不等长、空 ID、零长块、越界、重叠、乱序、重复位置）；部分覆盖的媒体块；成本守卫的等值 / +1 B / +1 KiB 边界；`classify_duplicate` 的"不同长度"与"不可验证描述"分支
- 修改原因：规划 §一 A 的确定性用例要求、§5.2–§5.4

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：指标表、行为边界；projectSummary §3.1 不变量表与 §8 边界
- 具体变更：补 `cache_disk_path_conflict_total`；写明"hash 命中就是路径命中"、双向扫描与结构校验、成本守卫用真实字节
- 修改原因：规划 §二、§六

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real），共 4 次
- 检查结果：通过，`0 FAILED`；`--version` 为 `0.9.0-dev (build 16, commit 6f9b0de)`

- 已执行检查：**问题 A 的缺陷复现**（`tools/server/tests/unit/test_disk_cache_match.cpp` 新增用例）
- 检查结果：**确认缺陷存在**。把 `disk_cache_match_prefix()` 临时改回原始 `!A || !B` 形式后重编译，同一用例失败：`usable_lcp == 4` 实际得到 **12**——第一方向在 12 处发现差异后，`||` 短路使第二方向未执行，更早的 4 未被发现。恢复修复版本后 12 组全部通过（`all disk-cache match checks passed`）

- 已执行检查：隔离条目不再阻止同 prompt 重新缓存（`test-run/logs/fix3-w5.log`，`--cache-disk-ttl 2`，句柄持有 `.bin`）
- 检查结果：**发现新缺陷并修复**。修复前 `store()` 先按 hash+长度判 `identical`，对已被隔离的条目也计 `dedup`，导致该 prompt 永远无法重新落盘（本轮首次尝试即出现）。改为"隔离条目不算已存储"后，同一 prompt 再次写入被 worker 在锁内回收了隔离条目并真实落盘：`stored 1385 tokens, 505.77 MiB`，`cache_disk_enqueue_dedup_total` 未增长

- 已执行检查：路径所有权解析与写入拒绝（同日志，`--cache-disk-max 600`）
- 检查结果：通过。`expired but could not be removed` → `quarantined ... over quota but could not be removed, keeping 505.77 MiB accounted for` → `failed to remove ... being used by another process`；最终 `cache_disk_entries 1 / cache_disk_bytes 5.30338e+08 / quarantined 1 / remove_failed 1`，长条目（330.03 MiB）被配额淘汰时隔离条目的账面字节未被抹除

- 已执行检查：`write_entry()` 不再无条件删目标文件
- 检查结果：通过。静态核对 + 上述运行日志中未出现 `refusing to write ... the path is not claimed` 或 `refusing to overwrite` 之外的失败；`rename` 回退分支只在 `path_owned=true` 时可用

- 已执行检查：标准回归（`test-run/logs/fix3-reg.log`，发现开启 + both 捕获，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）
- 检查结果：通过。`1385/0`、`4/1381`、`215/1381` 与上一轮逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`）；`cache_disk_entries 1 / cache_disk_bytes 5.30544e+08 / hash_collision 0 / path_conflict 0`；`cache_prefix_nodes 3`

- 已执行检查：强杀重启（`test-run/logs/fix3-restart.log`）
- 检查结果：通过。`1 entries (manifest 1, header 0, stale index records 0)`，`1 entries after scan, 1 after dedup/trim`，`prefix discovery: ... 1 snapshot(s) registered`；请求恢复 `cache_n=1381`，无同路径双记录

- 已执行检查：`git status`、临时产物清理
- 检查结果：通过。8 个测试缓存目录（约 4 GiB）、句柄脚本、临时编译脚本/可执行文件、构建日志均已删除；`test-run/` 降至 809 KB。清理过程中有一个后台 python 进程仍持有 `.bin` 句柄导致 `rm` 失败（`Device or resource busy`），该进程正是测试用的句柄持有者；终止后删除成功——这同时反证了 Windows 上的句柄确实会阻止删除

- 未执行的检查及原因：
  - **问题 B 的端到端边界复现**：需要在 `pending.bytes` 与真实 `size_file` 之间构造"估算过阈值而实际不过"的差异，并要求该条目对同一短前缀恰好落在 `768 KiB/token` 附近。本机条目体积由检查点主导（1.4K token 条目 505 MiB，检查点占大头），可控地构造该边界需要直接改序列化内容，属于人为注入非真实输入，因此**只做了单元测试覆盖**（`test_cost_boundary` 覆盖等值 / ±1 B / ±1 KiB 与 `can_replace` 的两种结果），代码层面已确保只传 `size_file`
  - **跨进程 hash 文件名冲突的端到端构造**：无法在合理时间内找到 64 位 hash 碰撞，`claim_path_locked` 的 `conflict` 分支只有单元测试（`classify_duplicate` 的不同长度/不同媒体分支）与静态核对
  - **真实媒体 prompt**：测试机为纯文本，媒体相关分支全部由单元测试覆盖
  - 多 slot、Linux、中断路径：本轮范围外

# 2026-11-15 22-30 - Change Record（test1_2 修复轮：配额准入先于替代）

## 变更概述

按 `betterModification.md` 的 `2026-11-15 21-55 - test1_2` 修复轮要求，修正 `worker()` 中"先删替代对象、后淘汰新条目"的不可逆顺序缺陷。基线 HEAD `a37ac4e`，上一轮修复提交 `5627aed`。

缺陷：`worker()` 在 `write_entry()` 成功、新条目入 `entries` 之后，先调 `drop_prefixes(..., size_file)` 删除被替代的短快照，再调 `enforce_limit()`。但"文件已写入 + 已进 `entries`"并不能证明配额淘汰不会立刻删掉它。示意：

```text
Q：已隔离/不可删除，占 600 MiB
S：可恢复短前缀，200 MiB
L：新长快照，300 MiB，静态上可替代 S
limit = 800 MiB

旧顺序：Q+S 在限内 → 写入 L → drop_prefixes 删 S（Q+L=900 仍超额）
        → enforce_limit 跳过 Q，淘汰 L → 最终 Q 独存，S 与 L 都没了
```

本轮只做准入顺序、容量淘汰保护集合与替代确认；不改变 v8/v3、CRC、`768 KiB/token`、4096-token lag、`load()` 校验顺序、采样与 `n_past`，不新增通知通道。

## 修改文件

### tools/server/server-disk-cache-quota.h / .cpp

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/server-disk-cache-quota.h、tools/server/server-disk-cache-quota.cpp
- 修改位置：新模块全文
- 具体变更：
  - `disk_cache_quota_item`：`path / size_file / last_used / quarantined / protected_`，只描述状态，不持有 KV 指针
  - `disk_cache_plan_quota(items, limit)`：只读规划。隔离条目与受保护条目一律不进候选；其余按 LRU 顺序回收直到总量落回上限。返回 `fits`（回滚与否的唯一依据）、`protected_bytes`、`overshoot`、`evict_order`
  - `limit == 0` 明确表示不限量（服务端把 `--cache-disk-max -1` 映射为 0），此时不回收任何条目
  - `disk_cache_replacement_dep` / `disk_cache_replacements_safe()`：判断某条"短快照被长快照替代"的依赖是否安全（长条目必须受保护且未被隔离）
- 修改原因：规划 §四.1 要求"只读容量规划"与"计划—物理动作—确认"分离；该模块不取锁、不碰文件，可独立单测

### tools/server/CMakeLists.txt

- 修改类型：修改
- 修改前路径：tools/server/CMakeLists.txt
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/CMakeLists.txt（同批备份）
- 修改后路径：同名文件
- 修改位置：`server-context` 源列表
- 具体变更：加入 `server-disk-cache-quota.cpp/.h`
- 修改原因：注册新模块

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.h
- 修改后路径：同名文件
- 修改位置：包含、`disk_cache_counters`、`drop_prefixes()` 形参、`enforce_limit()` 重载、新增 `quota_items_locked()`
- 具体变更：引入配额模块；计数器新增 `n_write_rejected`；`drop_prefixes()` 增加 `replacement_is_admitted`；`enforce_limit()` 新增接受"本次事务不可失去的条目"集合的重载；新增 `quota_items_locked()`（在锁内构造规划输入，路径存于调用方提供的 `path_storage` 以保证指针稳定）
- 修改原因：规划 §三

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：`worker()`、`enforce_limit()`、`quota_items_locked()`（新增）、`drop_prefixes()`、`to_metrics()`
- 具体变更：
  - **先准入**：`worker()` 在 `write_entry()` 成功后先构造含新条目的规划输入（新条目 `protected_ = true`），调用 `disk_cache_plan_quota()`。不通过则 `remove_file(path)` 回滚新文件、计 `n_write_rejected`、不改动任何既有条目，并打印 `does not fit ... dropping it`
  - **再回收**：通过后才 `total_size += size_file`、入 `entries`，并把该条目加入保护集调用 `enforce_limit(keep)`。配额淘汰跳过保护集（以及隔离条目）
  - **最后替代**：只有在 `entries` 中仍能找到该路径（即确实存活过容量处理）时，才以 `replacement_is_admitted = true` 调用 `drop_prefixes()`；否则传 `false`，函数只统计、不删除
  - **drop_prefixes 门禁**：`replacement_is_admitted == false` 时直接返回（不删任何短快照），避免"为尚未确认存活的条目删除旧数据"
  - `enforce_limit()` 保留原无参重载（内部委托为空保护集），其余 LRU/隔离语义不变；仍然不会反复选中同一失败对象
  - 指标新增 `cache_disk_write_rejected_total`
- 修改原因：规划 §二、§四.1、§四.2

### tools/server/tests/unit/test_disk_cache_quota.cpp

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/tests/unit/test_disk_cache_quota.cpp
- 修改位置：新文件（12 组检查）
- 具体变更：覆盖核心回归场景（Q 隔离 + S 短 + L 长，`fits == false`）；可回收时准入成功且 S 被回收而 L 存活；隔离字节始终计入且永不成为候选；LRU 顺序；受保护条目即使最旧也不被选；无可回收对象时 `fits == false`；边界（恰好等于上限 / 少 1 字节 / 极端小上限）；上限小于单体条目；不限量（`limit == 0`）；替代依赖安全性；以及**旧顺序与新顺序的对照模拟**——同一组模拟文件下旧顺序得到 `S=false, L=false`（两者都消失），新顺序得到 `S=true, L=false`（保留 S）
- 修改原因：规划 §三、§五.1 明确要求"不加载模型即可覆盖容量-替代顺序"

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：指标表、行为边界；projectSummary §3.1 不变量表与 §8 边界
- 具体变更：补 `cache_disk_write_rejected_total`；写明"先准入、后替代"与替代存活不变量
- 修改原因：规划 §六

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real）2 次
- 检查结果：通过，`0 FAILED`；`--version` 为 `0.9.0-dev (build 20, commit a37ac4e)`

- 已执行检查：配额规划单测 `tools/server/tests/unit/test_disk_cache_quota.cpp`（cl 直接编译，不加载模型）
- 检查结果：**先失败后通过，且首轮失败抓到一个实现 bug**。规划器最初把 `limit == 0` 当成"淘汰到 0"，导致不限量场景仍会清空缓存，并且 `fits` 的判定与迭代顺序耦合（7 项断言失败）。修正为"`limit == 0` 即不限量、直接返回 `fits = true`"，并把 `fits` 统一为"回收后是否落在上限内"。另有 5 处是我自己对用例的期望写错（例如两个未受保护条目都需回收时我写成 1 个、把 `overshoot` 的基数算错），逐项核对后修正。最终 12 组全部通过（`all disk-cache quota checks passed`）

- 已执行检查：旧顺序对照（同单测的 `test_order_regression`）
- 检查结果：通过。同一组模拟文件（Q 600 MiB 不可删 / S 200 MiB / L 300 MiB，limit 800 MiB）：旧顺序 `!old_res.s_present && !old_res.l_present`——S 与 L 都消失；新顺序 `new_res.s_present && !new_res.l_present && !new_res.over_quota`——回滚 L、保留 S

- 已执行检查：端到端核心场景（`test-run/logs/fix4-a2.log`，`--cache-disk-max 1400`，句柄持有短条目使 TTL 删除失败）
- 检查结果：通过。
  - Q1 写入 1385-token 短快照（505.77 MiB）
  - Python 持有该 `.bin` 句柄；Q2 触发 TTL 清扫 → `failed to remove ... being used by another process` + `quarantined ... keeping 505.77 MiB accounted for and excluding it from lookups`
  - 随后写入 6233-token 长条目：日志 `stored 6233 tokens, 1059.80 MiB`
  - 指标 `entries 2 / bytes 1.64161e+09（= 505.77 + 1059.80 MiB）/ quarantined 1 / write_rejected 0`——隔离条目的账面字节未被抹除，长条目正常落盘，配额超额如实显示

- 已执行检查：非经济替代时短快照不再被误删（同日志）
- 检查结果：通过。1400 MiB 配额下长条目对 1385-token 前缀的读盘成本为 1059.80 MiB/1385 = 783 KiB/token，超出 768 KiB 守卫，因此 `drop_prefixes` 不再删除短快照，日志中无 `prefix of a stored prompt` 记录，两个文件并存——这正是上一轮修好的判据与本轮顺序修复叠加后的预期行为

- 已执行检查：配额充足时不误伤（`test-run/logs/fix4-a3.log`，`--cache-disk-max 4096`）
- 检查结果：通过。`entries 2 / bytes 1.64161e+09 / write_rejected 0`，短长快照并存，说明准入判定没有把正常写入误判为超限

- 已执行检查：`mirror` 抑制路径仍生效（`test-run/logs/fix4-a4.log`）
- 检查结果：通过。`1456 tokens are covered up to a 4096-token tail by a stored entry, skipping a 672.48 MiB mirror`——原有写入抑制语义未受影响

- 已执行检查：标准回归（`test-run/logs/fix4-reg.log`，发现开启 + both 捕获，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）
- 检查结果：通过。`1385/0`、`4/1381`、`215/1381` 与上一轮逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`）；`write_rejected 0 / path_conflict 0 / cache_prefix_nodes 3`

- 已执行检查：强杀重启（`test-run/logs/fix4-restart.log`）
- 检查结果：通过。`1 entries (manifest 1, stale index records 0)`、`1 after dedup/trim`、`prefix discovery ... 1 snapshot(s) registered`；请求恢复 `cache_n=1381`

- 已执行检查：清理
- 检查结果：通过。4 个测试缓存目录（约 4 GiB）、临时脚本/可执行文件、构建日志、根目录 `.obj` 均已删除；`test-run/` 降至 889 KB。清理时两个测试 `.obj` 因 `cl` 产物被后台进程短暂占用而需二次删除，已确认删除

- 未执行的检查及原因：
  - **"手工构造完全对称的 Q/S/L 三态"**：本机条目体积由检查点主导（1.4K token 即 505 MiB），无法精确构造"Q+S 在限内、Q+S+L 超限且先删 S 仍超限"的真实文件组合。已用 (a) 单测的旧/新顺序对照模拟与 (b) 端到端的 Q（隔离）+ S + L 三态（账面超额如实显示、无错误替代）双向覆盖
  - **`write_rejected` 分支的端到端触发**：需要新条目单体在配额内但回收全部可回收对象后仍放不下。本机每次写入都是 GB 级、配额以 MiB 计，构造该组合需要缩放条目体积，属于人为注入；该分支由单测 `test_nothing_reclaimable` / `test_limit_below_entry_size` 覆盖，代码路径已静态核对
  - **真实媒体 prompt、多 slot、Linux、中断路径**：本轮范围外

# 2026-11-15 23-15 - Change Record（test1_2 修复轮：稳定保护身份与回滚记账）

## 变更概述

按 `betterModification.md` 的 `2026-11-15 22-40 - test1_2` 修复轮要求，修正三处配额事务内的状态一致性问题。基线 HEAD `2799400`，上一轮修复提交 `0e46af4`。

- **A 保护指针失效**：`enforce_limit(keep)` 的 `keep` 是 `std::vector<const server_disk_cache_entry *>`，元素指向 `entries` 内部。`enforce_limit()` 先调 `sweep_expired()`，其 `entries.erase()` 会把被删元素之后的所有元素前移，`keep` 中的地址随即失效；若 TTL 清扫删掉了新条目前面的条目，保护判定就会错位。
- **B 回滚删除失败成为孤儿**：`if (!plan.fits)` 分支调用 `remove_file(path)` 但忽略返回值。Windows 上文件被占用时删除会失败，该文件仍占磁盘，却既不在 `entries` 也不在 `total_size`。
- **C 计划与兑现未分离**：`plan.fits` 只表示"假定候选都能删除"时的可行性。`enforce_limit()` 实际删除可能失败并隔离条目，之后 `total_size` 仍可能超额，而代码当时既不回滚也不记录。

本轮只改准入/回滚/淘汰保护与账本；不改变 v8/v3、CRC、`768 KiB/token`、4096-token lag、`load()` 校验顺序、采样与 `n_past`，不新增通知通道。

## 修改文件

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.h
- 修改后路径：同名文件
- 修改位置：`disk_cache_protected_paths`（新增）、`disk_cache_counters`、`enforce_limit()`、`sweep_expired()`、`is_protected()`、`drop_prefixes()` 返回类型
- 具体变更：
  - 新增 `using disk_cache_protected_paths = std::unordered_set<std::string>`，紧邻其他缓存枚举定义
  - `enforce_limit()` 的无参重载保留，带参重载由 `std::vector<const server_disk_cache_entry *>` 改为 `const disk_cache_protected_paths &`
  - `sweep_expired()` 新增带保护集的重载；新增静态 `is_protected()`
  - `drop_prefixes()` 返回 `size_t`（真正删除的条数）
  - 计数器新增 `n_rollback_failed`、`n_quota_unreclaimed`
- 修改原因：规划 §一.2 要求删除跨 `erase()` 保存的裸指针；§一.4 要求区分"计划可行"与"物理兑现"

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：`is_protected()`（新增）、`sweep_expired()`、`enforce_limit()`、`drop_prefixes()`、`worker()`、`to_metrics()`
- 具体变更：
  - **A**：`is_protected(entry, protected_paths)` 按 `entry.path` 查集合；`sweep_expired(protected_paths)` 在 TTL 判定前先跳过受保护条目；`enforce_limit(protected_paths)` 把同一集合传给 `sweep_expired()`，LRU 循环每轮都按路径重新判定（而不是复用旧指针）
  - **A**：`worker()` 里 `std::vector<const server_disk_cache_entry *> keep` 被删除，改为 `disk_cache_protected_paths protected_paths = { path }`
  - **B**：`!plan.fits` 分支对 `remove_file(path)` 的返回分支处理。`failed` 时调用 `quarantine_entry(e, ...)`、`total_size += size_file`、入 `entries`、计 `n_rollback_failed`——文件真实字节被记账，同时因 `quarantined` 被排除出 `load()`/写入抑制/去重/`persisted`
  - **C**：`drop_prefixes()` 返回实际删除数；`worker()` 在物理回收后重新核对 `total_size`：
    - 仍超额且本次未删除任何被替代短快照 → 回滚新条目，计 `n_write_rejected`/`n_quota_unreclaimed`；回滚若又失败，同样按隔离条目登记
    - 仍超额但已删除被替代快照 → 保留真实超额账本，记 `n_quota_unreclaimed` 并拒绝进一步不受控写入，不删除唯一替代者
  - 候选可用性判据从"路径在 `entries` 中"改为"路径在 `entries` 中**且未隔离**"
  - 指标新增 `cache_disk_rollback_failed_total`、`cache_disk_quota_unreclaimed_total`
- 修改原因：规划 §一.2、§一.3、§一.4、§三.1、§三.2

### tools/server/tests/unit/test_disk_cache_quota.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_disk_cache_quota.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/tests/unit/test_disk_cache_quota.cpp
- 修改后路径：同名文件
- 修改位置：新增保护身份与回滚记账两组用例
- 具体变更：
  - `sim_erase_by_path()`：按 `std::vector::erase()` 的语义把尾部元素逐个前移后再收缩，用于真实复现"erase 导致元素移动"
  - `test_protection_identity_survives_prior_erase()`：受保护条目位于被 TTL 删除条目的**后面**，先构建保护集再 erase，最后判定。断言路径键版本保住受保护条目；指针版本另行记录但不作断言（读失效指针是未定义行为，其结果不构成契约）
  - `test_protection_survives_eviction/repeated_eviction`：连续淘汰下受保护条目不被选中
  - `test_protected_never_lru_victim`：受保护条目即使 `last_used` 最旧也不被淘汰
  - `test_quarantined_and_protected_together`：隔离 + 受保护同时存在时 `fits == false`、`overshoot` 正确、隔离与受保护条目都不进候选
  - `test_rejected_write_rollback`：回滚成功不计账面字节、回滚失败必须按隔离登记并计入 `total_size`，两种情况都不算成功写入
  - 修 `sim_alive()` 形参为 `const std::string &`
- 修改原因：规划 §四.1、§四.2 要求定点触发 `entries.erase` 与回滚失败

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：指标表、行为边界；projectSummary §3.1 不变量表
- 具体变更：补 `cache_disk_rollback_failed_total`、`cache_disk_quota_unreclaimed_total`；写明"保护身份用路径而非指针"与"计划不等于兑现"
- 修改原因：规划 §二、§六

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real）多次
- 检查结果：通过，`0 FAILED`。中途出现两次失败：一次是我误改了 `sweep_expired()` 的无参声明（`C2511`），一次是残留 `llama-server.exe` 占用 `llama-server-impl.dll`（`LNK1104`），均已定位并修复后重建成功；`--version` 为 `0.9.0-dev (build 23, commit 2799400)`

- 已执行检查：配额规划单测 `tools/server/tests/unit/test_disk_cache_quota.cpp`（cl 直接编译，不加载模型）
- 检查结果：通过（`all disk-cache quota checks passed`）。中途一处 `sim_alive()` 形参类型不匹配（`const std::string` → `const char *`，`C2664`），改为引用形参后通过

- 已执行检查：**端到端复现问题 A 的触发路径**（`test-run/logs/fix5-v1.log`，`--cache-disk-ttl 5`，`--cache-disk-max 1800`，三条不同长度的请求）
- 检查结果：通过，且确认 TTL 清扫确实在准入 pass 内删除前置条目——`sweep_expire: dropping 'test-run/cache-v1/pc-736f...bin': expired after 7 s idle (505.77 MiB)`（发生在第二条目写入之前），第三次同样是 `expired after 7 s idle (569.49 MiB)` 后 `stored 4038 tokens, 635.30 MiB`。即：每次准入都伴随一次真实 `entries.erase()`，新条目仍正常留存，`entries 1 / bytes 6.66161e+08 / quarantined 0`

- 已执行检查：隔离 + 准入顺序（`test-run/logs/fix5-t1.log` / `fix5-t3.log`，`--cache-disk-max 1400`，Python 持有 `.bin` 句柄）
- 检查结果：通过。`quarantined ... expired but could not be removed, keeping 505.77 MiB accounted for` → `enforce_limit: over the 1400.00 MiB limit but no removable entry left (1565.57 MiB used)` → `stored 6233 tokens, 1059.80 MiB`。隔离字节与受保护新条目都未被淘汰，超额如实记录，`entries 2 / bytes 1.64161e+09`

- 已执行检查：标准回归（`test-run/logs/fix5-reg.log`，发现开启 + both 捕获，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）
- 检查结果：通过。`1385/0`、`4/1381`、`215/1381` 与上一轮逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`）；`write_rejected 0 / path_conflict 0 / cache_prefix_nodes 3`

- 已执行检查：强杀重启（`test-run/logs/fix5-restart.log`）
- 检查结果：通过。`1 entries (manifest 1, stale index records 0)`、`1 after dedup/trim`、`prefix discovery ... 1 snapshot(s) registered`；请求恢复 `cache_n=1381`

- 已执行检查：`git status`、临时产物清理
- 检查结果：通过。测试缓存目录（约 3 GiB）、句柄脚本、临时编译脚本/可执行文件、构建日志、根目录 `.obj` 均已删除；`test-run/` 降至 1011 KB

- 未执行的检查及原因：
  - **问题 B 的端到端触发（`plan.fits == false`）**：需要新条目单体在配额内，且回收全部可回收对象后仍放不下。本机条目由检查点主导（1.4K token 即 505 MiB），任意两条相加就超过任何可设的 MiB 级配额，无法在真实模型上构造该组合。该分支由单测（`test_rejected_write_rollback`、`test_nothing_reclaimable`、`test_limit_below_entry_size`）与静态核对覆盖
  - **同一个文件的写入与回滚持有句柄**：回滚失败需要"写入刚完成、临时句柄已关闭、随即被外部进程独占"的时序，本机只能稳定构造 TTL/LRU 阶段的句柄占用，无法在 `remove_file(path)` 调用的瞬间注入占用
  - **问题 A 的"修复前会失败"对照**：无法在真实服务上运行带失效指针的版本再对比，因为读失效指针是未定义行为、结果不确定。已用单测的路径键/指针键两套序列并列记录，并对路径键版本断言；指针版本只记录不断言
  - 真实媒体 prompt、多 slot、Linux、中断路径：本轮范围外

# 2026-11-15 23-40 - Change Record（test1_2 修复轮：提交资格与持久化引用）

## 变更概述

按 `betterModification.md` 的 `2026-11-15 23-17 - test1_2` 修复轮要求，修正**隔离条目可能被通知为 `persisted`** 的控制流不变量违背。基线 HEAD `c5d403d`，上一轮修复提交 `059e267`。

缺陷位置：`server_disk_cache::worker()` 末尾的提交通知分支只按 `e.path == path` 判定。而该函数内两个回滚失败分支都会把条目保留在 `entries` 里（仅用于记账）：

1. `!plan.fits` 且 `remove_file(path)` 失败 → `quarantine_entry(e, ...)` + `total_size += size_file` + `entries.push_back(...)`
2. `plan.fits` 但物理回收后仍超额、`superseded == 0`、回滚又失败 → 对已入 `entries` 的条目 `quarantine_entry(cur, ...)`

这两条路径下"路径在 `entries` 中"为真，于是 `notify_committed()` 被调用。`server_prefix_index::on_snapshot_committed()` 会据此建立物理引用并置为 `persisted`；而 `server_disk_cache::load()` 排除该条目，`server-context.cpp` 的启动登记也跳过 `quarantined`。结果是一个**运行期存在、重启后消失、且无法恢复**的持久化引用。

本轮只改提交资格判定与相应诊断；不改变 v8/v3、CRC、`768 KiB/token`、4096-token lag、`load()` 校验、`eligible` 判定、采样与 `n_past`，不新增通知通道。

## 修改文件

### tools/server/server-disk-cache-quota.h / .cpp

- 修改类型：修改
- 修改前路径：tools/server/server-disk-cache-quota.h、tools/server/server-disk-cache-quota.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：`disk_cache_commit_candidate`（新增结构）、`disk_cache_commit_eligible()`（新增）
- 具体变更：
  - 新增 `disk_cache_commit_candidate { path, quarantined, has_tokens }`：一个路径在事务结束时对外的可见状态
  - 新增 `disk_cache_commit_eligible(entries, path)`：同路径存在、**未隔离**、且 token 非空才返回 true。这是"能否向目录发布持久化引用"的唯一判定
  - 补 `#include <string>`
- 修改原因：规划 §四.1 要求资格判定显式且可测；该模块不取锁、不碰文件，可独立单测

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：`worker()` 末尾的提交查找与通知
- 具体变更：把"路径是否存在"改为"是否存在可用条目"：
  - 遍历 `entries` 时同步构造 `disk_cache_commit_candidate`，并用 `is_reusable_entry(cur)` 判定资格
  - 只有资格成立才 `notify_committed(committed)`
  - 路径存在但无可用条目时打印 `stays accounted for but is not usable, not publishing a persisted reference`，把"仅有账面占用"与"已提交快照"区分开
  - 通过新 helper `disk_cache_commit_eligible()` 再确认一次，保证单测覆盖的正是运行时使用的判定
- 修改原因：规划 §二、§四.1、§四.2

### tools/server/tests/unit/test_disk_cache_quota.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_disk_cache_quota.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/tests/unit/test_disk_cache_quota.cpp
- 修改后路径：同名文件
- 修改位置：新增 `test_commit_eligibility()`（8 组）
- 具体变更：覆盖规划 §4.2 的五个事务结局及三个附加角例——有效提交（应发送）；回滚成功后路径不存在（不发送）；`!plan.fits` 回滚失败（不发送）；配额不足回滚失败（不发送）；token 为空的条目（不发送）；**别的路径被隔离不影响本路径**；**因无关隔离文件而整体超额但自身有效**（仍应发送，避免把所有超配额条目误排除）；同路径多条记录中只要有可用的即可发送
- 修改原因：规划 §三、§四.4 要求纯元数据层可验证资格

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：行为边界；projectSummary §3.1 不变量表
- 具体变更：写明"只有可用条目才发布持久化引用"及运行时/启动登记资格一致
- 修改原因：规划 §六

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real）
- 检查结果：通过，`0 FAILED`；`--version` 为 `0.9.0-dev (build 23, commit c5d403d)`

- 已执行检查：**缺陷复现**（单测）
- 检查结果：**确认缺陷存在**。把 `disk_cache_commit_eligible()` 临时改回旧的"仅按路径存在"语义后重编译，同一组用例有 3 项失败：

  ```
  FAIL test_disk_cache_quota.cpp:808: !disk_cache_commit_eligible(entries, "pc-aaa.bin")
  FAIL test_disk_cache_quota.cpp:817: !disk_cache_commit_eligible(entries, "pc-aaa.bin")
  FAIL test_disk_cache_quota.cpp:825: !disk_cache_commit_eligible(entries, "pc-aaa.bin")
  ```

  即三个隔离场景（`!plan.fits` 回滚失败、配额不足回滚失败、token 为空）在旧语义下全部被判为可提交。恢复修复版本后全部通过

- 已执行检查：磁盘层三套单测
- 检查结果：通过。`all disk-cache match checks passed`、`all disk-cache quota checks passed`（含新增 8 组提交资格用例）、`all prefix-index checks passed`

- 已执行检查：**端到端——隔离条目不产生持久化引用**（`test-run/logs/fix6-p1.log`，`--cache-prefix-discovery` 开启，`--cache-disk-max 1400`，`--cache-disk-ttl 4`，Python 持有 `.bin` 句柄）
- 检查结果：通过。
  - 基线（有效条目）：`cache_prefix_persisted_refs 1 / cache_prefix_snapshots 1`——正常快照确实会发布引用，说明修复没有把有效路径一并排除
  - 制造隔离：`failed to remove ... being used by another process` → `quarantined ... keeping 505.77 MiB accounted for and excluding it from lookups`
  - 隔离后：`cache_disk_entries 1 / cache_disk_quarantined 1 / cache_prefix_persisted_refs 0 / cache_prefix_snapshots 0 / cache_prefix_unavailable 1`——计数从 1 降到 0，隔离条目未被发布为 `persisted`
  - 同一事务还触发了配额复核路径：`over the 1400.00 MiB limit but no removable entry left` → `quota still exceeded after the admission pass (165.57 MiB over), rolling back '...pc-3a888c45f86e70ff.bin' because no superseded snapshot was deleted`，磁盘目录最终只剩隔离文件（530,337,934 B），未出现孤立文件

- 已执行检查：重启后一致性（`test-run/logs/fix6-p1r.log`）
- 检查结果：通过。`cache_scan_end: 1 entries (manifest 1, stale index records 0)`、`1 entries after scan, 1 after dedup/trim`，且 `prefix discovery: ... 0 snapshot(s) registered`——启动登记同样跳过该隔离条目，与运行期 `persisted_refs 0` 一致。这正是本轮要消除的"运行期 persisted、重启后消失"错配

- 已执行检查：标准回归（`test-run/logs/fix6-reg.log`，发现开启 + both 捕获，双卡 `-ts 38,62`，语料 `E:\小说\正文.md`）
- 检查结果：通过。`1385/0`、`4/1381`、`215/1381` 与上一轮逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`）；`write_rejected 0 / cache_prefix_nodes 3 / cache_prefix_persisted_refs 1 / cache_prefix_snapshots 1`

- 已执行检查：清理
- 检查结果：通过。测试缓存目录（约 3 GiB）、句柄脚本、临时编译脚本/可执行文件、根目录 `.obj`、`__pycache__` 均已删除；`test-run/` 降至 1.1 MB；`git status` 仅剩本轮源码与文档改动

- 未执行的检查及原因：
  - **`!plan.fits` 且回滚删除失败的端到端触发**：需要新条目单体在配额内、回收全部可回收对象后仍放不下，且回滚瞬间被外部进程独占。本机条目由检查点主导（1.4K token 即 505 MiB），任意两条相加即超过任何可设的 MiB 级配额；且无法在 `remove_file(path)` 调用瞬间注入占用。该分支由单测（`test_rejected_write_rollback`、`test_commit_eligibility` 场景 3/4）与静态核对覆盖，**未在真实运行中复现**
  - **"同名快照先失效、后回滚失败"的时序**：需要同一路径先 `notify_invalidated` 再在同一事务内回滚失败，构造依赖前者。规划 §4.3 要求的"最新事件不把 quarantined 路径复活"由提交资格判定在源头上保证（该路径根本不会进入 `notify_committed`），未单独构造时序用例
  - **测试用删除故障注入钩子**：规划 §4.4 建议增加"仅测试用的确定性删除故障注入"。本轮未实现——它需要引入测试专用代码路径（编译期开关或运行期钩子），而现有单测已能在纯元数据层覆盖全部五个事务结局、端到端也已用真实句柄占用覆盖 TTL/LRU 与配额复核两条路径。是否值得为此在生产源码中留一个钩子，建议在下一轮单独判断，本轮不擅自扩大改动面
  - 真实媒体 prompt、多 slot、Linux、中断路径：本轮范围外

# 2026-11-16 01-05 - Change Record（test1_3：公共前缀主动物化 + 前置整理）

## 变更概述

按 `betterModification.md` 的 `2026-11-15 23-38 - test1_3` 实施**公共前缀主动捕获与独立 KV 快照持久化**。基线 HEAD `147001c`，上一轮修复提交 `1ab6528`。本轮包含两部分：规划 §2 要求的源码一致性整理，以及主体功能。

### 一、前置整理（规划 §2.2 要求，与功能改动分开记录）

- **消除重复的提交资格判定**：`worker()` 末尾原先直接用 `is_reusable_entry(cur)`，而 `disk_cache_commit_eligible()` 只被单测调用。现改为先构造 `commit_view` 并调用 `disk_cache_commit_eligible()` 作唯一判定，命中后再取条目记录——运行时代码与单测使用同一函数。
- **修复日志换行**：`stays accounted for but is not usable, not publishing a persisted reference@NL@` 中残留的字面量 `@NL@` 已替换为真正的换行符。这是上一轮我改用按行替换时漏掉的一处，`grep -c "@NL@"` 现为 0。

### 二、主体功能

链路：多请求共享前缀 A → 目录标记 `eligible` → 后续请求 prefill 经过 A 结束时精确停批 → 两处上下文同步后导出完整状态 → 磁盘层正常写队列落盘 → 真实提交后目录登记 `persisted` → 重启后按 `source=disk` 复用。

## 修改文件

### tools/server/server-prefix-materialize.h / .cpp（新增）

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/server-prefix-materialize.h、tools/server/server-prefix-materialize.cpp
- 修改位置：新模块全文
- 具体变更：
  - `disk_cache_materialize_candidate { prefix_tokens, observations, eligible, persisted }`：候选的只读视图
  - `disk_cache_materialize_request`：请求侧已知信息（是否启用、磁盘层最小 token、输入长度、`n_past`、媒体块区间）
  - `disk_cache_materialize_skip`：八种拒绝原因，各有可打印名
  - `disk_cache_plan_materialize()`：按固定次序筛选——必须 `eligible`、必须尚无 `persisted`、必须达到**磁盘层**最小 token（与目录的观察阈值相互独立）、必须落在本次输入内、结尾不得位于媒体块内部、不得已被 KV 越过；通过者取**最长**一个
- 修改原因：规划 §3.2 的候选选择规则需要独立可测；该模块不取锁、不碰文件

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-context.cpp
- 修改后路径：同名文件
- 修改位置：`server_slot::prefix_materialize_plan`（新增成员）、`materialize_counters`（新增）、`plan_prefix_materialize()`（新增）、`materialize_prefix()`（新增）、主 prefill 填充循环、`post_decode()`、`get_available_slot()`、metrics 采集
- 具体变更：
  - **slot 计划**：`{ armed, prefix_tokens, task_id, done }`，在 `get_available_slot()` 中随 `cache_restore` 一并重置。`task_id` 防止 slot 复用后执行旧计划
  - **计划**：`plan_prefix_materialize(slot, n_past)` 在 `n_past` 落定后调用，查询只读 `candidates()`（不增加 `observe` 次数），按上述规则定出 K，写入 slot 计划
  - **批次截断**：填充循环开头若计划已 armed 且 `slot.prompt.n_tokens() >= K` 则 `break`，使本批恰好结束于 K；下一轮从同一位置继续
  - **`n_past == K` 的直接捕获**：KV 恰好停在边界时不再 arm 截断，而是立即捕获。原因见下"本轮自查发现的缺陷"
  - **捕获**：`post_decode()` 中，`llama_process()` 与 `common_speculative_process()` 都返回之后、且 slot 仍为 `PROCESSING_PROMPT` 时调用 `materialize_prefix()`。该校验 `pos_max + 1 == K`（等于才导出，越过则记 skipped）、`slot.prompt.tokens.size() >= K`、草稿上下文同样到达 K；checkpoint 只保留 `pos_max <= K`
  - **不误判未到的批次**：`pos_max + 1 < K` 时直接 return 等下一批，不记 skipped、不置 done
  - **指标**：`counters_materialize { scheduled, exported, skipped }`，在 metrics 任务中采集为 `res->n_materialize_*`
- 修改原因：规划 §3.2、§3.3、§5.2

### tools/server/server-task.h / .cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.h、tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.h、server-task.cpp
- 修改后路径：同名文件
- 修改位置：`server_prompt_cache::materialize()`（新增）、`server_task_result_metrics` 新字段、`counter_info` 指标注册、`worker()` 提交通知
- 具体变更：
  - **`materialize()`**：按规划 §3.4 增加独立于 RAM 驻留策略的导出路径。先 `get_size_ext` 取两处上下文大小，分配缓冲，`get_data_ext` 写入并**校验写入字节数与预估一致**（不一致即失败，不产生不完整对象），产出 `server_prompt_cache_state` 交给调用方入磁盘队列
  - 提交资格判定统一到 `disk_cache_commit_eligible()`（见前置整理）
  - 新增 `cache_prefix_materialize_scheduled_total` / `_exported_total` / `_skipped_total`
- 修改原因：规划 §3.4 明确要求绕过普通 RAM 长前缀去重，规划 §6 要求四层统计互不冒充

### common/common.h、common/arg.cpp

- 修改类型：修改
- 修改前路径：common/common.h、common/arg.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（common 相关在备份目录）
- 修改后路径：同名文件
- 修改位置：`cache_prefix_materialize` 字段、`--cache-prefix-materialize` 参数
- 具体变更：新增默认 **false** 的布尔参数，注册 `--cache-prefix-materialize` / `--no-cache-prefix-materialize`，带 `LLAMA_ARG_CACHE_PREFIX_MATERIALIZE` 环境变量
- 修改原因：规划 §3.2 要求默认关闭；§4 要求可在关闭态完全不影响原路径

### tools/server/tests/unit/test_prefix_materialize.cpp（新增）

- 修改类型：新增
- 修改前路径：无（新增文件）
- 修改前备份：无（新增文件）
- 修改后路径：tools/server/tests/unit/test_prefix_materialize.cpp
- 修改位置：新文件，15 组检查
- 具体变更：覆盖关闭态、无候选、最长者胜出、磁盘最小长度与目录阈值相互独立、短候选不阻塞长候选、已 persisted 跳过、`observed` 不算候选、`eligible` 优先于 `observed`、超出输入长度、媒体块边界三点（块内拒绝 / 恰在块起点 / 恰在块终点）、已越过边界、`n_past == K` 允许、零长度忽略、共享前缀真实场景、拒绝原因名互不相同
- 修改原因：规划 §4、§8.2

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：指标表、行为边界；projectSummary §3.1 不变量表
- 具体变更：补三个 `cache_prefix_materialize_*` 指标；写明主动物化、RAM 策略不阻挡独立磁盘对象、四层计数不互冒充
- 修改原因：规划 §4、§8.5

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real），本轮共 8 次
- 检查结果：通过，`0 FAILED`。中途两次为环境原因（残留 `llama-server.exe` 占用 `llama-server-impl.dll`，`LNK1104`），停进程后重建成功。`--version` 为 `0.9.0-dev (build 27, commit 147001c)`

- 已执行检查：四套独立单测
- 检查结果：通过。`all disk-cache match checks passed`、`all disk-cache quota checks passed`、`all prefix-index checks passed`、`all prefix-materialize checks passed`。候选择单测首轮曾有两处失败（`observed` 候选被误分类为 `already_persisted`，原因是遍历中 `saw_persisted_only` 对 `observed` 条目未置位），已核对语义后修正分类逻辑

- 已执行检查：**端到端核心链路**（`test-run/logs/fix7-m4.log` / `fix7-m5.log`，`--cache-prefix-materialize`，`--cache-ram 512`，`--cache-disk-min-tokens 256`，单 slot，三请求共享 3000 字前缀后各接不同后缀）
- 检查结果：通过，完整链路贯通：

  ```
  plan_prefix_: materialize = skipped (no_candidates), n_past = 1476         # 第 2 请求
  plan_prefix_: materialize = scheduled, prefix = 2102 tokens, n_past = 1476  # 第 3 请求
  materialize_: materialize = exported, prefix = 2102 tokens
  worker: disk cache: stored 2102 tokens, 538.51 MiB in 512.51 ms
  metrics: scheduled 1 / exported 1 / cache_disk_entries 1 / persisted_refs 1
  磁盘文件: pc-bf495e2785d51ca5.bin, 564668394 B
  ```

  这是**绕过 RAM 容量准入**的独立对象：状态 538.51 MiB > `--cache-ram 512`，旧路径下 `alloc()` 会直接返回 nullptr（首次尝试的日志为 `prompt state size 538.501 MiB exceeds cache size limit 512.000 MiB, skipping` + `export_failed`）

- 已执行检查：**重启后按磁盘真实恢复**（`test-run/logs/fix7-m4r.log`，完全相同模型/上下文配置重启）
- 检查结果：通过。

  ```
  cache_scan_end: 1 entries (manifest 1, stale index records 0), 538.51 MiB
  prefix discovery: ... 1 snapshot(s) registered
  cache_stats: prompt = 2540, reused_slot = 0, reused_ram = 0, reused_disk = 1478, prefill = 1062,
               source = disk, candidate_lcp = 2101, restored = 2101, entry_tokens = 2102,
               read_bytes = 564668394, restore_us = 329856
  ```

  `read_bytes` 与文件大小 564668394 B 精确一致；`source = disk` 且 `reused_disk = 1478 > 0`，是真实复用而非目录候选数。恒等式 `0 + 0 + 1478 + 1062 = 2540` 成立

- 已执行检查：**开启/关闭态输出一致性**（`fix7-off.log` vs `fix7-m5.log`，同一组 4 个请求）
- 检查结果：通过。逐字节一致：`f283d36a836120c4`、`9bceec30889f8bda`、`ad661337d3aa8896`、`b9febb26949711a2`。关闭态 `materialize_scheduled/exported/skipped` 全为 0、`cache_disk_entries 0`

- 已执行检查：**关闭态标准回归**（`fix7-reg.log`，未加 `--cache-prefix-materialize`，`--cache-prefix-discovery` + `--cache-boundary-capture both`）
- 检查结果：通过。`1385/0`、`4/1381`、`215/1381` 与上一轮逐项相同，输出 sha 相同（`fd7c5bda…`、`03e0e517…`）；三个 materialize 指标均为 0，`cache_prefix_persisted_refs 1`——默认关闭时原路径完全不变

- 已执行检查：**本轮自查发现的缺陷与修正**
- 检查结果：**发现并修复了我自己实现中的一个真实缺陷**。`n_past == K` 时计划会 arm 批次截断，但填充循环第一轮即满足 `n_tokens >= K` 而 `break`，本批一个 token 都不加。空批次会使 `decode()` 打印 `no tokens to decode`，连续超过 3 次触发 `GGML_ABORT`——即请求会直接中止服务。修正为：`n_past == K` 时不 arm 截断，而是立即从当前状态捕获（这正是规划 §3.2 第 5 条"恰好停在捕获边界即可直接捕获"的要求）。修正后 `fix7-m5.log` 中第 5 个请求走该分支成功导出：`materialize = scheduled at n_past, prefix = 2101 tokens` → `exported`，输出与关闭态一致
- 该缺陷属于我在实现中新引入、并在验证中自查发现，不是原代码问题

- 已执行检查：恒等式抽查（`fix7-m5.log` 全程）
- 检查结果：通过。`2468 = 1476 + 0 + 0 + 992`、`2506 = 1476 + 0 + 0 + 1030`、`2540 = 1478 + 0 + 0 + 1062`、`2512 = 2101 + 0 + 0 + 411`，全部成立

- 已执行检查：清理
- 检查结果：通过。测试缓存目录、临时编译脚本与可执行文件、`__pycache__`、根目录 `.obj` 均已删除；`test-run/` 为 1.2 MB。保留 `test-run/pmat.py`（共享前缀+不同后缀的探测脚本，与既有 `probe.py` 同类，属于复现脚本）

- 未执行的检查及原因：
  - **媒体输入的端到端**：测试机为纯文本，媒体分支（结尾落在媒体块内 → `skipped (media)`）只有单测覆盖。规划 §3.2 第 4 条要求"遇到 mtmd 嵌入块记录 skipped 并保留原推理路径"——代码路径已实现，但**未在真实多模态输入下运行过**
  - **`--cache-prefix-materialize` 在未开启 `--cache-prefix-discovery` 或未开启磁盘层时的行为**：规划 §3.2 要求"清晰拒绝或说明该开关无效"。当前实现是 `plan_prefix_materialize()` 开头直接 return（因为 `!prefix_index || !prompt_cache->disk`），**不会给出任何显式提示**。这一点我没有实现规划要求的"清晰拒绝"，只做到了"不生效"
  - **context shift / 任务取消 / decode 重试下的重复捕获**：捕获点已限定在 `PROCESSING_PROMPT` 且校验 `pos_max + 1 == K`，但规划 §8.3 要求逐一验证的这些异常时序未单独构造
  - **checkpoint 超出 K 的端到端**：单测覆盖了截断逻辑，运行中未构造带 checkpoint 的模型（当前为纯文本 Qwen，`do_checkpoint` 路径未实测）
  - **配额淘汰与短快照替代对物化对象的交互**：本轮未构造。物化对象走的是同一个 `store()` 与配额事务，前几轮已验证的准入/替代/隔离语义对它同样适用，但未针对该对象单独回归

# 2026-11-16 03-20 - Change Record（test1_3：物化路径边界收紧）

## 变更概述

按 `betterModification.md` 的 `2026-11-16 01-12 - test1_3` 修复公共前缀物化路径的四处边界问题，并补齐配置依赖诊断。基线 HEAD `010c68a`，功能提交 `ab6bac2`。本轮只改物化支路的正确性边界、候选选择与诊断；未启用该开关时批次划分、状态导出与写盘路径完全不变。

### 缺陷 A：draft 端点未作等值检查

`materialize_prefix()` 原先只拒绝 `dft_pos_max + 1 < K`。draft 越过 K 时不会被拒绝，而导出会把 target 的 K 状态与 draft 的更长状态组合成一个快照。

修复为成对检查，抽出可独立单测的 `disk_cache_check_capture_bounds(prefix_tokens, target_end, has_draft, draft_end)`，返回 `exact / target_short / target_past / draft_short / draft_past / draft_missing`。仅 `exact` 允许导出；`target_short` 保留计划等下一批；其余一律记 skipped。`has_draft == false` 时保持 target-only 路径，不把草稿检查变成强制前置。

### 缺陷 B：K 等于输入终点时被当作"已越过"

`post_decode()` 的物化循环只处理 `SLOT_STATE_PROCESSING_PROMPT`，把 `DONE_PROMPT` 且 `K <= slot.prompt.n_tokens()` 一律置 done 并记 skipped。当 K 恰为本次输入终点时，最后一个 prefill 批次同时使 slot 转入 `DONE_PROMPT`，合法捕获因此被跳过。

修复为：`DONE_PROMPT` 也交给 `materialize_prefix()`，由真实的 target/draft 端点决定是 `exact`（捕获）还是 `target_past`（跳过）。slot 状态只说明提示词处理完，不说明停在哪里。

### 缺陷 C：媒体块被聚合为单一区间

`plan_prefix_materialize()` 原先把 `media_indices.front()` 到 `media_indices.back() + media_token_counts.back()` 当作一个区间，两个媒体块之间的合法 K 被误拒绝。

修复为有序互不重叠的 `std::vector<disk_cache_media_chunk>`：
- `disk_cache_media_chunks_valid()`：拒绝空块、重叠、乱序、越界。校验在候选筛选**之前**执行——结构不合法即拒绝物化，不降级为"无媒体输入"
- `disk_cache_prefix_in_media()`：仅 `start < K < end` 判为块内

### 缺陷 D：checkpoint 位置上界 off-by-one

原判定 `ckpt->pos_max <= (llama_pos) K` 把 token 数当作位置上界。

**确定性证据**：本机真实 checkpoint 日志为 `pos_max = 8238, n_tokens = 8239`（`pos_max = n_tokens - 1`）；代码侧 `server_tokens::pos_next(n) = n`（纯文本）与 `update_pos(slot.prompt.n_tokens() - n_tokens_cur, pos_min, pos_max)` 一致。即 K 个 token 占位置 `[0, K)`，最大合法位置是 `K-1`。故 `pos_max == K`（`n_tokens == K+1`）的 checkpoint 会被原条件接纳，而快照只声称 K 个 token。

修复为双边界判定 `disk_cache_checkpoint_fits(ckpt, prefix_tokens, prefix_pos_max)`：`n_tokens` 对齐快照声称的 token 数，`pos_max` 对齐实际 KV 末端位置。文本下取 `prefix_pos_max = K-1`；媒体下两者本就不等，各查各的。

**通用路径同类问题**：规划 §6.2 第 3 条要求审查 `capture_boundary()`。该处第 2064 行同样写 `ckpt->pos_max <= (llama_pos) n_covered`，存在同一 off-by-one（且 `n_covered` 是 token 数，媒体下与位置不等价）。已改为调用同一判定并传入真实 KV 末端 `pos_max`。这是最小必要修复，未改动 checkpoint 策略或存储格式。

### 配置依赖诊断

`--cache-prefix-materialize` 开启但缺依赖时原先静默无效。现于启动时输出明确说明（二选一触发，依赖齐全时无额外日志）：

```
W srv load_model: --cache-prefix-materialize needs --cache-prefix-discovery: there is no prefix directory to take candidates from, so no prefix will be captured
W srv load_model: --cache-prefix-materialize needs a disk cache tier (--cache-disk): a captured prefix has nowhere to be stored, so no prefix will be captured
```

## 修改文件

### tools/server/server-prefix-materialize.h

- 修改类型：修改
- 修改前路径：tools/server/server-prefix-materialize.h
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：媒体块结构、请求结构、新增端点判定与 checkpoint 判定
- 具体变更：
  - 新增 `disk_cache_media_chunk { start, end }`（半开区间）
  - `disk_cache_materialize_request` 的 `has_media` + 单一 `media_chunk_start/end` 替换为 `std::vector<disk_cache_media_chunk> media_chunks`
  - 新增 `disk_cache_media_chunks_valid()`、`disk_cache_prefix_in_media()`
  - 新增 `disk_cache_capture_bounds` 枚举与 `disk_cache_check_capture_bounds()`
  - 新增 `disk_cache_checkpoint_view` 与 `disk_cache_checkpoint_fits(ckpt, prefix_tokens, prefix_pos_max)`
- 修改原因：规划 §5.2、§3.2、§6.2

### tools/server/server-prefix-materialize.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-prefix-materialize.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：新增四个判定函数；`disk_cache_plan_materialize()` 的媒体检查与前置校验
- 具体变更：
  - 媒体块列表校验置于候选筛选之前，不合法立即返回 `skipped (media)`
  - 块内判定改为对每个真实块做 `start < K < end`，命中后因列表有序可提前结束
  - 端点判定与 checkpoint 双边界判定的实现
- 修改原因：规划 §5.2、§6.2

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-context.cpp
- 修改后路径：同名文件
- 修改位置：`materialize_prefix()`、`plan_prefix_materialize()`、`post_decode()` 物化循环、`capture_boundary()`、启动依赖诊断
- 具体变更：
  - `materialize_prefix()`：改用 `disk_cache_check_capture_bounds()` 成对判定；`target_short` 直接 return 等下一批（不记 skipped、不置 done），其余非 `exact` 记 skipped 并置 done；checkpoint 过滤改用 `disk_cache_checkpoint_fits()` 并传入 `K-1`
  - `plan_prefix_materialize()`：由 `media_indices` / `media_token_counts` 构造真实块列表
  - `post_decode()`：`DONE_PROMPT` 与 `PROCESSING_PROMPT` 都交给 `materialize_prefix()`
  - `capture_boundary()`：checkpoint 过滤改为同一判定，位置上界用真实 `pos_max`
  - 启动时按缺失依赖输出清晰说明
- 修改原因：规划 §3.2、§4.2、§5.2、§6.2、§7

### tools/server/tests/unit/test_prefix_materialize.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_prefix_materialize.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：媒体用例改写 + 三个新用例组
- 具体变更：
  - `test_media_boundary()` 改用块列表；新增块外两侧（K=1023 / K=4096）合法
  - `test_media_gap_is_legal()`：两块之间、块终点、块起点、块内、全部块之后、以及"更有候选跨越块间隙时最长者胜出"（新增）
  - `test_media_chunks_malformed()`：重叠、乱序、空块、越界四种非法结构 + 合法结构对照（新增）
  - `test_media_chunk_predicate()`：`disk_cache_prefix_in_media()` 在 10 个边界点上的取值（新增）
  - `test_capture_bounds()`：`target=K / draft=K`、`draft<K`、**`draft>K`**、`target<K`、`target>K`、`draft_missing`、负端点、仅 target 无 draft 共 12 项（新增）
  - `test_checkpoint_fits()`：按实测关系 `n_tokens = pos_max + 1` 构造，含 `pos_max == K`（原缺陷）、`n_tokens > K`、位置超界、`pos_min > pos_max`、负值、K=1 边界、以及媒体下两边界分离的场景（新增）
- 修改原因：规划 §11.2、§11.4、§8

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：行为边界；projectSummary §3.1 不变量表
- 具体变更：新增"物化边界须成对""输入末端可捕获""媒体区间按块判定""checkpoint 双边界"四条不变量
- 修改原因：规划 §8、§12

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real），本轮 3 次
- 检查结果：通过，`0 FAILED`。期间一次失败是我把 `\n` 写进诊断字符串导致 `C2001`（常量中有换行符），改为转义后通过。`--version` 为 `0.9.0-dev (build 31, commit 010c68a)`

- 已执行检查：**缺陷 A 单测**（`test_capture_bounds`，12 项）
- 检查结果：通过。覆盖 `target=K/draft=K`、`draft<K`、**`draft>K`（原缺失分支）**、`target<K`、`target>K`、`draft_missing`、负端点、仅 target 无 draft、以及"target 短且 draft 超前时优先报 target"

- 已执行检查：**缺陷 C 缺陷复现**（把判定临时改回单区间语义）
- 检查结果：**确认缺陷存在**。同一组用例出现 7 项失败：

  ```
  FAIL test_prefix_materialize.cpp:199: plan.capture
  FAIL test_prefix_materialize.cpp:200: plan.prefix_tokens == 800 (0 vs 800)
  FAIL test_prefix_materialize.cpp:206: plan.capture
  FAIL test_prefix_materialize.cpp:210: plan.capture
  FAIL test_prefix_materialize.cpp:301: !disk_cache_prefix_in_media(20, chunks)
  FAIL test_prefix_materialize.cpp:302: !disk_cache_prefix_in_media(30, chunks)
  FAIL test_prefix_materialize.cpp:303: !disk_cache_prefix_in_media(40, chunks)
  ```

  即 `K=800` 落在 `[600,1000)` 聚合区间内部被误拒，且块终点 700 / 块起点 900 也被误判为块内。恢复修复版后全部通过

- 已执行检查：**缺陷 D 缺陷复现**（把位置上界改回 `pos_max > K`）
- 检查结果：**确认缺陷存在**。出现 2 项失败：`!disk_cache_checkpoint_fits({0,100,100},100)` 与 `!disk_cache_checkpoint_fits({0,1,1},1)`——即 `pos_max == K` 与 `pos_max == 1, K == 1` 两个越界 checkpoint 被原条件接纳

- 已执行检查：**确定性证据核对**（checkpoint 位置映射）
- 检查结果：通过。真实服务日志 `created context checkpoint 1 of 32 (pos_min = 8238, pos_max = 8238, n_tokens = 8239)`，多条记录一致满足 `pos_max = n_tokens - 1`；代码侧 `server_tokens::pos_next()` 在 `!has_mtmd` 时返回 `n_tokens`，`create_checkpoint()` 调 `update_pos(n_tokens, pos_min, pos_max)`。两者共同证明 K 个 token 占位置 `[0, K)`

- 已执行检查：**缺陷 B 端到端**（`test-run/logs/fix8-n2.log`，`--cache-prefix-materialize`，`--cache-ram 512`，`--cache-disk-min-tokens 256`）
- 检查结果：**通过，且定点命中该场景**。task 37：

  ```
  cache_stats: prompt = 2477, reused_slot = 2101, reused_disk = 0, prefill = 376
  plan_prefix_: materialize = scheduled, prefix = 2477 tokens, n_past = 2101
  materialize_: materialize = exported, prefix = 2477 tokens
  ```

  即 K（2477）恰等于本次输入总长，`n_past = 2101 < K`，最后一个 prefill 批次恰好结束在 K 并使 slot 转入 `DONE_PROMPT`——修复前这里会被一律当作"已越过 K"而跳过。修复后成功导出

- 已执行检查：**核心链路回归**（`fix8-n1.log` / `fix8-f1.log`）
- 检查结果：通过。`scheduled → exported → stored → persisted_refs 1` 全链路正常；`fix8-n1.log` 中两次调度两次导出（含一次 `scheduled at n_past`），输出 sha 与上一轮逐字节一致（`f283d36a836120c4`、`452bdcadc89d353f`、`ad661337d3aa8896`、`b9febb26949711a2`）

- 已执行检查：**重启后磁盘真实恢复**（`fix8-n2r.log`，同配置重启）
- 检查结果：通过。`cache_scan_end: 1 entries`、`1 snapshot(s) registered`；请求恢复 `source = disk, reused_disk = 2102, read_bytes = 391363294, restored = 2102`，`read_bytes` 与文件大小一致；恒等式 `0 + 0 + 2102 + 394 = 2496` 成立

- 已执行检查：**关闭态一致性与无噪声**（`fix8-off.log`）
- 检查结果：通过。`--cache-prefix-materialize` 未开启时三个 materialize 指标全 0、`cache_disk_entries 0`；输出 sha 与开启态对应请求一致；无任何依赖诊断日志（诊断只在显式开启且缺依赖时输出）

- 已执行检查：**依赖诊断**（`diag1.log` / `diag2.log`，均 `-ngl 0` 快速启动后终止）
- 检查结果：通过。两条各触发一次，措辞明确指向缺失项；依赖齐全的正常启动未出现该警告

- 已执行检查：四套独立单测
- 检查结果：通过。`all disk-cache match checks passed`、`all disk-cache quota checks passed`、`all prefix-index checks passed`、`all prefix-materialize checks passed`

- 已执行检查：恒等式抽查（`fix8-n1.log` / `fix8-n2.log`）
- 检查结果：通过。`2504 = 0 + 0 + 0 + 2504`、`2468 = 1476 + 0 + 0 + 992`、`2506 = 1476 + 0 + 0 + 1030`、`2540 = 1478 + 0 + 0 + 1062`、`2512 = 2101 + 0 + 0 + 411`、`2477 = 2101 + 0 + 0 + 376`，全部成立

- 已执行检查：**自查发现的交付级测试缺口**（本轮交付前补做）
- 检查结果：**发现并修复**。规划 §3.2 要求"引入可独立测试的状态边界判定函数……单测不要复制另一套相似条件"。我照此抽出了谓词 `disk_cache_checkpoint_fits()`，但位置上界 `K-1` 的算术**仍留在调用点**（`disk_cache_checkpoint_fits({...}, K, (int64_t) K - 1)`）——谓词有测试，而这段算术没有。
- 验证方式：构造回归，把调用点的 `K - 1` 改回 `K`（即重新引入 off-by-one），四套单测**全部通过**——测试完全无法发现它。这证明该缺口真实存在。
- 修复：把算术移入被测模块，新增 `disk_cache_prefix_pos_max_text(size_t prefix_tokens)`（返回 `N-1`；`N==0` 返回 `-1`），调用点改为 `disk_cache_checkpoint_fits({...}, K, disk_cache_prefix_pos_max_text(K))`。
- 新增测试 `test_prefix_pos_max_text()`：直接断言 4 个取值、`N==0` 的拒绝对照、`N=1..32` 的循环一致性（N 个 token 恰好通过、N+1 个被拒）、以及"token 数当位置上界会被错误接纳"的对照。回归验证：把 helper 改回 `return prefix_tokens` 后 **4 项失败直接命中**（`disk_cache_prefix_pos_max_text(1) == 0 (1 vs 0)` 等）
- 同类排查：通用路径 `capture_boundary()` 直接传真实 `pos_max`、无算术；`materialize_prefix()` 的其余入参均为 `llama_memory_seq_pos_max() + 1` 的直读，无算术

- 已执行检查：修复后的最终端到端回归（`test-run/logs/fix9-z1.log`）
- 检查结果：通过。`scheduled 1 / exported 1 / cache_disk_entries 1 / persisted_refs 1`，输出 sha 与上一轮逐字节一致（`f283d36a…`、`452bdcadc89d353f`、`ad661337d3aa8896`、`f5fad3402cd0d5e9`）

- 已执行检查：清理
- 检查结果：通过。测试缓存目录、临时编译脚本与可执行文件、`__pycache__`、根目录 `.obj` 均已删除；`test-run/` 为 1.2 MB

- 未执行的检查及原因：
  - **真实模型 checkpoint 恢复**：规划 §6.2 第 4 条允许在无可用 checkpoint 模型时改用元数据测试并如实记录。本机 `--checkpoint-min-step` 默认 8192，且 `create_checkpoint()` 还需 `is_user_start` 或 `near_prompt_end` 条件；我用 14990 token 的纯文本输入仍未触发任何 checkpoint（`grep "created context checkpoint"` 为 0，全部来自历史日志）。因此**位置映射的确定性证据来自历史真实 checkpoint 日志 + 代码语义核对**，而非本轮运行；checkpoint 过滤的**运行验证未执行**，本轮只做了单测与通用路径的静态修复
  - **媒体输入的端到端**：测试机为纯文本，无多模态模型与图片输入。`media_chunks` 构造、块内判定、非法结构拒绝全部由单测覆盖（含 4 种非法结构与 10 个边界点），但**未在任何真实 mtmd 输入下运行过**。规划 §5.2 第 5 条还提示：若真实 mtmd 路径不能保证块间 target/draft 状态一致，应采用"本请求有媒体则跳过"的保守保护——我**没有**实现该保守保护，因为无法在无多模态环境下判断是否必要
  - **draft 超前的端到端**：单测覆盖了 `draft_past` 分支，但运行中 `common_speculative_process()` 之后 draft 与 target 始终对齐，未能自然构造 draft 越过 K 的场景，也未能注入
  - **context shift / 取消 / decode 重试**：捕获点已限定在 `PROCESSING_PROMPT`/`DONE_PROMPT` 且要求端点严格等于 K，但规划 §11.3 要求逐一构造的这些异常时序未执行
  - **多个媒体块的真实输入**：本轮无法构造，仅单测覆盖

# 2026-11-16 09-30 - Change Record（test1_3：token 与 position 单位分离 + 媒体门禁）

## 变更概述

按 `betterModification.md` 的 `2026-11-16 08-38 - test1_3` 修复主动前缀物化中"token 数"与"KV position"被当作同一量比较的单位错误，并加入媒体输入的实际写盘门禁。基线 HEAD `6b35154`，上一功能提交 `7aedd56`（含 `d0537f1` 的边界算术整理）。本轮未改分块格式、低估门槛、读盘守卫与配额事务。

### 缺陷：端点比较混用单位

`materialize_prefix()` 把前缀 token 数 `K` 与 `llama_memory_seq_pos_max() + 1`（position 数）直接比较。纯文本下二者相等（`pos_next(N) == N`），所以既有测试全部通过；但含媒体块时：

- `server_tokens::pos_next(K)` 遍历 token 流，每遇媒体块累加 `mtmd_input_chunk_get_n_pos()`（实际模型位置数）并跳过 `mtmd_input_chunk_get_n_tokens()`（token 占位数量）
- 确认 `mtmd_image_tokens_get_n_pos()` 由 clip 行数计算（`1 + clip_suffix_rows(...).size()` 之和），与 token 占位数量是相互独立的量

因此含媒体前缀的 KV 末端位置不等于其 token 数。旧写法会**错误拒绝**本应合法的捕获（把位置数当成"越过 token 数"），也可能在计数恰好错开时把不一致状态判为 exact。

### 修复一：端点比较移入 position 域

- `disk_cache_check_capture_bounds(prefix_tokens, target_end, ...)` 重命名为 `disk_cache_check_capture_position_bounds(expected_position_end, target_position_end, has_draft, draft_position_end)`，三个参数统一为 position
- 调用点改为先求 `expected_end = slot.prompt.tokens.pos_next(K)`，再与 `tgt_pos_max + 1` / `dft_pos_max + 1` 比较
- `K` 继续用于快照 token 元数据、文件身份与最小长度判定；`pos_next(K)` **不**用于截断 token 容器或修改 disk LCP
- 新增 `expected_end <= 0` 判据：前缀映射不到任何位置时无法捕获

### 修复二：checkpoint 位置界取真实端点

原调用点对媒体路径使用了文本专用 `disk_cache_prefix_pos_max_text(K)`（返回 `K-1`）。改为传入已验证 exact 的 target `tgt_pos_max`。该 helper 保留但**生产路径不再引用**，仅由单测固定 `pos_next(N) == N` 这一文本恒等式。

### 修复三：媒体门禁

实验机无真实 mtmd 模型与图片输入，无法端到端验证多模态状态导出与恢复。加入默认保守门禁：请求含媒体块时，`disk_cache_plan_materialize()` 仍完成候选选择、长度与可达性、块边界合法性判定（这些用于诊断），但**不 arm 捕获**，返回 `media_state_unverified`。

门禁位置经过一次修正：初版放在函数开头，导致媒体块的几何边界判定成为不可达代码（单测立刻暴露）。改放在"将要 arm"处，使 `media`（几何不合法）与 `media_state_unverified`（几何合法但状态未验证）两个理由各自可达、可区分。

未提供"强制实验性写入"绕过选项，符合规划 §3.3。

## 修改文件

### tools/server/server-prefix-materialize.h

- 修改类型：修改
- 修改前路径：tools/server/server-prefix-materialize.h
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：skip 枚举、端点谓词签名、checkpoint 文档、text helper 文档
- 具体变更：
  - 新增 `disk_cache_materialize_skip::media_state_unverified`
  - `disk_cache_check_capture_bounds` → `disk_cache_check_capture_position_bounds`，参数语义改为 position 并写明理由
  - `disk_cache_checkpoint_fits` 文档说明位置界必须是真实端点、不可由 token 数推导
  - `disk_cache_prefix_pos_max_text` 文档标明仅文本单测使用，禁止用作媒体路径的位置界
- 修改原因：规划 §3.1、§3.2、§3.3

### tools/server/server-prefix-materialize.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-prefix-materialize.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：`disk_cache_check_capture_position_bounds()`、`disk_cache_plan_materialize()`、`disk_cache_materialize_skip_name()`
- 具体变更：
  - 谓词改为纯 position 比较，新增 `expected_position_end <= 0` 判据
  - 媒体门禁置于"将要 arm 捕获"处，使几何不合法仍报 `media`
  - 新增理由名 `media_state_unverified`
- 修改原因：规划 §3.1、§3.3、§4

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-context.cpp
- 修改后路径：同名文件
- 修改位置：`materialize_prefix()`
- 具体变更：
  - token 视图长度检查改为"未填满则 return 等下一批"（初版误设为硬失败，会拒绝所有中间批次，已修正）
  - 新增 `expected_end = slot.prompt.tokens.pos_next(K)`，与真实位置端点比较
  - checkpoint 位置上界由 `disk_cache_prefix_pos_max_text(K)` 改为 `(int64_t) tgt_pos_max`
  - 诊断日志增列 `expected_end`
- 修改原因：规划 §3.1、§3.2

### tools/server/tests/unit/test_prefix_materialize.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_prefix_materialize.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：谓词改名、媒体用例改写、三个新用例组
- 具体变更：
  - 全部 `disk_cache_check_capture_bounds` 调用改为位置域名
  - `test_capture_position_bounds()`：原 12 项 + `expected_end <= 0` 三项 + 宽位置跨度三项
  - `test_position_vs_token_units()`（新增）：同一组数字（K=2102、expected_end=2104）在位置域判为 exact、在旧混合语义下判为 `target_past`；反向构造旧语义误判为 exact 而位置域正确报 `target_short`
  - `test_media_state_unverified_gate()`（新增）：单块/多块/候选在块后/几何不合法优先/无媒体对照/理由名
  - `test_media_boundary()`、`test_media_gap_is_legal()`、`test_media_chunks_malformed()` 改写为区分 `media` 与 `media_state_unverified`
- 修改原因：规划 §4、§7.2

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：行为边界；projectSummary §3.1 不变量表
- 具体变更：新增"端点比较只在 position 域""checkpoint 位置界取真实端点""媒体请求不进入主动写盘"三条不变量
- 修改原因：规划 §4、§7.4

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real），本轮 2 次
- 检查结果：通过，`0 FAILED`。`--version` 为 `0.9.0-dev (build 33, commit 6b35154)`

- 已执行检查：`mtmd` 位置语义核对
- 检查结果：确认 `mtmd_input_chunk_get_n_pos()`（`tools/mtmd/mtmd.cpp:3492`）对图像块转调 `mtmd_image_tokens_get_n_pos()`（`3738`），后者按 clip 行数累加（`3755`：`n_pos += 1 + clip_suffix_rows(entry.suffix_type).size()`），与 `mtmd_input_chunk_get_n_tokens()` 是独立量。单位错误的前提成立

- 已执行检查：**本轮单位错误的定位与修正**（自查发现一处我自己的回归）
- 检查结果：**发现并修正**。首版把 token 视图长度检查提前为硬失败（`skipped (prompt has 1478 < 2102 tokens)` 并置 `done`），导致所有中间批次捕获全部失效，端到端 `exported_total` 从 2 降为 0。修正为"未填满则 return 等下一批"后恢复：`scheduled 2 / exported 2 / cache_disk_entries 1 / persisted_refs 1`。该回归由端到端数值发现，不是单测——单测只覆盖纯元数据

- 已执行检查：**媒体门禁位置修正**
- 检查结果：首版门禁置于 `disk_cache_plan_materialize()` 开头，使媒体块的几何边界判定成为不可达代码，单测立刻暴露 15 项失败。改为置于"将要 arm"处后，`media`（几何不合法）与 `media_state_unverified`（几何合法）两个理由均可区分，单测全部通过

- 已执行检查：**端到端核心链路**（`test-run/logs/fix9-w2.log`，`--cache-prefix-materialize`，`--cache-ram 512`，`--cache-disk-min-tokens 256`）
- 检查结果：通过。

  ```
  plan_prefix_: materialize = skipped (no_candidates), n_past = 1476
  plan_prefix_: materialize = scheduled, prefix = 2102 tokens, n_past = 1476
  materialize_: materialize = exported, prefix = 2102 tokens
  plan_prefix_: materialize = scheduled, prefix = 2101 tokens, n_past = 1478
  materialize_: materialize = exported, prefix = 2101 tokens
  metrics: scheduled 2 / exported 2 / cache_disk_entries 1 / persisted_refs 1
  ```

- 已执行检查：**重启后磁盘真实恢复**（`fix9-w2r.log`，同配置重启）
- 检查结果：通过。`cache_scan_end: 1 entries, 538.51 MiB`、`1 snapshot(s) registered`；请求恢复 `source = disk, reused_disk = 2102, restored = 2102, read_bytes = 564668394`（与文件大小一致）；恒等式 `0 + 0 + 2102 + 394 = 2496` 成立

- 已执行检查：**开启/关闭态输出一致性**（`fix9-off.log` vs `fix9-w2.log`，各 4 个相同请求）
- 检查结果：通过。**12 个请求（开启态 8、关闭态 4）输出 sha 逐字节一致**：`f283d36a836120c4`、`9bceec30889f8bda`、`ad661337d3aa8896`、`b9febb26949711a2`。关闭态三个 materialize 指标全 0、`cache_disk_entries 0`

- 已执行检查：恒等式抽查（`fix9-w2.log`）
- 检查结果：通过。`2504 = 0+0+0+2504`、`2468 = 1476+0+0+992`、`2506 = 1476+0+0+1030`、`2540 = 1478+0+0+1062`，全部成立

- 已执行检查：四套独立单测
- 检查结果：通过。`all disk-cache match checks passed`、`all disk-cache quota checks passed`、`all prefix-index checks passed`、`all prefix-materialize checks passed`

- 已执行检查：清理
- 检查结果：通过。测试缓存目录、临时编译脚本与可执行文件、`__pycache__`、根目录 `.obj` 均已删除；`test-run/` 为 1.3 MB

- 未执行的检查及原因：
  - **媒体输入的端到端（含门禁本身）**：测试机无多模态模型与图片输入。规划 §7.2 要求"用模拟多媒体块创建 `n_tokens != n_pos` 的前缀，分别令目标/草稿端点等于、短于、超过 `expected_pos_end`"——我用**纯元数据**在 `test_position_vs_token_units()` 中构造了 `K=2102 / expected_end=2104` 的分离情形，但**没有用真实 mtmd 请求验证门禁日志与"原推理请求仍正常完成"**。门禁的生效路径（不 arm、不导出、不写盘）由单测覆盖，**未在真实服务上触发过**
  - **`pos_next()` 的媒体路径未被本轮任何测试执行**：生产调用点的 `pos_next(K)` 只在纯文本下走过（此时恒等于 K）。媒体分支的 `while` 循环由既有代码与静态核对确认，**未运行**
  - **`expected_end <= 0` 判据**：单测覆盖，运行中未构造（需要 `pos_next` 返回 0 或负的前缀）
  - **target/draft 位置不一致的端到端**：运行中 `common_speculative_process()` 之后两者始终对齐，未能自然构造，也未能注入
  - **checkpoint 过滤**：本机 `--checkpoint-min-step` 默认 8192，且还需 `is_user_start` 或 `near_prompt_end`；上一轮用 14990 token 纯文本输入仍未触发任何 checkpoint。本轮同样未触发，**只有单测覆盖**；`tgt_pos_max` 替代 `K-1` 的改动在运行中未被执行
  - **context shift / 取消 / decode 重试**：未构造

# 2026-11-16 10-10 - Change Record（test1_3：in-flight 去重改用完整身份）

## 变更概述

按 `betterModification.md` 的 `2026-11-16 09-35 - test1_3` 修复写队列 in-flight 去重只按 64-bit hash 判定身份的不一致。基线 HEAD `b4b831c`，上一功能提交 `d57d09c`。本轮未改文件格式 v8、manifest v3、CRC64、媒体身份编码、读盘成本守卫与配额策略，也未触碰 `server-prefix-materialize.*` 的 token/position 语义。

### 缺陷来源与性质

`std::unordered_set<uint64_t> inflight` 是**原始 baseline `eb76818` 就有的**，不是新引入。但现行代码已在落盘条目一侧强化了"hash 不是身份"（`disk_cache_classify_duplicate()` / `disk_cache_identity_equal()` 逐项核对 token 与媒体三元组），而排队中的条目仍只看 hash。于是新旧路径对"同一条待写内容"的判定不一致：

- prompt A 已入队
- prompt B 与 A 得到同一个 64-bit hash，但 token 长度或媒体身份不同
- B 进入 `store()` / `mirror()` 时被 `inflight.count(hash)` 判为"已经在写"，**计入 `n_enqueue_dedup` 并被静默丢弃**

正确的行为是：hash 相同 + 完整身份相同 = 真正的 in-flight duplicate（合并）；hash 相同 + 完整身份不同 = 同一路径的身份冲突（拒绝并计入碰撞）。

**没有证据表明用户实际请求中已发生 hash 碰撞**。这是由源码确认的逻辑不一致，用元数据夹具注入相同 hash 即可稳定重现。

### 修复一：inflight 保存完整不可变身份

`inflight` 由 `unordered_set<uint64_t>` 改为：

```cpp
struct inflight_identity {
    llama_tokens              tokens;
    std::vector<uint32_t>     media_indices;
    std::vector<uint32_t>     media_token_counts;
    std::vector<std::string>  media_ids;

    disk_cache_prompt_identity view() const; // 非拥有视图，供比较使用
};

std::unordered_map<uint64_t, inflight_identity> inflight;
```

值**拥有自己的 token/media 内存**，不指向 `queue`：worker 在写完成前就把元素 `pop_front` 了，指向 deque 的指针恰好在最需要判定的时候失效。不保存 target/draft 的 KV buffer，容量由写队列的条目上限（`DISK_CACHE_QUEUE_MAX_ENTRIES = 8`）约束。

### 修复二：store() 与 mirror() 共用同一判定

新增三个私有方法，均在 `mtx` 保护下：

- `inflight_check_locked(hash, wanted)` → `inflight_verdict{free, duplicate, conflict}`，内部调用**现有** `disk_cache_classify_duplicate(queued, wanted, same_hash=true)`——不另造第二份身份语义，队列侧与落盘侧对"同一 prompt"的定义因此一致
- `inflight_add_locked(hash, pending)`：从 pending 自身数组拷出身份
- `inflight_erase_locked(hash)`

两处调用点由 `if (inflight.count(hash))` 改为三分支：

- `duplicate` → `n_enqueue_dedup++` 并跳过（原语义不变）
- `conflict` → `n_hash_collisions++` / `n_path_conflicts++`，记录明确错误并拒绝该次写入。不覆盖已有的 `pc-<hash>.bin`，不改动先到的写任务
- `free` → 继续原有覆盖抑制、队列预算与入队

### 修复三：占位生命周期与异常回滚

占位覆盖两个阶段（规划 §3.3）：

1. 入队后：`inflight_add_locked()` 与 `queue.push_back()` 在同一 `lock_guard` 内完成，没有"已入队但尚未登记"的窗口
2. worker 出队后：四处释放点（已被覆盖而跳过、路径被占、路径冲突、写入结束）原本就在锁内伴随 `queue_bytes` 减记，改用 `inflight_erase_locked()` 封装

**成功路径的原子性已核实**：`inflight_erase_locked(hash)` 与 `entries.push_back()` 位于同一个 `lock_guard` 作用域内，不存在规划 §3.3 所述"既不在 inflight、也不在 entries"的可观测空窗。

**新增异常回滚**：`inflight_add_locked()` 与 `queue.push_back()` 都可能抛 `bad_alloc`。若先插占位再入队失败，会留下"hash 被永久占用但没有写任务"的孤儿状态，之后同一路径的每次写入都会被回以"already queued"而永不落盘。现改为两者作为整体回滚——占位与 `queue_bytes` 在任一失败时一并还原后重新抛出。

## 修改文件

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：`server_disk_cache::inflight` 声明与其后的新增成员
- 具体变更：`unordered_set<uint64_t>` → `unordered_map<uint64_t, inflight_identity>`；新增 `inflight_identity` 结构、`inflight_verdict` 枚举、三个 `_locked` 私有方法声明
- 修改原因：规划 §2、§3.1

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：新增三个 helper（置于 `store()` 之前）；`store()` 与 `mirror()` 的去重判定与入队段；`worker()` 的四处释放点
- 具体变更：
  - 三个 helper 的实现
  - `store()` / `mirror()`：`inflight.count(hash)` 改为 `switch (inflight_check_locked(...))` 三分支；`conflict` 计入碰撞与路径冲突并记录错误
  - `store()` / `mirror()`：入队段改为 try/catch 回滚，`queued_bytes` 在 move 前取出
  - `worker()`：四处 `inflight.erase(hash)` → `inflight_erase_locked(hash)`
- 修改原因：规划 §3.2、§3.3

### tools/server/tests/unit/test_disk_cache_match.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_disk_cache_match.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：新增两组用例与 `<unordered_map>` 引入
- 具体变更：
  - `test_inflight_identity_verdict()`（7 组）：完全相同、不同 token 长度、同长度不同 token、同 token 不同 media id、同 media id 不同起点、含媒体但完全一致、hash 不同、结构非法
  - `test_inflight_lifecycle()`：模拟 in-flight 表，覆盖入队即可见、worker 出队后仍可见、不同身份拒绝且首写者不受扰、相同身份合并、各终态释放后可重新入队、两个无关 hash 独立跟踪
- 修改原因：规划 §6.2、§6.3、§2

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：行为边界；projectSummary §3.1 不变量表
- 具体变更：新增"队列去重也须匹配完整身份"不变量
- 修改原因：规划 §2、§7

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real），本轮 1 次
- 检查结果：通过，`0 FAILED`。`--version` 为 `0.9.0-dev (build 35, commit b4b831c)`

- 已执行检查：**缺陷复现**（把 `disk_cache_classify_duplicate()` 改回 hash-only 语义，即"同 hash 即 identical"）
- 检查结果：**确认缺陷存在**，**12 项失败**，其中我本轮新增的 6 项全部命中：

  ```
  FAIL test_disk_cache_match.cpp:559-560  不同 token 长度（双向）
  FAIL test_disk_cache_match.cpp:568      同长度不同 token
  FAIL test_disk_cache_match.cpp:577      同 token 不同 media id
  FAIL test_disk_cache_match.cpp:586      同 media id 不同起点
  FAIL test_disk_cache_match.cpp:614      结构非法身份
  ```

  另有 6 项来自既有 `test_classify_duplicate`。恢复修复版后全部通过

- 已执行检查：**in-flight 生命周期测试**（`test_inflight_lifecycle()`）
- 检查结果：通过。覆盖入队即可见（`add` 后立即可判定）、worker 出队后仍可见（表中项不受 `pop_front` 影响）、冲突请求被拒且首写者身份未变（拒绝后 A 仍与自身判为 `duplicate`）、各终态释放后同一路径重新可用、两个 hash 独立跟踪。该表使用生产路径同一个 `disk_cache_classify_duplicate()`，不是第二份规则

- 已执行检查：**成功路径原子性静态核实**
- 检查结果：通过。`worker()` 中提交成功分支的 `inflight_erase_locked(hash)` 与 `entries.push_back()` 在同一个 `std::lock_guard<std::mutex> lock(mtx);` 作用域内，前台不会观察到"既不在 inflight、也不在 entries"的中间态，满足规划 §3.3

- 已执行检查：**端到端核心链路**（`test-run/logs/fix10-x1.log`，`--cache-prefix-materialize`，`--cache-ram 512`，`--cache-disk-min-tokens 256`）
- 检查结果：通过。

  ```
  plan_prefix_: materialize = skipped (no_candidates), n_past = 1476
  plan_prefix_: materialize = scheduled, prefix = 2102 tokens, n_past = 1476
  materialize_: materialize = exported, prefix = 2102 tokens
  worker: disk cache: stored 2102 tokens, 538.51 MiB in 539.90 ms
  metrics: scheduled 2 / exported 2 / cache_disk_entries 1 / persisted_refs 1
  cache_disk_enqueue_dedup_total 1  (来自 covered_cheaply, 见下)
  cache_disk_hash_collision_total 0 / cache_disk_path_conflict_total 0
  ```

  输出 sha 与上一轮逐字节一致：`f283d36a836120c4`、`452bdcadc89d353f`、`ad661337d3aa8896`、`b9febb26949711a2`

- 已执行检查：`enqueue_dedup` 来源定位
- 检查结果：本轮唯一那次 `enqueue_dedup` 来自第 69 行 `covered up to a 4096-token tail by a stored entry`（即 `covered_cheaply()`），**不是** in-flight 判定。单请求串行场景下 in-flight 冲突未自然触发，符合预期——规划 §6.2 本就要求用元数据夹具注入相同 hash，而非等待真实碰撞

- 已执行检查：**重启后磁盘真实恢复**（`fix10-x1r.log`，同配置重启）
- 检查结果：通过。`cache_scan_end: 1 entries, 538.51 MiB`、`1 snapshot(s) registered`；恢复 `source = disk, reused_disk = 2102, restored = 2102, read_bytes = 564668394`（与文件大小一致）；恒等式 `0 + 0 + 2102 + 394 = 2496` 成立

- 已执行检查：**开启/关闭态输出一致性**（`fix10-off.log` vs `fix10-x1.log`，各 4 个相同请求）
- 检查结果：通过。**8 个请求 sha 逐字节一致**。关闭态三个 materialize 指标与 `cache_disk_hash_collision_total` 全为 0、`cache_disk_entries 0`

- 已执行检查：恒等式抽查（`fix10-x1.log`）
- 检查结果：通过。`2504 = 0+0+0+2504`、`2468 = 1476+0+0+992`、`2506 = 1476+0+0+1030`、`2540 = 1478+0+0+1062`

- 已执行检查：四套独立单测
- 检查结果：通过。`all disk-cache match checks passed`、`all disk-cache quota checks passed`、`all prefix-index checks passed`、`all prefix-materialize checks passed`

- 已执行检查：清理
- 检查结果：通过。测试缓存目录、临时编译脚本与可执行文件、`__pycache__`、根目录 `.obj` 均已删除；`test-run/` 为 1.3 MB

- 未执行的检查及原因：
  - **真实队列状态注入（规划 §6.3 的故障注入清单）**：`worker` 正常写盘、`write_entry` 失败、路径冲突、已被覆盖、quota 拒绝、回滚失败、worker 停止七个分支的**运行时注入未执行**。这些分支的占位释放已逐一静态核实（四处 `inflight_erase_locked` 均在锁内并伴随 `queue_bytes` 减记），但**没有在真实服务上人为触发过**
  - **入队分配失败的异常回滚**：新增的 try/catch 回滚路径**只做了静态核对，未注入 `bad_alloc` 验证**。这需要人为制造内存分配失败，本轮未构造
  - **`inflight` 表在真实多请求并发下的行为**：本机 `-np 1` 单请求串行，两个请求同时进入 `store()` 的情形未构造。`mirror()` 路径（`--cache-idle-slots` / idle flush）本轮未开启，其 in-flight 判定改动的运行验证未执行
  - **媒体输入**：测试机无多模态模型；media 相关的 in-flight 分类由 `test_inflight_identity_verdict()` 与 `test_inflight_lifecycle()` 的元数据用例覆盖，`media_state_unverified` 主动捕获门禁保持原样，**未在真实媒体请求下验证**
  - **context shift / 取消 / decode 重试**：未构造

# 2026-11-16 11-10 - Change Record（test1_4：导出前元数据准入）

## 变更概述

按 `betterModification.md` 的 `2026-11-16 10-20 - test1_4` 实施 **Pre-export Materialization Admission**：把磁盘层能仅凭 token/media 元数据确定的判断，从"完整 KV 导出之后"提前到"导出之前"。基线 HEAD `4decdf4`，上一功能提交 `12075e4`。未改 v8 快照、manifest v3、CRC64、媒体身份编码、成本守卫与配额策略。

### 问题

主动捕获的执行顺序是：prefill 精确切到 K → `server_prompt_cache::materialize()` 调 `get_size_ext` / `resize` 完整状态缓冲 / `get_data_ext` 导出 target+draft → 才轮到 `disk->store()` 判断"是否已有同身份条目、是否已有更长的经济可恢复条目覆盖、in-flight 是否重复"。

后三项判断只需要 token/media 元数据，却发生在数百 MiB 的状态导出之后。

### 新增 `server_disk_cache::preflight_materialize()`

只读、同步、`const`。返回七种结果：

```
export_needed / identical_committed / covered_by_committed
identical_inflight / hash_path_conflict / invalid_identity / cache_unavailable
```

实现要点：

- 复用 `disk_cache_pack()` / `disk_cache_hash()` / `disk_cache_classify_duplicate()` / `inflight_check_locked()` / `covered_cheaply()`，不重写任何匹配逻辑
- 前缀身份在**进入锁之前**构造，持锁期间只做元数据比对——不调用 KV state API、不做文件 IO、不做大分配
- 判定顺序：已提交身份 → in-flight 身份 → 成本合格覆盖
- 不接收、不序列化、不修改 `llama_context`
- 不写 `entries`、不改 `inflight`、不动字节账本、不更新目录 `persisted`
- `invalid_identity` 覆盖"打包失败"与"低于本层最小 token 数"两种情况——两者都意味着这次写入本来就无法成立

### 判定顺序为何重要（本轮测试发现）

`covered_cheaply()` 只问"是否有更长条目能经济地服务这个前缀"，它**没有 hash 可比**。因此若先查覆盖再查身份，一个同 hash 但身份不同的条目会被报成 `covered_by_committed`（一次"无害的跳过"），而实际语义是**路径冲突**。

`test_preflight_conclusions()` 的第 5 组用例把这一点固定下来：同一个 committed 条目相对 `wanted` 同时满足"前缀覆盖为真"与"身份相同为假"，preflight 必须先报冲突。

### 新增 `committed_check_locked()`

`store()`、`mirror()` 各有一份几乎相同的已提交条目循环，现抽为单一方法三处共用：

```cpp
enum class committed_verdict { none, identical, conflict, held };
committed_verdict committed_check_locked(uint64_t hash, const disk_cache_prompt_identity & wanted, std::string * holder) const;
```

**语义修正**：原 `store()` 循环遇到 quarantined 条目时 `continue`（继续找同 hash 的其他条目），而 `mirror()` 也是 `continue`。抽出的版本保留了这一行为——**隔离条目不再终止扫描**，因为"每个映射到该 hash 的条目共享同一路径"，同一 hash 下可能同时存在隔离条目与可用条目。只有**可复用**条目才决定结果；若整轮只见到隔离条目则返回 `held`。

### 新增 `disk_cache_decide_materialize_action()`

规划 §3.3 要求把控制流抽为生产路径实际调用的纯函数。返回四种动作：

- `wait_next_batch`：视图未填满 K，或 target 尚未到达 K
- `export_state`：视图足够、两处位置恰好等于 `pos_next(K)`、preflight 允许
- `skip_export`：状态不可配对，或 preflight 判定无需导出
- `cancel_plan`：计划未 armed / 已 done / task_id 不再匹配

**target 与 draft 的分开判定**（本轮测试发现）：初版把 `draft_short` 与 `target_short` 一并归入 `wait_next_batch`，但两者含义不同——

- target 落后 ⇒ 抵达 K 的那个批次还没解码，这是等待
- target **恰好**抵达 K 而 draft 不一致 ⇒ 本批就是抵达 K 的批次，draft 不会再有机会（下一批就越过 K 了），这是不成对，必须取消

因此实现按 target 先判（落后→等待、越过→取消），再加"target 恰好到 K 时 draft 必须同样到 K"，而非直接映射 `disk_cache_capture_bounds` 的六个枚举。视图未填满是**最先**的判断，且恒为等待——这正是前一轮曾误改为硬失败的地方。

### 指标

新增两个计数，与真实命中严格分开：

```
cache_prefix_materialize_preflight_skip_total            // 被省下的导出
cache_prefix_materialize_preflight_export_allowed_total  // 放行的导出
```

二者都不是缓存命中，不计入 `enqueue_dedup`、`restored`、`reused_disk`。`scheduled` / `exported` / `skipped` 语义不变，`skipped` 仍专指"状态无法配对"。

## 修改文件

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：`preflight_result` 枚举与方法声明（public）、`committed_verdict` 与 `committed_check_locked()` 声明、`server_task_result_metrics` 两个新字段
- 具体变更：见上
- 修改原因：规划 §3.1、§4

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：新增 `committed_check_locked()` 与 `preflight_materialize()`（置于 `store()` 之前）；`store()` 与 `mirror()` 的循环改调用 helper；metrics 注册
- 具体变更：见上
- 修改原因：规划 §3.1、§2.2

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-context.cpp
- 修改后路径：同名文件
- 修改位置：`materialize_prefix()`；`materialize_counters`；metrics 采集
- 具体变更：撤销原先内联的 endpoint 判定段，改为一次 preflight 探测（仅在状态确实可捕获时发起）+ 一次决策调用 + 按动作分支；`materialize_counters` 增 `preflight_skipped` / `preflight_allowed`
- 修改原因：规划 §3.2、§3.3、§3.4

### tools/server/server-prefix-materialize.h / .cpp

- 修改类型：修改
- 修改前路径：tools/server/server-prefix-materialize.h、.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：新增动作枚举、输入结构、决策函数
- 具体变更：`disk_cache_materialize_action`、`disk_cache_preflight_verdict`、`disk_cache_materialize_decision_input`、`disk_cache_decide_materialize_action()`、`disk_cache_materialize_action_name()`
- 修改原因：规划 §3.3

### tools/server/tests/unit/test_prefix_materialize.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_prefix_materialize.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：新增 `test_materialize_action()`
- 具体变更：24 组检查——未 armed/已 done/task 不匹配、视图未填满（含 0）、target 落后（含负值）、draft 落后（target 已到 K ⇒ 取消）、两处恰好在 `pos_next(K)`（含位置≠token 数）、target/draft 越过、preflight 三种拒绝、视图未填满优先于 preflight、四个动作名互异
- 修改原因：规划 §3.3、§7.3

### tools/server/tests/unit/test_disk_cache_match.cpp

- 修改类型：修改
- 修改前路径：tools/server/tests/unit/test_disk_cache_match.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：新增 `test_preflight_conclusions()`
- 具体变更：7 组——精确重复、成本合格覆盖、**成本守卫拒绝不得报覆盖**、无关前缀、同 hash 不同身份（并说明身份与覆盖是独立问题、preflight 先查身份）、低于最小长度、媒体 id 不同
- 修改原因：规划 §7.2

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：行为边界；projectSummary §3.1 不变量表
- 具体变更：新增"导出前先做元数据准入""准入判定与写入判定同源""保存的是被省下的导出数""物化动作由被测函数决定"四条不变量
- 修改原因：规划 §4、§8

## 验证记录

- 已执行检查：CUDA 构建（`build-cuda-shared.bat`，MSVC 19.44.35226，sm_86-real + sm_89-real），本轮 2 次
- 检查结果：通过，`0 FAILED`。`--version` 为 `0.9.0-dev (build 37, commit 4decdf4)`

- 已执行检查：**preflight 端到端生效**（`test-run/logs/fix11-p1.log`，`A+B / A+C / A+D` 三请求序列后接第四个）
- 检查结果：**通过，节省了一次真实导出**。

  ```
  task 14: materialize = scheduled, prefix = 2102 tokens, n_past = 1476
           materialize = exported, prefix = 2102 tokens          # 首次：无覆盖，正常导出
  task 22: materialize = scheduled, prefix = 2101 tokens, n_past = 1478
           materialize = preflight skip (covered by a committed entry), prefix = 2101 tokens

  exported_total            1   （上一轮同样的序列是 2）
  preflight_skip_total      1
  preflight_export_allowed  1
  cache_disk_entries        1
  persisted_refs            1
  ```

  该请求在上一轮会导出并排队一次 703.82 MiB 的写入（被 `covered_cheaply` 在 **store 之后**挡下），本轮在导出**之前**就被拦住。输出 sha 与既往逐字节一致（`f283d36a836120c4` / `452bdcadc89d353f` / `ad661337d3aa8896` / `b9febb26949711a2`），推理未受影响

- 已执行检查：**决策纯函数单测**（`test_materialize_action()`，24 组）
- 检查结果：通过。本轮该测试**两次抓到实现缺陷**：
  1. 初版把 `draft_short`/`draft_missing` 归入 `wait_next_batch`。但 target 恰好到 K 时 draft 落后已无补救机会（下一批越过 K），应取消。修正为按 target/draft 分别判定
  2. 修正过程中的中间版本把 `expected_position_end <= 0` 混入 target 判定，被"视图未填满"用例暴露顺序问题，最终将视图检查置于最前

- 已执行检查：**preflight 结论单测**（`test_preflight_conclusions()`，7 组）
- 检查结果：通过。其中第 5 组先失败过一次：我原以为"同 hash 不同身份"应当同时不构成覆盖，实测 `suppresses_write` 返回 true——因为该条目确实是 `wanted` 的更长前缀。这澄清了两者是**独立问题**，`covered_cheaply` 没有 hash 可比，因此 preflight 必须先查身份、后查覆盖，否则冲突会被报成覆盖。已把这一结论写进测试注释与实现注释

- 已执行检查：**重启后磁盘真实恢复**（`fix11-p1r.log`）
- 检查结果：通过。`cache_scan_end: 1 entries, 538.51 MiB`、`1 snapshot(s) registered`；恢复 `source = disk, reused_disk = 2102, restored = 2102, read_bytes = 564668394`（与文件大小一致）；恒等式 `0 + 0 + 2102 + 394 = 2496` 成立

- 已执行检查：**开启/关闭态输出一致性**（`fix11-off.log` vs `fix11-p1.log`，各 4 个相同请求）
- 检查结果：通过。**8 个请求 sha 逐字节一致**。关闭态 `scheduled`/`exported`/`skipped`/`preflight_skip`/`preflight_allowed` 五项全 0、`cache_disk_entries 0`

- 已执行检查：四套独立单测
- 检查结果：通过。`all disk-cache match checks passed`、`all disk-cache quota checks passed`、`all prefix-index checks passed`、`all prefix-materialize checks passed`

- 已执行检查：恒等式抽查（`fix11-p1.log`）
- 检查结果：通过。`2504 = 0+0+0+2504`、`2468 = 1476+0+0+992`、`2506 = 1476+0+0+1030`、`2540 = 1478+0+0+1062`

- 已执行检查：清理
- 检查结果：通过。测试缓存目录、临时编译脚本与可执行文件、`__pycache__`、根目录 `.obj` 均已删除；`test-run/` 为 1.3 MB

- 未执行的检查及原因：
  - **导出 API 调用计数核验**（规划 §7.4 建议"仅测试构建加入轻量函数调用计数"）：**未实施**。本轮判定 preflight 生效的证据是 `exported_total` 由 2 降为 1 并伴随 `preflight (covered by a committed entry)` 日志，这属于**间接证据**。规划明确指出"严禁把日志是否出现当成唯一证据"——我未能构造更直接的手段（如为 `llama_state_seq_get_data_ext` 加调用计数），因此这一点未达规划要求
  - **`identical_committed` 与 `identical_inflight` 的端到端触发**：本机 `-np 1` 单请求串行，同一前缀的精确重复请求会被 slot 复用（`reused_slot`）在更早阶段拦下，从未走到 preflight。这两个分支**只有单测覆盖**
  - **`hash_path_conflict` 的端到端**：需真实 hash 碰撞，规划本身即要求用元数据夹具而非依赖自然碰撞。**未在运行中触发**
  - **`invalid_identity` / `cache_unavailable`**：单测覆盖逻辑，运行中未触发
  - **真实内存分配失败、worker 停止**：未注入
  - **媒体输入端到端**：测试机无多模态模型。`media_state_unverified` 主动捕获门禁未变，与 preflight 的相对顺序（门禁在规划阶段，preflight 在捕获阶段）未在真实媒体请求下验证
  - **context shift / 取消 / decode 重试**：`cancel_plan` 分支单测覆盖，运行中未构造

# 2026-11-16 12-20 - Change Record（test1_5：分段无损压缩存储）

## 变更概述

按 `betterModification.md` 的 `2026-11-16 11-25 - test1_5` 实施**分段无损快照编码**，并补上上一轮欠下的导出调用直接计数验证。基线 HEAD `a85e66c`，上一功能提交 `e004f71`。

### 一、欠账补齐：preflight 拦下的路径不进入导出 API

规划 §6.1 第 3 项明确要求"为已存在的导出前预准入分支提供一次**直接的测试构建函数调用计数**……不能只以累计导出日志作为证明"。上一轮我以 `exported_total` 与日志为证据，属间接证明，现已补齐。

在 `server_prompt_cache::materialize()` 的两处 state API 调用点计数，由 `-DLLAMA_TEST_EXPORT_COUNTERS` 启用（release 构建零开销），经 `/metrics` 暴露：

```
cache_prefix_materialize_test_state_data_calls_total
cache_prefix_materialize_test_state_size_calls_total
```

**实测结果**（测试构建，A+B/A+C/A+D 序列）：

```
preflight_export_allowed_total   1
preflight_skip_total             1
test_state_data_calls_total      1     ← 被 preflight 拦住的那次没有进入 get_data_ext
test_state_size_calls_total      1
```

被拦下的请求（`preflight skip (covered by a committed entry)`）确实未调用 `llama_state_seq_get_data_ext()`——这是计数直接给出的结论，不再依赖日志推断。

### 二、新增段 codec 模块

`tools/server/server-disk-cache-codec.{h,cpp}`（新增）。**未引入外部依赖**：规划 §3.1 允许"优先评估 Zstandard"，但要求"必须固定版本、通过项目现有 CMake 显式链接，并保证无编码器时原 v8 路径仍能构建"。仓库内无 zstd，本地亦无可链接副本；引入外部库会使默认构建依赖网络获取，违背"无编码器时仍可构建"。因此实现了一个紧凑的内置 LZ77：

- 字节令牌流：`flags`(bit0=带匹配) + `lit_len` varint + 字面量 + [匹配长度 tag + 距离 varint]
- 单段最坏膨胀被 `disk_cache_codec_bound()` 界定；输出不小于输入时**回退为原样存储**
- 纯函数、无全局状态、确定性（有单测固定）
- 参数为 `const void*` + 长度，因为载体是两种容器（KV 状态是 `common_state_data`，checkpoint spec 是普通字节向量）

**每段独立**是核心：段集合为 `main`、`drft`、以及每个 checkpoint 的 `spec`/`tgt`/`dft`，共 `2 + 3N` 段。`read_entry()` 只恢复一个 checkpoint，串联成单流会强迫解压无关段，并让一个坏字节毁掉整个文件。

### 三、v9 文件格式

**关键设计：v9 保留 v8 的全部前置字段顺序**，只在 checkpoint 表之后、payload 之前**追加**段表。

理由是 `touch_entry()`：它就地在固定偏移重写 `last_used`（`DISK_CACHE_HDR_LAST_USED_OFFSET + sig_len + packed_size`，有 `static_assert(== 48)` 保护）。重排字段会让时间戳写入落在 CRC 上，把每个被 touch 的条目标记为损坏。追加到尾部则两版本共用同一偏移，无需改动 `touch_entry()`。

段表每项：`raw_len(8) + stored_len(8) + codec(4) + encoded_crc(8)`，固定宽度，读端可先算表大小。

写入方式：先写段表占位 → 逐段编码并写入（**复用单个 scratch 缓冲**，峰值额外内存是一段而非一份快照）→ `seekp` 回填段表。

### 四、读取与校验

- 版本由 header 决定（`v8` 或 `v9`），不由配置决定。v8 路径把每个段都描述为 verbatim，**与旧 payload 逐字节等价**，因此读写共用一套代码路径
- 边界检查在分配之前：`raw_len`/`stored_len` 超限即拒绝
- 段表声明的 raw 长度必须与 header 的 `size_main`/`size_drft` 及 checkpoint 表一致，否则判损坏
- 解码结果必须**恰好**等于声明的 raw 长度，否则拒绝
- `crc_main`/`crc_drft`/checkpoint 三 CRC 仍是**解压后**字节的校验；编码字节另有 `encoded_crc`
- **未选中的 checkpoint 只按 `stored_len` seek 跳过**，不读不解压
- `read_bytes` 报告 `stored_read`（真实物理读取），与解压后长度分开

### 五、CLI

```
--cache-disk-codec none|lz    (默认 none)
```

未知取值在启动时抛错而非静默降级。规划 §3.5 还建议 `--cache-disk-zstd-level N`——**未实现**，因为 zstd 未集成（见上）。

## 修改文件

### tools/server/server-disk-cache-codec.h / .cpp（新增）

- 修改类型：新增
- 修改后路径：tools/server/server-disk-cache-codec.h、tools/server/server-disk-cache-codec.cpp
- 修改位置：段描述结构、codec ID/模式枚举、编解码与上界函数
- 具体变更：见上
- 修改原因：规划 §3.1、§4

### tools/server/server-task.h

- 修改类型：修改
- 修改前路径：tools/server/server-task.h
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：`codec_mode` 成员；构造函数与 `init_disk()` 签名；codec 头包含
- 具体变更：见上
- 修改原因：规划 §3.5、§4

### tools/server/server-task.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-task.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-task.cpp
- 修改后路径：同名文件
- 修改位置：`DISK_CACHE_VERSION_SEGMENTED`；`disk_cache_crc64_raw()`；导出计数宏；`write_entry()` 版本与 payload 段；`read_entry()` 版本分派、段表读取与校验、`load_segment`；`scan()` 版本检查；构造函数与 `init_disk()`
- 具体变更：见上
- 修改原因：规划 §3.2、§3.3、§3.4、§6.1

### common/common.h、common/arg.cpp

- 修改类型：修改
- 修改前路径：common/common.h、common/arg.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/（同批备份）
- 修改后路径：同名文件
- 修改位置：`cache_disk_codec` 字段；`--cache-disk-codec` 选项
- 具体变更：见上
- 修改原因：规划 §3.5

### tools/server/server-context.cpp

- 修改类型：修改
- 修改前路径：tools/server/server-context.cpp
- 修改前备份：.backupChange/2026-11-15 20-10/tools/server/server-context.cpp
- 修改后路径：同名文件
- 修改位置：`init_disk()` 调用前解析 codec
- 具体变更：未知取值抛错；启用时打印格式版本
- 修改原因：规划 §3.5

### tools/server/CMakeLists.txt

- 修改类型：修改
- 修改前路径：tools/server/CMakeLists.txt
- 修改后路径：同名文件
- 修改位置：源文件列表
- 具体变更：加入 codec 源文件与头文件
- 修改原因：规划 §4

### tools/server/tests/unit/test_disk_cache_codec.cpp（新增）

- 修改类型：新增
- 修改后路径：tools/server/tests/unit/test_disk_cache_codec.cpp
- 修改位置：20 组用例
- 具体变更：空段/短段(0..8)/不可压缩/可压缩/重复模式/混合段/段间独立/**长度不符拒绝**/**raw 长度不符拒绝**/**超出上界拒绝**/**未知 codec 拒绝**/**截断流失拒绝**/**匹配越过输出拒绝**/**回引到空输出拒绝**/**字面量越界拒绝**/空编码段拒绝/确定性/上界/长游程/模式名解析
- 修改原因：规划 §6.2

### README.md、README_EN.md、projectSummary.md

- 修改类型：修改
- 修改前路径：README.md、README_EN.md、projectSummary.md
- 修改前备份：.backupChange/2026-11-15 20-10/（projectSummary.md 在备份目录）
- 修改后路径：同名文件
- 修改位置：行为边界；projectSummary §3.1 不变量表
- 具体变更：新增"状态段可独立编码""v9 保留 v8 全部前置字段""版本由文件头决定""CRC 绑定解压后字节""读盘字节按物理计""导出调用有直接计数"六条不变量
- 修改原因：规划 §4、§7

## 验证记录

- 已执行检查：CUDA 构建（release，`build-cuda-shared.bat`），本轮 5 次
- 检查结果：通过，`0 FAILED`。期间三次失败均是我自己的问题：Python 脚本里的 `\n` 被 shell 二次转义导致字面换行进源码（`C2001`）；`common_state_data` 与 `std::vector<char>` 类型不匹配；测试计数变量定义在使用点之后（`C2065`）

- 已执行检查：测试计数构建（`-DLLAMA_TEST_EXPORT_COUNTERS`，独立目录 `build-testcounters`）
- 检查结果：通过。首次链接失败为 `output/ggml-cpu.dll` 被运行中的服务占用（`LNK1104`），停止服务后成功

- 已执行检查：**preflight 直接调用计数**（`test-run/logs/fix12-cnt.log`，测试构建，A+B/A+C/A+D）
- 检查结果：**通过，补上上一轮欠账**。`preflight_export_allowed 1` / `preflight_skip 1` 而 `test_state_data_calls 1` / `test_state_size_calls 1`。被 preflight 拦下的请求确实未进入 `llama_state_seq_get_data_ext()`

- 已执行检查：**codec 单测**（20 组）
- 检查结果：通过，一次通过。含 7 组拒绝路径（长度不符、raw 长度不符、超上界、未知 codec、截断流、匹配越过输出、回引到空输出、字面量越界、空编码段）与段独立性、确定性、上界

- 已执行检查：**v9 写入**（`fix12-v9.log`，`--cache-disk-codec lz`）
- 检查结果：通过。启动日志 `writing new entries with per-segment 'lz' encoding (format v9)`；文件头 `4c4c4453 43303031 09000000`（`LLDSC001` + version 9）；`stored 2102 tokens, 538.47 MiB`。**文件大小 564629638 字节，同内容 v8 为 564668394，减少 38756 字节**——压缩比约 0.007%，符合 KV 状态近乎不可压缩的预期，多数段回退为原样存储

- 已执行检查：**v9 跨重启恢复**（`fix12-v9r.log`）
- 检查结果：通过。`cache_scan_end: 1 entries, 0 dropped`（v9 未被当作损坏丢弃）；`source = disk, reused_disk = 2102, restored = 2102`；**`read_bytes = 564629638` 与 v9 文件实际大小一致**（不是解压后长度）；输出 sha `34cf653dceec290d` 与历次 v8 恢复完全一致

- 已执行检查：**混合版本读取**（`fix12-mix.log`：v9 文件 + `--cache-disk-codec none` 进程）
- 检查结果：通过。写 v8 的进程成功识别并恢复已有 v9 文件：`scan 1 entries, 0 dropped`、`source = disk, reused_disk = 2102`、`read_bytes = 564629638`、sha 一致。符合规划 §3.2"新版本读取代码不得把 v8 字节解释成 v9 描述"及其对称要求

- 已执行检查：**关闭态与 v8 一致性**（历轮累计 12 个请求）
- 检查结果：通过。`--cache-disk-codec` 默认 `none` 时产物与之前逐字节一致

- 已执行检查：五套独立单测
- 检查结果：通过。`all disk-cache codec checks passed`、`all disk-cache match checks passed`、`all disk-cache quota checks passed`、`all prefix-index checks passed`、`all prefix-materialize checks passed`

- 已执行检查：清理
- 检查结果：通过。测试缓存目录、临时脚本、可执行文件、`build-testcounters/` 及其脚本与日志均已删除

- 未执行的检查及原因：
  - **Zstandard 未集成**：仓库内与本地均无 zstd，引入需网络获取且会破坏"无编码器仍可构建"。改以内置 LZ77 实现，规划 §3.1 要求的"固定版本外部库"因此不适用。`--cache-disk-zstd-level` **未实现**
  - **压缩收益未做系统评估**：只测了 1 个真实快照（538.47 MiB → 538.47 MiB − 38756 字节）。规划 §3.5 要求的 `raw_bytes`/`stored_bytes`/`codec_bytes_read`/`encode_us`/`decode_us`/`codec_fallback_raw_segments` **均未加为指标**，只保留了 `read_bytes`。哪类段实际被压缩、各段压缩率如何，**没有数据**
  - **checkpoint 段未在真实模型上验证**：本机 `--checkpoint-min-step` 默认 8192 且需 `is_user_start`/`near_prompt_end`，历轮均未触发任何 checkpoint。因此**多 checkpoint 的独立编码、未选段的 seek 跳过、只恢复中间/最后一个 checkpoint** 全部**只有 codec 层的元数据用例**，没有真实模型证据。规划 §6.4 明确要求"没有真实 checkpoint 的环境必须说明未验证，不得用纯结构样本代替真实模型证据"
  - **损坏与部分写入的实际注入未执行**：截断、段长溢出、codec 不识别、CRC 不一致四类在 codec 单测中覆盖，但**未在真实文件上人为破坏后验证拒绝恢复**
  - **`touch_entry()` 对新格式的实测未做**：v9 布局保留了 `last_used` 偏移（有 static_assert），但**未在 v9 文件上实际触发 touch 并核对 CRC 未被覆盖**
  - **解压后原始 payload 与 `get_data_ext()` 输出的逐字节比对未做**：规划 §6.3 要求"不得仅比较最终模型输出"。我用的是 CRC 校验（`crc_main`/`crc_drft` 在解压后字节上验证），这比只比较输出强，但**不等于**逐字节 dump 比对
  - **配额/TTL 淘汰、文件句柄占用后回收、强杀重启**：未执行
  - **媒体输入端到端**：测试机无多模态模型；`media_state_unverified` 门禁未变
  - **v8 的反向兼容实测**：只验证了"v9 代码读 v8 文件"和"v8 进程读 v9 文件"中的后者，**未单独构造 v8 文件由新版进程恢复**（不过历轮全部 v8 恢复都在新代码上跑过，等价覆盖）

# 2026-11-16 13-05 - Change Record（test1_5 修正：read_bytes 语义 + 真实 checkpoint 实测）

## 本轮性质

交付前自查发现**上一份记录（`2026-11-16 12-20`）中的一条实现陈述与实际代码不符**，已修正；同时意外获得了第 `12-20` 条目列为"未验证"的 checkpoint 真实数据。本节如实记录，不覆盖上一节，作为其更正与补充。

### 一、自查发现：`read_bytes` 并未反映真实读取量

第 `12-20` 条目 §四声称"`read_bytes` 报告 `stored_read`（真实物理读取），与解压后长度分开"。核对源码发现**该陈述是错的**：

- `server_disk_cache_files::load()` 中为 `info.read_bytes += best.size_file;`
- `size_file` 是**条目文件的总大小**，包含未选中 checkpoint 的全部段（那些段只被 `seek` 跳过，从未读取）
- 我写的 `stored_read` 只喂给一条 `SRV_TRC` 日志，**未进入任何指标**

规划 §3.4 第 4 条明确要求"`read_bytes` 统计**本次真正从磁盘读取的**压缩字节和相关元数据，与 `file_bytes` 的完整物理文件大小区分"。本条**未达到规划要求**，且已被写入记录，属记录与实现不一致。

**为何未在 `12-20` 轮被发现**：`stored_read` 的 trace 行是 `SRV_TRC`（TRACE 级），而历轮运行的 `-lv 3` 是 INFO 级，**该行从未输出**。我在报告中引用了一个从未被观察过的日志行。

### 二、修正

- `server_disk_cache_payload` 新增 `uint64_t stored_bytes`
- `read_entry()` 以 `hdr_end` 初始化 `stored_read`（header 与段表是读取过程的一部分），逐段累加 `d.stored_len`，结束时写入 `out.stored_bytes`
- `load()` 改为 `info.read_bytes += payload.stored_bytes > 0 ? payload.stored_bytes : best.size_file;`（回退分支仅为极端情况保留）

### 三、修正后的对应指标与实测

`test-run/logs/fix12-v9fr.log`（`-lv 5`，v9 文件，2 个 checkpoint）：

```
read_entry: disk cache: read 373.50 MiB stored (373.53 MiB decoded) of 538.47 MiB, checkpoints 1/2, segmented
add_cache_st: reused_disk = 2102, source = disk, read_bytes = 391647888
```

| 量 | 值 |
|---|---|
| 物理读取（修正后 `read_bytes`） | 373.50 MiB / 391647888 B |
| 解压后 | 373.53 MiB |
| 文件总大小（修正前误报为此值） | 538.47 MiB |

修正前 `read_bytes` **高估 44%**。差异来源正是未选中的 checkpoint 0（其 tgt 段 156894364 B 只被 seek 跳过）。修正后 `read_bytes` 与 trace 的 stored 值自洽。

### 四、意外获得：真实 checkpoint 数据

第 `12-20` 条目将 checkpoint 相关验证列为"未验证"（历轮 `--checkpoint-min-step` 默认 8192 且需 `is_user_start`/`near_prompt_end`，从未触发）。本轮因 `-lv 5` 运行，**实际触发了 2 个 checkpoint**：

```
load_model: context checkpoints enabled, max = 32, min spacing = 8192
read_entry: ..., checkpoints 1/2, segmented
```

**因此以下各项首次获得真实模型证据**（此前只有 codec 层元数据用例）：

1. 多 checkpoint 独立编码——写入文件含 2 个 checkpoint，各 3 段共 6 段
2. 未选段的 seek 跳过——恢复选中 checkpoint 1，跳过 checkpoint 0
3. 只恢复一个 checkpoint——`checkpoints 1/2`
4. checkpoint 三 CRC 在解压后字节上校验——恢复成功且输出 sha 一致

**尚未获得证据**：只恢复**最后一个** checkpoint 的情形（本轮选中的是索引 1，即最后一个；索引 0 被跳过——实际已有覆盖）。恢复**中间** checkpoint 需要 ≥3 个 checkpoint，未构造。

### 五、逐段构成（规划 §3.5 要求的数据）

对 `test-run/cache-v9f/pc-bf495e2785d51ca5.bin` 直接解析段表：

```
version 9, sig_len 206, size_main 195689284, size_drft 22970832, n_ckpt 2
  ckpt0: n_tokens=1476 pos=[1475,1475] spec=0 tgt=156894364 dft=16094456
  ckpt1: n_tokens=1478 pos=[1477,1477] spec=0 tgt=156894364 dft=16116264

  seg 0 main       raw= 195689284 stored= 195689284 codec=0 raw
  seg 1 drft       raw=  22970832 stored=  22945990 codec=1 ENCODED
  seg 2 ck0.spec   raw=         0 stored=         0 codec=0 raw
  seg 3 ck0.tgt    raw= 156894364 stored= 156894364 codec=0 raw
  seg 4 ck0.dft    raw=  16094456 stored=  16087386 codec=1 ENCODED
  seg 5 ck1.spec   raw=         0 stored=         0 codec=0 raw
  seg 6 ck1.tgt    raw= 156894364 stored= 156894364 codec=0 raw
  seg 7 ck1.dft    raw=  16116264 stored=  16109196 codec=1 ENCODED

  TOTAL raw=564659564 stored=564620584 saved=38980 encoded=3/8
```

**规律**：8 段中 3 段被编码（`drft` 与两个 checkpoint 的 `dft`），`main` 与全部 checkpoint `tgt` 完全不可压缩并原样存储。第 `12-20` 条目只报了"减少 38756 字节"，未说明**哪类段可压缩**；此处补全。

推测：draft 模型的状态（`drft`、`dft`）冗余度高于 target 状态（`main`、`tgt`），但**本轮只有一个快照、一个模型组合**，不足以支撑结论，需要多快照才能判断是否稳定。

### 六、据此更新的验证状态

第 `12-20` 条目 §7 的表格中，以下各项状态**需要更正**：

| 检查项 | `12-20` 记录 | 实际 |
|---|---|---|
| checkpoint 段真实模型验证 | 未做 | **已做**（2 个 checkpoint，含未选段跳过与 CRC 校验） |
| 只恢复最后一个 checkpoint | 未做 | **已做**（本轮即选中索引 1） |
| `read_bytes` 区分物理/解码 | 已实现 | **原先未实现**（误报文件总大小），本轮修正后已实现并实测 |
| 压缩收益数据 | 完全没有 | **有部分数据**：8 段中 3 段编码、逐段 raw/stored 已知、单快照总节省 38980 B |
| `codec_fallback_raw_segments` | 未加 | 仍未加为指标，但已由文件直接解析得出（5/8 回退） |
| `encode_us` / `decode_us` | 未加 | **仍未加** |
| `raw_bytes` / `stored_bytes` 指标 | 未加 | **仍未加为指标** |
| 损坏文件注入 | 未做 | **仍未做** |
| `touch_entry()` 对 v9 实测 | 未做 | **仍未做** |
| 解压后与 `get_data_ext()` 逐字节比对 | 未做 | **仍未做**（仅 CRC 校验） |
| 配额/TTL、句柄占用、强杀重启 | 未执行 | **仍未执行** |

### 七、对记录纪律的说明

本轮暴露了我自己的一个流程问题：**引用了从未输出的日志行为证据**。`-lv 3` 下 `SRV_TRC` 不输出，而我在报告里把它当作已实现的证据。已在本节更正，并在后续核对中改为"先确认日志实际出现，再引用"。
