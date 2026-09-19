param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$StateFixture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$script:PipeIoTimeoutMilliseconds = 30000
$script:LabelingTaskId = '77777777-7777-4777-8777-777777777777'
$script:PairedBarrierTimeoutMilliseconds = 5000
$script:RunnerMainFlowBudgetMilliseconds = 180000
$script:CleanupBudgetMilliseconds = 30000
$script:CleanupKillWaitMilliseconds = 1000
$script:FixtureCleanupBudgetMilliseconds = 10000
$script:FixtureFallbackBudgetMilliseconds = 3000
$script:AliasLockRoot = $null
$script:OwnedAliasLockDirectories = @()
$script:AliasOwnershipManifestReady = $false
$script:RunnerMainDeadlineUtc = [DateTime]::UtcNow.AddMilliseconds(
    $script:RunnerMainFlowBudgetMilliseconds)

# The cache and all output paths are under this run's unique fixture root.
# The state fixture records the exact coordination directories derived from
# this cache path; cleanup never infers ownership from a global directory scan.

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

function Resolve-PipeDeadline {
    param(
        [DateTime]$RequestedDeadlineUtc = [DateTime]::MinValue
    )

    if ($RequestedDeadlineUtc.Ticks -ne 0) {
        return $RequestedDeadlineUtc.ToUniversalTime()
    }
    $runnerDeadline = Get-Variable `
        -Name RunnerMainDeadlineUtc `
        -Scope Script `
        -ValueOnly `
        -ErrorAction SilentlyContinue
    if ($null -ne $runnerDeadline -and $runnerDeadline.Ticks -ne 0) {
        return $runnerDeadline.ToUniversalTime()
    }
    return [DateTime]::UtcNow.AddMilliseconds(
        $script:PipeIoTimeoutMilliseconds)
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

function Quote-WindowsArgument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }
    if ($Value.Length -eq 0) {
        return '""'
    }
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

function New-RandomHex {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ByteCount
    )

    $bytes = [byte[]]::new($ByteCount)
    $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $generator.GetBytes($bytes)
    }
    finally {
        $generator.Dispose()
    }
    return [System.BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
}

function Send-PipeJson {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.Pipes.NamedPipeClientStream]$Pipe,
        [Parameter(Mandatory = $true)]
        [object]$Message,
        [DateTime]$DeadlineUtc = [DateTime]::MinValue
    )

    $payload = [System.Text.UTF8Encoding]::new($false).GetBytes(
        ($Message | ConvertTo-Json -Compress -Depth 12))
    $cancellation = [System.Threading.CancellationTokenSource]::new()
    $DeadlineUtc = Resolve-PipeDeadline `
        -RequestedDeadlineUtc $DeadlineUtc
    try {
        $writeTask = $Pipe.WriteAsync(
            $payload,
            0,
            $payload.Length,
            $cancellation.Token)
        $remaining = [int][Math]::Ceiling(
            ($DeadlineUtc - [DateTime]::UtcNow).TotalMilliseconds)
        if ($remaining -le 0 -or -not $writeTask.Wait($remaining)) {
            throw [TimeoutException]::new(
                'Timed out writing a GUI automation pipe request.')
        }
        [void]$writeTask.GetAwaiter().GetResult()

        $flushTask = $Pipe.FlushAsync($cancellation.Token)
        $remaining = [int][Math]::Ceiling(
            ($DeadlineUtc - [DateTime]::UtcNow).TotalMilliseconds)
        if ($remaining -le 0 -or -not $flushTask.Wait($remaining)) {
            throw [TimeoutException]::new(
                'Timed out flushing a GUI automation pipe request.')
        }
        [void]$flushTask.GetAwaiter().GetResult()
    }
    catch {
        try {
            $cancellation.Cancel()
        }
        catch {
        }
        Dispose-Pipe -Pipe $Pipe
        throw
    }
    finally {
        $cancellation.Dispose()
    }
}

function Dispose-Pipe {
    param(
        [object]$Pipe
    )

    if ($null -ne $Pipe) {
        try {
            $Pipe.Dispose()
        }
        catch {
        }
    }
}

function Receive-PipeJson {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.Pipes.NamedPipeClientStream]$Pipe,
        [DateTime]$DeadlineUtc = [DateTime]::MinValue
    )

    $buffer = [byte[]]::new(65536)
    $message = [System.IO.MemoryStream]::new()
    $cancellation = [System.Threading.CancellationTokenSource]::new()
    $DeadlineUtc = Resolve-PipeDeadline `
        -RequestedDeadlineUtc $DeadlineUtc
    try {
        do {
            $remaining = [int][Math]::Ceiling(
                ($DeadlineUtc - [DateTime]::UtcNow).TotalMilliseconds)
            if ($remaining -le 0) {
                throw [TimeoutException]::new(
                    'Timed out waiting for a GUI automation pipe response.')
            }
            $readTask = $Pipe.ReadAsync(
                $buffer,
                0,
                $buffer.Length,
                $cancellation.Token)
            if (-not $readTask.Wait($remaining)) {
                throw [TimeoutException]::new(
                    'Timed out waiting for a GUI automation pipe response.')
            }
            $read = $readTask.GetAwaiter().GetResult()
            if ($read -le 0) {
                throw 'The GUI automation pipe closed before a complete response arrived.'
            }
            $message.Write($buffer, 0, $read)
        } while (-not $Pipe.IsMessageComplete)
        return [System.Text.UTF8Encoding]::new($false).GetString(
            $message.ToArray()) | ConvertFrom-Json
    }
    catch {
        try {
            $cancellation.Cancel()
        }
        catch {
        }
        Dispose-Pipe -Pipe $Pipe
        throw
    }
    finally {
        $message.Dispose()
        $cancellation.Dispose()
    }
}

function New-PipeRequestId {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Instance
    )

    $requestId = $Instance.Name + '-' + $Instance.NextRequest
    $Instance.NextRequest++
    return $requestId
}

function Send-PipeRequest {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Instance,
        [Parameter(Mandatory = $true)]
        [string]$RequestId,
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [object]$Parameters,
        [DateTime]$DeadlineUtc = [DateTime]::MinValue
    )

    $request = [ordered]@{
        type = 'request'
        request_id = $RequestId
        command = $Command
    }
    if ($null -ne $Parameters) {
        $request.params = $Parameters
    }
    $DeadlineUtc = Resolve-PipeDeadline `
        -RequestedDeadlineUtc $DeadlineUtc
    Send-PipeJson `
        -Pipe $Instance.Pipe `
        -Message $request `
        -DeadlineUtc $DeadlineUtc
    return Receive-PipeJson `
        -Pipe $Instance.Pipe `
        -DeadlineUtc $DeadlineUtc
}

function Receive-Terminal {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Instance,
        [Parameter(Mandatory = $true)]
        [string]$RequestId,
        [DateTime]$DeadlineUtc = [DateTime]::MinValue
    )

    $DeadlineUtc = Resolve-PipeDeadline `
        -RequestedDeadlineUtc $DeadlineUtc
    $terminal = Receive-PipeJson `
        -Pipe $Instance.Pipe `
        -DeadlineUtc $DeadlineUtc
    Assert-True `
        -Condition ([string]$terminal.request_id -eq $RequestId) `
        -Message "GUI $($Instance.Name) returned an unexpected terminal request ID."
    Assert-True `
        -Condition ([string]$terminal.status -in @('completed', 'failed', 'canceled')) `
        -Message "GUI $($Instance.Name) returned a non-terminal response for $RequestId."
    return $terminal
}

function Invoke-GuiCommand {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Instance,
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [object]$Parameters,
        [DateTime]$DeadlineUtc = [DateTime]::MinValue
    )

    $DeadlineUtc = Resolve-PipeDeadline `
        -RequestedDeadlineUtc $DeadlineUtc
    $requestId = New-PipeRequestId -Instance $Instance
    $accepted = Send-PipeRequest `
        -Instance $Instance `
        -RequestId $requestId `
        -Command $Command `
        -Parameters $Parameters `
        -DeadlineUtc $DeadlineUtc
    Assert-True `
        -Condition ([string]$accepted.status -eq 'accepted') `
        -Message "GUI $($Instance.Name) did not accept $Command ($requestId)."
    return Receive-Terminal `
        -Instance $Instance `
        -RequestId $requestId `
        -DeadlineUtc $DeadlineUtc
}

