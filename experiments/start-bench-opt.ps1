# =============================================================================
# Launcher for the opt tree benchmark variants.
#
# Same model, same KV types, same draft model, same split as the production
# deployment; the only differences are the opt tree itself and the knobs under
# test:
#
#   -Mmsq 1|0   GGML_CUDA_MMSQ  (small-batch Q4_K/Q6_K GEMM on int8 tensor cores)
#   -Q4fa 1|0   GGML_CUDA_FATTN_Q4_MMA (fused q4_0 KV loader, sm_86 + 8 queries)
#   -Nmax N     --spec-draft-n-max, so the verify batch is N+1 tokens
#
# The opt tree has no --cache-disk*, so nothing is written to the cache HDD.
# =============================================================================

param(
    [Parameter(Mandatory = $true)][string] $Name,
    [int] $Nmax = 6,
    [int] $Mmsq = 1,
    [int] $Q4fa = 0,
    [int] $Ctx = 262144,
    [int] $Ub  = 1024,
    [int] $B   = 4096,
    [string] $Spec = "draft-dflash",
    [int] $Nmin = 5
)

$ErrorActionPreference = "Stop"

$BaseDir    = "C:\Users\Administrator\Desktop\experience\_agentComprehensive\llama.cpp-opt-opt-main\output-opt"
$Exe        = "$BaseDir\llama-server.exe"
$ModelDir   = "D:\AiHelper\llama.cpp\localModel"
$Model      = "$ModelDir\Qwen3.8-27B-pi-Q4_K_M.gguf"
$Mmproj     = "$ModelDir\mmproj-Qwen3.8-27B-pi-BF16.gguf"
$DraftModel = "$ModelDir\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf"
$Template   = "$ModelDir\localModel-workbuddy-chat-template.jinja"

$LogFile      = "D:\AiHelper\llama.cpp\bench-opt-server.log"
$ErrorLogFile = "D:\AiHelper\llama.cpp\bench-opt-server-error.log"
$PidFile      = "D:\AiHelper\llama.cpp\bench-opt.pid"

foreach ($p in @($Exe, $Model, $DraftModel, $Mmproj, $Template)) {
    if (-not (Test-Path $p)) { Write-Host "[ERROR] missing $p" -ForegroundColor Red; exit 1 }
}

# stop anything still holding the GPUs (production or a previous bench run)
foreach ($pf in @($PidFile, "D:\AiHelper\llama.cpp\localModel.pid", "D:\AiHelper\llama.cpp\bench.pid")) {
    if (Test-Path $pf) {
        $old = Get-Content $pf -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($old -and (Get-Process -Id $old -ErrorAction SilentlyContinue)) {
            Write-Host "stopping PID $old"
            Stop-Process -Id $old -Force -ErrorAction SilentlyContinue
        }
        Remove-Item $pf -Force -ErrorAction SilentlyContinue
    }
}
Get-Process llama-server -ErrorAction SilentlyContinue | ForEach-Object {
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 3

$env:GGML_CUDA_MMSQ                = "$Mmsq"
$env:GGML_CUDA_FATTN_Q4_MMA        = "$Q4fa"
$env:GGML_CUDA_MMSQ_NMIN           = "$Nmin"
$env:LLAMA_DFLASH_GPU_FEATURES     = "1"

$Arguments = @(
    "-m", "`"$Model`"",
    "-ngl", "all", "-dev", "CUDA0,CUDA1", "-sm", "layer", "-ts", "38,62", "-fit", "off", "-fa", "on",
    "-np", "1",
    "-c", "$Ctx", "-ctk", "q4_0", "-ctv", "q4_0",
    "-t", "8", "-tb", "16",
    "--backend-sampling",
    "--spec-type", "$Spec",
    "--spec-draft-n-max", "$Nmax",
    "--spec-draft-n-min", "0",
    "--spec-draft-device", "CUDA0",
    "--spec-draft-ngl", "all",
    "--spec-draft-type-k", "q8_0",
    "--spec-draft-type-v", "q8_0",
    "--kv-unified",
    "--cache-prompt",
    "--cache-ram", "25600",
    "--cache-idle-slots",
    "--mmproj", "`"$Mmproj`"",
    "--mmproj-device", "CUDA0",
    "--offline",
    "--jinja",
    "--chat-template-file", "`"$Template`"",
    "--reasoning", "on",
    "--reasoning-format", "deepseek-legacy",
    "--reasoning-preserve",
    "--alias", "localModel",
    "--api-key", "sk-0",
    "--host", "127.0.0.1",
    "--port", "18080",
    "--sse-ping-interval", "15",
    "--timeout", "7200",
    "-lv", "3",
    "-b", "$B", "-ub", "$Ub"
)

# the DSpine draft GGUF (58 tensors) is not loadable by this tree's DFlash2 loader
# (73 tensors), so non-dflash variants run without a draft model. ngram-simple is
# used instead: it produces the same 1+n_max verification batches without one.
if ($Spec -eq "draft-dflash") {
    $Arguments += @("--spec-draft-model", "`"$DraftModel`"")
}

Remove-Item $LogFile, $ErrorLogFile -Force -ErrorAction SilentlyContinue

$CommandLine = '""' + $Exe + '" ' + ($Arguments -join ' ') + ' 1>"' + $LogFile + '" 2>>"' + $ErrorLogFile + '""'
$Launcher = Start-Process -FilePath $env:ComSpec `
    -ArgumentList @('/d', '/s', '/c', $CommandLine) `
    -WorkingDirectory $BaseDir -WindowStyle Hidden -PassThru

$Process = $null
for ($i = 0; $i -lt 300; $i++) {
    $Process = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($Launcher.Id) AND Name = 'llama-server.exe'" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($Process -or $Launcher.HasExited) { break }
    Start-Sleep -Milliseconds 100
}
if (-not $Process) { Write-Host "[ERROR] server did not start, see $ErrorLogFile" -ForegroundColor Red; exit 1 }

$Process.ProcessId | Set-Content $PidFile -Encoding ASCII
Write-Host "variant=$Name spec=$Spec n_max=$Nmax mmsq=$Mmsq nmin=$Nmin q4fa=$Q4fa pid=$($Process.ProcessId)"
