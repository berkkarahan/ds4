param(
    [string]$Label = 'baseline',
    [string]$Model = 'ds4flash.gguf',
    [string]$Executable = 'ds4-bench.exe',
    [int]$ModelCacheGiB = -1,
    [int]$Experts = 0,
    [int]$Q8CacheGiB = -1,
    [int]$ReadWorkers = 0,
    [int]$PromptTokens = 16,
    [int]$GenerateTokens = 16,
    [int]$PrefillChunk = 32,
    [int]$ContextTokens = 0,
    [string]$PromptFile = '',
    [int]$TimeoutSeconds = 900,
    [int]$RamCacheBytes = -1,
    [switch]$DumpLogits,
    [string]$RocmPath = 'C:\Program Files\AMD\ROCm\7.2'
)

# One model process, with WDDM GPU/process memory and physical-disk telemetry.
# Compare runs with the same prompt/length; file-cache warmth is not controlled.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root "win/build/streaming/$Label"
New-Item -ItemType Directory -Force $out | Out-Null
$prompt = Join-Path $out 'prompt.txt'
if ($PromptFile) {
    if (![IO.Path]::IsPathRooted($PromptFile)) { $PromptFile = Join-Path $root $PromptFile }
    Copy-Item -LiteralPath $PromptFile -Destination $prompt
} else {
    $passage = 'The capital of France is Paris. Paris is a city in Europe. The capital of Japan is Tokyo. Tokyo is a large city in Asia. One plus one equals two. Two plus two equals four. A week has seven days. Water freezes at zero degrees Celsius. This short passage is used to validate native Windows model loading, prefill, and autoregressive text generation with DeepSeek V4 Flash.'
    [IO.File]::WriteAllText($prompt, (($passage + "`n") * [math]::Max(1, [math]::Ceiling($PromptTokens / 64.0))))
}
if ($ContextTokens -eq 0) { $ContextTokens = [math]::Max(256, $PromptTokens + $GenerateTokens + 32) }
if (![IO.Path]::IsPathRooted($Model)) { $Model = Join-Path $root $Model }
if (!(Test-Path -LiteralPath $Model)) { throw "Missing model: $Model" }
if (![IO.Path]::IsPathRooted($Executable)) { $Executable = Join-Path $root $Executable }
if (!(Test-Path -LiteralPath $Executable)) { throw "Missing executable: $Executable" }

