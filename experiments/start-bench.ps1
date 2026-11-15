# =============================================================================
# Benchmark launcher for the D1..D4 decoder experiments.
#
# Mirrors D:\AiHelper\llama.cpp\start-localModel-dspine.ps1 argument for
# argument, changing ONLY the flag under test for each variant. The production
# script is never modified.
#
#   base : production args (spec on, n_max=15, 38/62, -c 262144)
#   d1   : base + --spec-type none
#   d2   : base + --spec-draft-n-max 6
#   d2b  : d2   + --spec-draft-device CUDA0
#   d3   : target model entirely on CUDA1, spec off, mmproj on CUDA0
#   d4   : base + -ub 2048
#
# The disk tier is disabled (--cache-disk-max 0) for every variant: decode and
# prefill timing must not be polluted by HDD writes, and the second request of
# each pair hits the RAM prompt cache instead.
# =============================================================================

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("base", "d1", "d2", "d2b", "d3", "d4")]
    [string] $Variant,

    [int] $Ctx = 262144,
    [int] $Ub  = 1024,
    # when set, the disk tier is enabled on this directory instead of disabled
    [string] $CacheDir = "",
    [int] $IdleFlush = 8
)

$ErrorActionPreference = "Stop"

$BaseDir      = "D:\AiHelper\llama.cpp"
$Exe          = "$BaseDir\llama-server.exe"
$ModelDir     = "$BaseDir\localModel"
$Model        = "$ModelDir\Qwen3.8-27B-pi-Q4_K_M.gguf"
$Mmproj       = "$ModelDir\mmproj-Qwen3.8-27B-pi-BF16.gguf"
$DraftModel   = "$ModelDir\Qwen3.8-27B-DSpine-b16-Q4_K_M.gguf"
$Template     = "$ModelDir\localModel-workbuddy-chat-template.jinja"
$BenchCache   = "$BaseDir\benchCache"

$PidFile      = "$BaseDir\bench.pid"
$LogFile      = "$BaseDir\bench-server.log"
$ErrorLogFile = "$BaseDir\bench-server-error.log"

