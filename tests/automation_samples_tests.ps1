[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Launcher,
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$StateFixture,
    [Parameter(Mandatory = $true)]
    [string]$FixtureDirectory,
    [Parameter(Mandatory = $true)]
    [string]$ArtifactsDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$sampleRunnerDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $sampleRunnerDirectory '..\scripts\automation_process_guard.ps1')
$sampleArtifactsOverride = [Environment]::GetEnvironmentVariable(
    'SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS')
if (-not [string]::IsNullOrWhiteSpace($sampleArtifactsOverride)) {
    $ArtifactsDirectory = $sampleArtifactsOverride
}

$script:SequenceTimeoutMilliseconds = 60000
$script:StateFixtureTimeoutMilliseconds = 30000
$script:CleanupWaitMilliseconds = 2000
$script:ProcessCleanupRetryCount = 3
$script:CleanupRetryCount = 10
$script:ArtifactRunCapacity = 3
$script:FailureEvidenceRetentionCount = $script:ArtifactRunCapacity
$script:FailureTempRetentionCount = 3
$script:SampleLeaseStaleAfterSeconds = 180
$script:SampleRunLease = $null
$script:SampleArtifactLeasePath = $null
$script:OwnedProcessIdentities = [System.Collections.Generic.List[object]]::new()
$script:PendingLaunches = [System.Collections.Generic.List[object]]::new()

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Write-Utf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Contents
    )

    [System.IO.File]::WriteAllText(
        $Path,
        $Contents,
        [System.Text.UTF8Encoding]::new($false))
}

function Write-AtomicUtf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Contents
    )

    $temporaryPath = "$Path.tmp-$([Guid]::NewGuid().ToString('N'))"
    try {
        Write-Utf8File -Path $temporaryPath -Contents $Contents
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            [System.IO.File]::Replace($temporaryPath, $Path, $null, $true)
        }
        else {
            [System.IO.File]::Move($temporaryPath, $Path)
        }
    }
    catch {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            try {
                Move-Item -LiteralPath $temporaryPath -Destination $Path -Force -ErrorAction Stop
            }
            catch {
                throw $_
            }
        }
        else {
            throw
        }
    }
}

function Get-ProcessIdentityRecord {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('helper', 'launcher', 'gui')]
        [string]$Role,
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedExecutable,
        [System.Diagnostics.Process]$ProcessHandle = $null,
        [string]$LaunchToken = $null,
        [int64]$KnownStartTicks = 0
    )

    if ($ProcessId -le 0) {
        return $null
    }
    $ownsProcessHandle = $null -eq $ProcessHandle
    $process = if ($ownsProcessHandle) {
        Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    }
    else {
        $ProcessHandle
    }
    if ($null -eq $process) {
        return $null
    }
    try {
        $process.Refresh()
        $processPath = $process.Path
        if ([string]::IsNullOrWhiteSpace($processPath) -and
            $null -ne $ProcessHandle) {
            # A fast helper/launcher may exit between Process.Start and this
            # registration call. The held handle plus ProcessStartInfo's
            # resolved FileName still identify that launch; do not discard the
            # start identity merely because Windows no longer exposes Path.
            $processPath = $ExpectedExecutable
        }
        Assert-True `
            -Condition (
                -not [string]::IsNullOrWhiteSpace($processPath) -and
                [string]::Equals(
                    $processPath,
                    $ExpectedExecutable,
                    [System.StringComparison]::OrdinalIgnoreCase)) `
            -Message "Refusing to record $Role PID $ProcessId because its executable path is not owned."
        $startUtc = if ($KnownStartTicks -gt 0) {
            [DateTime]::new($KnownStartTicks, [DateTimeKind]::Utc)
        }
        else {
            $process.StartTime.ToUniversalTime()
        }
        return [pscustomobject]@{
            role = $Role
            pid = $ProcessId
            executable = $processPath
            start_ticks = $startUtc.Ticks
            start_utc = $startUtc.ToString('O')
            launch_token = $LaunchToken
        }
    }
    finally {
        if ($ownsProcessHandle) {
            $process.Dispose()
        }
    }
}

function New-SampleLaunchToken {
    return [Guid]::NewGuid().ToString('N')
}

function Add-SampleLaunchPending {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('helper', 'launcher', 'gui')]
        [string]$Role,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedExecutable,
        [Parameter(Mandatory = $true)]
        [string]$Token
    )

    $existing = @(
        $script:PendingLaunches |
            Where-Object { [string]$_.token -eq $Token })
    if ($existing.Count -eq 0) {
        [void]$script:PendingLaunches.Add([pscustomobject]@{
                state = 'launch_pending'
                role = $Role
                token = $Token
                executable = $ExpectedExecutable
                pid = 0
                start_ticks = 0
                job_assigned = $false
                created_utc = [DateTime]::UtcNow.ToString('O')
            })
    }
}

function Set-SampleLaunchPendingIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Token,
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [int64]$StartTicks,
        [bool]$JobAssigned = $false
    )

    $pending = @(
        $script:PendingLaunches |
            Where-Object { [string]$_.token -eq $Token } |
            Select-Object -First 1)
    if ($pending.Count -ne 1) {
        throw "Launch pending token $Token was not found while recording process identity."
    }
    $pending[0].pid = $ProcessId
    $pending[0].start_ticks = $StartTicks
    $pending[0].job_assigned = $JobAssigned
}

function Complete-SampleLaunchPending {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Token
    )

    for ($index = $script:PendingLaunches.Count - 1; $index -ge 0; --$index) {
        if ([string]$script:PendingLaunches[$index].token -eq $Token) {
            $script:PendingLaunches.RemoveAt($index)
        }
    }
}

function Register-OwnedProcessIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('helper', 'launcher', 'gui')]
        [string]$Role,
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedExecutable,
        [System.Diagnostics.Process]$ProcessHandle = $null,
        [string]$LaunchToken = $null,
        [int64]$KnownStartTicks = 0
    )

    $record = Get-ProcessIdentityRecord `
        -Role $Role `
        -ProcessId $ProcessId `
        -ExpectedExecutable $ExpectedExecutable `
        -ProcessHandle $ProcessHandle `
        -LaunchToken $LaunchToken `
        -KnownStartTicks $KnownStartTicks
    if ($null -eq $record) {
        throw "Could not capture the $Role process start identity for PID $ProcessId."
    }
    $sameIdentity = @(
        $script:OwnedProcessIdentities |
            Where-Object {
                [string]$_.role -eq $Role -and
                [int]$_.pid -eq $ProcessId -and
                [int64]$_.start_ticks -eq [int64]$record.start_ticks -and
                [string]::Equals(
                    [string]$_.executable,
                    [string]$record.executable,
                    [System.StringComparison]::OrdinalIgnoreCase)
            })
    if ($sameIdentity.Count -eq 0) {
        [void]$script:OwnedProcessIdentities.Add($record)
    }
    return $record
}

function Get-OwnedProcessHandleForIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Record
    )

    $process = Get-Process -Id ([int]$Record.pid) -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return $null
    }
    try {
        $process.Refresh()
        if ([string]::IsNullOrWhiteSpace($process.Path) -or
            -not [string]::Equals(
                $process.Path,
                [string]$Record.executable,
                [System.StringComparison]::OrdinalIgnoreCase) -or
            $process.StartTime.ToUniversalTime().Ticks -ne
                [int64]$Record.start_ticks) {
            $process.Dispose()
            return $null
        }
        return $process
    }
    catch {
        $process.Dispose()
        return $null
    }
}

function Test-OwnedProcessIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Record
    )

    $process = Get-Process -Id ([int]$Record.pid) -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return $false
    }
    try {
        $process.Refresh()
        return (
            -not [string]::IsNullOrWhiteSpace($process.Path) -and
            [string]::Equals(
                $process.Path,
                [string]$Record.executable,
                [System.StringComparison]::OrdinalIgnoreCase) -and
            $process.StartTime.ToUniversalTime().Ticks -eq
                [int64]$Record.start_ticks)
    }
    catch {
        return $true
    }
    finally {
        $process.Dispose()
    }
}

function Get-SampleLeaseStatus {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LeasePath
    )

    if (-not (Test-Path -LiteralPath $LeasePath -PathType Leaf)) {
        return [pscustomobject]@{
            state = 'missing'
            reason = 'Lease file is missing.'
        }
    }
    try {
        $document = Get-Content -Raw -LiteralPath $LeasePath | ConvertFrom-Json
    }
    catch {
        return [pscustomobject]@{
            state = 'unknown'
            reason = "Lease file could not be parsed: $($_.Exception.Message)"
        }
    }
    $leaseProperty = $document.PSObject.Properties['lease']
    $lease = if ($null -ne $leaseProperty) {
        $leaseProperty.Value
    }
    else {
        $document
    }
    foreach ($propertyName in @(
            'owner_pid',
            'owner_start_ticks',
            'owner_executable_path',
            'heartbeat_utc')) {
        if ($null -eq $lease.PSObject.Properties[$propertyName]) {
            return [pscustomobject]@{
                state = 'legacy'
                reason = 'Lease does not contain an owner identity.'
            }
        }
    }
    try {
        $heartbeatUtc = [DateTime]::Parse(
            [string]$lease.heartbeat_utc).ToUniversalTime()
    }
    catch {
        return [pscustomobject]@{
            state = 'unknown'
            reason = "Lease heartbeat could not be parsed: $($_.Exception.Message)"
        }
    }
    $ageSeconds = ([DateTime]::UtcNow - $heartbeatUtc).TotalSeconds
    if ([string]$lease.state -eq 'completed') {
        return [pscustomobject]@{
            state = 'completed'
            reason = 'Lease owner marked the run completed.'
            age_seconds = [int]$ageSeconds
        }
    }
    $ownerProcess = Get-Process -Id ([int]$lease.owner_pid) -ErrorAction SilentlyContinue
    $ownerMatches = $false
    if ($null -ne $ownerProcess) {
        try {
            $ownerProcess.Refresh()
            $ownerMatches =
                [string]::Equals(
                    $ownerProcess.Path,
                    [string]$lease.owner_executable_path,
                    [System.StringComparison]::OrdinalIgnoreCase) -and
                $ownerProcess.StartTime.ToUniversalTime().Ticks -eq
                    [int64]$lease.owner_start_ticks
        }
        catch {
            $ownerMatches = $false
        }
        finally {
            $ownerProcess.Dispose()
        }
    }
    $state = if ($ownerMatches) {
        'active'
    }
    elseif ($ageSeconds -lt $script:SampleLeaseStaleAfterSeconds) {
        'grace'
    }
    else {
        'stale'
    }
    return [pscustomobject]@{
        state = $state
        reason = if ($ownerMatches) {
            'Lease owner process is active and its start identity matches.'
        }
        else {
            "Lease owner is inactive or no longer matches; heartbeat age is $([int]$ageSeconds)s."
        }
        age_seconds = [int]$ageSeconds
    }
}

function Initialize-SampleRunLease {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunId,
        [Parameter(Mandatory = $true)]
        [string]$ArtifactLeasePath
    )

    $ownerProcess = Get-Process -Id $PID -ErrorAction Stop
    try {
        $ownerProcess.Refresh()
        $ownerStartUtc = $ownerProcess.StartTime.ToUniversalTime()
        $script:SampleRunLease = [ordered]@{
            schema = 2
            run_id = $RunId
            token = [Guid]::NewGuid().ToString('N')
            owner_pid = [int]$PID
            owner_start_ticks = $ownerStartUtc.Ticks
            owner_start_utc = $ownerStartUtc.ToString('O')
            owner_executable_path = $ownerProcess.Path
            heartbeat_utc = [DateTime]::UtcNow.ToString('O')
            state = 'active'
        }
        $script:SampleArtifactLeasePath = $ArtifactLeasePath
        Write-AtomicUtf8File `
            -Path $ArtifactLeasePath `
            -Contents ($script:SampleRunLease | ConvertTo-Json -Depth 8)
    }
    finally {
        $ownerProcess.Dispose()
    }
}

function Update-SampleRunLease {
    if ($null -eq $script:SampleRunLease) {
        throw 'Sample run lease was not initialized.'
    }
    $script:SampleRunLease.heartbeat_utc = [DateTime]::UtcNow.ToString('O')
    if (-not [string]::IsNullOrWhiteSpace($script:SampleArtifactLeasePath)) {
        Write-AtomicUtf8File `
            -Path $script:SampleArtifactLeasePath `
            -Contents ($script:SampleRunLease | ConvertTo-Json -Depth 8)
    }
}

