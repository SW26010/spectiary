[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Headless', 'RealGui')]
    [string]$Mode,
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$ArtifactsDirectory,
    [Alias('SuiteTimeoutSec')]
    [ValidateRange(1, 3600)]
    [int]$TimeoutSec = 300,
    [ValidateRange(1, 3600)]
    [int]$TestTimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$ciRunnerDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ciRunnerDirectory 'automation_process_guard.ps1')

function Write-Utf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Contents
    )

    [System.IO.File]::WriteAllText(
        $Path,
        $Contents,
        [System.Text.UTF8Encoding]::new($false))
}

function Quote-WindowsArgument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ($Value.Length -eq 0) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

function Add-FailureMessage {
    param(
        [AllowNull()]
        [string]$Current,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ([string]::IsNullOrWhiteSpace($Current)) {
        return $Message
    }
    return "$Current $Message"
}

function Get-CTestTemporaryFileSnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TemporaryDirectory
    )

    $snapshot = @{}
    if (-not (Test-Path -LiteralPath $TemporaryDirectory -PathType Container)) {
        return $snapshot
    }
    foreach ($item in @(
            Get-ChildItem -LiteralPath $TemporaryDirectory -File -ErrorAction Stop)) {
        $snapshot[$item.FullName] = [pscustomobject]@{
            length = [int64]$item.Length
            last_write_ticks = [int64]$item.LastWriteTimeUtc.Ticks
        }
    }
    return $snapshot
}

