#!/bin/bash
# fixed parameter set: every restart must use exactly this, or the context/config
# signature changes and the whole cache is (correctly) invalidated
M="${MODEL_DIR:-D:/AiHelper/llama.cpp/localModel}"
DIR="$1"; LOG="$2"
./output/llama-server.exe -m "$M/Qwen3.8-27B-pi-Q4_K_M.gguf" -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 \
  -fa on -fit off -np 1 -c 20000 -ctk q4_0 -ctv q4_0 -t 8 -tb 16 --backend-sampling \
  --spec-type draft-dflash --spec-draft-model "$M/Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf" \
  --spec-draft-n-max 6 --spec-draft-n-min 0 --spec-draft-device CUDA0 --spec-draft-ngl all \
  --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 --kv-unified --cache-prompt --cache-ram 4096 \
  --cache-disk "$DIR" --cache-disk-max 8192 --cache-disk-min-tokens 512 --cache-disk-idle-flush 0 \
  --cache-stats --cache-boundary-capture input --no-cache-prefix-discovery \
  --metrics -b 4096 -ub 1024 -lv 3 --offline --host 127.0.0.1 --port 18090 > "$LOG" 2>&1