function Complete-SampleRunLease {
    if ($null -eq $script:SampleRunLease) {
        return
    }
    $script:SampleRunLease.state = 'completed'
    Update-SampleRunLease
}

function Get-FileSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        return [System.BitConverter]::ToString(
            $algorithm.ComputeHash($stream)).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
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

function Stop-OwnedLauncherProcessTree {
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
        throw "Refusing to stop launcher PID $ProcessId without a start identity."
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
            throw "Refusing to stop launcher PID $ProcessId because it is not the owned launcher executable."
        }
        if ($process.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) {
            throw "Refusing to stop launcher PID $ProcessId because its start identity does not match ownership."
        }
        Stop-AutomationProcessHandle `
            -ProcessHandle $process `
            -WaitMilliseconds $script:CleanupWaitMilliseconds
    }
    finally {
        if ($ownsProcessHandle) {
            $process.Dispose()
        }
    }
}

function Assert-OwnedLauncherProcessGone {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedExecutable,
        [Parameter(Mandatory = $true)]
        [int64]$ExpectedStartTicks
    )

    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return
    }
    if ($process.HasExited) {
        return
    }
    try {
        Stop-OwnedLauncherProcessTree `
            -ProcessId $ProcessId `
            -ExpectedExecutable $ExpectedExecutable `
            -ExpectedStartTicks $ExpectedStartTicks `
            -ProcessHandle $process
    }
    finally {
        if (-not $process.HasExited) {
            throw "Owned launcher process $ProcessId remained after bounded cleanup."
        }
        $process.Dispose()
    }
}

function Invoke-StateFixtureHelper {
    param(
        [Parameter(Mandatory = $true)]
        [string]$HelperPath,
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$LogPath,
        [Parameter(Mandatory = $true)]
        [string]$RunRoot,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedHelperPids,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedLauncherPids,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedGuiPids
    )

    $process = $null
    $stdoutTask = $null
    $stderrTask = $null
    $stdout = ''
    $stderr = ''
    $helperPid = 0
    $exitCode = $null
    $failureMessage = $null
    $cleanupFailure = $null
    $helperIdentity = $null
    $helperStartTicks = 0
    $jobHandle = [IntPtr]::Zero
    $launchToken = New-SampleLaunchToken

    try {
        Add-SampleLaunchPending `
            -Role helper `
            -ExpectedExecutable $HelperPath `
            -Token $launchToken
        Write-SampleRunRootOwnershipManifest `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $ExecutablePath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids
        $jobHandle = New-AutomationKillOnCloseJob
        $start = [System.Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $HelperPath
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
        $start.StandardOutputEncoding = $utf8WithoutBom
        $start.StandardErrorEncoding = $utf8WithoutBom
        $start.Arguments = ($Arguments |
                ForEach-Object { Quote-WindowsArgument -Value $_ }) -join ' '

        $process = [System.Diagnostics.Process]::Start($start)
        $helperPid = $process.Id
        $process.Refresh()
        $helperStartTicks = $process.StartTime.ToUniversalTime().Ticks
        Set-SampleLaunchPendingIdentity `
            -Token $launchToken `
            -ProcessId $helperPid `
            -StartTicks $helperStartTicks
        Write-SampleRunRootOwnershipManifest `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $ExecutablePath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids
        if (-not $OwnedHelperPids.Contains($helperPid)) {
            [void]$OwnedHelperPids.Add($helperPid)
        }
        $helperIdentity = Register-OwnedProcessIdentity `
            -Role helper `
            -ProcessId $helperPid `
            -ExpectedExecutable $HelperPath `
            -ProcessHandle $process `
            -LaunchToken $launchToken `
            -KnownStartTicks $helperStartTicks
        Assign-AutomationProcessToJob `
            -JobHandle $jobHandle `
            -ProcessHandle $process
        Set-SampleLaunchPendingIdentity `
            -Token $launchToken `
            -ProcessId $helperPid `
            -StartTicks $helperStartTicks `
            -JobAssigned $true
        Write-SampleRunRootOwnershipManifest `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $ExecutablePath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids
        Commit-SampleLaunchOwnership `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $ExecutablePath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids `
            -Token $launchToken
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if (-not $process.WaitForExit($script:StateFixtureTimeoutMilliseconds)) {
            $failureMessage =
                "State fixture helper did not finish within $($script:StateFixtureTimeoutMilliseconds) ms."
            throw $failureMessage
        }
        if (-not $stdoutTask.Wait(5000) -or -not $stderrTask.Wait(5000)) {
            throw 'State fixture helper output did not complete within the bounded output-drain deadline.'
        }
        $stdout = [string]$stdoutTask.Result
        $stderr = [string]$stderrTask.Result
        $process.Refresh()
        $exitCode = [int]$process.ExitCode
        if ($exitCode -ne 0) {
            throw "State fixture helper exited with code $exitCode."
        }
        return [pscustomobject]@{
            ExitCode = $exitCode
            HelperPid = $helperPid
            Stdout = $stdout
            Stderr = $stderr
        }
    }
    catch {
        $failureMessage = $_.Exception.Message
        throw
    }
    finally {
        if ($null -ne $process) {
            if (-not $process.HasExited) {
                try {
                    Stop-OwnedHelperProcess `
                        -ProcessId $helperPid `
                        -ExpectedExecutable $HelperPath `
                        -ExpectedStartTicks $helperStartTicks `
                        -ProcessHandle $process
                    if (-not $process.WaitForExit(5000)) {
                        $cleanupFailure =
                            "State fixture helper PID $helperPid remained alive after bounded cleanup."
                    }
                }
                catch {
                    $cleanupFailure = $_.Exception.Message
                }
            }
            if ($null -eq $exitCode -and $process.HasExited) {
                $process.Refresh()
                $exitCode = [int]$process.ExitCode
            }
            if ($null -ne $stdoutTask -and $stdoutTask.IsCompleted) {
                try {
                    $stdout = [string]$stdoutTask.Result
                }
                catch {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure =
                            'State fixture helper stdout could not be collected.'
                    }
                }
            }
            if ($null -ne $stderrTask -and $stderrTask.IsCompleted) {
                try {
                    $stderr = [string]$stderrTask.Result
                }
                catch {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure =
                            'State fixture helper stderr could not be collected.'
                    }
                }
            }
            if ($helperPid -gt 0) {
                try {
                    Assert-OwnedProcessIdentityGone `
                        -Role helper `
                        -ProcessId $helperPid `
                        -ProcessHandle $process
                }
                catch {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure = $_.Exception.Message
                    }
                    else {
                        $cleanupFailure =
                            "$cleanupFailure $($_.Exception.Message)"
                    }
                }
            }
            $process.Dispose()
        }
        if ($jobHandle -ne [IntPtr]::Zero) {
            Close-AutomationNativeHandle -Handle $jobHandle
            $jobHandle = [IntPtr]::Zero
        }
        if ($null -ne $cleanupFailure) {
            if ($null -eq $failureMessage) {
                $failureMessage = $cleanupFailure
            }
            else {
                $failureMessage = "$failureMessage $cleanupFailure"
            }
        }
        $log = [ordered]@{
            arguments = $Arguments
            exit_code = $exitCode
            helper_pid = $helperPid
            failure = $failureMessage
            stdout = $stdout
            stderr = $stderr
        }
        Write-Utf8File `
            -Path $LogPath `
            -Contents ($log | ConvertTo-Json -Depth 12)
        if ($null -ne $cleanupFailure) {
            throw $failureMessage
        }
    }
}

function Get-LauncherMessages {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Output
    )

    $messages = @()
    foreach ($line in ($Output -split "`r?`n")) {
        $trimmed = $line.Trim()
        if (-not $trimmed.StartsWith('{')) {
            continue
        }
        try {
            $messages += ($trimmed | ConvertFrom-Json)
        }
        catch {
            throw "Launcher emitted malformed JSON: $trimmed"
        }
    }
    return $messages
}

function Add-LauncherOutputLine {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Line,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Lines,
        [Parameter(Mandatory = $true)]
        [ref]$GuiPid,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedGuiPids
    )

    [void]$Lines.Add($Line)
    $pidMatch = [regex]::Match($Line, '^SpecForge PID: ([0-9]+)\s*$')
    if (-not $pidMatch.Success) {
        return
    }
    $observedPid = [int]$pidMatch.Groups[1].Value
    if ($observedPid -le 0) {
        return
    }
    if ($GuiPid.Value -eq 0) {
        $GuiPid.Value = $observedPid
    }
    if (-not $OwnedGuiPids.Contains($observedPid)) {
        [void]$OwnedGuiPids.Add($observedPid)
    }
}

function Receive-LauncherOutputLine {
    param(
        [Parameter(Mandatory = $true)]
        [ref]$ReadTask,
        [Parameter(Mandatory = $true)]
        [object]$Reader,
        [Parameter(Mandatory = $true)]
        [ref]$Completed,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Lines,
        [Parameter(Mandatory = $true)]
        [ref]$GuiPid,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedGuiPids
    )

    if ($Completed.Value -or $null -eq $ReadTask.Value) {
        return
    }
    if (-not $ReadTask.Value.IsCompleted) {
        return
    }
    try {
        $line = $ReadTask.Value.GetAwaiter().GetResult()
    }
    catch {
        # A forced launcher-tree termination can fault a pending read. Treat
        # that stream as drained so the bounded cleanup path can still log
        # the lines already observed and verify the owned GUI PID.
        $Completed.Value = $true
        return
    }
    if ($null -eq $line) {
        $Completed.Value = $true
        return
    }
    Add-LauncherOutputLine `
        -Line ([string]$line) `
        -Lines $Lines `
        -GuiPid $GuiPid `
        -OwnedGuiPids $OwnedGuiPids
    $ReadTask.Value = $Reader.ReadLineAsync()
}