function Start-PairedAcceptedWorker {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Instance,
        [Parameter(Mandatory = $true)]
        [object]$Request,
        [Parameter(Mandatory = $true)]
        [System.Threading.Barrier]$ReadyBarrier,
        [Parameter(Mandatory = $true)]
        [System.Threading.Barrier]$SentBarrier,
        [DateTime]$DeadlineUtc = [DateTime]::MinValue
    )

    $DeadlineUtc = Resolve-PipeDeadline `
        -RequestedDeadlineUtc $DeadlineUtc
    $barrierTimeoutMilliseconds = [Math]::Min(
        $script:PairedBarrierTimeoutMilliseconds,
        (Get-DeadlineRemainingMilliseconds -DeadlineUtc $DeadlineUtc))

    $initialState =
        [System.Management.Automation.Runspaces.InitialSessionState]::CreateDefault()
    foreach ($functionName in @(
            'Dispose-Pipe',
            'Resolve-PipeDeadline',
            'Send-PipeJson',
            'Receive-PipeJson')) {
        $function = Get-Command $functionName -CommandType Function
        $entry =
            [System.Management.Automation.Runspaces.SessionStateFunctionEntry]::new(
                $functionName,
                $function.ScriptBlock)
        [void]$initialState.Commands.Add($entry)
    }

    $runspace = $null
    $powerShell = $null
    try {
        $runspace =
            [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace(
                $initialState)
        $runspace.Open()
        $powerShell = [System.Management.Automation.PowerShell]::Create()
        $powerShell.Runspace = $runspace
        $workerScript = @'
param($Pipe, $Request, $ReadyBarrier, $SentBarrier, $TimeoutMilliseconds, $BarrierTimeoutMilliseconds, $DeadlineUtc)
$script:PipeIoTimeoutMilliseconds = $TimeoutMilliseconds
$script:RunnerMainDeadlineUtc = $DeadlineUtc
if (-not $ReadyBarrier.SignalAndWait($BarrierTimeoutMilliseconds)) {
    throw [TimeoutException]::new('Timed out waiting for the paired GUI ready barrier.')
}
Send-PipeJson -Pipe $Pipe -Message $Request -DeadlineUtc $DeadlineUtc
if (-not $SentBarrier.SignalAndWait($BarrierTimeoutMilliseconds)) {
    throw [TimeoutException]::new('Timed out waiting for both paired GUI requests to be sent.')
}
$accepted = Receive-PipeJson -Pipe $Pipe -DeadlineUtc $DeadlineUtc
if ([string]$accepted.status -ne 'accepted') {
    throw "Request $($Request.request_id) was not accepted."
}
[pscustomobject]@{
    RequestId = [string]$Request.request_id
    Accepted = $accepted
}
'@
        [void]$powerShell.AddScript($workerScript).
            AddArgument($Instance.Pipe).
            AddArgument($Request).
            AddArgument($ReadyBarrier).
            AddArgument($SentBarrier).
            AddArgument($script:PipeIoTimeoutMilliseconds).
            AddArgument($barrierTimeoutMilliseconds).
            AddArgument($DeadlineUtc)
        $asyncResult = $powerShell.BeginInvoke()
        return [pscustomobject]@{
            PowerShell = $powerShell
            Runspace = $runspace
            AsyncResult = $asyncResult
            Disposed = $false
        }
    }
    catch {
        if ($null -ne $powerShell) {
            try {
                $powerShell.Dispose()
            }
            catch {
            }
        }
        if ($null -ne $runspace) {
            try {
                $runspace.Dispose()
            }
            catch {
            }
        }
        throw
    }
}

function Dispose-PairedAcceptedWorker {
    param(
        [object]$Worker
    )

    if ($null -eq $Worker -or $Worker.Disposed) {
        return
    }
    try {
        if ($null -ne $Worker.AsyncResult -and
            -not $Worker.AsyncResult.IsCompleted) {
            try {
                [void]$Worker.PowerShell.Stop()
            }
            catch {
            }
            [void]$Worker.AsyncResult.AsyncWaitHandle.WaitOne(1000)
        }
    }
    finally {
        try {
            $Worker.PowerShell.Dispose()
        }
        catch {
        }
        try {
            $Worker.Runspace.Dispose()
        }
        catch {
        }
        $Worker.Disposed = $true
    }
}

function Complete-PairedAcceptedWorker {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Worker
    )

    try {
        $output = @($Worker.PowerShell.EndInvoke($Worker.AsyncResult))
        if ($Worker.PowerShell.Streams.Error.Count -gt 0) {
            throw $Worker.PowerShell.Streams.Error[0]
        }
        if ($output.Count -ne 1) {
            throw 'A paired GUI worker did not return exactly one accepted response.'
        }
        return $output[0]
    }
    finally {
        Dispose-PairedAcceptedWorker -Worker $Worker
    }
}

function Invoke-PairedGuiCommands {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Left,
        [Parameter(Mandatory = $true)]
        [string]$LeftCommand,
        [object]$LeftParameters,
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Right,
        [Parameter(Mandatory = $true)]
        [string]$RightCommand,
        [object]$RightParameters
    )

    $leftRequestId = New-PipeRequestId -Instance $Left
    $rightRequestId = New-PipeRequestId -Instance $Right
    $leftRequest = [ordered]@{
        type = 'request'
        request_id = $leftRequestId
        command = $LeftCommand
    }
    if ($null -ne $LeftParameters) {
        $leftRequest.params = $LeftParameters
    }
    $rightRequest = [ordered]@{
        type = 'request'
        request_id = $rightRequestId
        command = $RightCommand
    }
    if ($null -ne $RightParameters) {
        $rightRequest.params = $RightParameters
    }
    $readyBarrier = [System.Threading.Barrier]::new(2)
    $sentBarrier = [System.Threading.Barrier]::new(2)
    $leftWorker = $null
    $rightWorker = $null
    try {
        $deadline = Resolve-PipeDeadline
        $leftWorker = Start-PairedAcceptedWorker `
            -Instance $Left `
            -Request $leftRequest `
            -ReadyBarrier $readyBarrier `
            -SentBarrier $sentBarrier `
            -DeadlineUtc $deadline
        $rightWorker = Start-PairedAcceptedWorker `
            -Instance $Right `
            -Request $rightRequest `
            -ReadyBarrier $readyBarrier `
            -SentBarrier $sentBarrier `
            -DeadlineUtc $deadline

        foreach ($worker in @($leftWorker, $rightWorker)) {
            $remaining = Get-DeadlineRemainingMilliseconds `
                -DeadlineUtc $deadline
            if ($remaining -le 0 -or
                -not $worker.AsyncResult.AsyncWaitHandle.WaitOne($remaining)) {
                throw 'Timed out waiting for both paired GUI commands to be accepted.'
            }
        }

        $leftAccepted = Complete-PairedAcceptedWorker -Worker $leftWorker
        $leftWorker = $null
        $rightAccepted = Complete-PairedAcceptedWorker -Worker $rightWorker
        $rightWorker = $null
        Assert-True `
            -Condition (
                [string]$leftAccepted.RequestId -eq $leftRequestId -and
                [string]$rightAccepted.RequestId -eq $rightRequestId -and
                [string]$leftAccepted.Accepted.status -eq 'accepted' -and
                [string]$rightAccepted.Accepted.status -eq 'accepted') `
            -Message "The paired commands $LeftCommand/$RightCommand should both be accepted before either terminal is consumed."
        return [pscustomobject]@{
            Left = Receive-Terminal `
                -Instance $Left `
                -RequestId $leftRequestId `
                -DeadlineUtc $deadline
            Right = Receive-Terminal `
                -Instance $Right `
                -RequestId $rightRequestId `
                -DeadlineUtc $deadline
        }
    }
    finally {
        if ($null -ne $leftWorker -or $null -ne $rightWorker) {
            Dispose-Pipe -Pipe $Left.Pipe
            Dispose-Pipe -Pipe $Right.Pipe
        }
        Dispose-PairedAcceptedWorker -Worker $leftWorker
        Dispose-PairedAcceptedWorker -Worker $rightWorker
        $readyBarrier.Dispose()
        $sentBarrier.Dispose()
    }
}

