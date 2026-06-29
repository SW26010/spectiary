[CmdletBinding()]
param(
    [switch]$Configure,
    [string]$Preset = 'ninja-msvc-debug',
    [string[]]$Target = @(),
    [int]$TimeoutSec = 120,
    [switch]$Explain,
    [string]$VcvarsPath = '',
    [string]$LogDir = ''
)

$ErrorActionPreference = 'Stop'

function Quote-BatchArgument {
    param([Parameter(Mandatory = $true)] [string]$Value)

    if ($Value.Contains('"')) {
        throw "Cannot pass an argument containing a double quote to cmd.exe: $Value"
    }

    if ($Value -eq '') {
        return '""'
    }

    if ($Value -match '[\s&|<>^()%!]') {
        return '"' + $Value + '"'
    }

    return $Value
}

function Write-LogTail {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Label,
        [int]$Lines = 80
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    Write-Host ""
    Write-Host "---- $Label ($Path) ----"
    Get-Content -LiteralPath $Path -Tail $Lines | ForEach-Object {
        Write-Host $_
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path

if (-not $VcvarsPath) {
    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (-not $programFilesX86) {
        $programFilesX86 = 'C:\Program Files (x86)'
    }

    $VcvarsPath = Join-Path $programFilesX86 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
}

if (-not (Test-Path -LiteralPath $VcvarsPath)) {
    throw "vcvars64.bat not found: $VcvarsPath"
}

if (-not $LogDir) {
    $LogDir = Join-Path $repoRoot 'logs\build'
}
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null

$buildDir = Join-Path $repoRoot "build\$Preset"
$lockPath = Join-Path $buildDir '.ninja_lock'
$existingBuildDrivers = @(Get-Process | Where-Object { $_.ProcessName -in @('cmake', 'ninja') })
if ((Test-Path -LiteralPath $lockPath) -and $existingBuildDrivers.Count -gt 0) {
    $processSummary = $existingBuildDrivers |
        Select-Object Id,ProcessName,CPU,StartTime,Path |
        Format-Table -AutoSize |
        Out-String

    throw "Ninja lock already exists at $lockPath and cmake/ninja is already running. Stop the stale build first:`n$processSummary"
}

$cmakeArgs = @()
if ($Configure) {
    $cmakeArgs += '--preset'
    $cmakeArgs += $Preset
}
else {
    $cmakeArgs += '--build'
    $cmakeArgs += '--preset'
    $cmakeArgs += $Preset

    if ($Target.Count -gt 0) {
        $cmakeArgs += '--target'
        $cmakeArgs += $Target
    }

    if ($Explain) {
        $cmakeArgs += '--'
        $cmakeArgs += '-d'
        $cmakeArgs += 'explain'
    }
}

$cmakeCommand = 'cmake ' + (($cmakeArgs | ForEach-Object { Quote-BatchArgument $_ }) -join ' ')
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$mode = if ($Configure) { 'configure' } else { 'build' }
$batchPath = Join-Path $LogDir "specforge-$Preset-$mode-$timestamp.cmd"
$stdoutPath = Join-Path $LogDir "specforge-$Preset-$mode-$timestamp.out.log"
$stderrPath = Join-Path $LogDir "specforge-$Preset-$mode-$timestamp.err.log"
$exitCodePath = Join-Path $LogDir "specforge-$Preset-$mode-$timestamp.exit"

$batchLines = @(
    '@echo off',
    'setlocal',
    ('call "' + $VcvarsPath + '"'),
    'set SPECFORGE_EXITCODE=%ERRORLEVEL%',
    'if not "%SPECFORGE_EXITCODE%"=="0" goto done',
    $cmakeCommand,
    'set SPECFORGE_EXITCODE=%ERRORLEVEL%',
    ':done',
    ('> "' + $exitCodePath + '" echo %SPECFORGE_EXITCODE%'),
    'exit /b %SPECFORGE_EXITCODE%'
)
Set-Content -LiteralPath $batchPath -Value $batchLines -Encoding ASCII

Write-Host "SpecForge MSVC CMake $mode"
Write-Host "Command: $cmakeCommand"
Write-Host "Timeout: $TimeoutSec seconds"
Write-Host "Stdout: $stdoutPath"
Write-Host "Stderr: $stderrPath"
Write-Host "Exit file: $exitCodePath"

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$cmdArguments = '/d /c "' + $batchPath + '"'
$process = Start-Process -FilePath 'cmd.exe' `
    -ArgumentList $cmdArguments `
    -WorkingDirectory $repoRoot `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -WindowStyle Hidden `
    -PassThru

Wait-Process -Id $process.Id -Timeout $TimeoutSec -ErrorAction SilentlyContinue | Out-Null
$process.Refresh()

if (-not $process.HasExited) {
    & taskkill.exe /PID $process.Id /T /F | Out-Null
    $stopwatch.Stop()
    Write-LogTail -Path $stdoutPath -Label 'stdout'
    Write-LogTail -Path $stderrPath -Label 'stderr'
    throw "Timed out after $TimeoutSec seconds; killed process tree rooted at PID $($process.Id)."
}

$process.WaitForExit()
$process.Refresh()
$stopwatch.Stop()

$exitCode = $null
if (Test-Path -LiteralPath $exitCodePath) {
    $exitCodeLine = Get-Content -LiteralPath $exitCodePath -TotalCount 1
    $exitCodeText = if ($null -eq $exitCodeLine) { '' } else { $exitCodeLine.Trim() }
    if ($exitCodeText -match '^-?\d+$') {
        $exitCode = [int]$exitCodeText
    }
}
elseif ($null -ne $process.ExitCode) {
    $exitCode = [int]$process.ExitCode
}

if ($null -eq $exitCode) {
    Write-LogTail -Path $stdoutPath -Label 'stdout'
    Write-LogTail -Path $stderrPath -Label 'stderr'
    throw "CMake $mode finished but did not produce an exit code."
}

Write-Host "Exit code: $exitCode"
Write-Host ("Elapsed: {0:n1}s" -f $stopwatch.Elapsed.TotalSeconds)

if ($exitCode -ne 0) {
    Write-LogTail -Path $stdoutPath -Label 'stdout'
    Write-LogTail -Path $stderrPath -Label 'stderr'
    throw "CMake $mode failed with exit code $exitCode."
}

Write-LogTail -Path $stdoutPath -Label 'stdout' -Lines 40
Write-LogTail -Path $stderrPath -Label 'stderr' -Lines 40