function Invoke-LauncherSequence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$StateRoot,
        [Parameter(Mandatory = $true)]
        [string]$RunRoot,
        [Parameter(Mandatory = $true)]
        [string]$HelperPath,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedHelperPids,
        [string]$SeedPath,
        [Parameter(Mandatory = $true)]
        [string[]]$Commands,
        [Parameter(Mandatory = $true)]
        [string]$LogPath,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedGuiPids,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedLauncherPids
    )

    $process = $null
    $stdoutTask = $null
    $stderrTask = $null
    $stdoutReader = $null
    $stderrReader = $null
    $stdoutComplete = $false
    $stderrComplete = $false
    $stdoutLines = [System.Collections.Generic.List[string]]::new()
    $stderrLines = [System.Collections.Generic.List[string]]::new()
    $stdout = ''
    $stderr = ''
    $guiPid = 0
    $launcherPid = 0
    $manifestGuiPidCount = $OwnedGuiPids.Count
    $failureMessage = $null
    $exitCode = $null
    $cleanupFailure = $null
    $launcherIdentity = $null
    $launcherStartTicks = 0
    $jobHandle = [IntPtr]::Zero
    $launcherLaunchToken = New-SampleLaunchToken
    $guiLaunchToken = New-SampleLaunchToken

    try {
        Add-SampleLaunchPending `
            -Role launcher `
            -ExpectedExecutable $LauncherPath `
            -Token $launcherLaunchToken
        Add-SampleLaunchPending `
            -Role gui `
            -ExpectedExecutable $AppPath `
            -Token $guiLaunchToken
        Write-SampleRunRootOwnershipManifest `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $AppPath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids
        $jobHandle = New-AutomationKillOnCloseJob
        $start = [System.Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $LauncherPath
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardInput = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
        $start.StandardOutputEncoding = $utf8WithoutBom
        $start.StandardErrorEncoding = $utf8WithoutBom
        $arguments = @(
            '--app', $AppPath,
            '--state-root', $StateRoot
        )
        if (-not [string]::IsNullOrWhiteSpace($SeedPath)) {
            $arguments += @('--labeling-state-seed', $SeedPath)
        }
        $start.Arguments = ($arguments |
                ForEach-Object { Quote-WindowsArgument -Value $_ }) -join ' '

        $process = [System.Diagnostics.Process]::Start($start)
        $launcherPid = $process.Id
        $process.Refresh()
        $launcherStartTicks = $process.StartTime.ToUniversalTime().Ticks
        Set-SampleLaunchPendingIdentity `
            -Token $launcherLaunchToken `
            -ProcessId $launcherPid `
            -StartTicks $launcherStartTicks
        Write-SampleRunRootOwnershipManifest `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $AppPath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids
        # Attach output readers before identity registration so an immediate
        # launcher failure is preserved in the run evidence instead of being
        # mistaken for a silent ownership mismatch.
        $stdoutReader = $process.StandardOutput
        $stderrReader = $process.StandardError
        $stdoutTask = $stdoutReader.ReadLineAsync()
        $stderrTask = $stderrReader.ReadLineAsync()
        if (-not $OwnedLauncherPids.Contains($launcherPid)) {
            [void]$OwnedLauncherPids.Add($launcherPid)
        }
        $launcherIdentity = Register-OwnedProcessIdentity `
            -Role launcher `
            -ProcessId $launcherPid `
            -ExpectedExecutable $LauncherPath `
            -ProcessHandle $process `
            -LaunchToken $launcherLaunchToken `
            -KnownStartTicks $launcherStartTicks
        Assign-AutomationProcessToJob `
            -JobHandle $jobHandle `
            -ProcessHandle $process
        Set-SampleLaunchPendingIdentity `
            -Token $launcherLaunchToken `
            -ProcessId $launcherPid `
            -StartTicks $launcherStartTicks `
            -JobAssigned $true
        Write-SampleRunRootOwnershipManifest `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $AppPath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids
        Commit-SampleLaunchOwnership `
            -RunRoot $RunRoot `
            -HelperPath $HelperPath `
            -LauncherPath $LauncherPath `
            -ExecutablePath $AppPath `
            -OwnedHelperPids $OwnedHelperPids `
            -OwnedLauncherPids $OwnedLauncherPids `
            -OwnedGuiPids $OwnedGuiPids `
            -Token $launcherLaunchToken
        $inputBytes = $utf8WithoutBom.GetBytes(
            (($Commands -join "`n") + "`n"))
        $process.StandardInput.BaseStream.Write(
            $inputBytes,
            0,
            $inputBytes.Length)
        $process.StandardInput.Close()

        $deadline = [DateTime]::UtcNow.AddMilliseconds(
            $script:SequenceTimeoutMilliseconds)
        while (-not $process.HasExited) {
            Receive-LauncherOutputLine `
                -ReadTask ([ref]$stdoutTask) `
                -Reader $stdoutReader `
                -Completed ([ref]$stdoutComplete) `
                -Lines $stdoutLines `
                -GuiPid ([ref]$guiPid) `
                -OwnedGuiPids $OwnedGuiPids
            Receive-LauncherOutputLine `
                -ReadTask ([ref]$stderrTask) `
                -Reader $stderrReader `
                -Completed ([ref]$stderrComplete) `
                -Lines $stderrLines `
                -GuiPid ([ref]$guiPid) `
                -OwnedGuiPids $OwnedGuiPids
            if ($OwnedGuiPids.Count -gt $manifestGuiPidCount) {
                foreach ($observedGuiPid in @(
                        $OwnedGuiPids |
                    Select-Object -Skip $manifestGuiPidCount)) {
                    $guiProcess = Get-Process -Id ([int]$observedGuiPid) -ErrorAction Stop
                    try {
                        $guiProcess.Refresh()
                        $guiStartTicks = $guiProcess.StartTime.ToUniversalTime().Ticks
                        Set-SampleLaunchPendingIdentity `
                            -Token $guiLaunchToken `
                            -ProcessId ([int]$observedGuiPid) `
                            -StartTicks $guiStartTicks `
                            -JobAssigned $true
                        Write-SampleRunRootOwnershipManifest `
                            -RunRoot $RunRoot `
                            -HelperPath $HelperPath `
                            -LauncherPath $LauncherPath `
                            -ExecutablePath $AppPath `
                            -OwnedHelperPids $OwnedHelperPids `
                            -OwnedLauncherPids $OwnedLauncherPids `
                            -OwnedGuiPids $OwnedGuiPids
                        $existingGuiIdentity = @(
                            $script:OwnedProcessIdentities |
                                Where-Object {
                                    [string]$_.role -eq 'gui' -and
                                    [int]$_.pid -eq [int]$observedGuiPid
                                })
                        if ($existingGuiIdentity.Count -eq 0) {
                            [void](Register-OwnedProcessIdentity `
                                -Role gui `
                                -ProcessId ([int]$observedGuiPid) `
                                -ExpectedExecutable $AppPath `
                                -ProcessHandle $guiProcess `
                                -LaunchToken $guiLaunchToken `
                                -KnownStartTicks $guiStartTicks)
                        }
                        if (-not [string]::IsNullOrWhiteSpace($guiLaunchToken)) {
                            Commit-SampleLaunchOwnership `
                                -RunRoot $RunRoot `
                                -HelperPath $HelperPath `
                                -LauncherPath $LauncherPath `
                                -ExecutablePath $AppPath `
                                -OwnedHelperPids $OwnedHelperPids `
                                -OwnedLauncherPids $OwnedLauncherPids `
                                -OwnedGuiPids $OwnedGuiPids `
                                -Token $guiLaunchToken
                            $guiLaunchToken = $null
                        }
                    }
                    finally {
                        $guiProcess.Dispose()
                    }
                }
                $manifestGuiPidCount = $OwnedGuiPids.Count
            }
            if ([DateTime]::UtcNow -ge $deadline) {
                $failureMessage =
                    "Launcher did not finish within $($script:SequenceTimeoutMilliseconds) ms."
                Stop-OwnedLauncherProcessTree `
                    -ProcessId $launcherPid `
                    -ExpectedExecutable $LauncherPath `
                    -ExpectedStartTicks $launcherStartTicks `
                    -ProcessHandle $process
                throw $failureMessage
            }
            Start-Sleep -Milliseconds 10
        }

        $drainDeadline = [DateTime]::UtcNow.AddMilliseconds(5000)
        while (-not ($stdoutComplete -and $stderrComplete)) {
            Receive-LauncherOutputLine `
                -ReadTask ([ref]$stdoutTask) `
                -Reader $stdoutReader `
                -Completed ([ref]$stdoutComplete) `
                -Lines $stdoutLines `
                -GuiPid ([ref]$guiPid) `
                -OwnedGuiPids $OwnedGuiPids
            Receive-LauncherOutputLine `
                -ReadTask ([ref]$stderrTask) `
                -Reader $stderrReader `
                -Completed ([ref]$stderrComplete) `
                -Lines $stderrLines `
                -GuiPid ([ref]$guiPid) `
                -OwnedGuiPids $OwnedGuiPids
            if ([DateTime]::UtcNow -ge $drainDeadline) {
                throw 'Launcher output did not complete within the bounded output-drain deadline.'
            }
            Start-Sleep -Milliseconds 10
        }
        Write-SampleRunRootOwnershipManifest -RunRoot $RunRoot -HelperPath $HelperPath -LauncherPath $LauncherPath -ExecutablePath $AppPath -OwnedHelperPids $OwnedHelperPids -OwnedLauncherPids $OwnedLauncherPids -OwnedGuiPids $OwnedGuiPids
        $process.Refresh()
        $exitCode = [int]$process.ExitCode
        $stdout = $stdoutLines -join "`r`n"
        $stderr = $stderrLines -join "`r`n"
        Assert-True `
            -Condition ($guiPid -gt 0) `
            -Message 'Each automation sample sequence must report its owned GUI process ID.'
        $messages = @(Get-LauncherMessages -Output $stdout)
        Assert-True `
            -Condition (
                $messages.Count -gt 0 -and
                [string]$messages[0].type -eq 'hello' -and
                [int]$messages[0].protocol_version -eq 1) `
            -Message 'Each automation sample sequence must complete the protocol hello handshake.'
        return [pscustomobject]@{
            ExitCode = $exitCode
            GuiPid = $guiPid
            Messages = $messages
            Stdout = $stdout
            Stderr = $stderr
        }
    }
    catch {
        $failureMessage = $_.Exception.Message
        throw
    }
    finally {
        if ($null -ne $process) {
            if (-not $process.HasExited) {
                try {
                    Stop-OwnedLauncherProcessTree `
                        -ProcessId $launcherPid `
                        -ExpectedExecutable $LauncherPath `
                        -ExpectedStartTicks $launcherStartTicks `
                        -ProcessHandle $process
                }
                catch {
                    $cleanupFailure = $_.Exception.Message
                }
            }
            $drainDeadline = [DateTime]::UtcNow.AddMilliseconds(5000)
            while (-not ($stdoutComplete -and $stderrComplete) -and
                [DateTime]::UtcNow -lt $drainDeadline) {
                if ($null -ne $stdoutReader) {
                    Receive-LauncherOutputLine `
                        -ReadTask ([ref]$stdoutTask) `
                        -Reader $stdoutReader `
                        -Completed ([ref]$stdoutComplete) `
                        -Lines $stdoutLines `
                        -GuiPid ([ref]$guiPid) `
                        -OwnedGuiPids $OwnedGuiPids
                }
                if ($null -ne $stderrReader) {
                    Receive-LauncherOutputLine `
                        -ReadTask ([ref]$stderrTask) `
                        -Reader $stderrReader `
                        -Completed ([ref]$stderrComplete) `
                        -Lines $stderrLines `
                        -GuiPid ([ref]$guiPid) `
                        -OwnedGuiPids $OwnedGuiPids
                }
                if (-not ($stdoutComplete -and $stderrComplete)) {
                    Start-Sleep -Milliseconds 10
                }
            }
            if (-not ($stdoutComplete -and $stderrComplete) -and
                $null -eq $cleanupFailure) {
                $cleanupFailure =
                    'Launcher output remained incomplete after bounded cleanup.'
            }
            try {
                Write-SampleRunRootOwnershipManifest -RunRoot $RunRoot -HelperPath $HelperPath -LauncherPath $LauncherPath -ExecutablePath $AppPath -OwnedHelperPids $OwnedHelperPids -OwnedLauncherPids $OwnedLauncherPids -OwnedGuiPids $OwnedGuiPids
            }
            catch {
                if ($null -eq $cleanupFailure) {
                    $cleanupFailure = "Could not update launcher ownership manifest: $($_.Exception.Message)"
                }
                else {
                    $cleanupFailure =
                        "$cleanupFailure Could not update launcher ownership manifest: $($_.Exception.Message)"
                }
            }
            if ($null -eq $exitCode -and $process.HasExited) {
                $process.Refresh()
                $exitCode = [int]$process.ExitCode
            }
            $stdout = $stdoutLines -join "`r`n"
            $stderr = $stderrLines -join "`r`n"
            if ($guiPid -gt 0) {
                try {
                    Assert-OwnedProcessIdentityGone -Role gui -ProcessId $guiPid
                }
                catch {
                    $guiCleanupFailure = $_.Exception.Message
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure = $guiCleanupFailure
                    }
                    else {
                        $cleanupFailure =
                            "$cleanupFailure $guiCleanupFailure"
                    }
                }
            }
            if ($launcherPid -gt 0) {
                try {
                    Assert-OwnedProcessIdentityGone `
                        -Role launcher `
                        -ProcessId $launcherPid `
                        -ProcessHandle $process
                }
                catch {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure = $_.Exception.Message
                    }
                    else {
                        $cleanupFailure =
                            "$cleanupFailure $($_.Exception.Message)"
                    }
                }
            }
            $process.Dispose()
        }
        if ($jobHandle -ne [IntPtr]::Zero) {
            Close-AutomationNativeHandle -Handle $jobHandle
            $jobHandle = [IntPtr]::Zero
        }
        if ($null -ne $cleanupFailure) {
            if ($null -eq $failureMessage) {
                $failureMessage = $cleanupFailure
            }
            else {
                $failureMessage = "$failureMessage $cleanupFailure"
            }
        }
        $log = [ordered]@{
            commands = $Commands
            exit_code = $exitCode
            launcher_pid = $launcherPid
            gui_pid = $guiPid
            failure = $failureMessage
            stdout = $stdout
            stderr = $stderr
        }
        Write-Utf8File `
            -Path $LogPath `
            -Contents ($log | ConvertTo-Json -Depth 12)
        if ($null -ne $cleanupFailure) {
            throw $failureMessage
        }
    }
}

