[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$InitialSource = '',
    [ValidateSet('Both', 'Detached', 'NativeSize')][string]$Scenario = 'Both',
    [ValidateRange(30, 240)][int]$TimeoutSec = 180,
    [switch]$SystemTrace,
    [switch]$FeedbackBreakdown,
    [switch]$FeedbackAcquireOnly,
    [switch]$WindowedCapture,
    [switch]$IncrementalBuffers,
    [switch]$ObserveResources,
    [ValidateSet('None', 'A', 'B')][string]$RedirectionArm = 'None'
)
$ErrorActionPreference = 'Stop'
$windowed = $WindowedCapture -or $FeedbackBreakdown -or $ObserveResources
if ($IncrementalBuffers -and ($RedirectionArm -eq 'None' -or !$FeedbackBreakdown -or $FeedbackAcquireOnly)) {
    throw 'IncrementalBuffers requires an isolated RedirectionArm, FeedbackBreakdown, and ordinary feedback frequency.'
}
if ($FeedbackAcquireOnly -and ($RedirectionArm -eq 'None' -or !$FeedbackBreakdown)) {
    throw 'FeedbackAcquireOnly requires an isolated RedirectionArm and FeedbackBreakdown.'
}
if ($SystemTrace -and $Scenario -ne 'NativeSize') { throw 'SystemTrace requires -Scenario NativeSize.' }
if ($FeedbackBreakdown -and $Scenario -ne 'NativeSize') { throw 'FeedbackBreakdown requires -Scenario NativeSize.' }
$repo = Split-Path -Parent $PSScriptRoot
if ($RedirectionArm -ne 'None') {
    if ($Scenario -ne 'NativeSize') { throw 'Redirection A/B requires -Scenario NativeSize' }
    if (!$Executable) { $Executable = Join-Path $repo 'build/ninja-msvc-debug/redirection-experiment/Spectiary.exe' }
}
if (!$Executable) { $Executable = Join-Path $repo 'build/ninja-msvc-debug/Spectiary.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$directory = Join-Path $repo ('logs/live-resize-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $directory | Out-Null
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $Executable
$start.WorkingDirectory = $repo
$start.UseShellExecute = $false
# This is an explicitly interactive profile: the user must see and drag the window.
$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Normal
$start.EnvironmentVariables['SPECTIARY_PROFILE'] = $(if ($windowed) { '0' } else { '1' })
$start.EnvironmentVariables['SPECTIARY_PROFILE_WINDOWED'] = $(if ($windowed) { '1' } else { '0' })
$start.EnvironmentVariables['SPECTIARY_PROFILE_DIR'] = $directory
$start.EnvironmentVariables['SPECTIARY_EXPERIMENT_INCREMENTAL_BUFFERS'] = $(if ($IncrementalBuffers) { '1' } else { '0' })
$start.EnvironmentVariables['SPECTIARY_EXPERIMENT_FEEDBACK_ACQUIRE_ONLY'] = $(if ($FeedbackAcquireOnly) { '1' } else { '0' })
$start.EnvironmentVariables['SPECTIARY_EXPERIMENT_NO_REDIRECTION_BITMAP'] = $(if ($RedirectionArm -eq 'B') { '1' } else { '0' })
[pscustomobject]@{ arm = $RedirectionArm; feedback_acquire_only = [bool]$FeedbackAcquireOnly;
    windowed_capture = [bool]$windowed; incremental_buffers = [bool]$IncrementalBuffers;
    capture_seconds = $(if ($windowed) { 5 } else { 0 }); resource_observation = [bool]$ObserveResources; executable = $Executable;
    sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash } |
    ConvertTo-Json | Set-Content (Join-Path $directory 'redirection-experiment.json')
if ($InitialSource) {
    $source = (Resolve-Path -LiteralPath $InitialSource).Path
    if ($source.Contains('"')) { throw 'Invalid source path quote' }
    $start.Arguments = '"' + $source + '"'
}
if ($ObserveResources) {
    Write-Host 'Resource observation: samples every 2 seconds. Keep recording OFF for the resource workload; no performance recording is required.'
}
if ($windowed -and !$ObserveResources) {
    Write-Host 'Recording starts OFF. Prepare and detach Spectrum first. Use Settings > Diagnostics > Start Recording, then immediately drag its outer border.'
    Write-Host 'Recording stops automatically after 5 seconds, retaining the active frame tail. Then close normally; do not start a second recording.'
} elseif (!$ObserveResources -and $Scenario -in @('Detached', 'NativeSize')) {
    Write-Host 'Detach Spectrum; leave it still for 3 seconds, resize it for 10-15 seconds, then leave it still for 3 seconds.'
} elseif (!$ObserveResources) {
    Write-Host 'Resize the main window for 10-15 seconds; then detach Spectrum and resize it for 10-15 seconds.'
}
if (!$windowed) { Write-Host 'Let each interval settle, redock, and close normally. Record display setup and visual observations.' }
Write-Host "Recording directory: $directory"
if ($RedirectionArm -ne 'None') { Write-Host "Redirection A/B arm: $RedirectionArm (same isolated executable for both arms)." }
$instance = 'SpectiaryResize-' + [guid]::NewGuid().ToString('N')
function Invoke-ResizeWpr([string]$action) {
    $helper = Join-Path $PSScriptRoot 'profile-resize-wpr-helper.ps1'
    foreach ($value in @($helper, $directory)) { if ($value.Contains('"')) { throw 'Invalid path quote' } }
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$helper`" -Action $action -Instance $instance -Directory `"$directory`""
    Write-Host "WPR $action requires administrator approval; Spectiary retains this shell's privileges."
    $elevated = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe') -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
    try {
        $null = $elevated.Handle
        while (!$elevated.WaitForExit(250)) { }
        if ($elevated.ExitCode -ne 0) { throw "WPR $action failed. Inspect wpr logs in $directory" }
    } finally { $elevated.Dispose() }
}
function Get-ResizeResourceSample {
    param([Diagnostics.Process]$TargetProcess, [long]$ElapsedMs)
    $TargetProcess.Refresh()
    $sample = [ordered]@{
        elapsed_ms = $ElapsedMs; process_id = $TargetProcess.Id
        private_bytes = $TargetProcess.PrivateMemorySize64
        working_set_bytes = $TargetProcess.WorkingSet64
        handle_count = $TargetProcess.HandleCount
        gpu_status = 'unavailable'; gpu_instance_count = 0
        gpu_dedicated_bytes = $null; gpu_shared_bytes = $null; gpu_committed_bytes = $null
        gpu_error = ''
    }
    try {
        # Preserve absence/errors as unknown, not zero usage. Query only this launched process.
        $instances = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory `
            -Filter "Name LIKE 'pid_$($TargetProcess.Id)[_]%'" -OperationTimeoutSec 1 -ErrorAction Stop)
        if ($instances.Count -gt 0) {
            $sample.gpu_instance_count = $instances.Count
            $sample.gpu_dedicated_bytes = [long]0
            $sample.gpu_shared_bytes = [long]0
            $sample.gpu_committed_bytes = [long]0
            foreach ($instance in $instances) {
                if ($null -eq $instance.DedicatedUsage -or $null -eq $instance.SharedUsage -or $null -eq $instance.TotalCommitted) {
                    throw 'GPU counter instance lacks required values.'
                }
                $sample.gpu_dedicated_bytes += [long]$instance.DedicatedUsage
                $sample.gpu_shared_bytes += [long]$instance.SharedUsage
                $sample.gpu_committed_bytes += [long]$instance.TotalCommitted
            }
            $sample.gpu_status = 'available'
        }
    } catch {
        $sample.gpu_error = $_.Exception.Message
        $sample.gpu_dedicated_bytes = $null
        $sample.gpu_shared_bytes = $null
        $sample.gpu_committed_bytes = $null
    }
    [pscustomobject]$sample
}
$process = $null
$traceStarted = $false
try {
    if ($SystemTrace) {
        # Preserve matching symbols before any later rebuild, and never cancel another WPR instance.
        $symbolDirectory = Join-Path $directory 'symbols'
        New-Item -ItemType Directory -Path $symbolDirectory | Out-Null
        Copy-Item -LiteralPath $Executable -Destination $symbolDirectory
        $pdb = [IO.Path]::ChangeExtension($Executable, '.pdb')
        if (!(Test-Path -LiteralPath $pdb)) { throw "Matching PDB missing: $pdb" }
        Copy-Item -LiteralPath $pdb -Destination $symbolDirectory
        [pscustomobject]@{ instance = $instance; profile = 'CPU.Verbose'; executable = $Executable;
            sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;
            recovery_command = "wpr -stop `"$directory/system-trace.etl`" -instancename $instance" } |
            ConvertTo-Json | Set-Content (Join-Path $directory 'system-trace-session.json')
        Invoke-ResizeWpr 'Start'
        $traceStarted = $true
    }
    $process = [Diagnostics.Process]::Start($start)
    $process.Id | Set-Content (Join-Path $directory 'process-id.txt')
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    $resourceClock = [Diagnostics.Stopwatch]::StartNew()
    $nextResourceSample = 0
    $resourceSampleCount = 0
    while (!$process.WaitForExit(250)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            # Request normal close; never terminate a user's possibly unsaved work.
            [void]$process.CloseMainWindow()
            if (!$process.WaitForExit(10000)) { throw 'Profile time bound reached. Close the profile window normally, then analyze its JSONL.' }
            break
        }
        if ($ObserveResources -and $resourceClock.ElapsedMilliseconds -ge $nextResourceSample) {
            try {
                Get-ResizeResourceSample -TargetProcess $process -ElapsedMs $resourceClock.ElapsedMilliseconds |
                    Export-Csv -LiteralPath (Join-Path $directory 'resources.csv') -NoTypeInformation -Encoding UTF8 -Append
                ++$resourceSampleCount
            } catch {
                if (!$process.HasExited) { throw }
            }
            $nextResourceSample = $resourceClock.ElapsedMilliseconds + 2000
        }
    }
    if ($process.ExitCode -ne 0) { throw "Profile application failed: $($process.ExitCode)" }
} finally {
    if ($null -ne $process) { $process.Dispose() }
    if ($traceStarted) {
        try {
            Invoke-ResizeWpr 'Stop'
            $etl = Get-Item -LiteralPath (Join-Path $directory 'system-trace.etl')
            if ($etl.Length -eq 0) { throw 'WPR produced an empty ETL' }
        } catch {
            Write-Warning "Trace may still be active. Run the recovery_command in $directory/system-trace-session.json from an administrator terminal."
            throw
        }
    }
}
$profiles = @(Get-ChildItem -LiteralPath $directory -Filter '*.jsonl')
if ($ObserveResources -and $resourceSampleCount -lt 2) {
    throw 'Insufficient resource samples: keep the application open longer before closing normally.'
}
if ($ObserveResources -and $profiles.Count -eq 0) {
    Write-Host "Resource observations saved in $directory/resources.csv. No performance capture was recorded; no performance gate was run."
    return
}
if ($profiles.Count -ne 1) { throw 'Expected exactly one complete profile; inspect the recording directory.' }
if ($Scenario -eq 'NativeSize') {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireDetachedBreakdown -RequireNativeSizeBreakdown -RequireClockSync:$SystemTrace -RedirectionArm $RedirectionArm -RequireFeedbackBreakdown:$FeedbackBreakdown -RequireFeedbackAcquireOnly:$FeedbackAcquireOnly -RequireIncrementalBuffers:$IncrementalBuffers
} elseif ($Scenario -eq 'Detached') {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireDetachedBreakdown
} else {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireLiveResize
}
