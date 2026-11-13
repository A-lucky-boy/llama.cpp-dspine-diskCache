# llama.cpp-dspine-diskCache

[English](README_EN.md) | 简体中文

基于 [llama.cpp](https://github.com/ggml-org/llama.cpp) 的定制分支，面向 **Qwen3.8 27B** 长上下文本地推理，在原项目之上叠加了两项能力：

1. **DSpine 投机解码加速**（`draft-dflash`）— 用轻量草稿模型换取整段接受率，提升解码吞吐
2. **磁盘二级前缀缓存** — 长前缀的 KV 状态落盘，跨重启、跨会话复用，跳过重复的 prefill

目标硬件：NVIDIA 双卡（RTX 3090 + RTX 4070 Ti SUPER），单机本地部署，配合 WorkBuddy 代理做计费统计。

---

## 1. 已实现功能

### 1.1 Qwen3.8 27B DSpine 投机解码

在目标模型之外加载一个 DSpine 草稿模型，由它批量预测一段 token，目标模型只做校验，从而减少昂贵的自回归解码次数。

| 项 | 值 |
|---|---|
| 推理类型 | `--spec-type draft-dflash` |
| 目标模型 | `localModel/Qwen3.8-27B-pi-Q4_K_M.gguf`（Q4_K_M） |
| 草稿模型 | `localModel/Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf`（Q4_K_M） |
| 视觉投影 | `localModel/mmproj-Qwen3.8-27B-pi-BF16.gguf` |
| 草稿长度 | n_max = 15，n_min = 0 |
| 块参数 | block_size = 16，mask_token_id = 248070，n_extract = 5 |
| 采样 | sample_from_anchor = true，p_min = 0.00 |
| 显存放置 | 目标模型跨 CUDA0/CUDA1 分层（38/62），草稿模型整体在 CUDA1 |
| 草稿 KV | q8_0 / q8_0 |

解码吞吐实测约 **10 t/s**（12 万 token 上下文下的生产日志观测值）。本分支未做「关闭 DSpine」的对照基准，如需精确加速比请另行跑基准。

### 1.2 磁盘二级前缀缓存（本分支新增）

原版 llama.cpp 的提示缓存只在内存里，进程退出即失效。本分支在它下面挂了一级磁盘缓存：**长前缀的 KV 状态写入磁盘，重启后直接恢复，跳过 prefill**。

核心特性：

- **四个落盘时机**：RAM 缓存淘汰时迁移；空闲 N 秒后镜像当前会话；优雅退出时刷盘；启动时按容量修剪
- **LRU 循环覆盖**：超出 `--cache-disk-max` 自动删除最旧条目，无需人工清理
- **按需读取检查点**：恢复时只读取分歧点之前最新的那个上下文检查点，IO 减少约 35%
- **自动失效**：条目绑定「模型路径 + 文件大小 + mtime + 草稿模型 + KV 类型 + ctx + 并行数」，换模型、改参数后旧条目自动丢弃
- **后台写入**：序列化与落盘在独立线程，不阻塞推理

实测数据（RTX 3090 + RTX 4070 Ti SUPER）：

| 指标 | 结果 |
|---|---|
| 恢复 17,765 token | **0.9–1.0 s**（全量 prefill 需 8.85 s） |
| 端到端 | 8.85 s → **1.29 s，约 6.7×** |
| 空闲镜像 | RAM 未满时正常触发，切换会话后重启仍可命中 |
| 条目体积 | 约 235 MiB 固定开销 + 18 KB/token（含检查点与草稿状态） |
| 多会话切换 | 切回历史会话 **0.3–0.5 s**，命中率 99.97% |

### 1.3 其他

- WorkBuddy 兼容的 chat template（`localModel-workbuddy-chat-template.jinja`）
- tokens-proxy / tokens-dashboard 计费统计（部署侧工具，不在本仓库）

---

## 2. 构建

```
工具链:  MSVC 14.44.35207 (v143) + Ninja + CUDA 12.8.61
架构:    CMAKE_CUDA_ARCHITECTURES=86-real;89-real   (RTX 3090 / RTX 4070 Ti SUPER)
配置:    Release, BUILD_SHARED_LIBS=ON, GGML_CUDA=ON, LLAMA_CURL=OFF, GGML_NATIVE=OFF
```

> **注意**：CUDA 12.8 的 nvcc 不接受 MSVC 14.50（`_MSC_VER=1950`），必须固定工具集版本：
> `call vcvars64.bat -vcvars_ver=14.44.35207`
>
> `build-cuda128.bat` 会通过 vswhere 自动定位 Visual Studio；若你的 VS 未注册到安装器，
> 先设置环境变量 `VCVARS` 指向 `vcvars64.bat` 的完整路径。工具集版本可用 `VCVARS_VER` 覆盖。

```bat
:: 一键构建（仓库根目录，产物在 build\bin\）
build-cuda128.bat
```

如需手动构建：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CUDA_ARCHITECTURES=86-real;89-real ^
      -DGGML_CUDA=ON -DBUILD_SHARED_LIBS=ON
cmake --build build --config Release --parallel 16
```

产物为 `build/bin/` 下的 exe + dll（含 `llama-server.exe` 与 `llama-server-impl.dll`），部署时整目录拷贝即可。

---

## 3. 部署脚本用法

部署脚本 `start-localModel-dspine.ps1` / `stop-localModel.ps1` 位于部署目录（即存放 exe、dll 和模型的目录，下文以 `<部署目录>` 表示）。

### 启动

```powershell
powershell -ExecutionPolicy Bypass -File <部署目录>\start-localModel-dspine.ps1
```

脚本做的事：检查 WorkBuddy 模板 → 校验模型文件存在 → 后台拉起 llama-server → 写 PID 到 `localModel.pid`。

### 停止

```powershell
powershell -ExecutionPolicy Bypass -File <部署目录>\stop-localModel.ps1
```

按 PID + 命令行特征匹配后强制终止，清理 PID 文件。

### 验证

```bash
curl http://127.0.0.1:18080/health
# {"status":"ok"}
```

日志：`localModel-server.log`（stdout）、`localModel-server-error.log`（stderr，模型加载与缓存命中信息都在这里）。

### 脚本内关键参数

| 参数 | 值 | 说明 |
|---|---|---|
| `-m` | `Qwen3.8-27B-pi-Q4_K_M.gguf` | 目标模型 |
| `--mmproj` | `mmproj-Qwen3.8-27B-pi-BF16.gguf` | 视觉投影（CUDA0） |
| `--spec-type` | `draft-dflash` | DSpine 投机解码 |
| `--spec-draft-model` | `Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf` | 草稿模型 |
| `-dev` / `-sm` / `-ts` | `CUDA0,CUDA1` / `layer` / `38,62` | 目标模型分层放置 |
| `-c` | `262144` | 上下文长度 |
| `-ctk` / `-ctv` | `q4_0` / `q4_0` | KV 量化 |
| `--kv-unified` | — | 统一 KV 缓存（空闲槽可回收显存） |
| `--cache-ram` | `25600` | 内存提示缓存上限（MiB） |
| `--cache-idle-slots` | — | 新任务开始时把空闲槽存入缓存 |
| `--cache-disk` | `<缓存目录>` | 磁盘二级缓存目录（自选，建议放 SSD） |
| `--cache-disk-max` | `102400` | 磁盘缓存上限（MiB，LRU 循环覆盖） |
| `--cache-disk-min-tokens` | `2048` | 小于该长度的前缀不缓存 |
| `--cache-disk-idle-flush` | `60` | 空闲 60 秒后把内存条目镜像落盘 |
| 端口 / API key | `18080` / `sk-0` | 对外接口 |

---

## 4. 磁盘缓存参数调优

| 参数 | 默认 | 调整建议 |
|---|---|---|
| `--cache-disk PATH` | 关闭 | 必填，指向 SSD 上的目录 |
| `--cache-disk-max N` | 65536 | 按盘容量设。条目 ≈ 235 MiB + 18 KB/token，10 万 token 会话约 2 GB |
| `--cache-disk-min-tokens N` | 2048 | 调小可缓存更短前缀，但每条固定开销约 235 MiB，不建议小于 1024 |
| `--cache-disk-idle-flush N` | 60 | 0 = 仅淘汰时落盘；调小更持久但写入更多 |

**命中规则**（重要）：

- 按 **token 级最长公共前缀** 匹配，前缀必须从第一个 token 开始完全一致
- 命中部分需 ≥ 缓存条目自身长度的 25%
- 中途改一个字，从那里往后全部重新计算
- 上下文检查点只在**消息边界**生成，所以分叉点落在消息边界上时复用最充分；在单条长消息内部修改则收益有限

优化实践：系统提示、工具定义、few-shot 全部静态化并放在最前；动态内容（时间戳、用户名）后置；多轮对话只追加不改写。

---

## 5. 已知限制

1. 含图片/音频的前缀不进磁盘缓存（媒体 chunk 不持久化）
2. 首次请求必然全量 prefill，缓存只对重复前缀生效
3. `stop-localModel.ps1` 是强制终止，不会触发退出刷盘；依赖空闲镜像（60 s）或淘汰落盘
4. `--n-ctx-checkpoints` 上限 32，每个检查点约 150 MiB，长上下文下条目偏大
5. 单 slot（`-np 1`）串行服务，并发请求会排队

---

## 6. 目录说明

```
llama.cpp-dspine-diskCache/  本仓库（源码）
  src/ ggml/ tools/ common/  llama.cpp 主体
  include/ vendor/ cmake/    头文件、第三方依赖、CMake 模块
  tests/ examples/ pocs/     测试与示例（默认随构建生成）
  build-cuda128.bat          一键构建脚本

部署目录（部署机上，不在本仓库）
  llama-server.exe + *.dll   编译产物
  localModel/                GGUF 模型 + chat template
  <缓存目录>\                磁盘缓存条目
  start-localModel-dspine.ps1 / stop-localModel.ps1
  tokens-proxy.py / tokens-dashboard.html
  SHA256SUMS.txt             二进制校验
```

---

## 7. 本分支相对上游的改动

| 文件 | 改动 |
|---|---|
| `common/common.h` | 新增 4 个磁盘缓存参数 |
| `common/arg.cpp` | 新增对应命令行选项 |
| `tools/server/server-task.h/.cpp` | `server_disk_cache`：落盘、恢复、LRU、按需读检查点、空闲镜像 |
| `tools/server/server-context.cpp` | 缓存初始化 + 模型签名、空闲判定 |
| `tools/server/server-queue.h/.cpp` | 空闲回调钩子 |

---

## License

MIT，与上游 llama.cpp 一致。详见 [LICENSE](LICENSE)。
