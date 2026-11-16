@echo off
REM Same multimodal server, --cache-ram 0 with --cache-disk still set, to establish what
REM actually happens to the disk tier when the RAM prompt cache is switched off.
setlocal
set EXE=%~dp0..\output\llama-server.exe
set MD=D:\AiHelper\llama.cpp\localModel
"%EXE%" ^
  -m "%MD%\Qwen3.8-27B-pi-Q4_K_M.gguf" ^
  --mmproj "%MD%\mmproj-Qwen3.8-27B-pi-BF16.gguf" --mmproj-device CUDA0 ^
  -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fit off -fa on ^
  -np 1 -c 262144 -ctk q4_0 -ctv q4_0 -t 8 -tb 16 ^
  --backend-sampling ^
  --spec-type draft-dflash --spec-draft-model "%MD%\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf" ^
  --spec-draft-n-max 6 --spec-draft-n-min 0 --spec-draft-device CUDA0 --spec-draft-ngl all ^
  --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
  --kv-unified --cache-prompt --cache-ram 0 ^
  --cache-disk "test-run\cache-ram0" --cache-disk-max 204800 ^
  --cache-disk-min-tokens 2048 --cache-disk-idle-flush 60 ^
  --offline -lv 5 -b 4096 -ub 1024 ^
  --api-key sk-0 --host 127.0.0.1 --port 18090 ^
  --cache-stats --metrics ^
  > test-run\logs\ram0.log 2>&1
