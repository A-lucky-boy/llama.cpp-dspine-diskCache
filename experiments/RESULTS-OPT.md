# opt 树实验记录与迁移决策

日期：2026-11-15 02:45–03:20。硬件与语料同 D1–D4：4070 Ti SUPER (CUDA0) + 3090 (CUDA1)，
Qwen3.8-27B-pi-Q4_K_M + DSpine b16 草稿，`E:\小说\正文.md` 前缀，`temperature=0`。
opt 树按 `experiments/build-opt-cuda.bat` 编译（MSVC 14.44.35207 + CUDA 12.8，`86-real;89-real`，shared）。

## 1. 结论

**移植 MMSQ 回你的 fork，不要把磁盘缓存搬到新树。**

## 2. 关键测量

### 2.1 N=8 矩阵乘（MMSQ 的正面工况）

把 ubatch 强制设成 8，让 prefill 的全部 matmul 都以 N=8 运行，直接对应该模型实测的
7 草稿 + 1 校验批次：

| 配置 | run1 | run2 | 均值 |
|---|---|---|---|
| MMSQ=0 | 175.1 t/s | 171.2 | **173.2** |
| MMSQ=1 | 278.1 t/s | 277.6 | **277.9** |

**1.60×**，与 opt 树 README 声称的 1.63× 吻合。这里排除了一切其他变量（同一棵树、同一 batch
形状、同一次编译），所以这个数是 MMSQ 单独的贡献，不是整包效果。

### 2.2 N=1 解码（无投机，强制 MMSQ 生效）

| 配置 | 6K | 33K | 90K |
|---|---|---|---|
| opt 树，MMSQ=0 | 37.0 t/s / 27.04 ms | 33.1 / 30.23 | 26.3 / 37.98 |
| opt 树，MMSQ=1（`MMSQ_NMIN=1`） | 36.2 / 27.62 | 32.2 / 31.08 | 25.1 / 39.82 |

**N=1 时 MMSQ 反而慢 2%–5%**。这正是源码里 `n_min = 5` 默认值的原因：单列时没有跨列复用，
额外的激活量化白付。结论：MMSQ 只对投机校验批次有效，对普通单 token 解码无益也无害（它根本不进这条路径）。

### 2.3 跨树对比（无投机，排除 MMSQ）

| 上下文 | 你的 fork（d1） | opt 树（MMSQ=0） | 差异 |
|---|---|---|---|
| 6K | 26.76 ms | 27.04 ms | +1% |
| 33K | 32.35 ms | 30.23 ms | -7% |
| 90K | 43.83 ms | 37.98 ms | **-13%** |

opt 树在长上下文还有一块与 MMSQ 无关的收益，来源未定位（候选：`flash_attn_ext_vec` 的
write-after-read 竞态修复、small kernels）。属于后续线索，不影响本次决策。

## 3. 为什么不是"把磁盘缓存搬到新树"

决定性事实：**opt 树加载不了你的 DSpine 草稿模型。**

```
error loading model: done_getting_tensors: wrong number of tensors; expected 73, got 58
```

opt 树只有 `draft-dflash`（DFlash2，73 张量布局），全树 grep 不到任何 `dspine`。
你的草稿是 58 张量的 DSpine 布局。两者虽然都打 `is_dflash2=true`，GGUF 格式不兼容。

叠加以下两条，方案 B 的代价远超收益：

1. **会丢掉投机解码**。D1 实测投机相对无投机是 1.06× / 1.52× / 1.55×；用 1.55× 去换一个只作用于
   矩阵乘的 1.6×，净亏。
2. **磁盘缓存也得跟着搬**。两树 server 侧代码漂移很大：`server-task.cpp` 134 KB vs 67 KB、
   `server-context.cpp` 314 KB vs 230 KB。这不是 cherry-pick，是重做。

## 4. 移植 MMSQ 回来的实际收益预期

按 per_round 口径（与你 D2 的基准直接可比）：

| 上下文 | 现 per_round | 移植后估算 | decode |
|---|---|---|---|
| 6K | 54.9 ms | ~47 ms | 43.9 → ~51 t/s（+16%） |
| 33K | 60.9 ms | ~52 ms | 48.7 → ~57 t/s（+17%） |
| 90K | 73.3 ms | ~66 ms | 37.3 → ~41 t/s（+11%） |

推算依据：N=8 matmul 提速 1.60×，而 per_round 中矩阵乘约 20 ms、attention 与草稿块合计约 35 ms
不随之变化。**不要期待 1.6×**——那是 opt 树把 MMSQ + q4 KV FA + GPU 特征 + small kernels
六项打包后的整包数字。

## 5. 移植范围

- 新增 3 个文件：`mmsq.cu`（19 KB）、`mmsq.cuh`（1.2 KB）、`mmsq-kernels.cuh`（28 KB，header-only 设备码，可直接搬）
- `ggml-cuda.cu`：`should_use_mmsq` 派发（原树 1879 行对应位置）、`mmsq_add_rms_norm_mul` 图融合钩子、
  `mmsq_note_writes` 失效跟踪、每 context 的 xf 缓冲与 split-K 计数器
