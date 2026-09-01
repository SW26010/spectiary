[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Expected,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    Assert-True `
        -Condition ($Text.IndexOf($Expected, [StringComparison]::Ordinal) -ge 0) `
        -Message "$Description is missing '$Expected'."
}

function Read-FixtureProcessIdentity {
    param(
        [Parameter(Mandatory = $true)] [string]$Path
    )

    $parts = [IO.File]::ReadAllText($Path).Split('|')
    Assert-True `
        -Condition ($parts.Count -eq 2) `
        -Message "Fixture process identity is malformed: $Path"
    return [pscustomobject]@{
        ProcessId = [int]$parts[0]
        StartTicks = [long]$parts[1]
    }
}

function Assert-ProcessExited {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Identity,
        [Parameter(Mandatory = $true)] [string]$Description,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [Collections.Generic.List[Diagnostics.Process]]$OwnedProcessHandles
    )

    $process = $null
    $ownershipTransferred = $false
    try {
        $process = [Diagnostics.Process]::GetProcessById($Identity.ProcessId)
        [void]$process.Handle
        $startTicks = $process.StartTime.ToUniversalTime().Ticks
        if ($startTicks -ne $Identity.StartTicks) {
            # The fixture exited and Windows reused its PID. The new process
            # is unrelated and must never become cleanup-owned.
            return
        }
        $OwnedProcessHandles.Add($process)
        $ownershipTransferred = $true
        $process.Refresh()
        Assert-True `
            -Condition $process.HasExited `
            -Message "$Description PID $($Identity.ProcessId) is still running."
    }
    catch [ArgumentException] {
        # The PID is absent, which is the expected terminal state.
    }
    catch [InvalidOperationException] {
        # The held process exited while its start identity was inspected.
    }
    finally {
        if ($null -ne $process -and -not $ownershipTransferred) {
            $process.Dispose()
        }
    }
}

$runnerPath = Join-Path $RepoRoot 'scripts\lib\bounded_process.ps1'
. $runnerPath

$testRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    "specforge-bounded-process-tests-$PID-$([Guid]::NewGuid().ToString('N'))"
$cleanupStatus = 'passed'
$ownedFixtureProcesses =
    [Collections.Generic.List[Diagnostics.Process]]::new()
try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    $reusedPidHandles =
        [Collections.Generic.List[Diagnostics.Process]]::new()
    Assert-ProcessExited `
        -Identity ([pscustomobject]@{
            ProcessId = $PID
            StartTicks = 0
        }) `
        -Description 'Reused PID identity mismatch' `
        -OwnedProcessHandles $reusedPidHandles
    Assert-True `
        -Condition ($reusedPidHandles.Count -eq 0) `
        -Message 'A mismatched start identity acquired cleanup ownership.'

    $grandchildPath = Join-Path $testRoot 'grandchild.ps1'
    $fixturePath = Join-Path $testRoot 'fixture.ps1'
    [IO.File]::WriteAllText(
        $grandchildPath,
        @'
param([string]$PidPath)
$process = [Diagnostics.Process]::GetCurrentProcess()
[IO.File]::WriteAllText(
    $PidPath,
    ('{0}|{1}' -f $PID, $process.StartTime.ToUniversalTime().Ticks))
Start-Sleep -Seconds 300
'@)
    [IO.File]::WriteAllText(
        $fixturePath,
        @'
param(
    [string]$Mode,
    [string]$StateDirectory,
    [AllowEmptyString()]
    [string]$Value
)

function Start-SleepingGrandchild {
    param([string]$PidFileName)

    $grandchildPath = Join-Path $StateDirectory 'grandchild.ps1'
    $grandchildPidPath = Join-Path $StateDirectory $PidFileName
    $grandchild = Start-Process `
        -FilePath 'powershell.exe' `
        -ArgumentList @(
            '-NoProfile',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            ('"' + $grandchildPath + '"'),
            ('"' + $grandchildPidPath + '"')
        ) `
        -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $grandchildPidPath)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Grandchild '$PidFileName' did not report its PID."
        }
        Start-Sleep -Milliseconds 25
    }
    return [IO.File]::ReadAllText($grandchildPidPath)
}

