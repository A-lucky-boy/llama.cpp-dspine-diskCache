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
(no destructors run), then start it again on the same `--cache-disk` directory.

For the deletion-failure injection, hold a handle on a `pc-*.bin` file from a
second process while the server tries to reclaim it; Windows then reports a
sharing violation and the whole quarantine path is exercised.