- `mmq.cu`：派发入口
- `CMakeLists.txt`：加 `mmsq.cu`

风险点：两树 `ggml-cuda.cu` 差 31 KB，图优化循环的结构需要对齐；`mmsq.cu` 依赖 `ctx.curr_stream_no`、
`ctx.mmsq_x_valid` 等 context 字段，要在旧树的 `ggml_backend_cuda_context` 上补齐。

验收标准：编译通过；ubatch=8 的 A/B 能复现 ~1.6×；输出质量可接受（见第 6 节）。

## 6. 数值差异与质量影响

### 6.1 差在哪

| 路径 | 激活量化方式 |
|---|---|
| 本 fork 现有（`q4_K × q8_1`，`mmq-vec-dot.cuh:905`） | 每 32 个值一个 fp16 尺度，用满 ±127 |
| MMSQ | 每 256 个值一个 float 尺度 + 每 32 个值一个 2-bit 指数（步长 = u·2^e），自适应选最细且不溢出的档 |

MMSQ 的激活精度比现有路径粗一档（大值块约 1/64 步长，安静块更差），权重仍是 Q4_K/Q6_K 原样反量化。
所以这不是"换个加法顺序"，是实打实的精度取舍。

### 6.2 实测质量影响

`llama-perplexity`，同一份语料（`E:\小说\正文.md`），`-c 4096 --chunks 4`，`-fa on`，KV q4_0：

| MMSQ | 逐块 PPL | 最终 PPL | 单 pass 耗时 |
|---|---|---|---|
| 0 | 11.7533 / 11.9456 / 13.6284 / 14.4876 | **14.4876 ± 0.492** | 25.15 s |
| 1 | 11.7765 / 11.9421 / 13.6277 / 14.4925 | **14.4925 ± 0.492** | 15.84 s |

- PPL 差 +0.0049（+0.03%），比误差棒 ±0.49 小近百倍，统计上不可区分。
- 同一轮里单 pass 从 25.15 s 降到 15.84 s，**1.59×**，与 N=8 微基准的 1.60× 一致 —— 证明这一轮 MMSQ 确实生效了。

补充对照：默认 `-b 2048` 时两次 PPL 完全相同（14.5695），因为批大小超出 N≤16 窗口，MMSQ 根本没进路径。
这说明上面的差异只存在于 MMSQ 真正生效的工况。

### 6.3 正确解读 greedy 输出不一致

greedy 解码下开关 MMSQ 的输出相似度只有 0.56，但两边都是连贯正文。这是"任一 token 的 logit 有微小扰动就导致后续全变"的放大效应，**不能当作质量指标**。质量看 PPL，不看文本相似度。

### 6.4 剩余风险

本次只测了中文散文 16K token。代码、数学、工具调用这类对激活离群值更敏感的输入未覆盖；
移植落地后建议在真实 WorkBuddy 流量上抽检一轮。

## 7. q4 KV Flash Attention：暂不移植

- 你的 fork 的 `fattn-mma-f16.cuh` 里 `q4` 出现 0 次，opt 树 25 次；且 opt 把整个 MMA FA 重构成
  带 `use_q4` 模板参数的 `ggml_cuda_flash_attn_ext_mma_f16_case_impl`。这不是补丁，是换实现。
- 门槛苛刻：`cc == 860`（只有 3090，4070 Ti SUPER 永远走不到）+ `Q->ne == [256, 8, 24, 1]` 且
  `K->ne[2] == 4`。你的模型正好是 24 Q heads / 4 KV heads / head_dim 256，`ne[1]==8` 要求
  `n_max=7`；再叠加 `GGML_CUDA_FATTN_Q4_MMA=1`（默认关）。
- 收益 +14.5% @91K，但只作用于 3090 上那 62% 的层。
- 列为第二阶段：先把 MMSQ 落地验证，再评估是否值得动 MMA FA。

## 8. 复现命令

```powershell
# 编译 opt 树
cmd /c experiments\build-opt-cuda.bat

# N=8 口径（MMSQ A/B）
powershell -File experiments\start-bench-opt.ps1 -Name n8 -Spec none -Mmsq 0 -Ub 8 -B 8
python experiments\bench_decode.py --tag n8-mm0 --sizes 4000 --phases prefill --no-cache

# 无投机解码
powershell -File experiments\start-bench-opt.ps1 -Name ns -Spec none -Mmsq 1 -Nmin 1
python experiments\bench_decode.py --tag ns-mm1 --sizes 6000,36000,100000
```

注：opt 树在 Windows 上的 `server-sleep-cache.h` 无条件 include `sys/mman.h`，已加 `_WIN32`
分支降级为空实现（备份在 `.backupChange/2026-11-15 02-55/`），否则整棵树编不过。
