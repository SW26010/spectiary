[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$InitialSource = '',
    [ValidateSet('Both', 'Detached', 'NativeSize')][string]$Scenario = 'Both',
    [ValidateRange(30, 240)][int]$TimeoutSec = 180,
    [switch]$SystemTrace,
    [switch]$FeedbackBreakdown,
    [switch]$WindowedCapture
)
$ErrorActionPreference = 'Stop'
$windowed = $WindowedCapture -or $FeedbackBreakdown
if ($SystemTrace -and $Scenario -ne 'NativeSize') { throw 'SystemTrace requires -Scenario NativeSize.' }
if ($FeedbackBreakdown -and $Scenario -ne 'NativeSize') { throw 'FeedbackBreakdown requires -Scenario NativeSize.' }
$repo = Split-Path -Parent $PSScriptRoot
if (!$Executable) { $Executable = Join-Path $repo 'build/ninja-msvc-debug/SpecForge.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$directory = Join-Path $repo ('logs/live-resize-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $directory | Out-Null
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $Executable
$start.WorkingDirectory = $repo
$start.UseShellExecute = $false
# This is an explicitly interactive profile: the user must see and drag the window.
$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Normal
$start.EnvironmentVariables['SPECFORGE_PROFILE'] = $(if ($windowed) { '0' } else { '1' })
$start.EnvironmentVariables['SPECFORGE_PROFILE_WINDOWED'] = $(if ($windowed) { '1' } else { '0' })
$start.EnvironmentVariables['SPECFORGE_PROFILE_DIR'] = $directory
if ($InitialSource) {
    $source = (Resolve-Path -LiteralPath $InitialSource).Path
    if ($source.Contains('"')) { throw 'Invalid source path quote' }
    $start.Arguments = '"' + $source + '"'
}
if ($windowed) {
    Write-Host 'Recording starts OFF. Prepare and detach Spectrum first. Use Settings > Diagnostics > Start Recording, then immediately drag its outer border.'
    Write-Host 'Recording stops automatically after 5 seconds, retaining the active frame tail. Then close normally; do not start a second recording.'
} elseif ($Scenario -in @('Detached', 'NativeSize')) {
    Write-Host 'Detach Spectrum; leave it still for 3 seconds, resize it for 10-15 seconds, then leave it still for 3 seconds.'
} else {
    Write-Host 'Resize the main window for 10-15 seconds; then detach Spectrum and resize it for 10-15 seconds.'
}
if (!$windowed) { Write-Host 'Let each interval settle, redock, and close normally. Record display setup and visual observations.' }
Write-Host "Recording directory: $directory"
$instance = 'SpecForgeResize-' + [guid]::NewGuid().ToString('N')
function Invoke-ResizeWpr([string]$action) {
    $helper = Join-Path $PSScriptRoot 'profile-resize-wpr-helper.ps1'
    foreach ($value in @($helper, $directory)) { if ($value.Contains('"')) { throw 'Invalid path quote' } }
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$helper`" -Action $action -Instance $instance -Directory `"$directory`""
    Write-Host "WPR $action requires administrator approval; SpecForge retains this shell's privileges."
    $elevated = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe') -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
    try {
        $null = $elevated.Handle
        while (!$elevated.WaitForExit(250)) { }
        if ($elevated.ExitCode -ne 0) { throw "WPR $action failed. Inspect wpr logs in $directory" }
    } finally { $elevated.Dispose() }
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
    while (!$process.WaitForExit(250)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            # Request normal close; never terminate a user's possibly unsaved work.
            [void]$process.CloseMainWindow()
            if (!$process.WaitForExit(10000)) { throw 'Profile time bound reached. Close the profile window normally, then analyze its JSONL.' }
            break
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
if ($profiles.Count -ne 1) { throw 'Expected exactly one complete profile; inspect the recording directory.' }
if ($Scenario -eq 'NativeSize') {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireDetachedBreakdown -RequireNativeSizeBreakdown -RequireClockSync:$SystemTrace -RequireFeedbackBreakdown:$FeedbackBreakdown
} elseif ($Scenario -eq 'Detached') {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireDetachedBreakdown
} else {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireLiveResize
}
