[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$InitialSource = '',
    [ValidateSet('Both', 'Detached')][string]$Scenario = 'Both',
    [ValidateRange(30, 240)][int]$TimeoutSec = 180
)
$ErrorActionPreference = 'Stop'
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
$start.EnvironmentVariables['SPECFORGE_PROFILE'] = '1'
$start.EnvironmentVariables['SPECFORGE_PROFILE_DIR'] = $directory
if ($InitialSource) {
    $source = (Resolve-Path -LiteralPath $InitialSource).Path
    if ($source.Contains('"')) { throw 'Invalid source path quote' }
    $start.Arguments = '"' + $source + '"'
}
if ($Scenario -eq 'Detached') {
    Write-Host 'Detach Spectrum; leave it still for 3 seconds, resize it for 10-15 seconds, then leave it still for 3 seconds.'
} else {
    Write-Host 'Resize the main window for 10-15 seconds; then detach Spectrum and resize it for 10-15 seconds.'
}
Write-Host 'Let each interval settle, redock, and close normally. Record display setup and visual observations.'
Write-Host "Recording directory: $directory"
$process = [Diagnostics.Process]::Start($start)
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
try {
    while (!$process.WaitForExit(250)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            # Request normal close; never terminate a user's possibly unsaved work.
            [void]$process.CloseMainWindow()
            if (!$process.WaitForExit(10000)) { throw 'Profile time bound reached. Close the profile window normally, then analyze its JSONL.' }
            break
        }
    }
    if ($process.ExitCode -ne 0) { throw "Profile application failed: $($process.ExitCode)" }
} finally { $process.Dispose() }
$profiles = @(Get-ChildItem -LiteralPath $directory -Filter '*.jsonl')
if ($profiles.Count -ne 1) { throw 'Expected exactly one complete profile; inspect the recording directory.' }
if ($Scenario -eq 'Detached') {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireDetachedBreakdown
} else {
    & (Join-Path $PSScriptRoot 'analyze-presentation-profile.ps1') -Path $profiles[0].FullName -RequireLiveResize
}
