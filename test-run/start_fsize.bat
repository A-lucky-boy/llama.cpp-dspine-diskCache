@echo off
REM Same cache directory as the seed run, but a DIFFERENT context size, so scan()
REM rejects the entry as a configuration mismatch and tries to delete it - which fails
REM because the file is held open. That is the path that decides what happens to a
REM file that cannot be measured and cannot be removed.
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
  --cache-disk "test-run\cache-fsize" ^
  --cache-disk-max 204800 --cache-disk-min-tokens 2048 ^
  --cache-disk-idle-flush 0 ^
  --offline -lv 5 -b 4096 -ub 1024 ^
  --api-key sk-0 --host 127.0.0.1 --port 18090 ^
  --cache-stats --metrics ^
  > test-run\logs\fsize-run.log 2>&1