function Get-TerminalMessages {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Messages,
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $terminals = @(
        $Messages |
            Where-Object {
                $commandProperty =
                    $_.PSObject.Properties['command']
                $statusProperty =
                    $_.PSObject.Properties['status']
                $null -ne $commandProperty -and
                    $null -ne $statusProperty -and
                    [string]$commandProperty.Value -eq $Command -and
                    [string]$statusProperty.Value -in @(
                        'completed',
                        'failed',
                        'canceled')
            }
    )
    return $terminals
}

function Get-TerminalMessage {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Messages,
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $terminals = @(Get-TerminalMessages -Messages $Messages -Command $Command)
    Assert-True `
        -Condition ($terminals.Count -eq 1) `
        -Message "Expected exactly one terminal '$Command' response."
    return $terminals[0]
}

function Assert-PathEqual {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Actual,
        [Parameter(Mandatory = $true)]
        [string]$Expected,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    Assert-True `
        -Condition (
            [string]::Equals(
                $Actual,
                $Expected,
                [System.StringComparison]::OrdinalIgnoreCase)) `
        -Message $Message
}

function Get-PngIhdr {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $signature = [byte[]]@(
        0x89, 0x50, 0x4E, 0x47,
        0x0D, 0x0A, 0x1A, 0x0A)
    Assert-True `
        -Condition (
            $bytes.Length -ge 24 -and
            -not (Compare-Object -ReferenceObject $signature -DifferenceObject $bytes[0..7])) `
        -Message 'Captured sample output must have a PNG signature.'
    Assert-True `
        -Condition (
            [char]$bytes[12] -eq 'I' -and
            [char]$bytes[13] -eq 'H' -and
            [char]$bytes[14] -eq 'D' -and
            [char]$bytes[15] -eq 'R') `
        -Message 'Captured sample output must contain an IHDR chunk.'
    return [pscustomobject]@{
        Width =
            ([uint32]$bytes[16] -shl 24) -bor
            ([uint32]$bytes[17] -shl 16) -bor
            ([uint32]$bytes[18] -shl 8) -bor
            [uint32]$bytes[19]
        Height =
            ([uint32]$bytes[20] -shl 24) -bor
            ([uint32]$bytes[21] -shl 16) -bor
            ([uint32]$bytes[22] -shl 8) -bor
            [uint32]$bytes[23]
    }
}