function Connect-GuiInstance {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Instance,
        [DateTime]$DeadlineUtc = [DateTime]::MinValue
    )

    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.',
        $Instance.PipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::Asynchronous)
    $DeadlineUtc = Resolve-PipeDeadline `
        -RequestedDeadlineUtc $DeadlineUtc
    try {
        while ($true) {
            $remaining = Get-DeadlineRemainingMilliseconds `
                -DeadlineUtc $DeadlineUtc
            if ($remaining -le 0) {
                throw "GUI $($Instance.Name) automation pipe did not become available before the runner deadline."
            }
            try {
                $connectTimeout = [Math]::Min(500, $remaining)
                $pipe.Connect($connectTimeout)
                break
            }
            catch [TimeoutException] {
                if ($Instance.Process.HasExited) {
                    throw "GUI $($Instance.Name) exited before its automation pipe became available (exit $($Instance.Process.ExitCode))."
                }
            }
        }
        Assert-True `
            -Condition $pipe.IsConnected `
            -Message "GUI $($Instance.Name) automation pipe did not become available."
        $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
        $Instance.Pipe = $pipe
        $helloRequestId = New-PipeRequestId -Instance $Instance
        Send-PipeJson -Pipe $pipe -Message ([ordered]@{
                type = 'hello'
                request_id = $helloRequestId
                protocol_version = 1
                nonce = $Instance.Nonce
            }) -DeadlineUtc $DeadlineUtc
        $hello = Receive-PipeJson `
            -Pipe $pipe `
            -DeadlineUtc $DeadlineUtc
        Assert-True `
            -Condition (
                [string]$hello.type -eq 'hello' -and
                [string]$hello.status -eq 'completed' -and
                [int]$hello.protocol_version -eq 1) `
            -Message "GUI $($Instance.Name) did not complete the automation handshake."
    }
    catch {
        if ($Instance.Pipe -eq $pipe) {
            $Instance.Pipe = $null
        }
        Dispose-Pipe -Pipe $pipe
        throw
    }
}

function Start-GuiInstance {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$StateRoot,
        [Parameter(Mandatory = $true)]
        [string]$SourcePath
    )

    $instanceId = New-RandomHex -ByteCount 16
    $nonce = New-RandomHex -ByteCount 32
    $pipeName = '0238d5bf7b34bb99c006f9807537d31234ca2e3d.automation.v1.' + $instanceId
    $pipePath = '\\.\pipe\' + $pipeName
    $arguments = @(
        '--automation-pipe', (Quote-WindowsArgument -Value $pipePath),
        '--automation-nonce', (Quote-WindowsArgument -Value $nonce),
        '--automation-instance', (Quote-WindowsArgument -Value $instanceId),
        '--automation-state-root', (Quote-WindowsArgument -Value $StateRoot),
        '--automation-allow-persistent-labeling-outputs',
        (Quote-WindowsArgument -Value $SourcePath)) -join ' '

    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $AppPath
    $start.Arguments = $arguments
    $start.WorkingDirectory = Split-Path -Parent $AppPath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    foreach ($variable in @(
            'SPECFORGE_PROFILE',
            'SPECFORGE_PROFILE_DIR',
            'SPECFORGE_RUNTIME_RESOURCE_WORKLOAD',
            'SPECFORGE_RUNTIME_RESOURCE_STATE_DIR')) {
        [void]$start.Environment.Remove($variable)
    }
    if (-not [string]::IsNullOrWhiteSpace(
            [string]$script:RunnerTempRoot)) {
        $start.Environment['TEMP'] = $script:RunnerTempRoot
        $start.Environment['TMP'] = $script:RunnerTempRoot
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    Assert-True `
        -Condition $process.Start() `
        -Message "Could not start GUI instance $Name."
    return [pscustomobject]@{
        Name = $Name
        Process = $process
        Pipe = $null
        PipeName = $pipeName
        InstanceId = $instanceId
        Nonce = $nonce
        NextRequest = 1
    }
}

function Get-DeadlineRemainingMilliseconds {
    param(
        [Parameter(Mandatory = $true)]
        [DateTime]$DeadlineUtc
    )

    $remaining = (
        $DeadlineUtc.ToUniversalTime() - [DateTime]::UtcNow).TotalMilliseconds
    if ($remaining -le 0) {
        return 0
    }
    return [int][Math]::Ceiling(
        [Math]::Min(
            [double][int]::MaxValue,
            $remaining))
}

function Start-FixtureCleanupHelper {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $powerShellPath = (
        Get-Command powershell.exe -CommandType Application |
            Select-Object -First 1).Source
    Assert-True `
        -Condition (-not [string]::IsNullOrWhiteSpace($powerShellPath)) `
        -Message 'Could not locate powershell.exe for bounded fixture cleanup.'
    $encodedCommand = [Convert]::ToBase64String(
        [System.Text.Encoding]::Unicode.GetBytes($Command))
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $powerShellPath
    $start.Arguments =
        '-NoProfile -ExecutionPolicy Bypass -EncodedCommand ' +
        $encodedCommand
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    Assert-True `
        -Condition $process.Start() `
        -Message 'Could not start the bounded fixture cleanup helper.'
    return $process
}

function Stop-ProcessWithinDeadline {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [DateTime]$DeadlineUtc,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $processId = $Process.Id
    $killError = $null
    $waitError = $null
    if ($Process.HasExited) {
        return
    }
    try {
        $Process.Kill()
    }
    catch {
        $killError = $_
    }
    $remaining = Get-DeadlineRemainingMilliseconds `
        -DeadlineUtc $DeadlineUtc
    if ($remaining -gt 0) {
        try {
            [void]$Process.WaitForExit(
                [Math]::Min(
                    $script:CleanupKillWaitMilliseconds,
                    $remaining))
        }
        catch {
            $waitError = $_
        }
    }
    $hasExited = $false
    try {
        $hasExited = $Process.HasExited
    }
    catch {
        $waitError = $_
    }
    if (-not $hasExited) {
        $details = [System.Collections.Generic.List[string]]::new()
        if ($null -ne $killError) {
            $details.Add(
                'Kill error: ' + $killError.Exception.Message)
        }
        if ($null -ne $waitError) {
            $details.Add(
                'Wait error: ' + $waitError.Exception.Message)
        }
        $detailText = if ($details.Count -eq 0) {
            ''
        }
        else {
            ' ' + ($details -join ' ')
        }
        throw (
            "$Description process PID $processId did not exit after " +
            "bounded Kill().$detailText")
    }
}

function Wait-FixtureCleanupHelper {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [DateTime]$DeadlineUtc,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $processId = $Process.Id
    $timedOut = $false
    $stopAttempted = $false
    try {
        $remaining = Get-DeadlineRemainingMilliseconds `
            -DeadlineUtc $DeadlineUtc
        if ($remaining -le 0 -or
            -not $Process.WaitForExit($remaining)) {
            $timedOut = $true
        }
        if ($timedOut -and -not $Process.HasExited) {
            $stopAttempted = $true
            Stop-ProcessWithinDeadline `
                -Process $Process `
                -DeadlineUtc $DeadlineUtc.AddMilliseconds(
                    $script:CleanupKillWaitMilliseconds) `
                -Description $Description
        }
        if (-not $Process.HasExited) {
            throw (
                "$Description helper process PID $processId did not " +
                'terminate after bounded Kill().')
        }
        if ($timedOut) {
            throw [TimeoutException]::new(
                "$Description helper process PID $processId exceeded " +
                'its cleanup deadline.')
        }
        Assert-True `
            -Condition ($Process.ExitCode -eq 0) `
            -Message (
                "$Description helper process PID $processId exited " +
                "with $($Process.ExitCode).")
    }
    finally {
        try {
            if (-not $Process.HasExited) {
                if (-not $stopAttempted) {
                    $stopAttempted = $true
                    Stop-ProcessWithinDeadline `
                        -Process $Process `
                        -DeadlineUtc ([DateTime]::UtcNow.AddMilliseconds(
                            $script:CleanupKillWaitMilliseconds)) `
                        -Description "$Description final cleanup"
                }
                if (-not $Process.HasExited) {
                    throw (
                        "$Description final cleanup process PID $processId " +
                        'still running after bounded stop.')
                }
            }
        }
        finally {
            $Process.Dispose()
        }
    }
}