foreach ($p in @($Exe, $Model, $Mmproj, $DraftModel, $Template)) {
    if (-not (Test-Path $p)) { Write-Host "[ERROR] missing $p" -ForegroundColor Red; exit 1 }
}
if (-not (Test-Path $BenchCache)) { New-Item -ItemType Directory -Path $BenchCache | Out-Null }
if ($CacheDir) {
    if (-not (Test-Path $CacheDir)) { New-Item -ItemType Directory -Path $CacheDir | Out-Null }
    $ArgumentsExtra = @("--cache-disk", "`"$CacheDir`"", "--cache-disk-max", "204800",
                        "--cache-disk-min-tokens", "2048", "--cache-disk-idle-flush", "$IdleFlush")
} else {
    $ArgumentsExtra = @("--cache-disk", "`"$BenchCache`"", "--cache-disk-max", "0")
}

$Arguments = @("-m", "`"$Model`"", "-ngl", "all", "-fa", "on", "-np", "1",
               "-c", "$Ctx", "-ctk", "q4_0", "-ctv", "q4_0", "-t", "8", "-tb", "16",
               "--backend-sampling", "--kv-unified", "--cache-prompt", "--cache-ram", "25600",
               "--cache-idle-slots",
               "--mmproj", "`"$Mmproj`"", "--offline", "--jinja",
               "--chat-template-file", "`"$Template`"", "--reasoning", "on",
               "--reasoning-format", "deepseek-legacy", "--reasoning-preserve",
               "--alias", "localModel", "--api-key", "sk-0", "--host", "127.0.0.1",
               "--port", "18080", "--sse-ping-interval", "15", "--timeout", "7200",
               "-lv", "3", "-b", "4096")

switch ($Variant) {
    "base" {
        $Arguments += @("-dev", "CUDA0,CUDA1", "-sm", "layer", "-ts", "38,62", "-fit", "off",
                        "--spec-type", "draft-dflash", "--spec-draft-model", "`"$DraftModel`"",
                        "--spec-draft-n-max", "15", "--spec-draft-n-min", "0",
                        "--spec-draft-device", "CUDA1", "--spec-draft-ngl", "all",
                        "--spec-draft-type-k", "q8_0", "--spec-draft-type-v", "q8_0",
                        "--mmproj-device", "CUDA0", "-ub", "$Ub")
    }
    "d1" {
        $Arguments += @("-dev", "CUDA0,CUDA1", "-sm", "layer", "-ts", "38,62", "-fit", "off",
                        "--spec-type", "none", "--mmproj-device", "CUDA0", "-ub", "$Ub")
    }
    "d2" {
        $Arguments += @("-dev", "CUDA0,CUDA1", "-sm", "layer", "-ts", "38,62", "-fit", "off",
                        "--spec-type", "draft-dflash", "--spec-draft-model", "`"$DraftModel`"",
                        "--spec-draft-n-max", "6", "--spec-draft-n-min", "0",
                        "--spec-draft-device", "CUDA1", "--spec-draft-ngl", "all",
                        "--spec-draft-type-k", "q8_0", "--spec-draft-type-v", "q8_0",
                        "--mmproj-device", "CUDA0", "-ub", "$Ub")
    }
    "d2b" {
        $Arguments += @("-dev", "CUDA0,CUDA1", "-sm", "layer", "-ts", "38,62", "-fit", "off",
                        "--spec-type", "draft-dflash", "--spec-draft-model", "`"$DraftModel`"",
                        "--spec-draft-n-max", "6", "--spec-draft-n-min", "0",
                        "--spec-draft-device", "CUDA0", "--spec-draft-ngl", "all",
                        "--spec-draft-type-k", "q8_0", "--spec-draft-type-v", "q8_0",
                        "--mmproj-device", "CUDA0", "-ub", "$Ub")
    }
    "d3" {
        # single GPU: no layer split, therefore no serialisation across devices
        $Arguments += @("-dev", "CUDA1", "-fit", "off",
                        "--spec-type", "none", "--mmproj-device", "CUDA0", "-ub", "$Ub")
    }
    "d4" {
        $Arguments += @("-dev", "CUDA0,CUDA1", "-sm", "layer", "-ts", "38,62", "-fit", "off",
                        "--spec-type", "draft-dflash", "--spec-draft-model", "`"$DraftModel`"",
                        "--spec-draft-n-max", "15", "--spec-draft-n-min", "0",
                        "--spec-draft-device", "CUDA1", "--spec-draft-ngl", "all",
                        "--spec-draft-type-k", "q8_0", "--spec-draft-type-v", "q8_0",
                        "--mmproj-device", "CUDA0", "-ub", "$Ub")
    }
}

$Arguments += $ArgumentsExtra

# only stop a llama-server that this harness started, or the production one by pid file
foreach ($pf in @($PidFile, "$BaseDir\localModel.pid")) {
    if (Test-Path $pf) {
        $old = Get-Content $pf -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($old -and (Get-Process -Id $old -ErrorAction SilentlyContinue)) {
            Write-Host "stopping llama-server PID $old"
            Stop-Process -Id $old -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 3
        }
        Remove-Item $pf -Force -ErrorAction SilentlyContinue
    }
}
Get-Process llama-server -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "stopping leftover llama-server PID $($_.Id)"
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2

Remove-Item $LogFile, $ErrorLogFile -Force -ErrorAction SilentlyContinue

$CommandLine = '""' + $Exe + '" ' + ($Arguments -join ' ') + ' 1>"' + $LogFile + '" 2>>"' + $ErrorLogFile + '""'
$Launcher = Start-Process -FilePath $env:ComSpec `
    -ArgumentList @('/d', '/s', '/c', $CommandLine) `
    -WorkingDirectory $BaseDir -WindowStyle Hidden -PassThru

$Process = $null
for ($i = 0; $i -lt 200; $i++) {
    $Process = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($Launcher.Id) AND Name = 'llama-server.exe'" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($Process -or $Launcher.HasExited) { break }
    Start-Sleep -Milliseconds 100
}
if (-not $Process) { Write-Host "[ERROR] server did not start, see $ErrorLogFile" -ForegroundColor Red; exit 1 }

$Process.ProcessId | Set-Content $PidFile -Encoding ASCII
Write-Host "variant=$Variant ctx=$Ctx pid=$($Process.ProcessId)"
Write-Host "args: $($Arguments -join ' ')"