function Write-CTestTemporaryEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDirectory,
        [Parameter(Mandatory = $true)]
        [string]$ArtifactsDirectory,
        [Parameter(Mandatory = $true)]
        [hashtable]$Baseline,
        [Parameter(Mandatory = $true)]
        [DateTime]$StartedUtc,
        [Parameter(Mandatory = $true)]
        [int]$CTestPid,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $sourceDirectory = Join-Path $BuildDirectory 'Testing\Temporary'
    $destinationDirectory = Join-Path $ArtifactsDirectory 'ctest-temporary'
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    $capturedFiles = [System.Collections.Generic.List[object]]::new()
    $copyFailures = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $sourceDirectory -PathType Container) {
        foreach ($sourceItem in @(
                Get-ChildItem -LiteralPath $sourceDirectory -File -ErrorAction Stop)) {
            $baselineItem = $Baseline[$sourceItem.FullName]
            $changedThisRun = $null -eq $baselineItem -or
                [int64]$baselineItem.length -ne [int64]$sourceItem.Length -or
                [int64]$baselineItem.last_write_ticks -ne
                    [int64]$sourceItem.LastWriteTimeUtc.Ticks -or
                $sourceItem.LastWriteTimeUtc.ToUniversalTime() -ge $StartedUtc
            if (-not $changedThisRun) {
                continue
            }
            $destinationPath = Join-Path $destinationDirectory $sourceItem.Name
            try {
                Copy-Item -LiteralPath $sourceItem.FullName `
                    -Destination $destinationPath -Force -ErrorAction Stop
                [void]$capturedFiles.Add([ordered]@{
                    source = $sourceItem.FullName
                    artifact = $destinationPath
                    length = [int64]$sourceItem.Length
                    last_write_utc = $sourceItem.LastWriteTimeUtc.ToString('O')
                })
            }
            catch {
                [void]$copyFailures.Add(
                    "Could not copy current CTest evidence $($sourceItem.FullName): $($_.Exception.Message)")
            }
        }
    }
    $manifestPath = Join-Path $destinationDirectory 'manifest.json'
    $manifest = [ordered]@{
        schema = 1
        label = $Label
        ctest_pid = $CTestPid
        current_ctest_run_started_utc = $StartedUtc.ToString('O')
        source_directory = $sourceDirectory
        evidence_policy = 'Only files newly created or modified by this CTest run are copied; historical Testing/Temporary files are excluded.'
        files = @($capturedFiles)
        copy_failures = @($copyFailures)
        captured_utc = [DateTime]::UtcNow.ToString('O')
    }
    Write-Utf8File -Path $manifestPath -Contents ($manifest | ConvertTo-Json -Depth 12)
    if ($copyFailures.Count -gt 0) {
        throw (
            'Current CTest evidence capture failed: ' +
            ($copyFailures -join ' '))
    }
    return $manifestPath
}

function Stop-OwnedCTestProcess {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedExecutable,
        [Parameter(Mandatory = $true)]
        [int64]$ExpectedStartTicks,
        [System.Diagnostics.Process]$ProcessHandle = $null
    )

    if ($ProcessId -le 0) {
        return
    }
    if ($ExpectedStartTicks -le 0) {
        throw "Refusing to stop CTest PID $ProcessId without a start identity."
    }
    $ownsProcessHandle = $null -eq $ProcessHandle
    $process = if ($ownsProcessHandle) {
        Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    }
    else {
        $ProcessHandle
    }
    if ($null -eq $process) {
        return
    }
    if ($process.HasExited) {
        return
    }
    try {
        $process.Refresh()
        if ([string]::IsNullOrWhiteSpace($process.Path) -or
            -not [string]::Equals(
                $process.Path,
                $ExpectedExecutable,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to stop CTest PID $ProcessId because its executable path is not owned."
        }
        if ($process.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) {
            throw "Refusing to stop CTest PID $ProcessId because its start identity does not match ownership."
        }
        Stop-AutomationProcessHandle `
            -ProcessHandle $process `
            -WaitMilliseconds 5000
    }
    finally {
        if ($ownsProcessHandle) {
            $process.Dispose()
        }
    }
}

function Assert-OwnedCTestProcessGone {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [int64]$ExpectedStartTicks,
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$ProcessHandle
    )

    if ($ProcessId -le 0) {
        return
    }
    if ($ProcessHandle.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) {
        throw "CTest PID $ProcessId start identity changed during cleanup."
    }
    if (-not $ProcessHandle.HasExited) {
        throw "Owned CTest PID $ProcessId remained after bounded cleanup."
    }
}

function Invoke-BoundedCTest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDirectory,
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [Parameter(Mandatory = $true)]
        [string]$LogPath,
        [Parameter(Mandatory = $true)]
        [int]$SuiteTimeoutSec,
        [Parameter(Mandatory = $true)]
        [int]$TestTimeoutSec
    )

    $process = $null
    $stdoutTask = $null
    $stderrTask = $null
    $ctestPid = 0
    $ctestStartTicks = 0
    $expectedExecutable = $null
    $stdout = ''
    $stderr = ''
    $exitCode = 1
    $failureMessage = $null
    $processCleanupFailures = [System.Collections.Generic.List[string]]::new()
    $timedOut = $false
    $jobHandle = [IntPtr]::Zero

    try {
        $ctestCommand = Get-Command ctest.exe -ErrorAction Stop |
            Select-Object -First 1
        $expectedExecutable = (Resolve-Path -LiteralPath $ctestCommand.Source).Path
        $start = [System.Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $expectedExecutable
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
        $start.StandardOutputEncoding = $utf8WithoutBom
        $start.StandardErrorEncoding = $utf8WithoutBom
        $arguments = @(
            '--test-dir', $BuildDirectory,
            '--output-on-failure',
            '--no-tests=error',
            '--timeout', [string]$TestTimeoutSec,
            '-L', $Label
        )
        $start.Arguments = ($arguments |
                ForEach-Object { Quote-WindowsArgument -Value $_ }) -join ' '
        $jobHandle = New-AutomationKillOnCloseJob
        $process = [System.Diagnostics.Process]::Start($start)
        $ctestPid = $process.Id
        $process.Refresh()
        $ctestStartTicks = $process.StartTime.ToUniversalTime().Ticks
        Assign-AutomationProcessToJob `
            -JobHandle $jobHandle `
            -ProcessHandle $process
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if (-not $process.WaitForExit($SuiteTimeoutSec * 1000)) {
            $timedOut = $true
            $failureMessage =
                "CTest label '$Label' exceeded its $($SuiteTimeoutSec)s suite timeout."
            try {
                Stop-OwnedCTestProcess `
                    -ProcessId $ctestPid `
                    -ExpectedExecutable $expectedExecutable `
                    -ExpectedStartTicks $ctestStartTicks `
                    -ProcessHandle $process
            }
            catch {
                [void]$processCleanupFailures.Add($_.Exception.Message)
            }
        }
        elseif ($process.HasExited) {
            $process.Refresh()
            $exitCode = [int]$process.ExitCode
        }
    }
    catch {
        $failureMessage = Add-FailureMessage -Current $failureMessage -Message $_.Exception.Message
    }
    finally {
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited -and
                    -not [string]::IsNullOrWhiteSpace($expectedExecutable)) {
                    Stop-OwnedCTestProcess `
                        -ProcessId $ctestPid `
                        -ExpectedExecutable $expectedExecutable `
                        -ExpectedStartTicks $ctestStartTicks `
                        -ProcessHandle $process
                }
            }
            catch {
                [void]$processCleanupFailures.Add($_.Exception.Message)
            }
            $processExitConfirmed = $false
            if (-not $process.HasExited) {
                try {
                    [void]$process.WaitForExit(5000)
                }
                catch {
                    [void]$processCleanupFailures.Add(
                        "CTest PID $ctestPid did not confirm exit: $($_.Exception.Message)")
                }
            }
            try {
                $processExitConfirmed = $process.HasExited
            }
            catch {
                [void]$processCleanupFailures.Add(
                    "CTest PID $ctestPid exit state could not be confirmed: $($_.Exception.Message)")
            }
            if ($null -ne $stdoutTask) {
                if ($stdoutTask.Wait(5000)) {
                    try {
                        $stdout = [string]$stdoutTask.Result
                    }
                    catch {
                        $failureMessage = Add-FailureMessage -Current $failureMessage -Message 'CTest stdout could not be collected.'
                    }
                }
                else {
                    $failureMessage = Add-FailureMessage -Current $failureMessage -Message 'CTest stdout did not drain within the bounded cleanup deadline.'
                }
            }
            if ($null -ne $stderrTask) {
                if ($stderrTask.Wait(5000)) {
                    try {
                        $stderr = [string]$stderrTask.Result
                    }
                    catch {
                        $failureMessage = Add-FailureMessage -Current $failureMessage -Message 'CTest stderr could not be collected.'
                    }
                }
                else {
                    $failureMessage = Add-FailureMessage -Current $failureMessage -Message 'CTest stderr did not drain within the bounded cleanup deadline.'
                }
            }
            try {
                Assert-OwnedCTestProcessGone `
                    -ProcessId $ctestPid `
                    -ExpectedStartTicks $ctestStartTicks `
                    -ProcessHandle $process
            }
            catch {
                [void]$processCleanupFailures.Add($_.Exception.Message)
            }
            if (-not $processExitConfirmed -and $processCleanupFailures.Count -eq 0) {
                [void]$processCleanupFailures.Add(
                    "CTest PID $ctestPid did not confirm exit after bounded cleanup.")
            }
            foreach ($processCleanupFailure in @($processCleanupFailures)) {
                if (-not $processExitConfirmed -or
                    $processCleanupFailure -notmatch '^Owned CTest PID \d+ remained after bounded cleanup\.$') {
                    $failureMessage = Add-FailureMessage `
                        -Current $failureMessage `
                        -Message $processCleanupFailure
                }
            }
            if ($jobHandle -ne [IntPtr]::Zero) {
                Close-AutomationNativeHandle -Handle $jobHandle
                $jobHandle = [IntPtr]::Zero
            }
            $process.Dispose()
        }
        if ($jobHandle -ne [IntPtr]::Zero) {
            Close-AutomationNativeHandle -Handle $jobHandle
            $jobHandle = [IntPtr]::Zero
        }
        if ($timedOut -or $null -ne $failureMessage) {
            $exitCode = 1
        }
        $log = [ordered]@{
            label = $Label
            suite_timeout_sec = $SuiteTimeoutSec
            test_timeout_sec = $TestTimeoutSec
            ctest_pid = $ctestPid
            ctest_start_ticks = $ctestStartTicks
            exit_code = $exitCode
            failure = $failureMessage
            stdout = $stdout
            stderr = $stderr
        }
        Write-Utf8File -Path $LogPath -Contents ($log | ConvertTo-Json -Depth 12)
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Failure = $failureMessage
        Stdout = $stdout
        Stderr = $stderr
        CTestPid = $ctestPid
    }
}

$resolvedBuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
New-Item -ItemType Directory -Path $ArtifactsDirectory -Force | Out-Null
$resolvedArtifactsDirectory =
    (Resolve-Path -LiteralPath $ArtifactsDirectory).Path

$label = if ($Mode -eq 'Headless') { 'automation-headless' } else { 'real-gui' }
$logPath = Join-Path $resolvedArtifactsDirectory "ctest-$($Mode.ToLowerInvariant()).log"
$summaryPath = Join-Path $resolvedArtifactsDirectory "ctest-$($Mode.ToLowerInvariant()).summary.json"
$sampleArtifactsDirectory = Join-Path $resolvedArtifactsDirectory 'automation-samples'
$ctestTemporaryDirectory = Join-Path $resolvedBuildDirectory 'Testing\Temporary'
$ctestTemporarySnapshot = @{}
$previousSampleArtifactsDirectory =
    [Environment]::GetEnvironmentVariable('SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS')
Write-Utf8File -Path $logPath -Contents ''
$startedUtc = [DateTime]::UtcNow
$ctestExitCode = 1
$failureMessage = $null
$ctestPid = 0

try {
    New-Item -ItemType Directory -Path $sampleArtifactsDirectory -Force | Out-Null
    $ctestTemporarySnapshot = Get-CTestTemporaryFileSnapshot `
        -TemporaryDirectory $ctestTemporaryDirectory
    [Environment]::SetEnvironmentVariable(
        'SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS',
        $sampleArtifactsDirectory,
        'Process')
    Write-Host (
        "Running CTest label '$label' with a $TimeoutSec second suite budget " +
        "and a $TestTimeoutSec second per-test timeout in $resolvedBuildDirectory")
    $ctestResult = Invoke-BoundedCTest -BuildDirectory $resolvedBuildDirectory -Label $label -LogPath $logPath -SuiteTimeoutSec $TimeoutSec -TestTimeoutSec $TestTimeoutSec
    $ctestExitCode = $ctestResult.ExitCode
    $failureMessage = $ctestResult.Failure
    $ctestPid = $ctestResult.CTestPid
    if (-not [string]::IsNullOrWhiteSpace($ctestResult.Stdout)) {
        Write-Host $ctestResult.Stdout
    }
    if (-not [string]::IsNullOrWhiteSpace($ctestResult.Stderr)) {
        Write-Host $ctestResult.Stderr
    }
}
catch {
    $failureMessage = $_.Exception.Message
    Add-Content -LiteralPath $logPath -Value $failureMessage
}
finally {
    try {
        Write-CTestTemporaryEvidence `
            -BuildDirectory $resolvedBuildDirectory `
            -ArtifactsDirectory $resolvedArtifactsDirectory `
            -Baseline $ctestTemporarySnapshot `
            -StartedUtc $startedUtc `
            -CTestPid $ctestPid `
            -Label $label | Out-Null
    }
    catch {
        $failureMessage = Add-FailureMessage `
            -Current $failureMessage `
            -Message $_.Exception.Message
        $ctestExitCode = 1
    }
    [Environment]::SetEnvironmentVariable(
        'SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS',
        $previousSampleArtifactsDirectory,
        'Process')
    if ($ctestExitCode -ne 0 -and
        [string]::IsNullOrWhiteSpace($failureMessage)) {
        $failureMessage =
            "CTest label '$label' failed with exit code $ctestExitCode."
    }
    $summary = [ordered]@{
        mode = $Mode
        label = $label
        suite_timeout_sec = $TimeoutSec
        test_timeout_sec = $TestTimeoutSec
        ctest_pid = $ctestPid
        started_utc = $startedUtc.ToString('O')
        finished_utc = [DateTime]::UtcNow.ToString('O')
        exit_code = $ctestExitCode
        passed = ($ctestExitCode -eq 0)
        failure = $failureMessage
    }
    Write-Utf8File -Path $summaryPath -Contents ($summary | ConvertTo-Json -Depth 5)
}

if ($ctestExitCode -ne 0) {
    if ($null -eq $failureMessage) {
        $failureMessage = "CTest label '$label' failed."
    }
    Write-Error $failureMessage
    exit $ctestExitCode
}

Write-Host "CTest label '$label' passed. Log: $logPath"