if ($Mode -ceq 'immediate-orphan') {
    [IO.File]::WriteAllText(
        (Join-Path $StateDirectory 'immediate-target-started'),
        [string]$PID)
    $null = Start-SleepingGrandchild -PidFileName 'immediate-grandchild.pid'
    Write-Output 'immediate orphan root exiting'
    exit 0
}
if ($Mode -ceq 'success') {
    Write-Output 'success stdout marker'
    exit 0
}
if ($Mode -ceq 'empty-argument') {
    if ($Value -cne '') {
        throw "Expected an empty argument, received '$Value'."
    }
    Write-Output 'empty argument preserved'
    exit 0
}
if ($Mode -ceq 'failure') {
    Write-Output 'expected failure stdout marker'
    [Console]::Error.WriteLine('expected failure stderr marker')
    exit 7
}
if ($Mode -ceq 'orphan-after-exit') {
    $null = Start-SleepingGrandchild -PidFileName 'orphan-grandchild.pid'
    Write-Output 'orphan root exiting normally'
    exit 0
}
if ($Mode -ceq 'timeout-tree') {
    $process = [Diagnostics.Process]::GetCurrentProcess()
    [IO.File]::WriteAllText(
        (Join-Path $StateDirectory 'root.pid'),
        ('{0}|{1}' -f $PID, $process.StartTime.ToUniversalTime().Ticks))
    $null = Start-SleepingGrandchild -PidFileName 'grandchild.pid'
    Write-Output 'timeout stdout marker'
    [Console]::Error.WriteLine('timeout stderr marker')
    Start-Sleep -Seconds 300
}
throw "Unknown fixture mode '$Mode'."
'@)

    $successOutput = @(
        Invoke-SpecForgeBoundedValidationCase `
            -CaseId 'runner-success' `
            -Description 'Runner fast success' `
            -FilePath 'powershell.exe' `
            -Arguments @(
                '-NoProfile',
                '-ExecutionPolicy',
                'Bypass',
                '-File',
                $fixturePath,
                '-Mode',
                'success',
                '-StateDirectory',
                $testRoot
            ) `
            -TimeoutSec 10 `
            -ExpectedOutcome Success `
            -EchoOutput *>&1
    ) -join [Environment]::NewLine
    Assert-Contains $successOutput 'CASE BEGIN id=runner-success' 'Success case log'
    Assert-Contains $successOutput 'CASE END id=runner-success status=passed' 'Success case log'
    Assert-Contains $successOutput 'success stdout marker' 'Success case output'

    $emptyArgumentOutput = @(
        Invoke-SpecForgeBoundedValidationCase `
            -CaseId 'runner-empty-argument' `
            -Description 'Runner preserves an empty argument' `
            -FilePath 'powershell.exe' `
            -Arguments @(
                '-NoProfile',
                '-ExecutionPolicy',
                'Bypass',
                '-File',
                $fixturePath,
                '-Mode',
                'empty-argument',
                '-StateDirectory',
                $testRoot,
                '-Value',
                ''
            ) `
            -TimeoutSec 10 `
            -ExpectedOutcome Success `
            -EchoOutput *>&1
    ) -join [Environment]::NewLine
    Assert-Contains `
        $emptyArgumentOutput `
        'CASE END id=runner-empty-argument status=passed' `
        'Empty-argument case log'
    Assert-Contains `
        $emptyArgumentOutput `
        'empty argument preserved' `
        'Empty-argument case output'

    $failureOutput = @(
        Invoke-SpecForgeBoundedValidationCase `
            -CaseId 'runner-expected-failure' `
            -Description 'Runner expected failure' `
            -FilePath 'powershell.exe' `
            -Arguments @(
                '-NoProfile',
                '-ExecutionPolicy',
                'Bypass',
                '-File',
                $fixturePath,
                '-Mode',
                'failure',
                '-StateDirectory',
                $testRoot
            ) `
            -TimeoutSec 10 `
            -ExpectedOutcome Failure `
            -ExpectedMessage 'expected failure stderr marker' *>&1
    ) -join [Environment]::NewLine
    Assert-Contains `
        $failureOutput `
        'CASE END id=runner-expected-failure status=passed' `
        'Expected-failure case log'

    $orphanOutput = @(
        Invoke-SpecForgeBoundedValidationCase `
            -CaseId 'runner-normal-exit-orphan' `
            -Description 'Runner normal exit cleans a surviving descendant' `
            -FilePath 'powershell.exe' `
            -Arguments @(
                '-NoProfile',
                '-ExecutionPolicy',
                'Bypass',
                '-File',
                $fixturePath,
                '-Mode',
                'orphan-after-exit',
                '-StateDirectory',
                $testRoot
            ) `
            -TimeoutSec 10 `
            -ExpectedOutcome Success *>&1
    ) -join [Environment]::NewLine
    Assert-Contains `
        $orphanOutput `
        'CASE END id=runner-normal-exit-orphan status=passed' `
        'Normal-exit orphan case log'
    $orphanIdentity = Read-FixtureProcessIdentity `
        -Path (Join-Path $testRoot 'orphan-grandchild.pid')
    Assert-ProcessExited `
        -Identity $orphanIdentity `
        -Description 'Normal-exit orphan grandchild' `
        -OwnedProcessHandles $ownedFixtureProcesses

    $immediateMarkerPath = Join-Path $testRoot 'immediate-target-started'
    $immediateResult = Invoke-SpecForgeBoundedProcess `
        -FilePath 'powershell.exe' `
        -Arguments @(
            '-NoProfile',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            $fixturePath,
            '-Mode',
            'immediate-orphan',
            '-StateDirectory',
            $testRoot
        ) `
        -TimeoutSec 10 `
        -BeforeAssignmentProbe {
            Start-Sleep -Milliseconds 750
            Assert-True `
                -Condition (-not (Test-Path -LiteralPath $immediateMarkerPath)) `
                -Message 'Target started before the bootstrap entered its Job.'
        }
    Assert-True `
        -Condition (-not $immediateResult.TimedOut -and $immediateResult.ExitCode -eq 0) `
        -Message 'Immediate-orphan launch did not complete successfully.'
    Assert-True `
        -Condition (Test-Path -LiteralPath $immediateMarkerPath) `
        -Message 'Immediate target did not start after Job assignment.'
    $immediateGrandchildIdentity = Read-FixtureProcessIdentity `
        -Path (Join-Path $testRoot 'immediate-grandchild.pid')
    Assert-ProcessExited `
        -Identity $immediateGrandchildIdentity `
        -Description 'Immediate-launch orphan grandchild' `
        -OwnedProcessHandles $ownedFixtureProcesses

    $unexpectedSuccessOutput = @(
        & {
            try {
                Invoke-SpecForgeBoundedValidationCase `
                    -CaseId 'runner-unexpected-success' `
                    -Description 'Runner negative case unexpectedly succeeds' `
                    -FilePath 'powershell.exe' `
                    -Arguments @(
                        '-NoProfile',
                        '-ExecutionPolicy',
                        'Bypass',
                        '-File',
                        $fixturePath,
                        '-Mode',
                        'success',
                        '-StateDirectory',
                        $testRoot
                    ) `
                    -TimeoutSec 10 `
                    -ExpectedOutcome Failure
            }
            catch {
                Write-Output "CAPTURED FAILURE: $($_.Exception.Message)"
            }
        } *>&1
    ) -join [Environment]::NewLine
    Assert-Contains `
        $unexpectedSuccessOutput `
        'CASE END id=runner-unexpected-success status=unexpected-success' `
        'Unexpected-success case log'
    Assert-Contains `
        $unexpectedSuccessOutput `
        'success stdout marker' `
        'Unexpected-success stdout tail'

    $semanticOutput = @(
        & {
            try {
                Invoke-SpecForgeBoundedValidationCase `
                    -CaseId 'runner-wrong-reason' `
                    -Description 'Runner wrong failure reason' `
                    -FilePath 'powershell.exe' `
                    -Arguments @(
                        '-NoProfile',
                        '-ExecutionPolicy',
                        'Bypass',
                        '-File',
                        $fixturePath,
                        '-Mode',
                        'failure',
                        '-StateDirectory',
                        $testRoot
                    ) `
                    -TimeoutSec 10 `
                    -ExpectedOutcome Failure `
                    -ExpectedMessage 'absent semantic marker'
            }
            catch {
                Write-Output "CAPTURED FAILURE: $($_.Exception.Message)"
            }
        } *>&1
    ) -join [Environment]::NewLine
    Assert-Contains `
        $semanticOutput `
        'CASE END id=runner-wrong-reason status=wrong-failure' `
        'Wrong-reason case log'
    Assert-Contains $semanticOutput 'runner-wrong-reason' 'Wrong-reason failure'
    Assert-Contains `
        $semanticOutput `
        'expected failure stdout marker' `
        'Wrong-reason stdout tail'
    Assert-Contains `
        $semanticOutput `
        'expected failure stderr marker' `
        'Wrong-reason stderr tail'

    $timeoutOutput = @(
        & {
            try {
                Invoke-SpecForgeBoundedValidationCase `
                    -CaseId 'runner-timeout-tree' `
                    -Description 'Runner timeout tree' `
                    -FilePath 'powershell.exe' `
                    -Arguments @(
                        '-NoProfile',
                        '-ExecutionPolicy',
                        'Bypass',
                        '-File',
                        $fixturePath,
                        '-Mode',
                        'timeout-tree',
                        '-StateDirectory',
                        $testRoot
                    ) `
                    -TimeoutSec 2 `
                    -ExpectedOutcome Success
            }
            catch {
                Write-Output "CAPTURED FAILURE: $($_.Exception.Message)"
            }
        } *>&1
    ) -join [Environment]::NewLine
    Assert-Contains `
        $timeoutOutput `
        'CASE END id=runner-timeout-tree status=timeout' `
        'Timeout case log'
    Assert-Contains $timeoutOutput 'runner-timeout-tree' 'Timeout failure'
    Assert-Contains $timeoutOutput 'timeout stdout marker' 'Timeout stdout tail'
    Assert-Contains $timeoutOutput 'timeout stderr marker' 'Timeout stderr tail'

    $rootIdentity = Read-FixtureProcessIdentity `
        -Path (Join-Path $testRoot 'root.pid')
    $grandchildIdentity = Read-FixtureProcessIdentity `
        -Path (Join-Path $testRoot 'grandchild.pid')
    Assert-ProcessExited `
        -Identity $rootIdentity `
        -Description 'Timed-out root process' `
        -OwnedProcessHandles $ownedFixtureProcesses
    Assert-ProcessExited `
        -Identity $grandchildIdentity `
        -Description 'Timed-out grandchild process' `
        -OwnedProcessHandles $ownedFixtureProcesses
}
finally {
    Write-Host "CASE CLEANUP BEGIN id=bounded-process-test-root path=$testRoot"
    try {
        foreach ($ownedProcess in $ownedFixtureProcesses) {
            try {
                $ownedProcess.Refresh()
                if (-not $ownedProcess.HasExited) {
                $ownedProcess.Kill()
                [void]$ownedProcess.WaitForExit(5000)
                }
            }
            finally {
                $ownedProcess.Dispose()
            }
        }
        if (Test-Path -LiteralPath $testRoot) {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
        }
        if (Test-Path -LiteralPath $testRoot) {
            throw "Bounded process test root remains after cleanup: $testRoot"
        }
    }
    catch {
        $cleanupStatus = 'failed'
        throw
    }
    finally {
        Write-Host (
            'CASE CLEANUP END id=bounded-process-test-root status={0}' -f
                $cleanupStatus)
    }
}

Write-Output 'release artifact bounded process runner tests passed'