function Remove-OwnedDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $tempRoot = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath())
    Assert-True `
        -Condition (
            $resolved.StartsWith(
                $tempRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) `
        -Message "Refusing to remove a sample fixture outside the temp root: $resolved"
    for ($attempt = 0;
         $attempt -lt $script:CleanupRetryCount;
         ++$attempt) {
        try {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop
            return
        }
        catch {
            if ($attempt -eq $script:CleanupRetryCount - 1) {
                throw
            }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Remove-OwnedArtifactDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $resolvedRoot = (Resolve-Path -LiteralPath $ArtifactRoot).Path
    $parent = (Get-Item -LiteralPath $resolved).Parent.FullName
    Assert-True `
        -Condition (
            [string]::Equals(
                $parent,
                $resolvedRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) `
        -Message "Refusing to remove an artifact directory outside the owned root: $resolved"
    for ($attempt = 0;
         $attempt -lt $script:CleanupRetryCount;
         ++$attempt) {
        try {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop
            return
        }
        catch {
            if ($attempt -eq $script:CleanupRetryCount - 1) {
                throw
            }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Get-SampleArtifactParentLeaseStatus {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot,
        [Parameter(Mandatory = $true)]
        [string]$EvidencePath
    )

    try {
        $resolvedRoot = (Resolve-Path -LiteralPath $ArtifactRoot).Path.TrimEnd('\')
        $resolvedEvidence = (Resolve-Path -LiteralPath $EvidencePath).Path
        $evidenceItem = Get-Item -LiteralPath $resolvedEvidence -ErrorAction Stop
        $parentItem = $evidenceItem.Parent
        if ($null -eq $parentItem -or
            $parentItem.Name -notmatch '^sample-run-[0-9a-f]{32}$' -or
            $null -eq $parentItem.Parent -or
            -not [string]::Equals(
                $parentItem.Parent.FullName.TrimEnd('\'),
                $resolvedRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            return [pscustomobject]@{
                state = 'unknown'
                reason = "Evidence directory $resolvedEvidence is not directly owned by a sample-run artifact child below $resolvedRoot."
                lease_path = $null
                parent_path = if ($null -ne $parentItem) { $parentItem.FullName } else { $null }
            }
        }
        $leasePath = Join-Path `
            $parentItem.FullName `
            '.specforge-automation-samples-artifact-lease.json'
        $leaseStatus = Get-SampleLeaseStatus -LeasePath $leasePath
        return [pscustomobject]@{
            state = [string]$leaseStatus.state
            reason = "Parent artifact lease ${leasePath}: $($leaseStatus.reason)"
            lease_path = $leasePath
            parent_path = $parentItem.FullName
            age_seconds = if ($null -ne $leaseStatus.PSObject.Properties['age_seconds']) {
                $leaseStatus.age_seconds
            }
            else {
                $null
            }
        }
    }
    catch {
        return [pscustomobject]@{
            state = 'unknown'
            reason = "Parent artifact lease could not be validated for ${EvidencePath}: $($_.Exception.Message)"
            lease_path = $null
            parent_path = $null
        }
    }
}

function Prune-FailureEvidenceDirectories {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot
    )

    $resolvedArtifactRoot = (Resolve-Path -LiteralPath $ArtifactRoot).Path
    $candidates = [System.Collections.Generic.List[object]]::new()
    $directCandidates = @(
        Get-ChildItem -LiteralPath $resolvedArtifactRoot `
            -Directory -Filter 'sample-run-root-*' -ErrorAction Stop)
    foreach ($candidate in $directCandidates) {
        [void]$candidates.Add($candidate)
    }
    $artifactRunDirectories = @(
        Get-ChildItem -LiteralPath $resolvedArtifactRoot `
            -Directory -Filter 'sample-run-*' -ErrorAction Stop |
            Where-Object { $_.Name -match '^sample-run-[0-9a-f]{32}$' })
    foreach ($artifactRun in $artifactRunDirectories) {
        foreach ($candidate in @(
                Get-ChildItem -LiteralPath $artifactRun.FullName `
                    -Directory -Filter 'sample-run-root-*' -ErrorAction Stop)) {
            [void]$candidates.Add($candidate)
        }
    }
    foreach ($candidate in $candidates) {
        Assert-True `
            -Condition ($candidate.Name -match '^sample-run-root-[0-9a-f]{32}$') `
            -Message "Refusing to prune an unrecognized sample evidence directory: $($candidate.Name)"
    }
    $diagnostics = [System.Collections.Generic.List[string]]::new()
    $stale = @(
        $candidates |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -Skip $script:FailureEvidenceRetentionCount)
    foreach ($candidate in $stale) {
        $parentLeaseStatus = Get-SampleArtifactParentLeaseStatus `
            -ArtifactRoot $resolvedArtifactRoot `
            -EvidencePath $candidate.FullName
        if ([string]$parentLeaseStatus.state -notin @('completed', 'stale')) {
            [void]$diagnostics.Add(
                "Sample failure evidence retention deferred for $($candidate.FullName): $($parentLeaseStatus.reason)")
            continue
        }
        try {
            Remove-OwnedArtifactDirectory `
                -Path $candidate.FullName `
                -ArtifactRoot $parentLeaseStatus.parent_path
        }
        catch {
            [void]$diagnostics.Add(
                "Sample failure evidence cleanup failed for $($candidate.FullName): $($_.Exception.Message)")
        }
    }
    return @($diagnostics)
}

function Get-SampleArtifactRunDirectories {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot
    )

    $allCandidates = @(
        Get-ChildItem -LiteralPath $ArtifactRoot `
            -Directory -Filter 'sample-run-*' -ErrorAction Stop)
    foreach ($candidate in $allCandidates) {
        Assert-True `
            -Condition ($candidate.Name -match '^sample-run-[0-9a-f]{32}$') `
            -Message "Refusing to manage an unrecognized sample artifact directory: $($candidate.Name)"
    }
    return @($allCandidates)
}

function Enter-SampleArtifactCapacityLock {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot,
        [int]$TimeoutMilliseconds = 5000
    )

    $lockPath = Join-Path `
        $ArtifactRoot `
        '.specforge-automation-samples-capacity.lock'
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            # An OS-owned file handle makes the reservation atomic and is
            # released automatically if CTest or PowerShell is terminated.
            return [System.IO.File]::Open(
                $lockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
        }
        catch [System.IO.IOException] {
            Start-Sleep -Milliseconds 100
        }
    }
    throw (
        'Could not acquire the bounded sample artifact capacity lock within ' +
        "$TimeoutMilliseconds ms; no new run artifact directory was created.")
}

function Write-SampleArtifactCapacityDiagnostic {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot,
        [Parameter(Mandatory = $true)]
        [int]$Capacity,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$ExistingDirectories,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$PruneDiagnostics,
        [string]$Status = 'artifact_capacity_exhausted',
        [string]$Message = 'No new run artifact directory was created because the bounded artifact capacity is exhausted.'
    )

    $diagnosticPath = Join-Path `
        $ArtifactRoot `
        'sample-artifact-capacity-diagnostic.json'
    $record = [ordered]@{
        schema = 1
        status = $Status
        message = $Message
        capacity = $Capacity
        existing_run_directories = @($ExistingDirectories)
        prune_diagnostics = @($PruneDiagnostics)
        checked_utc = [DateTime]::UtcNow.ToString('O')
    }
    Write-AtomicUtf8File `
        -Path $diagnosticPath `
        -Contents ($record | ConvertTo-Json -Depth 12)
    return $diagnosticPath
}

function Assert-SampleArtifactCapacity {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot,
        [Parameter(Mandatory = $true)]
        [int]$Capacity,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$PruneDiagnostics
    )

    $runDirectories = @()
    try {
        $runDirectories = @(Get-SampleArtifactRunDirectories -ArtifactRoot $ArtifactRoot)
    }
    catch {
        $existingDirectories = @(
            Get-ChildItem -LiteralPath $ArtifactRoot `
                -Directory -Filter 'sample-run-*' -ErrorAction SilentlyContinue |
                ForEach-Object FullName)
        $diagnosticPath = Write-SampleArtifactCapacityDiagnostic `
            -ArtifactRoot $ArtifactRoot `
            -Capacity $Capacity `
            -ExistingDirectories $existingDirectories `
            -PruneDiagnostics (@($PruneDiagnostics) + @($_.Exception.Message)) `
            -Status 'artifact_capacity_check_failed' `
            -Message 'No new run artifact directory was created because the bounded artifact capacity check failed.'
        throw (
            'Sample artifact capacity check failed; no new run artifact directory was created. ' +
            "Inspect the bounded diagnostic $diagnosticPath. $($_.Exception.Message)")
    }
    if ($runDirectories.Count -ge $Capacity) {
        $diagnosticPath = Write-SampleArtifactCapacityDiagnostic `
            -ArtifactRoot $ArtifactRoot `
            -Capacity $Capacity `
            -ExistingDirectories @($runDirectories | ForEach-Object FullName) `
            -PruneDiagnostics $PruneDiagnostics
        throw (
            'Sample artifact capacity is exhausted; no new run artifact directory was created. ' +
            "retained=$($runDirectories.Count), capacity=$Capacity. Inspect the bounded diagnostic $diagnosticPath.")
    }
    return @($runDirectories)
}

function Prune-SampleArtifactRunDirectories {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot,
        [int]$KeepCount = $script:FailureEvidenceRetentionCount,
        [string]$ExcludePath = ''
    )

    $allCandidates = @(Get-SampleArtifactRunDirectories -ArtifactRoot $ArtifactRoot)
    $candidates = @(
        $allCandidates |
            Where-Object {
                [string]::IsNullOrWhiteSpace($ExcludePath) -or
                -not [string]::Equals(
                    $_.FullName,
                    $ExcludePath,
                    [System.StringComparison]::OrdinalIgnoreCase)
            })
    $diagnostics = [System.Collections.Generic.List[string]]::new()
    $stale = @(
            $candidates |
                Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -Skip $KeepCount)
    foreach ($candidate in $stale) {
        $leasePath = Join-Path `
            $candidate.FullName `
            '.specforge-automation-samples-artifact-lease.json'
        $leaseStatus = Get-SampleLeaseStatus -LeasePath $leasePath
        if ([string]$leaseStatus.state -notin @('stale', 'completed')) {
            [void]$diagnostics.Add(
                "Sample artifact retention deferred for $($candidate.FullName): $($leaseStatus.reason)")
            continue
        }
        try {
            Remove-OwnedArtifactDirectory `
                -Path $candidate.FullName `
                -ArtifactRoot $ArtifactRoot
        }
        catch {
            [void]$diagnostics.Add(
                "Sample artifact cleanup failed for $($candidate.FullName): $($_.Exception.Message)")
        }
    }
    return @($diagnostics)
}

function Remove-OwnedSampleTempDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $tempRoot = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath()).TrimEnd('\')
    $item = Get-Item -LiteralPath $resolved
    Assert-True -Condition ([string]::Equals($item.Parent.FullName.TrimEnd('\'), $tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) -Message "Refusing to remove a sample run root outside the system temp root: $resolved"
    Assert-True -Condition ($item.Name -match '^specforge-automation-samples-[0-9a-f]{32}$') -Message "Refusing to remove an unrecognized sample run root: $resolved"
    for ($attempt = 0; $attempt -lt $script:CleanupRetryCount; ++$attempt) {
        try {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop
            return
        }
        catch {
            if ($attempt -eq $script:CleanupRetryCount - 1) {
                throw
            }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Write-SampleRunRootOwnershipManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunRoot,
        [Parameter(Mandatory = $true)]
        [string]$HelperPath,
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedHelperPids,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedLauncherPids,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedGuiPids
    )

    if (-not (Test-Path -LiteralPath $RunRoot -PathType Container)) {
        return
    }
    # PID lists are display/diagnostic fields only. Registration happens at
    # launch with a held Process handle; re-looking up a PID here could bind a
    # reused PID to an unrelated same-path process.
    if ($null -ne $script:SampleRunLease) {
        Update-SampleRunLease
    }
    $manifest = [ordered]@{
        schema = 2
        generated_utc = [DateTime]::UtcNow.ToString('O')
        helper_path = $HelperPath
        launcher_path = $LauncherPath
        executable_path = $ExecutablePath
        helper_pids = @($OwnedHelperPids)
        launcher_pids = @($OwnedLauncherPids)
        gui_pids = @($OwnedGuiPids)
        lease = $script:SampleRunLease
        launch_pending = @($script:PendingLaunches)
        processes = @($script:OwnedProcessIdentities)
    }
    Write-AtomicUtf8File `
        -Path (Join-Path $RunRoot '.specforge-automation-samples-ownership.json') `
        -Contents ($manifest | ConvertTo-Json -Depth 12)
}

function Commit-SampleLaunchOwnership {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunRoot,
        [Parameter(Mandatory = $true)]
        [string]$HelperPath,
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedHelperPids,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedLauncherPids,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[int]]$OwnedGuiPids,
        [Parameter(Mandatory = $true)]
        [string]$Token
    )

    # First persist the complete identity while launch_pending is still set.
    # If the second write is interrupted, stale-root cleanup remains
    # conservative because the on-disk pending token is still present.
    Write-SampleRunRootOwnershipManifest `
        -RunRoot $RunRoot `
        -HelperPath $HelperPath `
        -LauncherPath $LauncherPath `
        -ExecutablePath $ExecutablePath `
        -OwnedHelperPids $OwnedHelperPids `
        -OwnedLauncherPids $OwnedLauncherPids `
        -OwnedGuiPids $OwnedGuiPids
    Complete-SampleLaunchPending -Token $Token
    Write-SampleRunRootOwnershipManifest `
        -RunRoot $RunRoot `
        -HelperPath $HelperPath `
        -LauncherPath $LauncherPath `
        -ExecutablePath $ExecutablePath `
        -OwnedHelperPids $OwnedHelperPids `
        -OwnedLauncherPids $OwnedLauncherPids `
        -OwnedGuiPids $OwnedGuiPids
}

function Test-SampleRunRootHasLiveOwnedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunRoot,
        [switch]$CurrentRun
    )

    $manifestPath = Join-Path $RunRoot '.specforge-automation-samples-ownership.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        # A missing manifest is ambiguous after a crash or forced CTest
        # termination. Never infer that an empty process list means safe
        # deletion; retain the root and emit a cleanup diagnostic instead.
        return $true
    }
    try {
        $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    }
    catch {
        return $true
    }
    $leaseStatus = Get-SampleLeaseStatus -LeasePath $manifestPath
    if (-not $CurrentRun -and
        [string]$leaseStatus.state -in @('active', 'grace', 'unknown')) {
        return $true
    }
    $pendingProperty = $manifest.PSObject.Properties['launch_pending']
    if ($null -ne $pendingProperty -and @($pendingProperty.Value).Count -gt 0) {
        return $true
    }
    $processesProperty = $manifest.PSObject.Properties['processes']
    if ($null -eq $processesProperty) {
        foreach ($legacyPropertyName in @('helper_pids', 'launcher_pids', 'gui_pids')) {
            $legacyProperty = $manifest.PSObject.Properties[$legacyPropertyName]
            if ($null -ne $legacyProperty -and
                @($legacyProperty.Value | Where-Object { [int]$_ -gt 0 }).Count -gt 0) {
                # Legacy manifests lack start identity. Do not infer ownership
                # from PID and executable path alone.
                return $true
            }
        }
        return $false
    }
    foreach ($record in @($processesProperty.Value)) {
        if ($null -eq $record) {
            return $true
        }
        foreach ($propertyName in @('role', 'pid', 'executable', 'start_ticks')) {
            if ($null -eq $record.PSObject.Properties[$propertyName]) {
                return $true
            }
        }
        if (Test-OwnedProcessIdentity -Record $record) {
            return $true
        }
    }
    return $false
}

function Stop-SampleRunRootOwnedProcesses {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunRoot
    )

    $manifestPath = Join-Path $RunRoot '.specforge-automation-samples-ownership.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        return @(
            "Ownership manifest is missing for $RunRoot; launch state is ambiguous and the root was retained.")
    }
    try {
        $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    }
    catch {
        return @("Ownership manifest could not be read: $($_.Exception.Message)")
    }

    $leaseStatus = Get-SampleLeaseStatus -LeasePath $manifestPath
    if ([string]$leaseStatus.state -in @('active', 'grace', 'unknown')) {
        return @(
            "Stale sample run-root cleanup deferred for ${RunRoot}: $($leaseStatus.reason)")
    }

    $diagnostics = [System.Collections.Generic.List[string]]::new()
    $pendingProperty = $manifest.PSObject.Properties['launch_pending']
    $pendingRecords = if ($null -ne $pendingProperty) {
        @($pendingProperty.Value)
    }
    else {
        @()
    }
    $pendingCanBeCleared = $true
    foreach ($pending in $pendingRecords) {
        $pendingComplete = $null -ne $pending -and
            $null -ne $pending.PSObject.Properties['role'] -and
            $null -ne $pending.PSObject.Properties['token'] -and
            $null -ne $pending.PSObject.Properties['executable'] -and
            $null -ne $pending.PSObject.Properties['pid'] -and
            $null -ne $pending.PSObject.Properties['start_ticks'] -and
            [int]$pending.pid -gt 0 -and
            [int64]$pending.start_ticks -gt 0
        if (-not $pendingComplete) {
            $pendingCanBeCleared = $false
            [void]$diagnostics.Add(
                "Ownership manifest contains incomplete launch_pending identity for token $([string]$pending.token).")
            continue
        }
        $pendingIdentity = [pscustomobject]@{
            role = [string]$pending.role
            pid = [int]$pending.pid
            executable = [string]$pending.executable
            start_ticks = [int64]$pending.start_ticks
        }
        $pendingHandle = Get-OwnedProcessHandleForIdentity -Record $pendingIdentity
        if ($null -eq $pendingHandle) {
            $currentPendingProcess = Get-Process -Id ([int]$pending.pid) -ErrorAction SilentlyContinue
            if ($null -ne $currentPendingProcess) {
                $currentPendingProcess.Dispose()
                $pendingCanBeCleared = $false
                [void]$diagnostics.Add(
                    "Refusing to clean launch_pending $([string]$pending.role) PID $([int]$pending.pid) because its start identity no longer matches.")
            }
            continue
        }
        try {
            if ([string]$pending.role -eq 'helper') {
                Stop-OwnedHelperProcess `
                    -ProcessId ([int]$pending.pid) `
                    -ExpectedExecutable ([string]$pending.executable) `
                    -ExpectedStartTicks ([int64]$pending.start_ticks) `
                    -ProcessHandle $pendingHandle
            }
            elseif ([string]$pending.role -eq 'launcher') {
                Stop-OwnedLauncherProcessTree `
                    -ProcessId ([int]$pending.pid) `
                    -ExpectedExecutable ([string]$pending.executable) `
                    -ExpectedStartTicks ([int64]$pending.start_ticks) `
                    -ProcessHandle $pendingHandle
            }
            elseif ([string]$pending.role -eq 'gui') {
                Stop-OwnedGuiProcess `
                    -ProcessId ([int]$pending.pid) `
                    -ExpectedExecutable ([string]$pending.executable) `
                    -ExpectedStartTicks ([int64]$pending.start_ticks) `
                    -ProcessHandle $pendingHandle
            }
            else {
                $pendingCanBeCleared = $false
                [void]$diagnostics.Add(
                    "Ownership manifest contains an unknown launch_pending role '$([string]$pending.role)'.")
            }
            if (-not $pendingHandle.HasExited) {
                $pendingCanBeCleared = $false
                [void]$diagnostics.Add(
                    "Owned launch_pending $([string]$pending.role) PID $([int]$pending.pid) remained after bounded cleanup.")
            }
        }
        catch {
            $pendingCanBeCleared = $false
            [void]$diagnostics.Add(
                "Owned launch_pending $([string]$pending.role) PID $([int]$pending.pid) cleanup failed: $($_.Exception.Message)")
        }
        finally {
            $pendingHandle.Dispose()
        }
    }
    if ($pendingRecords.Count -gt 0 -and $pendingCanBeCleared) {
        try {
            $manifest.launch_pending = @()
            Write-AtomicUtf8File `
                -Path $manifestPath `
                -Contents ($manifest | ConvertTo-Json -Depth 12)
        }
        catch {
            [void]$diagnostics.Add(
                "Could not persist cleared launch_pending records for ${RunRoot}: $($_.Exception.Message)")
        }
    }
    $processesProperty = $manifest.PSObject.Properties['processes']
    if ($null -eq $processesProperty) {
        $legacyPids = @(
            @('helper_pids', 'launcher_pids', 'gui_pids') |
                ForEach-Object {
                    $property = $manifest.PSObject.Properties[$_]
                    if ($null -ne $property) {
                        @($property.Value)
                    }
                } |
                Where-Object { [int]$_ -gt 0 })
        if ($legacyPids.Count -gt 0) {
            [void]$diagnostics.Add(
                "Stale sample run-root cleanup deferred for $RunRoot because legacy PID records lack start identity.")
        }
        return @($diagnostics)
    }

    $roleOrder = @('launcher', 'helper', 'gui')
    foreach ($role in $roleOrder) {
        foreach ($record in @(
                $processesProperty.Value |
                    Where-Object { [string]$_.role -eq $role })) {
            if ($null -eq $record) {
                [void]$diagnostics.Add(
                    'Ownership manifest contains an empty process identity record.')
                continue
            }
            $recordComplete = $true
            foreach ($propertyName in @('role', 'pid', 'executable', 'start_ticks')) {
                if ($null -eq $record.PSObject.Properties[$propertyName]) {
                    $recordComplete = $false
                }
            }
            if (-not $recordComplete) {
                [void]$diagnostics.Add(
                    "Ownership manifest contains an incomplete $role process identity record.")
                continue
            }
            $numericPid = [int]$record.pid
            $ownedProcessHandle = Get-OwnedProcessHandleForIdentity -Record $record
            if ($null -eq $ownedProcessHandle) {
                $reusedProcess = Get-Process -Id $numericPid -ErrorAction SilentlyContinue
                if ($null -ne $reusedProcess) {
                    $reusedProcess.Dispose()
                    [void]$diagnostics.Add(
                        "Refusing to clean $role PID $numericPid because its start identity no longer matches ownership.")
                }
                continue
            }
            try {
                if ($role -eq 'helper') {
                    Stop-OwnedHelperProcess `
                        -ProcessId $numericPid `
                        -ExpectedExecutable ([string]$record.executable) `
                        -ExpectedStartTicks ([int64]$record.start_ticks) `
                        -ProcessHandle $ownedProcessHandle
                }
                elseif ($role -eq 'launcher') {
                    Stop-OwnedLauncherProcessTree `
                        -ProcessId $numericPid `
                        -ExpectedExecutable ([string]$record.executable) `
                        -ExpectedStartTicks ([int64]$record.start_ticks) `
                        -ProcessHandle $ownedProcessHandle
                }
                else {
                    Stop-OwnedGuiProcess `
                        -ProcessId $numericPid `
                        -ExpectedExecutable ([string]$record.executable) `
                        -ExpectedStartTicks ([int64]$record.start_ticks) `
                        -ProcessHandle $ownedProcessHandle
                }
                if (-not $ownedProcessHandle.HasExited) {
                    [void]$diagnostics.Add("Owned $role PID $numericPid remained after bounded stale-root cleanup.")
                }
            }
            catch {
                [void]$diagnostics.Add("Owned $role PID $numericPid cleanup failed: $($_.Exception.Message)")
            }
            finally {
                $ownedProcessHandle.Dispose()
            }
        }
    }
    return @($diagnostics)
}

function Prune-SampleTempRunRoots {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TempRoot
    )

    $resolvedTempRoot = (Resolve-Path -LiteralPath $TempRoot).Path
    $candidates = @(Get-ChildItem -LiteralPath $resolvedTempRoot -Directory -Filter 'specforge-automation-samples-*' -ErrorAction Stop)
    foreach ($candidate in $candidates) {
        Assert-True -Condition ($candidate.Name -match '^specforge-automation-samples-[0-9a-f]{32}$') -Message "Refusing to prune an unrecognized sample run root: $($candidate.FullName)"
    }
    $diagnostics = [System.Collections.Generic.List[string]]::new()
    $stale = @($candidates | Sort-Object LastWriteTimeUtc -Descending | Select-Object -Skip $script:FailureTempRetentionCount)
    foreach ($candidate in $stale) {
        $candidateProcessDiagnostics = @(
            Stop-SampleRunRootOwnedProcesses -RunRoot $candidate.FullName)
        foreach ($processDiagnostic in $candidateProcessDiagnostics) {
            if (-not [string]::IsNullOrWhiteSpace([string]$processDiagnostic)) {
                [void]$diagnostics.Add(
                    "Stale sample run-root process cleanup: $processDiagnostic")
            }
        }
        if ($candidateProcessDiagnostics.Count -gt 0) {
            # A diagnostic means ownership could not be proven or cleanup was
            # incomplete. Keep the root for evidence; never delete a root
            # merely because a PID/path check failed.
            continue
        }
        if (Test-SampleRunRootHasLiveOwnedProcess -RunRoot $candidate.FullName) {
            [void]$diagnostics.Add("Temporary sample run root retention deferred because an owned process may still be alive: $($candidate.FullName)")
            continue
        }
        try {
            Remove-OwnedSampleTempDirectory -Path $candidate.FullName
        }
        catch {
            [void]$diagnostics.Add("Temporary sample run root cleanup failed for $($candidate.FullName): $($_.Exception.Message)")
        }
    }
    return @($diagnostics)
}

function Stop-OwnedGuiProcess {
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
        throw "Refusing to stop GUI PID $ProcessId without a start identity."
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
            throw "Refusing to stop GUI PID $ProcessId because it is not the sample executable."
        }
        if ($process.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) {
            throw "Refusing to stop GUI PID $ProcessId because its start identity does not match ownership."
        }
        Stop-AutomationProcessHandle `
            -ProcessHandle $process `
            -WaitMilliseconds $script:CleanupWaitMilliseconds
    }
    finally {
        if ($ownsProcessHandle) {
            $process.Dispose()
        }
    }
}

function Assert-OwnedGuiProcessGone {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedExecutable,
        [Parameter(Mandatory = $true)]
        [int64]$ExpectedStartTicks
    )

    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return
    }
    try {
        Stop-OwnedGuiProcess `
            -ProcessId $ProcessId `
            -ExpectedExecutable $ExpectedExecutable `
            -ExpectedStartTicks $ExpectedStartTicks `
            -ProcessHandle $process
    }
    finally {
        if (-not $process.HasExited) {
            throw "Owned GUI process $ProcessId remained after bounded cleanup."
        }
        $process.Dispose()
    }
}

function Stop-OwnedHelperProcess {
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
        throw "Refusing to stop helper PID $ProcessId without a start identity."
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
            throw "Refusing to stop helper PID $ProcessId because it is not the state fixture helper."
        }
        if ($process.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) {
            throw "Refusing to stop helper PID $ProcessId because its start identity does not match ownership."
        }
        Stop-AutomationProcessHandle `
            -ProcessHandle $process `
            -WaitMilliseconds $script:CleanupWaitMilliseconds
    }
    finally {
        if ($ownsProcessHandle) {
            $process.Dispose()
        }
    }
}

function Assert-OwnedHelperProcessGone {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedExecutable,
        [Parameter(Mandatory = $true)]
        [int64]$ExpectedStartTicks
    )

    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return
    }
    try {
        Stop-OwnedHelperProcess `
            -ProcessId $ProcessId `
            -ExpectedExecutable $ExpectedExecutable `
            -ExpectedStartTicks $ExpectedStartTicks `
            -ProcessHandle $process
    }
    finally {
        if (-not $process.HasExited) {
            throw "State fixture helper PID $ProcessId remained after bounded cleanup."
        }
        $process.Dispose()
    }
}

function Assert-OwnedProcessIdentityGone {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('helper', 'launcher', 'gui')]
        [string]$Role,
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [System.Diagnostics.Process]$ProcessHandle = $null
    )

    $record = @(
        $script:OwnedProcessIdentities |
            Where-Object {
                [string]$_.role -eq $Role -and
                [int]$_.pid -eq $ProcessId
            } |
            Select-Object -Last 1)
    if ($record.Count -eq 0) {
        if ($null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
            throw "Refusing to clean PID $ProcessId because no owned start-identity record exists."
        }
        return
    }
    $ownedRecord = $record[0]
    if ($null -ne $ProcessHandle -and $ProcessHandle.HasExited) {
        return
    }
    $ownsProcessHandle = $null -eq $ProcessHandle
    $ownedProcessHandle = if ($ownsProcessHandle) {
        Get-OwnedProcessHandleForIdentity -Record $ownedRecord
    }
    else {
        $ProcessHandle
    }
    if ($null -eq $ownedProcessHandle) {
        $currentProcess = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if ($null -ne $currentProcess) {
            $currentProcess.Dispose()
            throw "Refusing to clean $Role PID $ProcessId because its start identity no longer matches ownership."
        }
        return
    }
    try {
        if ($Role -eq 'helper') {
            Stop-OwnedHelperProcess `
                -ProcessId $ProcessId `
                -ExpectedExecutable ([string]$ownedRecord.executable) `
                -ExpectedStartTicks ([int64]$ownedRecord.start_ticks) `
                -ProcessHandle $ownedProcessHandle
        }
        elseif ($Role -eq 'launcher') {
            Stop-OwnedLauncherProcessTree `
                -ProcessId $ProcessId `
                -ExpectedExecutable ([string]$ownedRecord.executable) `
                -ExpectedStartTicks ([int64]$ownedRecord.start_ticks) `
                -ProcessHandle $ownedProcessHandle
        }
        else {
            Stop-OwnedGuiProcess `
                -ProcessId $ProcessId `
                -ExpectedExecutable ([string]$ownedRecord.executable) `
                -ExpectedStartTicks ([int64]$ownedRecord.start_ticks) `
                -ProcessHandle $ownedProcessHandle
        }
        if (-not $ownedProcessHandle.HasExited) {
            throw "Owned $Role process $ProcessId remained after bounded cleanup."
        }
    }
    finally {
        if ($ownsProcessHandle) {
            $ownedProcessHandle.Dispose()
        }
    }
}

$resolvedLauncher = (Resolve-Path -LiteralPath $Launcher).Path
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedStateFixture = (Resolve-Path -LiteralPath $StateFixture).Path
$resolvedFixtureDirectory = (Resolve-Path -LiteralPath $FixtureDirectory).Path
New-Item -ItemType Directory -Path $ArtifactsDirectory -Force | Out-Null
$resolvedArtifactsDirectory =
    (Resolve-Path -LiteralPath $ArtifactsDirectory).Path
$artifactCapacityLock = $null
try {
    try {
        $artifactCapacityLock = Enter-SampleArtifactCapacityLock `
            -ArtifactRoot $resolvedArtifactsDirectory
    }
    catch {
        $lockErrorMessage = $_.Exception.Message
        $existingDirectories = @(
            Get-ChildItem -LiteralPath $resolvedArtifactsDirectory `
                -Directory -Filter 'sample-run-*' -ErrorAction SilentlyContinue |
                ForEach-Object FullName)
        try {
            $lockDiagnosticPath = Write-SampleArtifactCapacityDiagnostic `
                -ArtifactRoot $resolvedArtifactsDirectory `
                -Capacity $script:ArtifactRunCapacity `
                -ExistingDirectories $existingDirectories `
                -PruneDiagnostics @($lockErrorMessage) `
                -Status 'artifact_capacity_lock_unavailable' `
                -Message 'No new run artifact directory was created because the bounded artifact capacity lock was unavailable.'
        }
        catch {
            $lockDiagnosticPath = $null
        }
        $lockSuffix = if ($null -ne $lockDiagnosticPath) {
            " Inspect the bounded diagnostic $lockDiagnosticPath."
        }
        else {
            ''
        }
        throw ("Sample artifact capacity coordination failed; no new run artifact directory was created. $lockErrorMessage$lockSuffix")
    }

    try {
        $failureEvidenceRetentionDiagnostics = @(
            Prune-FailureEvidenceDirectories -ArtifactRoot $resolvedArtifactsDirectory)
    }
    catch {
        $existingDirectories = @(
            Get-ChildItem -LiteralPath $resolvedArtifactsDirectory `
                -Directory -Filter 'sample-run-*' -ErrorAction SilentlyContinue |
                ForEach-Object FullName)
        $diagnosticPath = Write-SampleArtifactCapacityDiagnostic `
            -ArtifactRoot $resolvedArtifactsDirectory `
            -Capacity $script:ArtifactRunCapacity `
            -ExistingDirectories $existingDirectories `
            -PruneDiagnostics @($_.Exception.Message) `
            -Status 'artifact_capacity_check_failed' `
            -Message 'No new run artifact directory was created because the bounded failure-evidence retention check failed.'
        throw (
            'Sample failure-evidence retention check failed; no new run artifact directory was created. ' +
            "Inspect the bounded diagnostic $diagnosticPath. $($_.Exception.Message)")
    }
    $legacyArtifactCleanupDiagnostics = [System.Collections.Generic.List[string]]::new()
    foreach ($artifactName in @(
            'labeling-seed.json',
            'sample-labeling-tasks.json',
            'sample-label.png',
            'sample-seed.log',
            'sample-verify-labeling.log',
            'sample-source-navigation.log',
            'sample-label-capture.log',
            'sample-summary.json',
            'sample-artifact-capacity-diagnostic.json')) {
        $staleArtifact = Join-Path $resolvedArtifactsDirectory $artifactName
        if (Test-Path -LiteralPath $staleArtifact -PathType Leaf) {
            try {
                Remove-Item -LiteralPath $staleArtifact -Force -ErrorAction Stop
            }
            catch {
                [void]$legacyArtifactCleanupDiagnostics.Add(
                    "Could not remove legacy fixed-name artifact ${staleArtifact}: $($_.Exception.Message)")
            }
        }
    }
    try {
        $artifactRetentionDiagnostics = @(
            Prune-SampleArtifactRunDirectories `
                -ArtifactRoot $resolvedArtifactsDirectory `
                -KeepCount ([Math]::Max(0, $script:ArtifactRunCapacity - 1)))
    }
    catch {
        $existingDirectories = @(
            Get-ChildItem -LiteralPath $resolvedArtifactsDirectory `
                -Directory -Filter 'sample-run-*' -ErrorAction SilentlyContinue |
                ForEach-Object FullName)
        $diagnosticPath = Write-SampleArtifactCapacityDiagnostic `
            -ArtifactRoot $resolvedArtifactsDirectory `
            -Capacity $script:ArtifactRunCapacity `
            -ExistingDirectories $existingDirectories `
            -PruneDiagnostics @($_.Exception.Message) `
            -Status 'artifact_capacity_check_failed' `
            -Message 'No new run artifact directory was created because the bounded artifact retention check failed.'
        throw (
            'Sample artifact retention check failed; no new run artifact directory was created. ' +
            "Inspect the bounded diagnostic $diagnosticPath. $($_.Exception.Message)")
    }
    $artifactRunDirectories = @(
        Assert-SampleArtifactCapacity `
            -ArtifactRoot $resolvedArtifactsDirectory `
            -Capacity $script:ArtifactRunCapacity `
            -PruneDiagnostics $artifactRetentionDiagnostics)
    $temporaryRootRetentionDiagnostics = @(
        Prune-SampleTempRunRoots -TempRoot ([System.IO.Path]::GetTempPath()))
    $remainingTemporaryRoots = @(
        Get-ChildItem `
            -LiteralPath ([System.IO.Path]::GetTempPath()) `
            -Directory `
            -Filter 'specforge-automation-samples-*' `
            -ErrorAction Stop)
    if ($remainingTemporaryRoots.Count -ge $script:FailureTempRetentionCount) {
        throw (
            'TEMP sample-run retention capacity is exhausted by unresolved roots; ' +
            "retained=$($remainingTemporaryRoots.Count), capacity=$($script:FailureTempRetentionCount). " +
            'No new run root was created; inspect and clear only the explicitly retained evidence roots.')
    }
    $runId = [Guid]::NewGuid().ToString('N')
    $resolvedRunArtifactsDirectory = Join-Path `
        $resolvedArtifactsDirectory `
        ('sample-run-' + $runId)
    New-Item -ItemType Directory -Path $resolvedRunArtifactsDirectory -Force | Out-Null
    $artifactLeasePath = Join-Path `
        $resolvedRunArtifactsDirectory `
        '.specforge-automation-samples-artifact-lease.json'
    Initialize-SampleRunLease `
        -RunId $runId `
        -ArtifactLeasePath $artifactLeasePath
}
finally {
    if ($null -ne $artifactCapacityLock) {
        $artifactCapacityLock.Dispose()
    }
}

$runRoot = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-automation-samples-' + [Guid]::NewGuid().ToString('N'))
$stateRootA = Join-Path $runRoot 'source-navigation-state'
$stateRootB = Join-Path $runRoot 'label-capture-state'
$seedPath = Join-Path $runRoot 'labeling-seed.json'
$capturePath = Join-Path $stateRootB 'artifacts\sample-label.png'
$seedLogPath = Join-Path $resolvedRunArtifactsDirectory 'sample-seed.log'
$verifyLogPath = Join-Path $resolvedRunArtifactsDirectory 'sample-verify-labeling.log'
$summaryPath = Join-Path $resolvedRunArtifactsDirectory 'sample-summary.json'
$failureDiagnostics = @(
    $legacyArtifactCleanupDiagnostics
    $failureEvidenceRetentionDiagnostics
    $artifactRetentionDiagnostics
    $temporaryRootRetentionDiagnostics)
$failureMessage = if ($failureDiagnostics.Count -gt 0) {
    'Sample run retention reported: ' +
        ($failureDiagnostics -join ' ')
}
else {
    $null
}
$successSummary = $null
$ownedGuiPids = [System.Collections.Generic.List[int]]::new()
$ownedHelperPids = [System.Collections.Generic.List[int]]::new()
$ownedLauncherPids = [System.Collections.Generic.List[int]]::new()

try {
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
    Write-SampleRunRootOwnershipManifest -RunRoot $runRoot -HelperPath $resolvedStateFixture -LauncherPath $resolvedLauncher -ExecutablePath $resolvedExecutable -OwnedHelperPids $ownedHelperPids -OwnedLauncherPids $ownedLauncherPids -OwnedGuiPids $ownedGuiPids
    $seedResult = Invoke-StateFixtureHelper `
        -HelperPath $resolvedStateFixture `
        -LauncherPath $resolvedLauncher `
        -ExecutablePath $resolvedExecutable `
        -Arguments @(
            '--write-labeling-seed',
            $seedPath,
            '--source',
            $resolvedFixtureDirectory) `
        -LogPath $seedLogPath `
        -RunRoot $runRoot `
        -OwnedHelperPids $ownedHelperPids `
        -OwnedLauncherPids $ownedLauncherPids `
        -OwnedGuiPids $ownedGuiPids
    $seedExitCode = $seedResult.ExitCode
    Assert-True `
        -Condition ($seedExitCode -eq 0 -and (Test-Path -LiteralPath $seedPath -PathType Leaf)) `
        -Message 'The production state fixture helper must create a labeling seed for the checked-in source collection.'
    $seedHashBefore = Get-FileSha256 -Path $seedPath
    Write-SampleRunRootOwnershipManifest -RunRoot $runRoot -HelperPath $resolvedStateFixture -LauncherPath $resolvedLauncher -ExecutablePath $resolvedExecutable -OwnedHelperPids $ownedHelperPids -OwnedLauncherPids $ownedLauncherPids -OwnedGuiPids $ownedGuiPids

    $sourceSequence = Invoke-LauncherSequence `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedExecutable `
        -StateRoot $stateRootA `
        -RunRoot $runRoot `
        -HelperPath $resolvedStateFixture `
        -OwnedHelperPids $ownedHelperPids `
        -Commands @(
            "source open $resolvedFixtureDirectory",
            'wait idle',
            'spectrum goto name bravo.csv',
            'state get',
        'app quit') `
        -OwnedGuiPids $ownedGuiPids `
        -OwnedLauncherPids $ownedLauncherPids `
        -LogPath (Join-Path $resolvedRunArtifactsDirectory 'sample-source-navigation.log')
    Assert-True `
        -Condition $ownedGuiPids.Contains([int]$sourceSequence.GuiPid) `
        -Message 'The source-navigation sequence must register its GUI PID while the launcher is running.'
    Assert-OwnedProcessIdentityGone -Role gui -ProcessId $sourceSequence.GuiPid
    Write-SampleRunRootOwnershipManifest -RunRoot $runRoot -HelperPath $resolvedStateFixture -LauncherPath $resolvedLauncher -ExecutablePath $resolvedExecutable -OwnedHelperPids $ownedHelperPids -OwnedLauncherPids $ownedLauncherPids -OwnedGuiPids $ownedGuiPids
    Assert-True `
        -Condition ($sourceSequence.ExitCode -eq 0) `
        -Message "Source navigation sample launcher exited with $($sourceSequence.ExitCode)."
    $sourceTerminal = Get-TerminalMessage -Messages $sourceSequence.Messages -Command 'source.open'
    $sourceWaitTerminal = Get-TerminalMessage -Messages $sourceSequence.Messages -Command 'wait.idle'
    $sourceGotoTerminal = Get-TerminalMessage -Messages $sourceSequence.Messages -Command 'spectrum.goto'
    $sourceStateTerminal = Get-TerminalMessage -Messages $sourceSequence.Messages -Command 'state.get'
    $sourceQuitTerminal = Get-TerminalMessage -Messages $sourceSequence.Messages -Command 'app.quit'
    Assert-True `
        -Condition (
            [string]$sourceTerminal.status -eq 'completed' -and
            [int]$sourceTerminal.result.source.spectrum_count -eq 3 -and
            [string]$sourceWaitTerminal.status -eq 'completed' -and
            [string]$sourceGotoTerminal.status -eq 'completed' -and
            [int]$sourceGotoTerminal.result.spectrum.index -eq 1 -and
            [string]$sourceGotoTerminal.result.spectrum.name -eq 'bravo.csv' -and
            [string]$sourceStateTerminal.status -eq 'completed' -and
            [bool]$sourceStateTerminal.state.shell.idle -and
            [bool]$sourceStateTerminal.state.window.visible -and
            -not [bool]$sourceStateTerminal.state.window.minimized -and
            [int]$sourceStateTerminal.state.spectrum.index -eq 1 -and
            [string]$sourceStateTerminal.state.spectrum.name -eq 'bravo.csv' -and
            [string]$sourceQuitTerminal.status -eq 'completed') `
        -Message 'Source navigation sample did not reach its documented terminal states.'
    Assert-PathEqual `
        -Actual ([string]$sourceStateTerminal.state.source.path) `
        -Expected $resolvedFixtureDirectory `
        -Message 'Source navigation sample state must identify the checked-in fixture directory.'

    $labelSequence = Invoke-LauncherSequence `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedExecutable `
        -StateRoot $stateRootB `
        -RunRoot $runRoot `
        -HelperPath $resolvedStateFixture `
        -OwnedHelperPids $ownedHelperPids `
        -SeedPath $seedPath `
        -Commands @(
            "source open $resolvedFixtureDirectory",
            'wait idle',
            'spectrum goto 2',
            'label assign 5 spectrum 1',
            'spectrum goto 1',
            'wait idle',
            "frame capture $capturePath",
            'state get',
        'app quit') `
        -OwnedGuiPids $ownedGuiPids `
        -OwnedLauncherPids $ownedLauncherPids `
        -LogPath (Join-Path $resolvedRunArtifactsDirectory 'sample-label-capture.log')
    Assert-True `
        -Condition $ownedGuiPids.Contains([int]$labelSequence.GuiPid) `
        -Message 'The label/capture sequence must register its GUI PID while the launcher is running.'
    Assert-OwnedProcessIdentityGone -Role gui -ProcessId $labelSequence.GuiPid
    Write-SampleRunRootOwnershipManifest -RunRoot $runRoot -HelperPath $resolvedStateFixture -LauncherPath $resolvedLauncher -ExecutablePath $resolvedExecutable -OwnedHelperPids $ownedHelperPids -OwnedLauncherPids $ownedLauncherPids -OwnedGuiPids $ownedGuiPids
    Assert-True `
        -Condition ($labelSequence.ExitCode -eq 0) `
        -Message "Label/capture sample launcher exited with $($labelSequence.ExitCode)."
    $labelSourceTerminal = Get-TerminalMessage -Messages $labelSequence.Messages -Command 'source.open'
    $labelTerminal = Get-TerminalMessage -Messages $labelSequence.Messages -Command 'label.assign'
    $labelWaitTerminals = @(
        Get-TerminalMessages -Messages $labelSequence.Messages -Command 'wait.idle')
    $labelGotoTerminals = @(
        Get-TerminalMessages -Messages $labelSequence.Messages -Command 'spectrum.goto')
    Assert-True `
        -Condition ($labelWaitTerminals.Count -eq 2) `
        -Message 'Sequence B must produce exactly two wait.idle terminal responses.'
    Assert-True `
        -Condition ($labelGotoTerminals.Count -eq 2) `
        -Message 'Sequence B must produce exactly two spectrum.goto terminal responses.'
    $labelFirstWaitTerminal = $labelWaitTerminals[0]
    $labelSecondWaitTerminal = $labelWaitTerminals[1]
    $labelFirstGotoTerminal = $labelGotoTerminals[0]
    $labelLastGotoTerminal = $labelGotoTerminals[1]
    $captureTerminal = Get-TerminalMessage -Messages $labelSequence.Messages -Command 'frame.capture'
    $labelStateTerminal = Get-TerminalMessage -Messages $labelSequence.Messages -Command 'state.get'
    $labelQuitTerminal = Get-TerminalMessage -Messages $labelSequence.Messages -Command 'app.quit'
    Assert-True `
        -Condition (
            [string]$labelSourceTerminal.status -eq 'completed' -and
            [int]$labelSourceTerminal.result.source.spectrum_count -eq 3 -and
            [string]$labelTerminal.status -eq 'completed' -and
            [int]$labelTerminal.result.assignment.spectrum.index -eq 1 -and
            [int]$labelTerminal.result.assignment.previous_code -eq -1 -and
            [int]$labelTerminal.result.assignment.new_code -eq 5 -and
            [bool]$labelTerminal.result.assignment.changed -and
            [string]$labelFirstWaitTerminal.status -eq 'completed' -and
            [string]$labelSecondWaitTerminal.status -eq 'completed' -and
            [string]$labelFirstGotoTerminal.status -eq 'completed' -and
            [int]$labelFirstGotoTerminal.result.spectrum.index -eq 2 -and
            [string]$labelFirstGotoTerminal.result.spectrum.name -eq 'charlie.csv' -and
            [string]$labelLastGotoTerminal.status -eq 'completed' -and
            [int]$labelLastGotoTerminal.result.spectrum.index -eq 1 -and
            [string]$labelLastGotoTerminal.result.spectrum.name -eq 'bravo.csv' -and
            [string]$captureTerminal.status -eq 'completed' -and
            [string]$captureTerminal.result.format -eq 'png' -and
            [string]$captureTerminal.result.scope -eq 'main_viewport' -and
            [int]$captureTerminal.result.width -gt 0 -and
            [int]$captureTerminal.result.height -gt 0 -and
            [string]$labelStateTerminal.status -eq 'completed' -and
            [bool]$labelStateTerminal.state.shell.idle -and
            [int]$labelStateTerminal.state.spectrum.index -eq 1 -and
            [int]$labelStateTerminal.state.labeling.current_spectrum_label.code -eq 5 -and
            [string]$labelStateTerminal.state.capture.last_result -eq 'succeeded' -and
            [string]$labelQuitTerminal.status -eq 'completed') `
        -Message 'Label/capture sample did not reach its documented terminal states.'
    Assert-True `
        -Condition (
            (Test-Path -LiteralPath $capturePath -PathType Leaf) -and
            (Get-Item -LiteralPath $capturePath).Length -gt 8) `
        -Message 'Label/capture sample must leave a non-empty application-rendered PNG before cleanup.'
    $pngIhdr = Get-PngIhdr -Path $capturePath
    Assert-True `
        -Condition (
            [uint32]$pngIhdr.Width -eq [uint32]$captureTerminal.result.width -and
            [uint32]$pngIhdr.Height -eq [uint32]$captureTerminal.result.height) `
        -Message 'Label/capture sample PNG dimensions must match the frame.capture terminal.'
    Assert-PathEqual `
        -Actual ([string]$labelStateTerminal.state.source.path) `
        -Expected $resolvedFixtureDirectory `
        -Message 'Label/capture sample state must identify the checked-in fixture directory.'

    $verifyResult = Invoke-StateFixtureHelper `
        -HelperPath $resolvedStateFixture `
        -LauncherPath $resolvedLauncher `
        -ExecutablePath $resolvedExecutable `
        -Arguments @(
            '--verify-labeling-state',
            (Join-Path $stateRootB 'sample-labeling-tasks.json'),
            '--source',
            $resolvedFixtureDirectory,
            '--spectrum-index',
            '1',
            '--expected-code',
            '5') `
        -LogPath $verifyLogPath `
        -RunRoot $runRoot `
        -OwnedHelperPids $ownedHelperPids `
        -OwnedLauncherPids $ownedLauncherPids `
        -OwnedGuiPids $ownedGuiPids
    $verifyExitCode = $verifyResult.ExitCode
    Assert-True `
        -Condition ($verifyExitCode -eq 0) `
        -Message 'The production labeling reader must reload code 5 after the sample app quits.'
    Assert-True `
        -Condition ((Get-FileSha256 -Path $seedPath) -ceq $seedHashBefore) `
        -Message 'The read-only labeling seed must remain unchanged after the sample sequence.'
    Write-SampleRunRootOwnershipManifest -RunRoot $runRoot -HelperPath $resolvedStateFixture -LauncherPath $resolvedLauncher -ExecutablePath $resolvedExecutable -OwnedHelperPids $ownedHelperPids -OwnedLauncherPids $ownedLauncherPids -OwnedGuiPids $ownedGuiPids

    $summary = [ordered]@{
        passed = $true
        fixture = 'tests/fixtures/automation/samples'
        sequences = @(
            [ordered]@{
                name = 'source-navigation'
                source_count = 3
                selected_name = 'bravo.csv'
                selected_index = 1
                terminal = 'app.quit:completed'
            },
            [ordered]@{
                name = 'label-capture'
                label_code = 5
                selected_index = 1
                capture = 'png:main_viewport'
                terminal = 'app.quit:completed'
            }
        )
        seed_unchanged = $true
        production_reload_exit_code = $verifyExitCode
        owned_processes = [ordered]@{
            helper_pids = @($ownedHelperPids)
            launcher_pids = @($ownedLauncherPids)
            gui_pids = @($ownedGuiPids)
        }
        temporary_run_root = 'pending_cleanup'
        temporary_root_retention_count = $script:FailureTempRetentionCount
    }
    $successSummary = $summary
}
catch {
    $failureMessage = $_.Exception.Message
    Write-Error $failureMessage
    throw
}
finally {
    $cleanupFailures = [System.Collections.Generic.List[string]]::new()
    foreach ($ownedHelperPid in $ownedHelperPids) {
        try {
            Assert-OwnedProcessIdentityGone `
                -Role helper `
                -ProcessId ([int]$ownedHelperPid)
        }
        catch {
            [void]$cleanupFailures.Add($_.Exception.Message)
        }
    }
    foreach ($ownedLauncherPid in $ownedLauncherPids) {
        try {
            Assert-OwnedProcessIdentityGone `
                -Role launcher `
                -ProcessId ([int]$ownedLauncherPid)
        }
        catch {
            [void]$cleanupFailures.Add($_.Exception.Message)
        }
    }
    foreach ($ownedGuiPid in $ownedGuiPids) {
        try {
            Assert-OwnedProcessIdentityGone `
                -Role gui `
                -ProcessId ([int]$ownedGuiPid)
        }
        catch {
            [void]$cleanupFailures.Add($_.Exception.Message)
        }
    }

    try {
        Write-SampleRunRootOwnershipManifest -RunRoot $runRoot -HelperPath $resolvedStateFixture -LauncherPath $resolvedLauncher -ExecutablePath $resolvedExecutable -OwnedHelperPids $ownedHelperPids -OwnedLauncherPids $ownedLauncherPids -OwnedGuiPids $ownedGuiPids
    }
    catch {
        [void]$cleanupFailures.Add(
            "Could not update the final sample ownership manifest: $($_.Exception.Message)")
    }

    $preservedRunRootArtifact = $null
    $needsFailureEvidence = $null -ne $failureMessage -or
        $cleanupFailures.Count -gt 0
    if ($needsFailureEvidence) {
        try {
            foreach ($source in @(
                    $seedPath,
                    (Join-Path $stateRootB 'sample-labeling-tasks.json'),
                    $capturePath)) {
                if (Test-Path -LiteralPath $source -PathType Leaf) {
                    $destination = Join-Path `
                        $resolvedRunArtifactsDirectory `
                        ([System.IO.Path]::GetFileName($source))
                    Copy-Item -LiteralPath $source -Destination $destination -Force
                }
            }
        }
        catch {
            [void]$cleanupFailures.Add(
                "Could not preserve known sample failure evidence: $($_.Exception.Message)")
        }
    }

    if ($cleanupFailures.Count -gt 0) {
        $preservedRunRootArtifact = Join-Path `
            $resolvedRunArtifactsDirectory `
            ('sample-run-root-' + [Guid]::NewGuid().ToString('N'))
        try {
            Copy-Item -LiteralPath $runRoot `
                -Destination $preservedRunRootArtifact `
                -Recurse -Force -ErrorAction Stop
        }
        catch {
            [void]$cleanupFailures.Add(
                "Could not preserve the owned sample run root: $($_.Exception.Message)")
        }
    }

    $runRootCanBeRemoved = -not (
        Test-SampleRunRootHasLiveOwnedProcess -RunRoot $runRoot -CurrentRun)
    if (-not $runRootCanBeRemoved) {
        [void]$cleanupFailures.Add(
            'Owned sample run root was retained because an owned process may still be alive.')
    }
    if ($runRootCanBeRemoved) {
        try {
            Remove-OwnedDirectory -Path $runRoot
        }
        catch {
            [void]$cleanupFailures.Add(
                "Owned sample run-root cleanup failed: $($_.Exception.Message)")
            $preservedRunRootArtifact = Join-Path `
                $resolvedRunArtifactsDirectory `
                ('sample-run-root-' + [Guid]::NewGuid().ToString('N'))
            try {
                Copy-Item -LiteralPath $runRoot `
                    -Destination $preservedRunRootArtifact `
                    -Recurse -Force -ErrorAction Stop
            }
            catch {
                [void]$cleanupFailures.Add(
                    "Could not preserve the owned sample run root: $($_.Exception.Message)")
            }
        }
    }

    $finalArtifactCapacityLock = $null
    try {
        $finalArtifactCapacityLock = Enter-SampleArtifactCapacityLock `
            -ArtifactRoot $resolvedArtifactsDirectory
        Complete-SampleRunLease
        if (Test-Path -LiteralPath $runRoot -PathType Container) {
            Write-SampleRunRootOwnershipManifest `
                -RunRoot $runRoot `
                -HelperPath $resolvedStateFixture `
                -LauncherPath $resolvedLauncher `
                -ExecutablePath $resolvedExecutable `
                -OwnedHelperPids $ownedHelperPids `
                -OwnedLauncherPids $ownedLauncherPids `
                -OwnedGuiPids $ownedGuiPids
        }
        $retainedArtifactCount = [Math]::Max(
            0,
            $script:FailureEvidenceRetentionCount - 1)
        foreach ($artifactDiagnostic in @(
                Prune-SampleArtifactRunDirectories `
                    -ArtifactRoot $resolvedArtifactsDirectory `
                    -KeepCount $retainedArtifactCount `
                    -ExcludePath $resolvedRunArtifactsDirectory)) {
            if (-not [string]::IsNullOrWhiteSpace([string]$artifactDiagnostic)) {
                [void]$cleanupFailures.Add(
                    "Final sample artifact retention: $artifactDiagnostic")
            }
        }
    }
    catch {
        [void]$cleanupFailures.Add(
            "Could not complete the sample run lease: $($_.Exception.Message)")
    }
    finally {
        if ($null -ne $finalArtifactCapacityLock) {
            $finalArtifactCapacityLock.Dispose()
        }
    }

    $temporaryRunRootState = if (Test-Path -LiteralPath $runRoot -PathType Container) {
        'preserved_bounded'
    }
    else {
        'removed'
    }
    if ($null -ne $successSummary) {
        $successSummary.temporary_run_root = $temporaryRunRootState
        $successSummary.temporary_root_retention_count = $script:FailureTempRetentionCount
    }

    if ($cleanupFailures.Count -gt 0) {
        $cleanupDiagnostic = ($cleanupFailures -join ' ')
        $summaryFailure = if ($null -ne $failureMessage) {
            "$failureMessage $cleanupDiagnostic"
        }
        else {
            "Automation cleanup failed: $cleanupDiagnostic"
        }
        $failureSummary = [ordered]@{
            passed = $false
            failure = $summaryFailure
            cleanup_failures = @($cleanupFailures)
            owned_gui_pids = @($ownedGuiPids)
            owned_helper_pids = @($ownedHelperPids)
            owned_launcher_pids = @($ownedLauncherPids)
            preserved_run_root = $preservedRunRootArtifact
            temporary_run_root = $temporaryRunRootState
            temporary_root_retention_count = $script:FailureTempRetentionCount
        }
        Write-Utf8File `
            -Path $summaryPath `
            -Contents ($failureSummary | ConvertTo-Json -Depth 8)
        throw $summaryFailure
    }

    if ($null -ne $failureMessage) {
        $failureSummary = [ordered]@{
            passed = $false
            failure = $failureMessage
            owned_gui_pids = @($ownedGuiPids)
            owned_helper_pids = @($ownedHelperPids)
            owned_launcher_pids = @($ownedLauncherPids)
            temporary_run_root = $temporaryRunRootState
            temporary_root_retention_count = $script:FailureTempRetentionCount
        }
        Write-Utf8File `
            -Path $summaryPath `
            -Contents ($failureSummary | ConvertTo-Json -Depth 8)
        throw $failureMessage
    }
    else {
        Write-Utf8File `
            -Path $summaryPath `
            -Contents ($successSummary | ConvertTo-Json -Depth 8)
        Write-Host 'Automation sample sequences passed.'
    }
}