function Remove-FixtureWithinDeadline {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,
        [Parameter(Mandatory = $true)]
        [DateTime]$DeadlineUtc
    )

    $literalRoot = "'" + $FixtureRoot.Replace("'", "''") + "'"
    $primaryCommand =
        '$ErrorActionPreference = "Stop"; Remove-Item -LiteralPath ' +
        $literalRoot +
        ' -Recurse -Force'
    $fallbackCommand =
        '$ErrorActionPreference = "Stop"; [System.IO.Directory]::Delete(' +
        $literalRoot +
        ', $true)'
    $primaryDeadline = $DeadlineUtc.AddMilliseconds(
        -($script:FixtureFallbackBudgetMilliseconds +
            $script:CleanupKillWaitMilliseconds))
    $fallbackDeadline = $DeadlineUtc.AddMilliseconds(
        -$script:CleanupKillWaitMilliseconds)
    $errors = [System.Collections.Generic.List[string]]::new()

    if ((Get-DeadlineRemainingMilliseconds -DeadlineUtc $primaryDeadline) -gt 0) {
        try {
            $primary = Start-FixtureCleanupHelper -Command $primaryCommand
            Wait-FixtureCleanupHelper `
                -Process $primary `
                -DeadlineUtc $primaryDeadline `
                -Description 'Remove-Item fixture cleanup'
        }
        catch {
            $errors.Add($_.Exception.Message)
        }
    }
    else {
        $errors.Add('Primary fixture cleanup had no remaining deadline budget.')
    }

    if (-not (Test-Path -LiteralPath $FixtureRoot)) {
        return
    }

    if ((Get-DeadlineRemainingMilliseconds -DeadlineUtc $fallbackDeadline) -gt 0) {
        try {
            $fallback = Start-FixtureCleanupHelper -Command $fallbackCommand
            Wait-FixtureCleanupHelper `
                -Process $fallback `
                -DeadlineUtc $fallbackDeadline `
                -Description 'Directory.Delete fixture fallback'
        }
        catch {
            $errors.Add($_.Exception.Message)
        }
    }
    else {
        $errors.Add('Fixture cleanup fallback had no remaining deadline budget.')
    }

    if (Test-Path -LiteralPath $FixtureRoot) {
        $details = if ($errors.Count -eq 0) {
            'no helper error was reported'
        }
        else {
            $errors -join ' '
        }
        throw "Fixture root remains after bounded cleanup: $FixtureRoot ($details)"
    }
}