$saved = @{}
$settings = @{
    PATH = "$RocmPath\bin;$env:PATH"
    HIP_VISIBLE_DEVICES = '0'
    DS4_ROCM_STREAM_CACHE_STATS = '1'
    DS4_ROCM_STREAM_READ_PROFILE = '1'
    DS4_ROCM_STREAM_MODEL_CACHE_GB = $null
    DS4_ROCM_STREAM_Q8_F16_CACHE_GB = $null
    DS4_ROCM_STREAM_READ_WORKERS = $null
    DS4_ROCM_STREAM_RAM_CACHE_GB = $null
    DS4_ROCM_STREAM_RAM_CACHE_BYTES = $null
}
if ($ModelCacheGiB -ge 0) { $settings.DS4_ROCM_STREAM_MODEL_CACHE_GB = "$ModelCacheGiB" }
if ($Q8CacheGiB -ge 0) { $settings.DS4_ROCM_STREAM_Q8_F16_CACHE_GB = "$Q8CacheGiB" }
if ($ReadWorkers -gt 0) { $settings.DS4_ROCM_STREAM_READ_WORKERS = "$ReadWorkers" }
if ($RamCacheBytes -ge 0) { $settings.DS4_ROCM_STREAM_RAM_CACHE_BYTES = "$RamCacheBytes" }
$child = $null
try {
    foreach ($key in $settings.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, $settings[$key], 'Process')
    }
    $csv = Join-Path $out 'bench.csv'
    $argsLine = "--rocm -m `"$Model`" --prompt-file `"$prompt`" --ssd-streaming --ssd-streaming-cold --ctx-start $PromptTokens --ctx-max $PromptTokens --ctx-alloc $ContextTokens --prefill-chunk $PrefillChunk --gen-tokens $GenerateTokens --show-output --csv `"$csv`""
    if ($Experts -gt 0) { $argsLine += " --ssd-streaming-cache-experts $Experts" }
    if ($DumpLogits) {
        $logits = Join-Path $out 'logits'
        New-Item -ItemType Directory -Force $logits | Out-Null
        $argsLine += " --dump-frontier-logits-dir `"$logits`""
    }
    $argsLine | Set-Content -Encoding UTF8 (Join-Path $out 'command.txt')
    [pscustomobject]@{
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        executable = $Executable
        executable_sha256 = (Get-FileHash -Algorithm SHA256 $Executable).Hash
        model = $Model
        model_cache_gib = $ModelCacheGiB
        expert_slots = $Experts
        q8_cache_gib = $Q8CacheGiB
        read_workers = $ReadWorkers
        prompt_tokens = $PromptTokens
        generate_tokens = $GenerateTokens
        prefill_chunk = $PrefillChunk
        context_tokens = $ContextTokens
        ram_cache_bytes = $RamCacheBytes
        prompt_sha256 = (Get-FileHash -Algorithm SHA256 $prompt).Hash
    } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $out 'configuration.json')
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $child = Start-Process -FilePath $Executable -ArgumentList $argsLine -WorkingDirectory $root -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError (Join-Path $out 'stderr.log')
    # Retain the handle before Refresh/HasExited; otherwise Windows PowerShell
    # can lose the exit code of a short-lived Start-Process -PassThru child.
    $null = $child.Handle
    $processId = $child.Id
    $samples = @()
    while (!$child.HasExited) {
        # Sampling is the purpose of this loop; wait on the process between samples.
        $child.Refresh()
        $prefix = "pid_${processId}_"
        $engine = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine -Filter "Name LIKE '$prefix%'" -ErrorAction SilentlyContinue | Where-Object { $_.Name.StartsWith($prefix) })
        $gpuMem = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -Filter "Name LIKE '$prefix%'" -ErrorAction SilentlyContinue | Where-Object { $_.Name.StartsWith($prefix) })
        $disk = Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk -Filter "Name = '_Total'" -ErrorAction SilentlyContinue
        $proc = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -Filter "IDProcess = $processId" -ErrorAction SilentlyContinue
        $compute = ($engine | Where-Object Name -Match 'engtype_Compute' | Measure-Object UtilizationPercentage -Maximum).Maximum
        $graphics = ($engine | Where-Object Name -Match 'engtype_3D' | Measure-Object UtilizationPercentage -Maximum).Maximum
        $copy = ($engine | Where-Object Name -Match 'engtype_Copy' | Measure-Object UtilizationPercentage -Maximum).Maximum
        $samples += [pscustomobject]@{
            elapsed_s = [math]::Round($clock.Elapsed.TotalSeconds, 3)
            cpu_s = [math]::Round($child.TotalProcessorTime.TotalSeconds, 3)
            working_set_mib = [math]::Round($child.WorkingSet64 / 1MB, 1)
            private_mib = [math]::Round($child.PrivateMemorySize64 / 1MB, 1)
            gpu_dedicated_mib = [math]::Round(($gpuMem | Measure-Object DedicatedUsage -Sum).Sum / 1MB, 1)
            gpu_shared_mib = [math]::Round(($gpuMem | Measure-Object SharedUsage -Sum).Sum / 1MB, 1)
            compute_pct = [double]$compute
            graphics_pct = [double]$graphics
            copy_pct = [double]$copy
            disk_read_mib_s = [math]::Round($disk.DiskReadBytesPersec / 1MB, 1)
            process_read_mib_s = [math]::Round($proc.IOReadBytesPersec / 1MB, 1)
        }
        if ($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw "Benchmark exceeded $TimeoutSeconds seconds" }
        [void]$child.WaitForExit(1000)
    }
    $child.WaitForExit()
    $samples | Export-Csv -NoTypeInformation (Join-Path $out 'telemetry.csv')
    [pscustomobject]@{
        label = $Label
        exit_code = $child.ExitCode
        elapsed_s = [math]::Round($clock.Elapsed.TotalSeconds, 3)
        samples = $samples.Count
        compute_avg_pct = ($samples | Measure-Object compute_pct -Average).Average
        compute_max_pct = ($samples | Measure-Object compute_pct -Maximum).Maximum
        graphics_avg_pct = ($samples | Measure-Object graphics_pct -Average).Average
        copy_avg_pct = ($samples | Measure-Object copy_pct -Average).Average
        peak_gpu_dedicated_mib = ($samples | Measure-Object gpu_dedicated_mib -Maximum).Maximum
        peak_gpu_shared_mib = ($samples | Measure-Object gpu_shared_mib -Maximum).Maximum
        peak_process_mib = ($samples | Measure-Object working_set_mib -Maximum).Maximum
        disk_read_avg_mib_s = ($samples | Measure-Object disk_read_mib_s -Average).Average
        process_read_avg_mib_s = ($samples | Measure-Object process_read_mib_s -Average).Average
    } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $out 'summary.json')
    Get-Content (Join-Path $out 'summary.json')
    if (Test-Path $csv) { Get-Content $csv }
    if ($child.ExitCode -ne 0) { throw "Benchmark exited $($child.ExitCode); see $out/stderr.log" }
} finally {
    if ($child -and !$child.HasExited) { $child.Kill(); $child.WaitForExit() }
    foreach ($key in $saved.Keys) {
        [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process')
    }
}
