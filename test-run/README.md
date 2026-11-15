# test-run — local reproduction harness

Scratch space for driving a single `llama-server` instance and recording what the
prompt cache does. Nothing here is part of the build; the scripts are kept in the
repository because every claim in `change.md` was produced with them, and the
raw logs are **not** committed (see the ignore rules at the bottom of the root
`.gitignore`).

## What is ignored, and why

| Path | Reason |
|---|---|
| `logs/*.log` | server logs, tens of KB each; reproducible from the commands below |
| `logs/out-*.txt` | model continuations. They are derived from the local test corpus and therefore contain third-party prose — never published |
| `cache-*/` | disk-cache directories, each 0.5–1.5 GiB |
| `*.exe`, `hold*.py` | throwaway helpers for the deletion-failure injection runs |

## Corpus and models

The harness drives one long text corpus so that prompts of different lengths
share a real prefix. It is **not** in the repository:

```bat
set MODEL_DIR=D:\path\to\gguf\directory
set LLAMA_TEST_CORPUS=D:\path\to\a\large\utf8\text.md
set LLAMA_TEST_PORT=18090
```

`MODEL_DIR` must hold the target and draft gguf files the batch scripts reference.
No machine-specific path is hard-coded: `ROOT` is derived from the script
location and `MODEL_DIR` defaults to `<repo>\models`.

## Scripts

| Script | Purpose |
|---|---|
| `start_test_server.bat` | launches a dual-GPU server with the cache options the experiments use |
| `probe.py <chars\|start:end> <n_predict> <label> [ext:N]` | one completion; prints `prompt_n / cache_n / sha`, writes `logs/out-<label>.txt` |
| `metrics.py` | dumps the `llamacpp:cache_*` metrics from `/metrics` |
| `req.py <chars> [label] [n_predict]` | minimal single request, prints the first characters of the continuation |
| `prep_cases.py`, `drive_case.sh`, `drive_two.sh`, `run_case.bat` | corruption / restart case generation for the phase-0 checks |
| `srv.sh <cache-dir> <log>` | starts the server with one frozen parameter set. use it for every restart of a check: changing `-c`, `-ctk`/`-ctv`, the model or the draft model changes the configuration signature, and the disk cache is then (correctly) invalidated with `index rejected (different model or context configuration)`. that looks like a cache bug and is not one |

## Reproducing the checks quoted in change.md

```bat
rem 1) start a server on an empty cache directory
start_test_server.bat

rem 2) cold request, then the same request again (slot reuse)
python probe.py 2000 8 A1
python probe.py 2000 8 A2

rem 3) append and re-request (prefix growth)
python probe.py 2000 8 A3 ext:2300

rem 4) inspect the counters
python metrics.py
```

For the restart checks, kill the server with `taskkill /F /IM llama-server.exe`
(no destructors run), then start it again on the same `--cache-disk` directory
**with the identical flags** - use `srv.sh` so the parameter set cannot drift
between the two runs.

A restart that changes the context size or the KV quantization is not a restart
of the same cache: `--cache-disk-max`-style knobs only affect eviction, but
`-c`, `-ctk`, `-ctv`, the models and the draft settings are part of the config
signature. Get one of them different and `scan()` reports
`index rejected (different model or context configuration)` and drops every
entry, which is correct behaviour and easy to mistake for the cache losing data.

To exercise the "a longer snapshot replaces a shorter one" path end to end, the
short prefix has to be long enough that the longer entry still fits the
`768 KiB/token` guard while being long enough to escape the 4096-token mirror
suppression window. With this model a short prefix of ~2500 tokens and a long one
of ~7500 tokens works: the short entry is 563 MiB, the long one 1147 MiB, and
1147 MiB / 2557 tokens = 459 KiB/token stays under the guard. Below ~1400 tokens
the window closes and no legal pair exists.

For the deletion-failure injection, hold a handle on a `pc-*.bin` file from a
second process while the server tries to reclaim it; Windows then reports a
sharing violation and the whole quarantine path is exercised.
