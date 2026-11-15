# llama.cpp-dspine-diskCache

English | [简体中文](README_CN.md)

A fork of [llama.cpp](https://github.com/ggml-org/llama.cpp) for long-context local inference of **Qwen3.8 27B**: speculative decoding across two consumer GPUs, plus a KV state tier that lets a long prefix survive restarts and sessions.

Target hardware: RTX 4070 Ti SUPER 16GB + RTX 3090 24GB, single machine, layer split. It runs as the backend of a local agent.

## Features

| Capability | What it does |
|---|---|
| **DSpine speculative decoding** | `--spec-type draft-dflash`; a 1.2 GiB draft model predicts blocks, the target verifies them in one pass |
| **Two-tier prefix cache** | RAM prompt cache plus an on-disk KV state tier; a long prefix hit skips prefill and survives a restart |
| **Integrity** | Config signature, CRC64-ECMA checksums, startup index, corrupt-entry isolation, LoRA awareness |
| **Write suppression** | Skips rewriting an entry that already covers all but a short tail, which removes GB-scale duplicate writes on a mechanical disk |
| **Observability** | Leveled logs for hits, restores, writes, skipped writes, evictions and draft decode shape |

The cache tier never changes computed results: an entry holds the KV bytes that were produced in the first place, and a hit reads them back unchanged.

---

## 1. Requirements

| Item | Version |
|---|---|
| Compiler | MSVC 14.44.35207 (v143 toolset) |
| CUDA | 12.8 |
| Build | CMake ≥ 3.14 + Ninja |
| GPU arch | `86-real;89-real` (RTX 3090 / RTX 4070 Ti SUPER) |

`nvcc` from CUDA 12.8 rejects MSVC 14.50, so the compiler path is pinned to 14.44.

Model files are not distributed with the repository; they live in `localModel/` under the deployment directory:

```
Qwen3.8-27B-pi-Q4_K_M.gguf          # target: 64 layers, head_count 24 / kv 4 / head_dim 256
Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf  # DSpine draft (block_size 16)
mmproj-Qwen3.8-27B-pi-BF16.gguf     # vision projector, optional
```

## 2. Build

```bat
build-cuda-shared.bat
```

The script configures and incrementally builds. **Artifacts land directly in `output/`** — `CMakeLists.txt` points `CMAKE_RUNTIME_OUTPUT_DIRECTORY` and `CMAKE_LIBRARY_OUTPUT_DIRECTORY` there, so there is no copy step. The script only adds the CUDA runtime DLLs, which do not come out of the build.

```
config  Release, BUILD_SHARED_LIBS=ON, GGML_CUDA=ON,
        LLAMA_CURL=OFF, LLAMA_BUILD_TESTS=OFF, GGML_NATIVE=OFF
```

`output/` is the deployment layout: 47 `.exe` and 19 `.dll` files, kept in version control as an empty directory (`.gitkeep`). The three that matter:

| File | Role |
|---|---|
| `llama-server.exe` | server launcher |
| `llama-server-impl.dll` | server implementation; disk cache and LoRA guard live here |
| `llama-common.dll` | common parameters and CLI parsing |

`llama-server.exe --version` / `--help` verifies the artifacts and argument registration. To deploy, copy the exe/dll set over an existing install; no model or flag changes are needed.

## 3. Running

Full production arguments (`--cache-disk` points at the cache disk; the rest are fixed):

```bat
llama-server.exe ^
  -m localModel\Qwen3.8-27B-pi-Q4_K_M.gguf ^
  -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fit off -fa on ^
  -np 1 -c 262144 -ctk q4_0 -ctv q4_0 -t 8 -tb 16 --backend-sampling ^
  --spec-type draft-dflash ^
  --spec-draft-model localModel\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf ^
  --spec-draft-n-max 6 --spec-draft-n-min 0 ^
  --spec-draft-device CUDA0 --spec-draft-ngl all ^
  --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
  --kv-unified --cache-prompt --cache-ram 25600 --cache-idle-slots ^
  --cache-disk E:\llama.cpp.cache --cache-disk-max 204800 ^
  --cache-disk-min-tokens 2048 --cache-disk-idle-flush 60 ^
  --mmproj localModel\mmproj-Qwen3.8-27B-pi-BF16.gguf --mmproj-device CUDA0 ^
  --jinja --reasoning on --reasoning-format deepseek-legacy --reasoning-preserve ^
  -b 4096 -ub 1024 -lv 3 --api-key sk-0 --host 0.0.0.0 --port 18080
```

The start/stop scripts used in production are not part of this repository. On top of the flags above they export the GGUF chat template with a `reasoning_effort` compatibility shim, check that the model files exist, launch in the background and write a PID file. Readiness check:

```bash
curl http://127.0.0.1:18080/health
```

stdout and stderr are written to separate logs; model loading and cache behaviour go to stderr.

## 4. Placement and context

| Item | Current setting |
|---|---|
| Target placement | `-dev CUDA0,CUDA1 -sm layer -ts 38,62` |
| Draft placement | `--spec-draft-device CUDA0`, `--spec-draft-ngl all` |
| Vision projector | `--mmproj-device CUDA0` |
| Draft length | `--spec-draft-n-max 6`, `--spec-draft-n-min 0` |
| KV quantization | target `q4_0/q4_0`, draft `q8_0/q8_0` |
| Context | `-c 262144`, `--kv-unified`, `-np 1` |

The split is fixed at `38,62`. The CUDA backend has no split-buffer support, so `-sm row` is rejected at model load; layer split is the only multi-GPU option. With 672 GB/s (4070 Ti SUPER) and 936 GB/s (3090) the bandwidth optimum is about 42/58, which is equivalent to `38,62`; 30/70 and 45/55 showed no gain.

The draft model sits on CUDA0 rather than CUDA1: a 1.26 GiB draft sharing the 3090 with 62% of the target layers contends for the same HBM. Moving it to the 4070 Ti SUPER cuts per-round time by 2–5% and frees 2.1 GiB on the 3090. The cost is a 1–2% slower prefill: the draft also processes the whole prompt during prefill to build its own KV, and the 4070 Ti SUPER has fewer SMs (66) than the 3090 (82), so that phase is compute-bound.

## 5. Decode performance

`--spec-draft-n-max 6`, single slot, 128-token decode, temperature 0. Corpus is long Chinese prose (1.406 chars/token); the context size is set by how much of the corpus prefix is used:

| Context | Speculative | No spec | Gain | Per round | Acceptance / mean len |
|---|---|---|---|---|---|
| ~6K | 43.9 t/s | 37.4 t/s | 1.06× | 54.9 ms | 0.09 / 2.4 |
| ~33K | 48.7 t/s | 30.9 t/s | 1.52× | 60.9 ms | 0.14 / 3.2 |
| ~90K | 37.3 t/s | 22.8 t/s | 1.55× | 73.3 ms | 0.12 / 2.8 |

How to read this: the t/s figure swings with the draft acceptance rate, so compare configurations by **per-round time** (`predicted_ms / (draft_n / n_max)`), which does not depend on it; `ms/token = per-round time / mean len`.

Prefill throughput (cold, one full pass): about 1.3–1.4K t/s at 6K and 1.25–1.4K t/s at 90K; a full 123K-token prefill takes about 98 s. UB=2048 is 10% slower under a two-GPU layer split, so `-ub 1024` is the current optimum.

## 6. Two-tier prefix cache

### 6.1 Data flow

```
request → RAM prompt cache (--cache-ram)
            ├─ hit: reuse the KV directly
            └─ evicted / idle mirror → on-disk write queue (background thread)
                                        └─ atomic write pc-<hash>.bin → update pc-index.dat
miss in RAM → disk tier prefix lookup → restore KV and checkpoints → resume at the divergence point
```

An entry reaches the disk when a RAM entry is evicted, when the session has been idle for N seconds, through the background queue, or when the queue is drained on exit. Serialization and writing happen on a separate thread and do not block inference; the writer yields 10 ms every 256 MiB.

### 6.2 Entry format and integrity

| Part | Content |
|---|---|
| Entry file | `pc-<hash>.bin`, format version 7; header holds magic, version, config signature, packed length, main/drft sizes, checkpoint count, timestamps and CRCs |
| Payload | `main` (target KV), `drft` (draft KV), and `data_spec / data_tgt / data_dft` per checkpoint |
| Packed metadata | token id sequence plus media placeholder ranges and identifiers |
| Checksums | CRC64-ECMA (reflected polynomial `0xC96C5795D7870F42`), slice-by-8; covers main, drft and all three checkpoint blobs |
| Startup index | `pc-index.dat`, format version 2; hash, `size_file`, `size_main`, `size_drft`, `last_used` and packed metadata |
| Config signature | target/draft model path + `size@mtime`, target K/V types, `n_ctx`, `n_parallel`, draft K/V types, `kv_unified`, plus `mmproj:path@size@mtime` when a projector is loaded |

The index is validated first at startup: only when magic, version, signature length and content all match, and every record can be unpacked with its hash recomputed, is the fast path taken; otherwise the scanner falls back to per-file header checks. The index is written as temp-file-plus-rename and rewritten whenever the entry set changes.

### 6.3 Lookup and restore

- Matching is by **longest common token prefix**, longest wins; the shared part must be at least `--cache-disk-min-tokens`.
- A restore is gated on entry size not exceeding `lcp × 768 KiB` (`DISK_CACHE_MAX_BYTES_PER_TOKEN`), which makes `best_lcp ≥ 1` always true; only the checkpoint that does not cross the divergence point is read, the rest are skipped by seek.
- After a restore, `prompt.tokens` keeps the shared prefix, `seq_rm` drops the KV tail, and `--cache-idle-slots` keeps managing VRAM.
- `load()` contract: any return before the ctx state has been modified is `true` (KV and prompt are still consistent); a failure after KV has been written returns `false`, and the caller clears the slot and re-prefills.
- Failures are split: entry read failure, draft state without a draft context, packed decode failure and media metadata mismatch delete the entry; possibly transient failures (VRAM pressure) keep it for a retry.

### 6.4 Invalidation, eviction and write suppression

- Signature mismatch, TTL expiry and corruption discard entries at scan or access time.
- Exceeding `--cache-disk-max` removes entries by smallest `last_used`; startup also trims to the current limit and removes leftover `.tmp` files.
- A new entry that subsumes shorter stored prefixes causes those shorter entries to be deduplicated.
- **Write suppression**: when a stored entry already shares all but a short tail of the prompt being written, the write is skipped. Rewriting a 2.7 GiB entry takes about 18 s at 150 MiB/s, while re-prefilling a 4k-token tail takes about 3 s. The tolerated lag is bounded by `DISK_CACHE_MAX_PREFIX_LAG_TOKENS` (4096 tokens). The test uses the longest common prefix rather than a strict token prefix, because appending text re-merges the tokens around the cut.
- The pending queue is bounded by `--cache-disk-queue-max` (MiB) and 8 pending entries.
- Hit timestamps are updated in memory and, on graceful shutdown, written to the index before the entry headers, preserving LRU order across restarts.

### 6.5 LoRA awareness

`lora_blocks_cache()` is true when any active adapter has `scale != 0` and is not an aLoRA. In that case the slot state is not written to the shared cache, a hit guard keeps the slot's own KV, per-request LoRA is pre-checked during slot selection, the slot prompt is cleared when the active adapter set changes, and startup logs an INFO that the shared cache is unavailable.

### 6.6 Parameters

| Flag | Default | Deployed | Meaning |
|---|---|---|---|
| `--cache-prompt` | on | on | master switch for prompt caching |
| `--cache-ram N` | 8192 MiB | 25600 | RAM prompt cache limit; `--cache-disk` depends on it |
| `--cache-idle-slots` | on | on | store idle slots into the cache when a new task starts |
| `--cache-disk PATH` | off | `E:\llama.cpp.cache` | on-disk cache directory |
| `--cache-disk-max N` | 65536 MiB | 204800 | `-1` unlimited, `0` disables |
| `--cache-disk-min-tokens N` | 2048 | 2048 | prefixes shorter than this are not stored |
| `--cache-disk-idle-flush N` | 60 s | 60 | mirror RAM entries after N idle seconds, `0` disables |
| `--cache-disk-ttl N` | 0 | 0 | drop entries idle longer than N seconds, `0` never expires |
| `--cache-disk-queue-max N` | 24576 MiB | 24576 | pending-write byte budget |
| `--n-ctx-checkpoints` | 32 | 32 | per-slot checkpoint cap |

### 6.7 Measured entry composition

An entry is three parts, not one lump. Measured with the same configuration:

| Prefix | File | main | drft | checkpoints | count |
|---|---|---|---|---|---|
| 123,674 tokens | 3039 MiB | 2326 MiB | 88 MiB | 624 MiB | 3 |
| 5,852 tokens | 727 MiB | 255 MiB | 62 MiB | 410 MiB | 2 |

Two things worth knowing:

- The target is a hybrid architecture, so a single state carries about **151 MiB of fixed overhead** independent of context length. Short prefixes therefore cost more per token out of necessity — about 128 KiB/token for a 6K entry against 25 KiB/token at 123K. This is not a defect.
- The checkpoint count grows with the session rather than staying fixed: 1–3 for a short session, climbing to **31** (the `--n-ctx-checkpoints 32` cap) in a long one, at roughly 200 MiB each. That is how the 8.6 GiB entries in the production log come about, with checkpoints accounting for about 6 GiB of them. Lowering `--n-ctx-checkpoints` therefore does shrink entries considerably, at the cost of fewer usable restart points for a partial hit.

On a mechanical disk the measured write throughput is 130–155 MiB/s and restore reads run at about 180 MiB/s: a 3 GiB entry takes about 20 s to write and 17 s to restore, against about 98 s for a full 123K-token prefill.

**Hit-rate practice**: keep system prompts, tool definitions and few-shot examples static and first; put timestamps and usernames later; append to multi-turn conversations instead of rewriting them.

## 7. Observability

`-lv 3` is the default INFO level, `TRACE=4`, `DEBUG=5`.

| Log line | Level | Purpose |
|---|---|---|
| `causal_attn=... selector_top_k=... is_dflash2=...` | INFO | distinguishes the draft lattice path from per-row vocabulary top-k |
| `disk cache: restored N/M tokens (a + b bytes)` | DEBUG | `N` reused prefix, `M` entry length |
| `entry with length ... lcp = ... f_keep = ...` | TRACE | per-candidate match detail |
| `disk cache: stored ... tokens ... in ... ms` | INFO | write duration and size |
| `covered up to a N-token tail ... skipping a X MiB write` | INFO | write suppression fired, with the size not written |
| `disk cache: limit reached, removing ...` | WRN | LRU eviction |
| `dropping ... different model or context configuration` | WRN | signature mismatch cleanup |
| `prompt cache effectively disabled: active LoRA ...` | INFO | shared cache unavailable under the current LoRA setup |
| `write queue is full` | WRN | pending queue hit its byte or entry limit |

## 8. Boundaries

- The CUDA backend only supports layer split (`-sm layer`); `-sm row` is rejected at model load.
- With an active non-aLoRA adapter the shared cache does not write for that slot; the slot's own incremental continuation still works.
- Packed metadata has no separate CRC; its integrity rests on the startup index self-check (hash recomputation), the media semantic comparison at restore, and the payload CRCs.
- Entries match by prefix, so the first request always prefills in full; `-np 1` means one slot serving serially.
- A forced kill does not run destructors; persistence depends on the idle mirror (60 s) or an eviction.
- A restore always reads the whole entry file, so a long entry hit at a low ratio reads bytes it does not use; `DISK_CACHE_MAX_BYTES_PER_TOKEN` only catches the extreme cases.
- Each growth step of a conversation produces a new entry; write suppression only absorbs small deltas, and a cumulative delta beyond 4096 tokens still triggers a rewrite.
- Long sessions accumulate checkpoints until entries reach the 8 GiB range, and the pending queue is bounded by `--cache-disk-queue-max`. When a write (8 GiB ≈ 57 s) cannot keep up with the turn rate, the entry is dropped and the log records `write queue is full, skipping`. The production log shows 25 consecutive occurrences.

## 9. Where the code lives

| File | Responsibility |
|---|---|
| `common/common.h` / `common/arg.cpp` | cache parameter definitions and CLI registration |
| `tools/server/server-task.h/.cpp` | `server_disk_cache`: entry format, CRC64, write queue, restore, index, LRU/TTL, write suppression |
| `tools/server/server-context.cpp` | cache init, config signature, slot save/load guard, LoRA cleanup, idle detection |
| `tools/server/server-common.h/.cpp` | `lora_blocks_cache()` and LoRA comparison helpers |
| `tools/server/server-queue.h/.cpp` | idle callback hook |
| `common/speculative.cpp` | draft decode implementation and startup shape logging |
| `CMakeLists.txt` | artifact directories pointed at `output/` |
| `build-cuda-shared.bat` | configure, incremental build, CUDA runtime DLLs |

## 10. Experiment records

- `change.md` — per-change record with pre-change backup paths and verification results
- `experienceHistory.md` — rejected approaches with evidence, confidence levels, debugging discipline
- `experiments/` — benchmark scripts (`bench_decode.py`, `start-bench*.ps1`) and measurements (`RESULTS.md`, `RESULTS-OPT.md`)

## 11. Ideas welcome

Speeding up local dense-model inference is far from exhausted. On this two-GPU setup the time goes to three places, and an idea for any of them is welcome:

| Stage | Measured today |
|---|---|
| Decode | 37–49 t/s, 55–73 ms per round; speculative decoding carries a fixed ~36 ms per round independent of context |
| Prefill | about 1.3K t/s, roughly 98 s for a full 123K-token pass |
| Disk cache | long sessions grow entries to 8.6 GiB (31 checkpoints) and saturate the write queue |

If you have another idea for speeding up local dense-model deployment, **open an [issue](https://github.com/A-lucky-boy/llama.cpp-dspine-diskCache/issues) and I will try to implement it**. It does not have to fit an existing direction in this repository — a different kernel, quantization, scheduling scheme or cache policy is all fair game.

To make an idea land faster, these help a lot:

- where exactly to change: which kernel, which flag, which code path
- expected gain and the reasoning behind it: a bandwidth model, a comparable implementation, a paper, or your own measurements elsewhere
- whether it changes numerical results, and if so how you would estimate the accuracy cost

The rule of thumb for this repository is that **ideas backed by measurements come first**. `change.md` and `experienceHistory.md` record what each round concluded, which directions were rejected and why — worth a skim before proposing, so we do not repeat work.

## License

MIT, same as upstream llama.cpp. See [LICENSE](LICENSE).
