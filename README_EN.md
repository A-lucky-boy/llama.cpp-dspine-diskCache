# llama.cpp-dspine-diskCache

English | [简体中文](README_CN.md)

A custom fork of [llama.cpp](https://github.com/ggml-org/llama.cpp) for long-context local inference with **Qwen3.8 27B**, adding two capabilities on top of the original project:

1. **DSpine speculative decoding** (`draft-dflash`) — a lightweight draft model proposes spans of tokens that the target model only has to verify, raising decode throughput
2. **Disk-based second-level prefix cache** — KV state for long prefixes is written to disk and restored across restarts and across sessions, skipping repeated prefill

Target hardware: a single machine with two NVIDIA GPUs (RTX 3090 + RTX 4070 Ti SUPER), deployed locally together with a WorkBuddy proxy for usage accounting.

---

## 1. Implemented features

### 1.1 Qwen3.8 27B DSpine speculative decoding

A DSpine draft model runs alongside the target model. It proposes a span of tokens in one go, and the target model only verifies them, which reduces the number of expensive autoregressive decode steps.

| Item | Value |
|---|---|
| Inference type | `--spec-type draft-dflash` |
| Target model | `localModel/Qwen3.8-27B-pi-Q4_K_M.gguf` (Q4_K_M) |
| Draft model | `localModel/Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf` (Q4_K_M) |
| Vision projector | `localModel/mmproj-Qwen3.8-27B-pi-BF16.gguf` |
| Draft length | n_max = 15, n_min = 0 |
| Block parameters | block_size = 16, mask_token_id = 248070, n_extract = 5 |
| Sampling | sample_from_anchor = true, p_min = 0.00 |
| GPU placement | target model split across CUDA0/CUDA1 (layers 38/62), draft model entirely on CUDA1 |
| Draft KV | q8_0 / q8_0 |

Measured decode throughput is about **10 t/s** (observed in production logs with a 120k-token context). No A/B baseline with DSpine disabled was run in this branch; run your own benchmark if you need a precise speedup figure.

### 1.2 Disk-based second-level prefix cache (new in this branch)

Upstream llama.cpp keeps its prompt cache in memory only, so it is lost when the process exits. This branch adds a disk tier underneath it: **KV state for long prefixes is written to disk and restored on restart, skipping prefill entirely.**

Key properties:

- **Four write-out triggers**: migration when the RAM cache evicts an entry; mirroring the current session after N seconds of idle time; flushing on graceful exit; trimming to the size limit at startup
- **LRU ring eviction**: once `--cache-disk-max` is exceeded, the oldest entry is deleted automatically — no manual cleanup
- **On-demand checkpoint reads**: a restore only reads the newest context checkpoint that does not cross the divergence point, cutting I/O by about 35%
- **Automatic invalidation**: entries are bound to the model path, file size and mtime, draft model, KV types, ctx size and parallelism; stale entries are dropped when the model or the parameters change
- **Background writes**: serialization and disk I/O happen on a separate thread and never block inference

Measured numbers (RTX 3090 + RTX 4070 Ti SUPER):

| Metric | Result |
|---|---|
| Restore of 17,765 tokens | **0.9–1.0 s** (a full prefill needs 8.85 s) |
| End to end | 8.85 s → **1.29 s, about 6.7×** |
| Idle mirroring | triggers normally while RAM is far from full; a session survives a restart |
| Entry size | about 235 MiB fixed overhead + 18 KB/token (checkpoints and draft state included) |
| Switching between sessions | returning to an earlier session takes **0.3–0.5 s**, hit rate 99.97% |

### 1.3 Other

- WorkBuddy-compatible chat template (`localModel-workbuddy-chat-template.jinja`)
- tokens-proxy / tokens-dashboard usage accounting (deployment-side tools, not part of this repository)

---

## 2. Building

```
Toolchain:  MSVC 14.44.35207 (v143) + Ninja + CUDA 12.8.61
Archs:      CMAKE_CUDA_ARCHITECTURES=86-real;89-real   (RTX 3090 / RTX 4070 Ti SUPER)
Options:    Release, BUILD_SHARED_LIBS=ON, GGML_CUDA=ON, LLAMA_CURL=OFF, GGML_NATIVE=OFF
```

> **Note**: nvcc 12.8 rejects MSVC 14.50 (`_MSC_VER=1950`), so the toolset version must be pinned:
> `call vcvars64.bat -vcvars_ver=14.44.35207`
>
> `build-cuda128.bat` locates Visual Studio automatically through vswhere. If your VS is not
> registered with the installer, set the `VCVARS` environment variable to the full path of
> `vcvars64.bat` first. The toolset version can be overridden with `VCVARS_VER`.

```bat
:: one-shot build (from the repository root, output goes to build\bin\)
build-cuda128.bat
```

To build manually:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CUDA_ARCHITECTURES=86-real;89-real ^
      -DGGML_CUDA=ON -DBUILD_SHARED_LIBS=ON
cmake --build build --config Release --parallel 16
```

The output is a set of exe + dll files under `build/bin/` (including `llama-server.exe` and `llama-server-impl.dll`); copy the whole directory to deploy.

---

## 3. Deployment scripts

The deployment scripts `start-localModel-dspine.ps1` / `stop-localModel.ps1` live in the deployment directory — the directory that holds the exe, dll and model files, referred to as `<deploy dir>` below.

### Start

```powershell
powershell -ExecutionPolicy Bypass -File <deploy dir>\start-localModel-dspine.ps1
```

What the script does: check the WorkBuddy template → verify that the model files exist → launch llama-server in the background → write the PID to `localModel.pid`.

### Stop

```powershell
powershell -ExecutionPolicy Bypass -File <deploy dir>\stop-localModel.ps1
```

Matches the process by PID and command line, force-terminates it, then removes the PID file.

### Verify

```bash
curl http://127.0.0.1:18080/health
# {"status":"ok"}
```

Logs: `localModel-server.log` (stdout), `localModel-server-error.log` (stderr — model loading and cache hits show up here).

### Key parameters in the script

| Parameter | Value | Description |
|---|---|---|
| `-m` | `Qwen3.8-27B-pi-Q4_K_M.gguf` | target model |
| `--mmproj` | `mmproj-Qwen3.8-27B-pi-BF16.gguf` | vision projector (CUDA0) |
| `--spec-type` | `draft-dflash` | DSpine speculative decoding |
| `--spec-draft-model` | `Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf` | draft model |
| `-dev` / `-sm` / `-ts` | `CUDA0,CUDA1` / `layer` / `38,62` | layer-wise placement of the target model |
| `-c` | `262144` | context length |
| `-ctk` / `-ctv` | `q4_0` / `q4_0` | KV quantization |
| `--kv-unified` | — | unified KV cache (idle slots release VRAM) |
| `--cache-ram` | `25600` | in-memory prompt cache limit (MiB) |
| `--cache-idle-slots` | — | publish idle slots to the prompt cache when a new task starts |
| `--cache-disk` | `<cache dir>` | disk cache directory (pick your own; SSD recommended) |
| `--cache-disk-max` | `102400` | disk cache size limit (MiB, LRU ring eviction) |
| `--cache-disk-min-tokens` | `2048` | prefixes shorter than this are not cached |
| `--cache-disk-idle-flush` | `60` | mirror in-memory entries to disk after 60 s of idle time |
| port / API key | `18080` / `sk-0` | external interface |

---

## 4. Disk cache tuning

| Parameter | Default | Guidance |
|---|---|---|
| `--cache-disk PATH` | off | required; point it at a directory on an SSD |
| `--cache-disk-max N` | 65536 | size it to your disk. An entry is about 235 MiB + 18 KB/token, so a 100k-token session is roughly 2 GB |
| `--cache-disk-min-tokens N` | 2048 | lower it to cache shorter prefixes, but each entry carries about 235 MiB of fixed overhead — not worth going below 1024 |
| `--cache-disk-idle-flush N` | 60 | 0 = write out only on eviction; lower values are more durable but write more |

**Hit rules** (important):

- Matching is by **token-level longest common prefix**; the prefix must be identical from the very first token
- The matched portion must be at least 25% of the cached entry's own length
- Change a single word in the middle and everything after that point is recomputed
- Context checkpoints are only created at **message boundaries**, so a divergence at a message boundary reuses the most; editing inside one long message reuses little

Practical optimization: keep the system prompt, tool definitions and few-shot examples static and place them first; put dynamic content (timestamps, user names) at the end; in multi-turn conversations only append, never rewrite.

---

## 5. Known limitations

1. Prefixes containing images or audio are not cached (media chunks are not persisted)
2. The first request always pays a full prefill; the cache only helps repeated prefixes
3. `stop-localModel.ps1` force-terminates the process and therefore does not trigger the exit flush; persistence relies on idle mirroring (60 s) or eviction
4. `--n-ctx-checkpoints` caps out at 32 checkpoints, each about 150 MiB, so entries grow large with long contexts
5. A single slot (`-np 1`) serves requests serially; concurrent requests are queued

---

## 6. Directory layout

```
llama.cpp-dspine-diskCache/  this repository (source)
  src/ ggml/ tools/ common/  the llama.cpp core
  include/ vendor/ cmake/    headers, third-party dependencies, CMake modules
  tests/ examples/ pocs/     tests and examples (built by default)
  build-cuda128.bat          one-shot build script

deployment directory (on the target machine, not in this repository)
  llama-server.exe + *.dll   build output
  localModel/                GGUF models + chat template
  <cache dir>\               disk cache entries
  start-localModel-dspine.ps1 / stop-localModel.ps1
  tokens-proxy.py / tokens-dashboard.html
  SHA256SUMS.txt             binary checksums
```

---

## 7. Changes relative to upstream

| File | Change |
|---|---|
| `common/common.h` | 4 new disk cache parameters |
| `common/arg.cpp` | matching command line options |
| `tools/server/server-task.h/.cpp` | `server_disk_cache`: write-out, restore, LRU, on-demand checkpoint reads, idle mirroring |
| `tools/server/server-context.cpp` | cache initialization + model signature, idle detection |
| `tools/server/server-queue.h/.cpp` | idle tick callback hook |

---

## License

MIT, same as upstream llama.cpp. See [LICENSE](LICENSE).
