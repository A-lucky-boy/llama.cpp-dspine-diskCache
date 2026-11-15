@echo off
rem test instance: new binary, scratch cache, port 18090
setlocal
rem ROOT is derived from this script location; MODEL_DIR must point at the
rem directory holding the target and draft gguf files
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not defined MODEL_DIR set "MODEL_DIR=%ROOT%\models"
set "BASE=%MODEL_DIR%"
set "EXE=%ROOT%\output\llama-server.exe"
set "CACHE=%ROOT%\test-run\cache"

"%EXE%" ^
 -m "%BASE%\localModel\Qwen3.8-27B-pi-Q4_K_M.gguf" ^
 -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fa on -fit off ^
 -np 1 -c 16384 -ctk q4_0 -ctv q4_0 -t 8 -tb 16 --backend-sampling ^
 --spec-type draft-dflash ^
 --spec-draft-model "%BASE%\localModel\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf" ^
 --spec-draft-n-max 6 --spec-draft-n-min 0 ^
 --spec-draft-device CUDA0 --spec-draft-ngl all ^
 --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
 --kv-unified --cache-prompt --cache-ram 2048 ^
 --cache-disk "%CACHE%" --cache-disk-max 8192 --cache-disk-min-tokens 512 ^
 --cache-disk-idle-flush 5 ^
 -b 4096 -ub 1024 -lv 4 --offline ^
 --host 127.0.0.1 --port 18090