function Remove-AliasLockDirectoriesWithinDeadline {
    param(
        [Parameter(Mandatory = $true)]
        [string]$AliasRoot,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Directories,
        [Parameter(Mandatory = $true)]
        [DateTime]$DeadlineUtc
    )

    $ownedDirectories = @($Directories | Where-Object {
            $candidate = [System.IO.Path]::GetFullPath($_)
            $root = [System.IO.Path]::GetFullPath($AliasRoot)
            $candidate.StartsWith(
                $root + [System.IO.Path]::DirectorySeparatorChar,
                [System.StringComparison]::OrdinalIgnoreCase) -and
            [System.IO.Path]::GetDirectoryName($candidate).Equals(
                $root,
                [System.StringComparison]::OrdinalIgnoreCase)
        })
    if ($ownedDirectories.Count -eq 0) {
        return
    }

    $literalDirectories = ($ownedDirectories | ForEach-Object {
            "'" + ([string]$_).Replace("'", "''") + "'"
        }) -join ', '
    $deleteCommand =
        '$ErrorActionPreference = "Stop"; $paths = @(' +
        $literalDirectories +
        '); foreach ($path in $paths) { if (Test-Path -LiteralPath $path -PathType Container) { [System.IO.Directory]::Delete($path, $true) } }'
    $errors = [System.Collections.Generic.List[string]]::new()
    if ((Get-DeadlineRemainingMilliseconds -DeadlineUtc $DeadlineUtc) -gt 0) {
        try {
            $helper = Start-FixtureCleanupHelper -Command $deleteCommand
            Wait-FixtureCleanupHelper `
                -Process $helper `
                -DeadlineUtc $DeadlineUtc `
                -Description 'Alias lock directory cleanup'
        }
        catch {
            $errors.Add($_.Exception.Message)
        }
    }
    else {
        $errors.Add('Alias lock directory cleanup had no remaining deadline budget.')
    }

    $remainingDirectories = @($ownedDirectories | Where-Object {
            Test-Path -LiteralPath $_ -PathType Container
        })
    if ($remainingDirectories.Count -gt 0) {
        $details = if ($errors.Count -eq 0) {
            'no helper error was reported'
        }
        else {
            $errors -join ' '
        }
        throw (
            'Owned alias lock directories remain after bounded cleanup: ' +
            (($remainingDirectories -join ', ')) +
            " ($details)")
    }
}

function Stop-OwnedGuiInstance {
    param(
        [pscustomobject]$Instance,
        [Parameter(Mandatory = $true)]
        [DateTime]$CleanupDeadline
    )

    if ($null -eq $Instance) {
        return
    }
    try {
        if ($null -ne $Instance.Process -and -not $Instance.Process.HasExited) {
            $pipeConnected = $false
            if ($null -ne $Instance.Pipe) {
                try {
                    $pipeConnected = $Instance.Pipe.IsConnected
                }
                catch {
                    $pipeConnected = $false
                }
            }
            if ($pipeConnected -and
                (Get-DeadlineRemainingMilliseconds `
                    -DeadlineUtc $CleanupDeadline) -gt 0) {
                try {
                    [void](Invoke-GuiCommand `
                        -Instance $Instance `
                        -Command 'app.quit' `
                        -DeadlineUtc $CleanupDeadline)
                }
                catch {
                }
            }

            # Close the pipe before any process wait so a stalled GUI cannot
            # keep graceful shutdown or cleanup blocked behind pipe I/O.
            Dispose-Pipe -Pipe $Instance.Pipe
            $Instance.Pipe = $null
            if (-not $Instance.Process.HasExited) {
                $remaining = Get-DeadlineRemainingMilliseconds `
                    -DeadlineUtc $CleanupDeadline
                if ($remaining -gt 0) {
                    [void]$Instance.Process.WaitForExit($remaining)
                }
            }
            if (-not $Instance.Process.HasExited) {
                Stop-ProcessWithinDeadline `
                    -Process $Instance.Process `
                    -DeadlineUtc $CleanupDeadline.AddMilliseconds(
                        $script:CleanupKillWaitMilliseconds) `
                    -Description "Owned GUI instance $($Instance.Name)"
            }
        }
    }
    finally {
        Dispose-Pipe -Pipe $Instance.Pipe
        $Instance.Pipe = $null
        if ($null -ne $Instance.Process) {
            try {
                $Instance.Process.Dispose()
            }
            catch {
            }
        }
    }
}

function Get-PortableApp {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ResolvedExecutable,
        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    [void][System.IO.Directory]::CreateDirectory($Destination)
    $destinationExecutable = Join-Path $Destination 'Spectiary.exe'
    Copy-Item -LiteralPath $ResolvedExecutable -Destination $destinationExecutable
    $buildRoot = Split-Path -Parent $ResolvedExecutable
    foreach ($runtimeFile in @(
            'cfitsio.dll',
            'pthreadVC3d.dll',
            'zd.dll',
            'yaml-cppd.dll')) {
        $source = Join-Path $buildRoot $runtimeFile
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $Destination $runtimeFile)
        }
    }
    $runtimeConfig = Join-Path $buildRoot 'config'
    if (Test-Path -LiteralPath $runtimeConfig -PathType Container) {
        Copy-Item -LiteralPath $runtimeConfig -Destination (Join-Path $Destination 'config') -Recurse
    }
    $metadata = Get-Content -Raw -LiteralPath (Join-Path $buildRoot 'spectiary_metadata.json') |
        ConvertFrom-Json
    [void]$metadata.PSObject.Properties.Remove('deployment')
    $metadata | Add-Member -NotePropertyName deployment -NotePropertyValue ([pscustomobject][ordered]@{
            distribution = 'portable'
            storage_profile = 'portable'
        }) -Force
    Write-Utf8File `
        -Path (Join-Path $Destination 'spectiary_metadata.json') `
        -Contents ($metadata | ConvertTo-Json -Depth 10)
    [void][System.IO.Directory]::CreateDirectory((Join-Path $Destination 'Data'))
    return $destinationExecutable
}

function Invoke-StateFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $deadline = Resolve-PipeDeadline
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $script:ResolvedStateFixture
    $start.Arguments = (@(
            $Arguments | ForEach-Object {
                Quote-WindowsArgument -Value ([string]$_)
            }) -join ' ')
    $start.WorkingDirectory = Split-Path -Parent $script:ResolvedStateFixture
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardError = $true
    if (-not [string]::IsNullOrWhiteSpace(
            [string]$script:RunnerTempRoot)) {
        $start.Environment['TEMP'] = $script:RunnerTempRoot
        $start.Environment['TMP'] = $script:RunnerTempRoot
    }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    $errorRead = $null
    try {
        Assert-True `
            -Condition $process.Start() `
            -Message 'Could not start the labeling state fixture.'
        $errorRead = $process.StandardError.ReadToEndAsync()
        Wait-FixtureCleanupHelper `
            -Process $process `
            -DeadlineUtc $deadline `
            -Description ("State fixture: " + ($Arguments -join ' '))
    }
    catch {
        $detail = if ($null -ne $errorRead -and $errorRead.IsCompleted) {
            $errorRead.GetAwaiter().GetResult()
        } else { '' }
        throw ("{0} {1}" -f $_.Exception.Message, $detail)
    }
    finally {
        $process.Dispose()
    }
}

function Set-OwnedAliasLockDirectoriesFromManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ManifestPath,
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,
        [Parameter(Mandatory = $true)]
        [string]$AliasRoot
    )

    $fixtureRootFull = [System.IO.Path]::GetFullPath($FixtureRoot)
    $aliasRootFull = [System.IO.Path]::GetFullPath($AliasRoot)
    $fixturePrefix = $fixtureRootFull + [System.IO.Path]::DirectorySeparatorChar
    $ownedAliasDirectories =
        [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
    $manifestDirectories = @(Get-Content -LiteralPath $ManifestPath -Encoding UTF8)
    Assert-True `
        -Condition ($manifestDirectories.Count -gt 0) `
        -Message 'The labeling coordination manifest should contain at least one directory.'
    foreach ($manifestDirectory in $manifestDirectories) {
        if ([string]::IsNullOrWhiteSpace([string]$manifestDirectory)) {
            continue
        }
        $candidate = [System.IO.Path]::GetFullPath(
            ([string]$manifestDirectory).Trim())
        $candidateParent = [System.IO.Path]::GetDirectoryName($candidate)
        if ($candidateParent.Equals(
                $aliasRootFull,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            [void]$ownedAliasDirectories.Add($candidate)
            continue
        }
        Assert-True `
            -Condition $candidate.StartsWith(
                $fixturePrefix,
                [System.StringComparison]::OrdinalIgnoreCase) `
            -Message (
                'The labeling coordination manifest contains an unexpected external directory: ' +
                $candidate)
    }
    $script:OwnedAliasLockDirectories = @($ownedAliasDirectories)
    $script:AliasOwnershipManifestReady = $true
}

function Get-SourceEntry {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Cache,
        [Parameter(Mandatory = $true)]
        [string]$Identity
    )

    foreach ($entry in @($Cache.sources)) {
        if ([string]$entry.identity -eq $Identity) {
            return $entry
        }
    }
    throw "The final cache did not contain source identity $Identity."
}

function Invoke-SourceOpenAndAssert {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Instance,
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedSourcePath,
        [Parameter(Mandatory = $true)]
        [string]$Description,
        [switch]$RequireActiveTask
    )

    $terminal = Invoke-GuiCommand `
        -Instance $Instance `
        -Command 'source.open' `
        -Parameters ([ordered]@{ path = $SourcePath })
    Assert-True `
        -Condition ([string]$terminal.status -eq 'completed') `
        -Message "$Description should complete. Terminal=$($terminal | ConvertTo-Json -Compress -Depth 12)"
    Assert-True `
        -Condition (
            $null -ne $terminal.result.source -and
            [string]$terminal.result.source.id -ne '' -and
            [System.IO.Path]::GetFullPath(
                [string]$terminal.result.source.path) -eq
                [System.IO.Path]::GetFullPath($ExpectedSourcePath)) `
        -Message (
            "$Description should activate the expected source path $ExpectedSourcePath. " +
            "Terminal=$($terminal | ConvertTo-Json -Compress -Depth 12)")
    [void](Invoke-GuiCommand -Instance $Instance -Command 'wait.idle')
    $state = Invoke-GuiCommand -Instance $Instance -Command 'state.get'
    Assert-True `
        -Condition (
            [System.IO.Path]::GetFullPath(
                [string]$state.state.source.path) -eq
                [System.IO.Path]::GetFullPath($ExpectedSourcePath)) `
        -Message (
            "$Description should leave the expected source path active in the GUI. " +
            "State=$($state | ConvertTo-Json -Compress -Depth 12)")
    if ($RequireActiveTask) {
        Assert-True `
            -Condition (
                [bool]$state.state.labeling.has_active_task -and
                [string]$state.state.labeling.active_task.id -eq
                    $script:LabelingTaskId) `
            -Message (
                "$Description should retain the seeded quality task in the GUI cache. " +
                "State=$($state | ConvertTo-Json -Compress -Depth 12)")
    }
    return $terminal
}

function Invoke-LabelVerification {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [int]$Index,
        [Parameter(Mandatory = $true)]
        [int]$Code,
        [Parameter(Mandatory = $true)]
        [string]$CachePath
    )

    Invoke-StateFixture -Arguments @(
        '--verify-labeling-state', $CachePath,
        '--source', $SourcePath,
        '--spectrum-index', [string]$Index,
        '--expected-code', [string]$Code)
}

function Invoke-StateFixtureEventually {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $lastError = $null
    do {
        try {
            Invoke-StateFixture -Arguments $Arguments
            return
        }
        catch {
            $lastError = $_.Exception.Message
            Start-Sleep -Milliseconds 100
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "$Description did not converge before the bounded retry deadline. Last error: $lastError"
}

function Invoke-LabelVerificationEventually {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [int]$Index,
        [Parameter(Mandatory = $true)]
        [int]$Code,
        [Parameter(Mandatory = $true)]
        [string]$CachePath
    )

    Invoke-StateFixtureEventually `
        -Arguments @(
            '--verify-labeling-state', $CachePath,
            '--source', $SourcePath,
            '--spectrum-index', [string]$Index,
            '--expected-code', [string]$Code) `
        -Description "labeling state for $SourcePath index $Index"
}

function Assert-OutputSaveSucceeded {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Terminal,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Assert-True `
        -Condition ([string]$Terminal.status -eq 'completed') `
        -Message "$Description should complete. Terminal=$($Terminal | ConvertTo-Json -Compress -Depth 12)"
    $persistence = $Terminal.result.persistence
    Assert-True `
        -Condition (
            $null -ne $persistence -and
            [bool]$persistence.output_save_attempted -and
            [bool]$persistence.output_saved) `
        -Message "$Description should report output_save_attempted=true and output_saved=true. Terminal=$($Terminal | ConvertTo-Json -Compress -Depth 12)"
}

function Assert-OutputSaveAccepted {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Terminal,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Assert-True `
        -Condition ([string]$Terminal.status -eq 'completed') `
        -Message "$Description should complete. Terminal=$($Terminal | ConvertTo-Json -Compress -Depth 12)"
    $persistence = $Terminal.result.persistence
    $output_saved =
        $null -ne $persistence -and
        [bool]$persistence.output_save_attempted -and
        [bool]$persistence.output_saved
    $output_pending =
        $null -ne $persistence -and
        [bool]$persistence.state_save_scheduled -and
        [bool]$persistence.output_retry_scheduled -and
        -not [bool]$persistence.output_save_attempted
    Assert-True `
        -Condition ($output_saved -or $output_pending) `
        -Message (
            "$Description should either save output or report persistence pending/retrying. " +
            "Terminal=$($Terminal | ConvertTo-Json -Compress -Depth 12)")
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$script:ResolvedStateFixture = (Resolve-Path -LiteralPath $StateFixture).Path
$fixtureParent = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-multi-instance-labeling-' + [Guid]::NewGuid().ToString('N'))
$sharedStateRoot = Join-Path $fixtureParent 'shared-state'
$sourcesRoot = Join-Path $fixtureParent 'sources'
$portableARoot = Join-Path $fixtureParent 'portable-a'
$portableBRoot = Join-Path $fixtureParent 'portable-b'
$cachePath = Join-Path $sharedStateRoot 'sample-labeling-tasks.json'
$coordinationManifestPath = Join-Path `
    $fixtureParent `
    'labeling-coordination-directories.txt'
$runnerTempRoot = Join-Path $fixtureParent 'runner-temp'
$script:RunnerTempRoot = $runnerTempRoot
$script:AliasLockRoot = Join-Path `
    $runnerTempRoot `
    'SpecForge\sample-labeling-cache-locks'
$instanceA = $null
$instanceB = $null
$failure = $null
$cleanupErrors = [System.Collections.Generic.List[string]]::new()
$script:CurrentStep = 'startup'

try {
    $script:CurrentStep = 'create isolated source fixtures and labeling seeds'
    foreach ($directory in @(
            $fixtureParent,
            $sharedStateRoot,
            $sourcesRoot,
            $script:RunnerTempRoot)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    [void][System.IO.Directory]::CreateDirectory((Join-Path $sharedStateRoot 'outputs'))

    $definitions = @(
        [pscustomobject]@{
            Name = 'formal-a'
            Output = Join-Path $sharedStateRoot 'outputs\formal-a.asdf'
        },
        [pscustomobject]@{
            Name = 'formal-b'
            Output = Join-Path $sharedStateRoot 'outputs\formal-b.asdf'
        },
        [pscustomobject]@{
            Name = 'temporary-a'
            Output = $null
        },
        [pscustomobject]@{
            Name = 'temporary-b'
            Output = $null
        },
        [pscustomobject]@{
            Name = 'deleted'
            Output = $null
        }
    )
    $fixtures = [ordered]@{}
    $seedSources = @()
    foreach ($definition in $definitions) {
        $sourcePath = Join-Path $sourcesRoot $definition.Name
        [void][System.IO.Directory]::CreateDirectory($sourcePath)
        foreach ($spectrumName in @('alpha.csv', 'bravo.csv', 'charlie.csv')) {
            Write-Utf8File `
                -Path (Join-Path $sourcePath $spectrumName) `
                -Contents "wavelength,flux`n5000,1`n5001,2`n"
        }
        $seedPath = Join-Path $fixtureParent ($definition.Name + '-seed.json')
        $seedArguments = @(
            '--write-labeling-seed', $seedPath,
            '--source', $sourcePath)
        if ($null -ne $definition.Output) {
            $seedArguments += @(
                '--output-path', [string]$definition.Output,
                '--materialize-labeling-output')
        }
        Invoke-StateFixture -Arguments $seedArguments
        $seed = Get-Content -Raw -LiteralPath $seedPath | ConvertFrom-Json
        $sourceEntry = @($seed.sources)[0]
        $fixtures[$definition.Name] = [pscustomobject]@{
            Name = $definition.Name
            SourcePath = $sourcePath
            Identity = [string]$sourceEntry.identity
            Output = $definition.Output
        }
        $seedSources += @($seed.sources)
    }
    $cacheDocument = [ordered]@{
        format_kind = 'specforge.sample_labeling_tasks.cache'
        schema_version = 4
        sources = @($seedSources)
    }
    Write-Utf8File `
        -Path $cachePath `
        -Contents ($cacheDocument | ConvertTo-Json -Depth 20)
    $cachePath = Join-Path $sharedStateRoot 'state\sample-labeling-state.json'
    $script:CurrentStep = 'record exact labeling coordination directories'
    Invoke-StateFixture -Arguments @(
        '--write-labeling-coordination-directories', $cachePath,
        '--manifest-path', $coordinationManifestPath)
    Set-OwnedAliasLockDirectoriesFromManifest `
        -ManifestPath $coordinationManifestPath `
        -FixtureRoot $fixtureParent `
        -AliasRoot $script:AliasLockRoot
    $script:CurrentStep = 'start and handshake both GUI instances'
    Write-Host 'smoke: fixtures ready'

    $portableA = Get-PortableApp `
        -ResolvedExecutable $resolvedExecutable `
        -Destination $portableARoot
    $portableB = Get-PortableApp `
        -ResolvedExecutable $resolvedExecutable `
        -Destination $portableBRoot

    $instanceA = Start-GuiInstance `
        -Name 'A' `
        -AppPath $portableA `
        -StateRoot $sharedStateRoot `
        -SourcePath $fixtures['formal-a'].SourcePath
    $instanceB = Start-GuiInstance `
        -Name 'B' `
        -AppPath $portableB `
        -StateRoot $sharedStateRoot `
        -SourcePath $fixtures['formal-b'].SourcePath
    Write-Host 'smoke: GUI processes started'
    Connect-GuiInstance -Instance $instanceA
    Connect-GuiInstance -Instance $instanceB
    Write-Host 'smoke: GUI pipes connected'

    $script:CurrentStep = 'wait for both initial source loads and inspect state'
    $stateA = Invoke-GuiCommand -Instance $instanceA -Command 'wait.idle'
    $stateB = Invoke-GuiCommand -Instance $instanceB -Command 'wait.idle'
    $stateA = Invoke-GuiCommand -Instance $instanceA -Command 'state.get'
    $stateB = Invoke-GuiCommand -Instance $instanceB -Command 'state.get'
    Assert-True `
        -Condition (
            [bool]$stateA.state.window.visible -and
            -not [bool]$stateA.state.window.minimized -and
            [bool]$stateB.state.window.visible -and
            -not [bool]$stateB.state.window.minimized -and
            [bool]$stateA.state.labeling.has_active_task -and
            [bool]$stateB.state.labeling.has_active_task) `
        -Message (
            'Both ordinary GUI instances should be visible and start with their isolated formal labeling task active. ' +
            'A=' + ($stateA | ConvertTo-Json -Compress -Depth 12) +
            ' B=' + ($stateB | ConvertTo-Json -Compress -Depth 12))
    Write-Host 'smoke: initial states ready'

    Write-Host 'smoke: initial formal labels'

    $script:CurrentStep = 'edit different formal targets concurrently'
    $paired = Invoke-PairedGuiCommands `
        -Left $instanceA `
        -LeftCommand 'label.assign' `
        -LeftParameters ([ordered]@{ code = 5; target = [ordered]@{ index = 0 } }) `
        -Right $instanceB `
        -RightCommand 'label.assign' `
        -RightParameters ([ordered]@{ code = 7; target = [ordered]@{ index = 0 } })
    Assert-True `
        -Condition ([string]$paired.Left.status -eq 'completed' -and [string]$paired.Right.status -eq 'completed') `
        -Message 'Different formal targets should accept concurrent production label assignments.'
    Assert-OutputSaveAccepted `
        -Terminal $paired.Left `
        -Description 'formal-a spectrum 0 label assignment'
    Assert-OutputSaveAccepted `
        -Terminal $paired.Right `
        -Description 'formal-b spectrum 0 label assignment'
    [void](Invoke-GuiCommand -Instance $instanceA -Command 'wait.idle')
    [void](Invoke-GuiCommand -Instance $instanceB -Command 'wait.idle')
    # wait.idle is a command/loader barrier, not the labeling autosave timer.
    # Observe durable output while each GUI still owns its active formal task.
    Invoke-StateFixtureEventually `
        -Description 'initial formal-a autosave before source switch' `
        -Arguments @('--verify-label-output', $fixtures['formal-a'].Output,
            '--source', $fixtures['formal-a'].SourcePath,
            '--expected-values', '5,-1,-1', '--task-id', $script:LabelingTaskId)
    Invoke-StateFixtureEventually `
        -Description 'initial formal-b autosave before source switch' `
        -Arguments @('--verify-label-output', $fixtures['formal-b'].Output,
            '--source', $fixtures['formal-b'].SourcePath,
            '--expected-values', '7,-1,-1', '--task-id', $script:LabelingTaskId)
    Write-Host 'smoke: temporary labels'

    $script:CurrentStep = 'edit different temporary targets concurrently'
    $paired = Invoke-PairedGuiCommands `
        -Left $instanceA `
        -LeftCommand 'source.open' `
        -LeftParameters ([ordered]@{ path = $fixtures['temporary-a'].SourcePath }) `
        -Right $instanceB `
        -RightCommand 'source.open' `
        -RightParameters ([ordered]@{ path = $fixtures['temporary-b'].SourcePath })
    Assert-True `
        -Condition ([string]$paired.Left.status -eq 'completed' -and [string]$paired.Right.status -eq 'completed') `
        -Message 'Different temporary targets should open concurrently.'
    [void](Invoke-GuiCommand -Instance $instanceA -Command 'wait.idle')
    [void](Invoke-GuiCommand -Instance $instanceB -Command 'wait.idle')
    $paired = Invoke-PairedGuiCommands `
        -Left $instanceA `
        -LeftCommand 'label.assign' `
        -LeftParameters ([ordered]@{ code = 7; target = [ordered]@{ index = 0 } }) `
        -Right $instanceB `
        -RightCommand 'label.assign' `
        -RightParameters ([ordered]@{ code = 5; target = [ordered]@{ index = 0 } })
    Assert-True `
        -Condition ([string]$paired.Left.status -eq 'completed' -and [string]$paired.Right.status -eq 'completed') `
        -Message 'Different temporary targets should preserve both task-cache edits.'

    $script:CurrentStep = 'open the same formal target and verify read-only conflict'
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceA `
        -SourcePath $fixtures['formal-a'].SourcePath `
        -ExpectedSourcePath $fixtures['formal-a'].SourcePath `
        -Description 'GUI A formal-a source.open')
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceB `
        -SourcePath $fixtures['formal-a'].SourcePath `
        -ExpectedSourcePath $fixtures['formal-a'].SourcePath `
        -Description 'GUI B formal-a source.open')
    $conflictState = Invoke-GuiCommand -Instance $instanceB -Command 'state.get'
    Assert-True `
        -Condition (-not [bool]$conflictState.state.labeling.has_active_task) `
        -Message 'A second instance opening the same formal target should expose a read-only labeling projection.'
    Assert-True `
        -Condition (
            @($conflictState.state.labeling.task_ids) -contains
                $script:LabelingTaskId) `
        -Message (
            'A read-only conflicting instance should retain the quality task projection. ' +
            ($conflictState | ConvertTo-Json -Compress -Depth 12))
    $conflictLabel = Invoke-GuiCommand `
        -Instance $instanceB `
        -Command 'label.assign' `
        -Parameters ([ordered]@{ code = 7; target = [ordered]@{ index = 1 } })
    Assert-True `
        -Condition (
            [string]$conflictLabel.status -eq 'failed' -and
            [string]$conflictLabel.error.code -eq 'no_active_label_task') `
        -Message 'A read-only conflicting instance should reject a label assignment with no_active_label_task.'

    $script:CurrentStep = 'load the deleted source into both GUI caches before tombstone'
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceA `
        -SourcePath $fixtures['deleted'].SourcePath `
        -ExpectedSourcePath $fixtures['deleted'].SourcePath `
        -Description 'GUI A deleted source.open before tombstone' `
        -RequireActiveTask)
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceA `
        -SourcePath $fixtures['formal-a'].SourcePath `
        -ExpectedSourcePath $fixtures['formal-a'].SourcePath `
        -Description 'GUI A formal-a source.open after stale deleted cache load')
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceB `
        -SourcePath $fixtures['deleted'].SourcePath `
        -ExpectedSourcePath $fixtures['deleted'].SourcePath `
        -Description 'GUI B deleted source.open before tombstone' `
        -RequireActiveTask)
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceB `
        -SourcePath $fixtures['formal-b'].SourcePath `
        -ExpectedSourcePath $fixtures['formal-b'].SourcePath `
        -Description 'GUI B formal-b source.open after stale deleted cache load')

    Write-Host 'smoke: tombstone helper'
    $script:CurrentStep = 'commit explicit selection and deletion tombstone after both GUI stale-cache loads'
    Invoke-StateFixture -Arguments @(
        '--exercise-labeling-delete', $cachePath,
        '--source', $fixtures['deleted'].SourcePath,
        '--task-id', $script:LabelingTaskId)

    $script:CurrentStep = 'reopen deleted source from both stale GUI caches after tombstone'
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceA `
        -SourcePath $fixtures['deleted'].SourcePath `
        -ExpectedSourcePath $fixtures['deleted'].SourcePath `
        -Description 'GUI A deleted source.open after tombstone')
    $staleDeletedStateA = Invoke-GuiCommand -Instance $instanceA -Command 'state.get'
    Assert-True `
        -Condition (-not [bool]$staleDeletedStateA.state.labeling.has_active_task) `
        -Message (
            'GUI A should not restore the deleted task from its stale source cache. ' +
            ($staleDeletedStateA | ConvertTo-Json -Compress -Depth 12))
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceB `
        -SourcePath $fixtures['deleted'].SourcePath `
        -ExpectedSourcePath $fixtures['deleted'].SourcePath `
        -Description 'GUI B deleted source.open after tombstone')
    $staleDeletedStateB = Invoke-GuiCommand -Instance $instanceB -Command 'state.get'
    Assert-True `
        -Condition (-not [bool]$staleDeletedStateB.state.labeling.has_active_task) `
        -Message (
            'GUI B should not restore the deleted task from its stale source cache. ' +
            ($staleDeletedStateB | ConvertTo-Json -Compress -Depth 12))
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceA `
        -SourcePath $fixtures['formal-a'].SourcePath `
        -ExpectedSourcePath $fixtures['formal-a'].SourcePath `
        -Description 'GUI A formal-a source.open after tombstone recovery')
    [void](Invoke-SourceOpenAndAssert `
        -Instance $instanceB `
        -SourcePath $fixtures['formal-b'].SourcePath `
        -ExpectedSourcePath $fixtures['formal-b'].SourcePath `
        -Description 'GUI B formal-b source.open after tombstone recovery')

    Write-Host 'smoke: terminate lease owner'
    $script:CurrentStep = 'terminate the owned lease owner process'
    Assert-True `
        -Condition (-not $instanceA.Process.HasExited) `
        -Message 'The first GUI should still own the formal lease before the takeover step.'
    $remaining = Get-DeadlineRemainingMilliseconds `
        -DeadlineUtc $script:RunnerMainDeadlineUtc
    $instanceA.Process.Kill()
    Assert-True `
        -Condition ($remaining -gt 0 -and $instanceA.Process.WaitForExit($remaining)) `
        -Message 'The first GUI process should terminate when its owned lease owner is killed.'

    Write-Host 'smoke: remaining GUI open formal-b'
    $script:CurrentStep = 'reopen another source before lease takeover'
    [void](Invoke-GuiCommand -Instance $instanceB -Command 'source.open' -Parameters ([ordered]@{ path = $fixtures['formal-b'].SourcePath }))
    Write-Host 'smoke: remaining GUI wait formal-b'
    [void](Invoke-GuiCommand -Instance $instanceB -Command 'wait.idle')
    Write-Host 'smoke: remaining GUI open formal-a'
    $script:CurrentStep = 'retry recovery and reacquire formal target lease'
    [void](Invoke-GuiCommand -Instance $instanceB -Command 'source.open' -Parameters ([ordered]@{ path = $fixtures['formal-a'].SourcePath }))
    Write-Host 'smoke: remaining GUI wait formal-a'
    [void](Invoke-GuiCommand -Instance $instanceB -Command 'wait.idle')
    Write-Host 'smoke: remaining GUI state'
    $takeoverState = Invoke-GuiCommand -Instance $instanceB -Command 'state.get'
    $takeoverCache = Get-Content -Raw -LiteralPath $cachePath | ConvertFrom-Json
    $takeoverFormalA = Get-SourceEntry `
        -Cache $takeoverCache `
        -Identity $fixtures['formal-a'].Identity
    Assert-True `
        -Condition ([bool]$takeoverState.state.labeling.has_active_task) `
        -Message (
            'The remaining GUI should reacquire the formal target after the lease owner terminates. ' +
            ($takeoverState | ConvertTo-Json -Compress -Depth 12) +
            ' CacheFormalA=' +
            ($takeoverFormalA | ConvertTo-Json -Compress -Depth 12))
    Write-Host 'smoke: remaining GUI label'
    $script:CurrentStep = 'edit the recovered formal target'
    $takeoverLabel = Invoke-GuiCommand `
        -Instance $instanceB `
        -Command 'label.assign' `
        -Parameters ([ordered]@{ code = 7; target = [ordered]@{ index = 1 } })
    Assert-True `
        -Condition ([string]$takeoverLabel.status -eq 'completed') `
        -Message 'The remaining GUI should edit the recovered formal task.'
    Assert-OutputSaveSucceeded `
        -Terminal $takeoverLabel `
        -Description 'formal-a recovered spectrum 1 label assignment'
    Write-Host 'smoke: remaining GUI wait after label'
    [void](Invoke-GuiCommand -Instance $instanceB -Command 'wait.idle')

    $script:CurrentStep = 'verify final task values, active selections, outputs, and tombstone'
    Invoke-LabelVerificationEventually `
        -SourcePath $fixtures['formal-a'].SourcePath `
        -Index 0 `
        -Code 5 `
        -CachePath $cachePath
    Invoke-LabelVerificationEventually `
        -SourcePath $fixtures['formal-a'].SourcePath `
        -Index 1 `
        -Code 7 `
        -CachePath $cachePath
    Invoke-LabelVerificationEventually `
        -SourcePath $fixtures['formal-b'].SourcePath `
        -Index 0 `
        -Code 7 `
        -CachePath $cachePath
    Invoke-LabelVerificationEventually `
        -SourcePath $fixtures['temporary-a'].SourcePath `
        -Index 0 `
        -Code 7 `
        -CachePath $cachePath
    Invoke-LabelVerificationEventually `
        -SourcePath $fixtures['temporary-b'].SourcePath `
        -Index 0 `
        -Code 5 `
        -CachePath $cachePath

    $finalCache = Get-Content -Raw -LiteralPath $cachePath | ConvertFrom-Json
    foreach ($name in @('formal-a', 'formal-b', 'temporary-a', 'temporary-b')) {
        $entry = Get-SourceEntry -Cache $finalCache -Identity $fixtures[$name].Identity
        Assert-True `
            -Condition (
                [string]$entry.active_task_id -eq
                    $script:LabelingTaskId) `
            -Message "Source $name should retain its explicit active task selection."
    }
    $deletedEntry = Get-SourceEntry -Cache $finalCache -Identity $fixtures['deleted'].Identity
    Assert-True `
        -Condition (
            [string]$deletedEntry.active_task_id -eq '' -and
            @($deletedEntry.tasks).Count -eq 0) `
        -Message 'The deleted task should remain tombstoned after a stale GUI cache patch.'
    Invoke-StateFixtureEventually `
        -Description 'formal-a durable label output' `
        -Arguments @(
        '--verify-label-output',
        (Join-Path $sharedStateRoot 'outputs\formal-a.asdf'),
        '--source', $fixtures['formal-a'].SourcePath,
        '--expected-values', '5,7,-1',
        '--task-id', $script:LabelingTaskId)
    Invoke-StateFixtureEventually `
        -Description 'formal-b durable label output' `
        -Arguments @(
        '--verify-label-output',
        (Join-Path $sharedStateRoot 'outputs\formal-b.asdf'),
        '--source', $fixtures['formal-b'].SourcePath,
        '--expected-values', '7,-1,-1',
        '--task-id', $script:LabelingTaskId)

    $script:CurrentStep = 'quit the remaining owned GUI process'
    $quit = $null
    $quitPipeClosedAfterCleanExit = $false
    try {
        $quit = Invoke-GuiCommand -Instance $instanceB -Command 'app.quit'
    }
    catch {
        if (-not $instanceB.Process.HasExited) {
            throw
        }
        $quitPipeClosedAfterCleanExit = $true
    }
    $remaining = Get-DeadlineRemainingMilliseconds `
        -DeadlineUtc $script:RunnerMainDeadlineUtc
    $processExited = $remaining -gt 0 -and
        $instanceB.Process.WaitForExit($remaining)
    $exitCode = if ($processExited) {
        $instanceB.Process.ExitCode
    }
    else {
        $null
    }
    Assert-True `
        -Condition ((
                $quitPipeClosedAfterCleanExit -or
                [string]$quit.status -eq 'completed') -and
            $processExited -and
            $exitCode -eq 0) `
        -Message (
            'The second GUI should quit cleanly after the smoke sequence. ' +
            'Quit=' + ($quit | ConvertTo-Json -Compress -Depth 12) +
            " PipeClosedAfterCleanExit=$quitPipeClosedAfterCleanExit ExitCode=$exitCode")
}
catch {
    $failure = $_
}
finally {
    $cleanupDeadline = [DateTime]::UtcNow.AddMilliseconds(
        $script:CleanupBudgetMilliseconds)
    foreach ($instance in @($instanceA, $instanceB)) {
        try {
            Stop-OwnedGuiInstance `
                -Instance $instance `
                -CleanupDeadline $cleanupDeadline
        }
        catch {
            $cleanupErrors.Add(
                "GUI $($instance.Name) cleanup failed: $($_.Exception.Message)")
        }
    }
    $fixtureCleanupDeadline = [DateTime]::UtcNow.AddMilliseconds(
        $script:FixtureCleanupBudgetMilliseconds)
    try {
        if ($script:AliasOwnershipManifestReady) {
            Remove-AliasLockDirectoriesWithinDeadline `
                -AliasRoot $script:AliasLockRoot `
                -Directories @($script:OwnedAliasLockDirectories) `
                -DeadlineUtc $fixtureCleanupDeadline
        }
    }
    catch {
        $cleanupErrors.Add(
            "Alias lock directory cleanup failed: $($_.Exception.Message)")
    }
    try {
        if ($null -ne $fixtureParent -and (Test-Path -LiteralPath $fixtureParent)) {
            $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
            $fixtureRoot = [System.IO.Path]::GetFullPath($fixtureParent)
            Assert-True `
                -Condition $fixtureRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) `
                -Message 'Refusing to remove a fixture outside the system temporary directory.'
            Remove-FixtureWithinDeadline `
                -FixtureRoot $fixtureRoot `
                -DeadlineUtc $fixtureCleanupDeadline
        }
    }
    catch {
        $cleanupErrors.Add(
            "Fixture cleanup failed: $($_.Exception.Message)")
    }
    if ($cleanupErrors.Count -gt 0) {
        $cleanupMessage = ' ' + ($cleanupErrors -join ' ')
        if ($null -eq $failure) {
            $failure = [pscustomobject]@{
                Exception = [System.Exception]::new($cleanupMessage)
            }
        }
        else {
            $failure = [pscustomobject]@{
                Exception = [System.Exception]::new(
                    $failure.Exception.Message + $cleanupMessage)
            }
        }
    }
}

if ($null -ne $failure) {
    Write-Error (
        'Multi-instance labeling smoke runner failed at step "' +
        $script:CurrentStep + '": ' +
        $failure.Exception.Message)
    exit 1
}

Write-Host 'Multi-instance labeling smoke runner passed.'
exit 0
