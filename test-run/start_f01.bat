@echo off
REM F01: force a delete failure by holding an entry file open in another process,
REM while a 1-second TTL makes the server try to sweep it.
setlocal
set EXE=%~dp0..\output\llama-server.exe
set MD=D:\AiHelper\llama.cpp\localModel
"%EXE%" ^
  -m "%MD%\Qwen3.8-27B-pi-Q4_K_M.gguf" ^
  -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fit off -fa on ^
  -np 1 -c 20000 -ctk q4_0 -ctv q4_0 -t 8 -tb 16 ^
  --backend-sampling ^
  --spec-type draft-dflash --spec-draft-model "%MD%\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf" ^
  --spec-draft-n-max 6 --spec-draft-n-min 0 --spec-draft-device CUDA0 --spec-draft-ngl all ^
  --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
  --kv-unified --cache-prompt --cache-ram 25600 --cache-idle-slots ^
  --cache-disk "test-run\cache-f01" ^
  --cache-disk-max 204800 --cache-disk-min-tokens 2048 ^
  --cache-disk-idle-flush 0 ^
  --cache-disk-ttl 1 ^
  --offline -lv 5 -b 4096 -ub 1024 ^
  --api-key sk-0 --host 127.0.0.1 --port 18090 ^
  --cache-stats --metrics ^
  > test-run\logs\f01-server.log 2>&1
