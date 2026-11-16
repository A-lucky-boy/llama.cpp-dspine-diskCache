@echo off
REM Same multimodal server with the disk tier switched off entirely (--cache-disk-max 0),
REM to see whether the image crash depends on the disk cache at all.
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
  --kv-unified --cache-prompt --cache-ram 25600 --cache-idle-slots ^
  --cache-disk "test-run\cache-nodisk" --cache-disk-max 0 ^
  --cache-disk-min-tokens 2048 --cache-disk-idle-flush 60 ^
  --offline -lv 5 -b 4096 -ub 1024 ^
  --api-key sk-0 --host 127.0.0.1 --port 18090 ^
  --cache-stats --metrics ^
  > test-run\logs\nodisk.log 2>&1
