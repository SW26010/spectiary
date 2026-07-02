[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Configure,
    [string]$Preset = 'ninja-msvc-portable-debug',
    [string[]]$Target = @(),
    [int]$TimeoutSec = 120,
    [switch]$Explain,
    [string]$VcvarsPath = '',
    [string]$LogDir = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AdditionalTarget = @()
)

$ErrorActionPreference = 'Stop'

if ($AdditionalTarget.Count -gt 0) {
    if (-not $PSBoundParameters.ContainsKey('Target')) {
        throw "Unexpected positional arguments: $($AdditionalTarget -join ' '). Pass build targets with -Target."
    }

    $unexpectedOptions = @($AdditionalTarget | Where-Object { $_.StartsWith('-') })
    if ($unexpectedOptions.Count -gt 0) {
        throw "Unexpected option after -Target: $($unexpectedOptions -join ' '). Pass build targets as plain names."
    }

    $Target += $AdditionalTarget
}

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

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [string]$Value)

    return "'" + $Value.Replace("'", "''") + "'"
}

function Invoke-TaskkillCommand {
    param([Parameter(Mandatory = $true)] [int]$ProcessId)

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    try {
        $output = @(& taskkill.exe /PID $ProcessId /T /F 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

function Invoke-TaskkillTree {
    param(
        [Parameter(Mandatory = $true)] [int]$ProcessId,
        [string]$LogPath = ''
    )

    $messages = New-Object System.Collections.Generic.List[string]
    $messages.Add(("taskkill /PID {0} /T /F" -f $ProcessId))

    $taskkillResult = Invoke-TaskkillCommand -ProcessId $ProcessId
    foreach ($line in $taskkillResult.Output) {
        $messages.Add(("taskkill: {0}" -f $line))
    }
    $messages.Add(("taskkill exit code: {0}" -f $taskkillResult.ExitCode))

    Start-Sleep -Milliseconds 500
    $rootProcess = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -ne $rootProcess) {
        $messages.Add(("root PID {0} still alive after taskkill; trying Stop-Process" -f $ProcessId))
        Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }

    $rootProcess = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -ne $rootProcess) {
        $messages.Add(("root PID {0} still alive after kill attempts" -f $ProcessId))
    }
    else {
        $messages.Add(("root PID {0} is gone" -f $ProcessId))
    }

    foreach ($message in $messages) {
        if ($LogPath) {
            Add-Content -LiteralPath $LogPath -Value ("{0:o} {1}" -f (Get-Date), $message)
        }
        else {
            Write-Host $message
        }
    }

    return $null -eq $rootProcess
}

function Assert-TaskkillTreeAvailable {
    param([Parameter(Mandatory = $true)] [string]$LogPath)

    Add-Content -LiteralPath $LogPath -Value ("{0:o} starting process-kill preflight" -f (Get-Date))

    $probeArguments = '/d /c "ping -n 6 127.0.0.1 >nul"'
    $probeJobHandle = [IntPtr]::Zero
    $probe = Start-Process -FilePath 'cmd.exe' `
        -ArgumentList $probeArguments `
        -WindowStyle Hidden `
        -PassThru

    try {
        $probeJobHandle = New-KillOnCloseJob
        $assigned = [SpecForgeBuildJob.NativeMethods]::AssignProcessToJobObject($probeJobHandle, $probe.Handle)
        if (-not $assigned) {
            $message = Get-LastWin32ErrorMessage
            Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue
            throw "Preflight AssignProcessToJobObject failed: $message"
        }

        Start-Sleep -Milliseconds 300

        Add-Content -LiteralPath $LogPath -Value ("{0:o} preflight probe PID: {1}" -f (Get-Date), $probe.Id)
        $taskkillResult = Invoke-TaskkillCommand -ProcessId $probe.Id
        foreach ($line in $taskkillResult.Output) {
            Add-Content -LiteralPath $LogPath -Value ("{0:o} taskkill: {1}" -f (Get-Date), $line)
        }
        Add-Content -LiteralPath $LogPath -Value ("{0:o} taskkill exit code: {1}" -f (Get-Date), $taskkillResult.ExitCode)

        Start-Sleep -Milliseconds 500
        $probeProcess = Get-Process -Id $probe.Id -ErrorAction SilentlyContinue
        if (($taskkillResult.ExitCode -eq 0) -and ($null -eq $probeProcess)) {
            Add-Content -LiteralPath $LogPath -Value ("{0:o} process-kill preflight passed" -f (Get-Date))
            return
        }

        if ($null -ne $probeProcess) {
            Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue
            [void]$probe.WaitForExit(7000)
        }

        throw "This shell cannot kill job-assigned process trees with taskkill /T /F. Run this build through Codex escalation or an unsandboxed developer shell. Preflight log: $LogPath"
    }
    finally {
        Close-NativeHandle -Handle $probeJobHandle
    }
}

if (-not ('SpecForgeBuildJob.NativeMethods' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace SpecForgeBuildJob
{
    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public IntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    public static class NativeMethods
    {
        public const int JobObjectExtendedLimitInformation = 9;
        public const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string lpName);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool SetInformationJobObject(
            IntPtr hJob,
            int JobObjectInfoClass,
            IntPtr lpJobObjectInfo,
            uint cbJobObjectInfoLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr hObject);
    }
}
'@
}

function Get-LastWin32ErrorMessage {
    $errorCode = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return "${errorCode}: $((New-Object System.ComponentModel.Win32Exception($errorCode)).Message)"
}

function Close-NativeHandle {
    param([IntPtr]$Handle)

    if ($Handle -ne [IntPtr]::Zero) {
        [void][SpecForgeBuildJob.NativeMethods]::CloseHandle($Handle)
    }
}

function New-KillOnCloseJob {
    $jobHandle = [SpecForgeBuildJob.NativeMethods]::CreateJobObject([IntPtr]::Zero, $null)
    if ($jobHandle -eq [IntPtr]::Zero) {
        throw "CreateJobObject failed: $(Get-LastWin32ErrorMessage)"
    }

    $info = New-Object SpecForgeBuildJob.JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    $info.BasicLimitInformation.LimitFlags = [SpecForgeBuildJob.NativeMethods]::JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    $length = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
    $buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($length)

    try {
        [System.Runtime.InteropServices.Marshal]::StructureToPtr($info, $buffer, $false)
        $ok = [SpecForgeBuildJob.NativeMethods]::SetInformationJobObject(
            $jobHandle,
            [SpecForgeBuildJob.NativeMethods]::JobObjectExtendedLimitInformation,
            $buffer,
            [uint32]$length)
        if (-not $ok) {
            $message = Get-LastWin32ErrorMessage
            Close-NativeHandle -Handle $jobHandle
            throw "SetInformationJobObject failed: $message"
        }
    }
    finally {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    }

    return $jobHandle
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
$preflightLogPath = Join-Path $LogDir "specforge-$Preset-$mode-$timestamp.preflight.log"
$watchdogPath = Join-Path $LogDir "specforge-$Preset-$mode-$timestamp.watchdog.ps1"
$watchdogLogPath = Join-Path $LogDir "specforge-$Preset-$mode-$timestamp.watchdog.log"

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
Write-Host "Preflight: $preflightLogPath"
Write-Host "Watchdog: $watchdogPath"

Assert-TaskkillTreeAvailable -LogPath $preflightLogPath

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$cmdArguments = '/d /c "' + $batchPath + '"'
$process = $null
$jobHandle = [IntPtr]::Zero
$watchdogProcess = $null

try {
    $jobHandle = New-KillOnCloseJob
    $process = Start-Process -FilePath 'cmd.exe' `
        -ArgumentList $cmdArguments `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -PassThru

    $assigned = [SpecForgeBuildJob.NativeMethods]::AssignProcessToJobObject($jobHandle, $process.Handle)
    if (-not $assigned) {
        $message = Get-LastWin32ErrorMessage
        [void](Invoke-TaskkillTree -ProcessId $process.Id)
        throw "AssignProcessToJobObject failed: $message"
    }

    $watchdogLines = @(
        '$ErrorActionPreference = ''Continue''',
        ('$pidToWatch = ' + $process.Id),
        ('$timeoutSec = ' + $TimeoutSec),
        ('$exitCodePath = ' + (Quote-PowerShellLiteral $exitCodePath)),
        ('$watchdogLogPath = ' + (Quote-PowerShellLiteral $watchdogLogPath)),
        '$started = Get-Date',
        'function Add-WatchdogLog {',
        '    param([string]$Message)',
        '    Add-Content -LiteralPath $watchdogLogPath -Value ("{0:o} {1}" -f (Get-Date), $Message)',
        '}',
        'Start-Sleep -Seconds $timeoutSec',
        'if (Test-Path -LiteralPath $exitCodePath) {',
        '    Add-WatchdogLog "exit file exists; watchdog exiting"',
        '    exit 0',
        '}',
        '$process = Get-Process -Id $pidToWatch -ErrorAction SilentlyContinue',
        'if ($null -eq $process) {',
        '    Add-WatchdogLog "root process already exited without exit file"',
        '    exit 0',
        '}',
        'Add-WatchdogLog ("timeout after {0}s; killing process tree rooted at PID {1}" -f $timeoutSec, $pidToWatch)',
        '$taskkillOutput = @(& taskkill.exe /PID $pidToWatch /T /F 2>&1)',
        '$taskkillExitCode = $LASTEXITCODE',
        'foreach ($line in $taskkillOutput) { Add-WatchdogLog ("taskkill: " + $line) }',
        'Add-WatchdogLog ("taskkill exit code: " + $taskkillExitCode)',
        'Start-Sleep -Milliseconds 500',
        '$process = Get-Process -Id $pidToWatch -ErrorAction SilentlyContinue',
        'if ($null -ne $process) {',
        '    Add-WatchdogLog ("root PID {0} still alive after taskkill; trying Stop-Process fallback" -f $pidToWatch)',
        '    Stop-Process -Id $pidToWatch -Force -ErrorAction SilentlyContinue',
        '    Start-Sleep -Milliseconds 500',
        '}',
        '$process = Get-Process -Id $pidToWatch -ErrorAction SilentlyContinue',
        'if ($null -ne $process) {',
        '    Add-WatchdogLog ("root PID {0} still alive after kill attempts" -f $pidToWatch)',
        '    exit 1',
        '}',
        'Add-WatchdogLog ("root PID {0} is gone" -f $pidToWatch)',
        'exit 0'
    )
    Set-Content -LiteralPath $watchdogPath -Value $watchdogLines -Encoding ASCII
    $watchdogArguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $watchdogPath + '"'
    $watchdogProcess = Start-Process -FilePath 'powershell.exe' `
        -ArgumentList $watchdogArguments `
        -WorkingDirectory $repoRoot `
        -WindowStyle Hidden `
        -PassThru

    while (-not $process.WaitForExit(1000)) {
        if ($stopwatch.Elapsed.TotalSeconds -ge $TimeoutSec) {
            Close-NativeHandle -Handle $jobHandle
            $jobHandle = [IntPtr]::Zero
            Start-Sleep -Milliseconds 250
            $process.Refresh()
            if (-not $process.HasExited) {
                [void](Invoke-TaskkillTree -ProcessId $process.Id)
            }

            $stopwatch.Stop()
            Write-LogTail -Path $stdoutPath -Label 'stdout'
            Write-LogTail -Path $stderrPath -Label 'stderr'
            throw "Timed out after $TimeoutSec seconds; killed process job rooted at PID $($process.Id)."
        }
    }

    $process.WaitForExit()
    $process.Refresh()
}
finally {
    Close-NativeHandle -Handle $jobHandle
    if ($null -ne $watchdogProcess) {
        $watchdogProcess.Refresh()
        if (-not $watchdogProcess.HasExited) {
            Stop-Process -Id $watchdogProcess.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

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
