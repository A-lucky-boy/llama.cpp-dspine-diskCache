@echo off
REM Launch llama-server with the exact parameters from the user's deployment script
REM D:\AiHelper\llama.cpp\start-localModel-dspine.ps1, with two deliberate changes:
REM
REM   1. --cache-disk points at an isolated test directory, so the user's production
REM      cache at E:\llama.cpp.cache is never written to or cleaned.
REM   2. --host 127.0.0.1 / a different port, so the user's 0.0.0.0:18080 instance is
REM      not disturbed if it is running.
REM
REM Everything that affects cache correctness - context size, KV quantisation, cache
REM sizes, the disk floor, idle flush, model, draft model and mmproj - is kept
REM identical, because changing any of them changes the configuration signature and
REM would make the disk tier reject its own entries.

setlocal

REM Default to the build under test (output\), NOT the older deployed copy at
REM D:\AiHelper\llama.cpp\llama-server.exe (build 2 / 60f3891), which predates the
REM quarantine fixes and rejects --cache-stats. Override with EXE= to A/B them.
set EXE=%~dp0..\output\llama-server.exe
set MODELDIR=D:\AiHelper\llama.cpp\localModel

set MODEL=%MODELDIR%\Qwen3.8-27B-pi-Q4_K_M.gguf
set MMPROJ=%MODELDIR%\mmproj-Qwen3.8-27B-pi-BF16.gguf
set DRAFT=%MODELDIR%\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf
set TEMPLATE=%MODELDIR%\localModel-workbuddy-chat-template.jinja

if "%CACHE%"=="" set CACHE=test-run\cache-prod
if "%LOG%"=="" set LOG=test-run\logs\prod-params-server.log
if "%PORT%"=="" set PORT=18090
if "%LV%"=="" set LV=5
if "%MODE%"=="" set MODE=text
if "%NP%"=="" set NP=204800
if "%RAM%"=="" set RAM=25600

set MMPROJARGS=
if "%MODE%"=="vision" set MMPROJARGS=--mmproj "%MMPROJ%" --mmproj-device CUDA0

"%EXE%" ^
  -m "%MODEL%" ^
  -ngl all -dev CUDA0,CUDA1 -sm layer -ts 38,62 -fit off -fa on ^
  -np 1 ^
  -c 262144 -ctk q4_0 -ctv q4_0 ^
  -t 8 -tb 16 ^
  --backend-sampling ^
  --spec-type draft-dflash ^
  --spec-draft-model "%DRAFT%" ^
  --spec-draft-n-max 6 --spec-draft-n-min 0 ^
  --spec-draft-device CUDA0 --spec-draft-ngl all ^
  --spec-draft-type-k q8_0 --spec-draft-type-v q8_0 ^
  --kv-unified ^
  --cache-prompt ^
  --cache-ram %RAM% ^
  --cache-idle-slots ^
  --cache-disk "%CACHE%" ^
  --cache-disk-max %NP% ^
  --cache-disk-min-tokens 2048 ^
  --cache-disk-idle-flush 60 ^
  %MMPROJARGS% ^
  --offline ^
  --jinja ^
  --chat-template-file "%TEMPLATE%" ^
  --reasoning on ^
  -lv %LV% -b 4096 -ub 1024 ^
  --reasoning-format deepseek-legacy ^
  --reasoning-preserve ^
  --alias localModel ^
  --api-key sk-0 ^
  --host 127.0.0.1 --port %PORT% ^
  --sse-ping-interval 15 --timeout 7200 ^
  --cache-stats ^
  --metrics ^
  > "%LOG%" 2>&1
