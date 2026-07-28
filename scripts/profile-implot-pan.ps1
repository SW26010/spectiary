[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$InitialSource = '',
    [double]$BudgetMs = 7.6923,
    [double]$MinDragMs = 10000.0,
    [int]$MinInputSamples = 100,
    [ValidateSet('Default', 'Uncapped')]
    [string]$PanPacing = 'Default',
    [switch]$SkipAnalyze,
    [switch]$ReportOnly
)

$ErrorActionPreference = 'Stop'

function Quote-StartProcessArgument {
    param([Parameter(Mandatory = $true)] [string]$Value)

    if ($Value.Contains('"')) {
        throw "Cannot pass an argument containing a double quote to Start-Process: $Value"
    }
    return '"' + $Value + '"'
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
if (-not $Executable) {
    $Executable = Join-Path $repoRoot 'build\ninja-msvc-debug\SpecForge.exe'
}

$resolvedExecutable = Resolve-Path -Path $Executable -ErrorAction SilentlyContinue
if ($null -eq $resolvedExecutable) {
    $buildCommand =
        'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1'
    throw "Executable not found: $Executable. Build from the repository root with: $buildCommand"
}

$logDir = Join-Path $repoRoot 'logs'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null

Write-Host 'SpecForge ImPlot pan-drag profile'
Write-Host "Pan pacing: $PanPacing"
if ($InitialSource) {
    Write-Host "Initial source: $InitialSource"
}
Write-Host '1. In the Spectrum plot, left-drag pan for 10-15 seconds.'
Write-Host '2. Keep the interaction focused: avoid wheel zoom, docking changes, and side panels.'
Write-Host '3. Close SpecForge to run the analyzer.'
Write-Host ''

$previousProfile = $env:SPECFORGE_PROFILE
$previousProfileDir = $env:SPECFORGE_PROFILE_DIR
$previousPanPacing = $env:SPECFORGE_PAN_PACING
$env:SPECFORGE_PROFILE = '1'
$env:SPECFORGE_PROFILE_DIR = $logDir
if ($PanPacing -eq 'Uncapped') {
    $env:SPECFORGE_PAN_PACING = 'uncapped'
}
else {
    Remove-Item Env:SPECFORGE_PAN_PACING -ErrorAction SilentlyContinue
}
$launchTime = Get-Date
try {
    $startProcessArguments = @{
        FilePath = $resolvedExecutable.Path
        WorkingDirectory = $repoRoot
        Wait = $true
        PassThru = $true
    }
    if ($InitialSource) {
        $resolvedInitialSource = Resolve-Path -LiteralPath $InitialSource -ErrorAction Stop
        $startProcessArguments.ArgumentList = @(Quote-StartProcessArgument $resolvedInitialSource.Path)
    }
    $process = Start-Process @startProcessArguments
    if ($process.ExitCode -ne 0) {
        throw "SpecForge exited with code $($process.ExitCode)."
    }
}
finally {
    if ($null -eq $previousProfile) {
        Remove-Item Env:SPECFORGE_PROFILE -ErrorAction SilentlyContinue
    }
    else {
        $env:SPECFORGE_PROFILE = $previousProfile
    }

    if ($null -eq $previousProfileDir) {
        Remove-Item Env:SPECFORGE_PROFILE_DIR -ErrorAction SilentlyContinue
    }
    else {
        $env:SPECFORGE_PROFILE_DIR = $previousProfileDir
    }

    if ($null -eq $previousPanPacing) {
        Remove-Item Env:SPECFORGE_PAN_PACING -ErrorAction SilentlyContinue
    }
    else {
        $env:SPECFORGE_PAN_PACING = $previousPanPacing
    }
}

$latestLog = Get-ChildItem -Path $logDir -Filter 'specforge-profile-*.jsonl' |
    Where-Object { $_.LastWriteTime -ge $launchTime } |
    Sort-Object LastWriteTime |
    Select-Object -Last 1

if ($null -eq $latestLog) {
    throw "No profile log from this run found in $logDir."
}

Write-Host ''
Write-Host "Profile log: $($latestLog.FullName)"

if (-not $SkipAnalyze) {
    $analyzerArgs = @{
        Profile = $latestLog.FullName
        BudgetMs = $BudgetMs
        MinDragMs = $MinDragMs
        MinInputSamples = $MinInputSamples
        ExpectedPanPacing = $PanPacing
    }
    if ($ReportOnly) {
        $analyzerArgs.ReportOnly = $true
    }

    & (Join-Path $scriptRoot 'analyze-profile.ps1') @analyzerArgs
}
