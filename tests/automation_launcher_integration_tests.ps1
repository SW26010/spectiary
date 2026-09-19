[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Launcher,
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$CleanupFixture,
    [Parameter(Mandatory = $true)]
    [string]$TimeoutFixture,
    [Parameter(Mandatory = $true)]
    [string]$StateFixture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$labelingTaskId = '77777777-7777-4777-8777-777777777777'

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

function Test-JsonBooleanProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [bool]$Expected
    )

    $property = $Object.PSObject.Properties[$Name]
    return (
        $null -ne $property -and
        $property.Value -is [bool] -and
        [bool]$property.Value -eq $Expected)
}

function Get-FileSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $algorithm =
        [System.Security.Cryptography.SHA256]::Create()
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
            -not (
                Compare-Object `
                    -ReferenceObject $signature `
                    -DifferenceObject $bytes[0..7])) `
        -Message 'Captured output should have a complete PNG signature and IHDR.'
    Assert-True `
        -Condition (
            [char]$bytes[12] -eq 'I' -and
            [char]$bytes[13] -eq 'H' -and
            [char]$bytes[14] -eq 'D' -and
            [char]$bytes[15] -eq 'R') `
        -Message 'Captured output should begin with a PNG IHDR chunk.'
    $width =
        ([uint32]$bytes[16] -shl 24) -bor
        ([uint32]$bytes[17] -shl 16) -bor
        ([uint32]$bytes[18] -shl 8) -bor
        [uint32]$bytes[19]
    $height =
        ([uint32]$bytes[20] -shl 24) -bor
        ([uint32]$bytes[21] -shl 16) -bor
        ([uint32]$bytes[22] -shl 8) -bor
        [uint32]$bytes[23]
    return [pscustomobject]@{
        Width = $width
        Height = $height
    }
}

function Get-TreeFingerprint {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return 'missing'
    }
    $root = (Resolve-Path -LiteralPath $Path).Path
    $entries = @(
        Get-ChildItem -LiteralPath $root -Force -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                $relative = $_.FullName.Substring($root.Length)
                if ($_.PSIsContainer) {
                    "D|$relative|$($_.LastWriteTimeUtc.Ticks)"
                }
                else {
                    $hash =
                        Get-FileSha256 -Path $_.FullName
                    "F|$relative|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)|$hash"
                }
            }
    )
    return ($entries -join "`n")
}

function New-TestDirectoryJunction {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Target
    )

    $output = @(
        & cmd.exe /d /c (
            "mklink /J `"$Path`" `"$Target`"") 2>&1
    )
    if ($LASTEXITCODE -ne 0) {
        throw (
            'Could not create test directory junction: ' +
            ($output -join [Environment]::NewLine))
    }
}

function Remove-TestDirectoryJunction {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not [SpecForgeAutomationWindowTestNative]::
            RemoveDirectory($Path)) {
        throw (
            'Could not remove test directory junction. Win32 error: ' +
            [Runtime.InteropServices.Marshal]::
                GetLastWin32Error())
    }
}

function Get-OrdinaryStateRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$AppPath
    )

    $appDirectory = Split-Path -Parent $AppPath
    $metadataPath = Join-Path $appDirectory 'spectiary_metadata.json'
    if (Test-Path -LiteralPath $metadataPath -PathType Leaf) {
        $metadata = Get-Content -Raw -LiteralPath $metadataPath |
            ConvertFrom-Json
        $deploymentProperty =
            $metadata.PSObject.Properties['deployment']
        $storageProfileProperty = $null
        if ($null -ne $deploymentProperty -and
            $null -ne $deploymentProperty.Value) {
            $storageProfileProperty =
                $deploymentProperty.Value.PSObject.Properties[
                    'storage_profile']
        }
        if ($null -ne $storageProfileProperty -and
            [string]$storageProfileProperty.Value -eq 'portable') {
            return $appDirectory
        }
    }
    return Join-Path $env:LOCALAPPDATA 'Spectiary'
}

function Get-LauncherProcessId {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Output
    )

    foreach ($line in $Output) {
        if ([string]$line -match '^SpecForge PID: ([0-9]+)$') {
            return [int]$Matches[1]
        }
    }
    throw (
        'Launcher output did not contain the owned GUI process ID. Output: ' +
        ($Output -join [Environment]::NewLine))
}

function Get-ProtocolMessage {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Messages,
        [Parameter(Mandatory = $true)]
        [string]$RequestId,
        [Parameter(Mandatory = $true)]
        [string]$Status
    )

    $selected = @(
        $Messages |
            Where-Object {
                [string]$_.request_id -eq $RequestId -and
                [string]$_.status -eq $Status
            }
    )
    if ($selected.Count -ne 1) {
        throw (
            "Expected one $Status message for $RequestId, found " +
            $selected.Count +
            '. Messages: ' +
            ($Messages |
                ConvertTo-Json -Compress -Depth 20))
    }
    return $selected[0]
}

function Get-ProtocolTerminalMessage {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Messages,
        [Parameter(Mandatory = $true)]
        [string]$RequestId
    )

    $selected = @(
        $Messages |
            Where-Object {
                [string]$_.request_id -eq $RequestId -and
                [string]$_.status -in @('completed', 'failed')
            }
    )
    if ($selected.Count -ne 1) {
        throw (
            "Expected one terminal message for $RequestId, found " +
            $selected.Count +
            '. Messages: ' +
            ($Messages |
                ConvertTo-Json -Compress -Depth 20))
    }
    return $selected[0]
}

function Read-LauncherLine {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutMilliseconds = 10000
    )

    $read = $Process.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) {
        throw 'Timed out waiting for interactive launcher output.'
    }
    if ($null -eq $read.Result) {
        throw (
            'Interactive launcher closed stdout unexpectedly with exit code ' +
            $(if ($Process.HasExited) {
                $Process.ExitCode
            }
            else {
                'still-running'
            }))
    }
    return [string]$read.Result
}

function Invoke-LauncherWithInput {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string[]]$Lines,
        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds
    )

    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $LauncherPath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $utf8WithoutBom =
        [System.Text.UTF8Encoding]::new($false)
    $start.StandardOutputEncoding = $utf8WithoutBom
    $start.StandardErrorEncoding = $utf8WithoutBom
    $start.Arguments =
        '--app "' + $AppPath +
        '" --state-root "' + $Root + '"'

    $process = [System.Diagnostics.Process]::Start($start)
    $stdoutTask =
        $process.StandardOutput.ReadToEndAsync()
    $stderrTask =
        $process.StandardError.ReadToEndAsync()
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $payload =
            $utf8WithoutBom.GetBytes(
                ($Lines -join "`n") + "`n")
        $inputStream = $process.StandardInput.BaseStream
        $inputStream.Write(
            $payload,
            0,
            $payload.Length)
        $inputStream.Flush()
        $process.StandardInput.Close()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
            throw (
                'Launcher did not exit within the timeout-fixture test budget of ' +
                $TimeoutMilliseconds + ' ms.')
        }
        $stopwatch.Stop()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $output = @()
        if (-not [string]::IsNullOrEmpty($stdout)) {
            $output += $stdout -split "`r?`n"
        }
        if (-not [string]::IsNullOrEmpty($stderr)) {
            $output += $stderr -split "`r?`n"
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            ElapsedMilliseconds = $stopwatch.ElapsedMilliseconds
            Output = $output
        }
    }
    finally {
        $stopwatch.Stop()
        $process.Dispose()
    }
}

function Get-TimeoutFixturePid {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $pidPath = Join-Path $Root 'timeout-fixture.pid'
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $pidPath -PathType Leaf) {
            return [int](Get-Content -Raw -LiteralPath $pidPath)
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Timeout fixture did not publish its PID under $Root."
}

function Assert-ProcessGoneWithin {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq (
                Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw $Message
}

function Send-InteractiveLauncherRequest {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string[]]$Lines,
        [Parameter(Mandatory = $true)]
        [string]$RequestId
    )

    $payload =
        [System.Text.UTF8Encoding]::new($false).GetBytes(
            ($Lines -join "`n") + "`n")
    $inputStream =
        $Process.StandardInput.BaseStream
    $inputStream.Write(
        $payload,
        0,
        $payload.Length)
    $inputStream.Flush()
    $messages = @()
    while ($true) {
        $line =
            Read-LauncherLine -Process $Process
        if ($line -notmatch '^\{') {
            continue
        }
        $message = $line | ConvertFrom-Json
        if ([string]$message.request_id -ne $RequestId) {
            throw (
                "Interactive launcher returned an unexpected request ID: $line")
        }
        $messages += $message
        if ([string]$message.status -in @(
                'completed',
                'failed',
                'canceled')) {
            break
        }
    }
    Assert-True `
        -Condition (
            $messages.Count -eq 2 -and
            [string]$messages[0].status -eq 'accepted') `
        -Message "Interactive request $RequestId should emit accepted and one terminal."
    return $messages
}

function Begin-InteractiveLauncherRequest {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string]$Line,
        [Parameter(Mandatory = $true)]
        [string]$RequestId
    )

    $payload =
        [System.Text.UTF8Encoding]::new($false).GetBytes(
            $Line + "`n")
    $inputStream =
        $Process.StandardInput.BaseStream
    $inputStream.Write(
        $payload,
        0,
        $payload.Length)
    $inputStream.Flush()
    while ($true) {
        $line =
            Read-LauncherLine -Process $Process
        if ($line -notmatch '^\{') {
            continue
        }
        $message = $line | ConvertFrom-Json
        Assert-True `
            -Condition (
                [string]$message.request_id -eq
                    $RequestId -and
                [string]$message.status -eq
                    'accepted') `
            -Message (
                "Interactive request $RequestId should begin with one accepted message: $line")
        return $message
    }
}

function Complete-InteractiveLauncherRequest {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string]$RequestId
    )

    while ($true) {
        $line =
            Read-LauncherLine -Process $Process
        if ($line -notmatch '^\{') {
            continue
        }
        $message = $line | ConvertFrom-Json
        Assert-True `
            -Condition (
                [string]$message.request_id -eq
                    $RequestId) `
            -Message (
                "Interactive launcher returned an unexpected request ID: $line")
        if ([string]$message.status -in @(
                'completed',
                'failed',
                'canceled')) {
            return $message
        }
    }
}

function Send-InteractiveLauncherBatch {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string[]]$Lines,
        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedRequestIds
    )

    $payload =
        [System.Text.UTF8Encoding]::new($false).GetBytes(
            ($Lines -join "`n") + "`n")
    $inputStream =
        $Process.StandardInput.BaseStream
    $inputStream.Write(
        $payload,
        0,
        $payload.Length)
    $inputStream.Flush()
    $messages = @()
    $terminalIds = @{}
    while ($terminalIds.Count -lt $ExpectedRequestIds.Count) {
        $line =
            Read-LauncherLine -Process $Process
        if ($line -notmatch '^\{') {
            continue
        }
        $message = $line | ConvertFrom-Json
        $requestId = [string]$message.request_id
        if ($requestId -notin $ExpectedRequestIds) {
            throw (
                "Interactive launcher returned an unexpected batch request ID: $line")
        }
        $messages += $message
        if ([string]$message.status -in @(
                'completed',
                'failed',
                'canceled')) {
            $terminalIds[$requestId] = $true
        }
    }
    foreach ($requestId in $ExpectedRequestIds) {
        $acceptedCount = @(
            $messages |
                Where-Object {
                    [string]$_.request_id -eq $requestId -and
                    [string]$_.status -eq 'accepted'
                }
        ).Count
        $terminalCount = @(
            $messages |
                Where-Object {
                    [string]$_.request_id -eq $requestId -and
                    [string]$_.status -in @(
                        'completed',
                        'failed',
                        'canceled')
                }
        ).Count
        Assert-True `
            -Condition (
                $acceptedCount -eq 1 -and
                $terminalCount -eq 1) `
            -Message "Interactive batch request $requestId should emit accepted and one terminal."
    }
    return $messages
}

if ($null -eq (
        'SpecForgeAutomationWindowTestNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SpecForgeAutomationWindowTestNative
{
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window,
        out uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenThread(
        uint desiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
        uint threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SuspendThread(IntPtr thread);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(IntPtr thread);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ShowWindowAsync(
        IntPtr window,
        int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessageW(
        IntPtr window,
        uint message,
        IntPtr wparam,
        IntPtr lparam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsIconic(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLengthW(
        IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(
        IntPtr window,
        StringBuilder text,
        int maximumCount);

    public static string WindowTitle(IntPtr window)
    {
        int length = GetWindowTextLengthW(window);
        StringBuilder text = new StringBuilder(length + 1);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    public static uint WindowThreadId(IntPtr window)
    {
        uint processId;
        return GetWindowThreadProcessId(
            window,
            out processId);
    }

    public static uint SuspendWindowThread(IntPtr window)
    {
        uint processId;
        uint threadId = GetWindowThreadProcessId(
            window,
            out processId);
        IntPtr thread = OpenThread(
            0x0002,
            false,
            threadId);
        if (thread == IntPtr.Zero)
        {
            return 0;
        }
        try
        {
            return SuspendThread(thread) == UInt32.MaxValue
                ? 0
                : threadId;
        }
        finally
        {
            CloseHandle(thread);
        }
    }

    public static bool ResumeWindowThread(uint threadId)
    {
        IntPtr thread = OpenThread(
            0x0002,
            false,
            threadId);
        if (thread == IntPtr.Zero)
        {
            return false;
        }
        try
        {
            return ResumeThread(thread) != UInt32.MaxValue;
        }
        finally
        {
            CloseHandle(thread);
        }
    }

    [DllImport(
        "kernel32.dll",
        CharSet = CharSet.Unicode,
        SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFile(
        string existingPath,
        string newPath);

    [DllImport(
        "kernel32.dll",
        CharSet = CharSet.Unicode,
        SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool RemoveDirectory(string path);
}
'@
}

function Invoke-RejectedGuiWorkflow {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Commands,
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Seed
    )

    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(
            $Commands |
                & $LauncherPath `
                    --app $AppPath `
                    --state-root $Root `
                    --labeling-state-seed $Seed 2>&1
        )
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $messages = @(
        $output |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
        Messages = $messages
        Pid = Get-LauncherProcessId -Output $output
    }
}

function Invoke-UnrenderablePanelQuitScenario {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [ValidateSet('hidden', 'minimized', 'immersive')]
        [string]$Mode
    )

    $launcherProcess = $null
    $guiPid = 0
    try {
        $start = [System.Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $LauncherPath
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardInput = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $utf8WithoutBom =
            [System.Text.UTF8Encoding]::new($false)
        $start.StandardOutputEncoding = $utf8WithoutBom
        $start.StandardErrorEncoding = $utf8WithoutBom
        $start.Arguments =
            '--app "' + $AppPath +
            '" --state-root "' + $Root + '"'
        $launcherProcess =
            [System.Diagnostics.Process]::Start($start)

        while ($true) {
            $line =
                Read-LauncherLine -Process $launcherProcess
            if ($line -match '^SpecForge PID: ([0-9]+)$') {
                $guiPid = [int]$Matches[1]
            }
            if ($line -eq (
                    'Harness controls: pipeline begin ... pipeline end; ' +
                    'disconnect after accepted <next command>')) {
                break
            }
        }
        Assert-True `
            -Condition ($guiPid -gt 0) `
            -Message "$Mode panel/quit scenario should report its owned GUI PID."

        $windowHandle = [IntPtr]::Zero
        $windowDeadline = [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $windowDeadline) {
            $guiProcess =
                Get-Process `
                    -Id $guiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $guiProcess) {
                $guiProcess.Refresh()
                $windowHandle =
                    [IntPtr]$guiProcess.MainWindowHandle
            }
            if ($windowHandle -ne [IntPtr]::Zero -and
                [SpecForgeAutomationWindowTestNative]::
                    IsWindowVisible($windowHandle) -and
                -not [SpecForgeAutomationWindowTestNative]::
                    IsIconic($windowHandle)) {
                break
            }
            Start-Sleep -Milliseconds 25
        }
        Assert-True `
            -Condition ($windowHandle -ne [IntPtr]::Zero) `
            -Message "$Mode panel/quit scenario should find the real GUI HWND."

        $nextRequestNumber = 1
        if ($Mode -eq 'immersive') {
            Assert-True `
                -Condition (
                    [SpecForgeAutomationWindowTestNative]::
                        PostMessageW(
                            $windowHandle,
                            0x0100,
                            [IntPtr]0x7A,
                            [IntPtr]1)) `
                -Message 'Immersive panel/quit scenario should post F11 to the real GUI HWND.'
            $immersiveBarrier = @(
                Send-InteractiveLauncherRequest `
                    -Process $launcherProcess `
                    -Lines @('state get') `
                    -RequestId 'request-1'
            )
            Assert-True `
                -Condition (
                    [string]$immersiveBarrier[1].status -eq
                        'completed') `
                -Message 'Immersive panel/quit scenario should cross a UI-thread protocol barrier after F11.'
            $nextRequestNumber = 2
        }
        else {
            $showCommand = if ($Mode -eq 'hidden') { 0 } else { 7 }
            [void][SpecForgeAutomationWindowTestNative]::
                ShowWindowAsync($windowHandle, $showCommand)
            $unrenderableDeadline =
                [DateTime]::UtcNow.AddSeconds(5)
            while ([DateTime]::UtcNow -lt $unrenderableDeadline) {
                $isUnrenderable =
                    if ($Mode -eq 'hidden') {
                        -not [SpecForgeAutomationWindowTestNative]::
                            IsWindowVisible($windowHandle)
                    }
                    else {
                        [SpecForgeAutomationWindowTestNative]::
                            IsIconic($windowHandle)
                    }
                if ($isUnrenderable) {
                    break
                }
                Start-Sleep -Milliseconds 25
            }
        }

        $panelRequestId =
            'request-' + $nextRequestNumber
        $waitRequestId =
            'request-' + ($nextRequestNumber + 1)
        $quitRequestId =
            'request-' + ($nextRequestNumber + 2)
        $messages = @(
            Send-InteractiveLauncherBatch `
                -Process $launcherProcess `
                -Lines @(
                    'pipeline begin',
                    'panel set files false',
                    'wait idle',
                    'app quit',
                    'pipeline end') `
                -ExpectedRequestIds @(
                    $panelRequestId,
                    $waitRequestId,
                    $quitRequestId)
        )
        $panelTerminal =
            Get-ProtocolMessage `
                -Messages $messages `
                -RequestId $panelRequestId `
                -Status 'failed'
        $waitTerminal =
            Get-ProtocolMessage `
                -Messages $messages `
                -RequestId $waitRequestId `
                -Status 'completed'
        $quitTerminal =
            Get-ProtocolMessage `
                -Messages $messages `
                -RequestId $quitRequestId `
                -Status 'completed'
        $launcherProcess.StandardInput.Close()
        Assert-True `
            -Condition (
                [string]$panelTerminal.error.code -eq
                    $(if ($Mode -eq 'immersive') {
                        'panel_not_renderable'
                    }
                    else {
                        'window_not_renderable'
                    }) -and
                [string]$waitTerminal.command -eq
                    'wait.idle' -and
                [string]$quitTerminal.command -eq
                    'app.quit' -and
                $launcherProcess.WaitForExit(15000) -and
                $launcherProcess.ExitCode -eq 0) `
            -Message (
                "$Mode panel.set must terminate without mutation and must not strand the following wait.idle/app.quit. Messages: " +
                ($messages | ConvertTo-Json -Compress -Depth 20) +
                '; launcher exit: ' +
                $(if ($launcherProcess.HasExited) {
                    $launcherProcess.ExitCode
                }
                else {
                    'still-running'
                }))

        $savedPanelPath =
            Join-Path $Root 'state\panel-visibility.json'
        if (Test-Path -LiteralPath $savedPanelPath) {
            $savedPanels =
                Get-Content -Raw -LiteralPath $savedPanelPath |
                    ConvertFrom-Json
            Assert-True `
                -Condition (
                    Test-JsonBooleanProperty `
                        -Object $savedPanels `
                        -Name 'files' `
                        -Expected $true) `
                -Message "$Mode rejected panel.set must not persist a visibility mutation."
        }
        Assert-True `
            -Condition (
                $null -eq (
                    Get-Process `
                        -Id $guiPid `
                        -ErrorAction SilentlyContinue)) `
            -Message "$Mode panel/quit scenario should leave no GUI process."
    }
    finally {
        if ($null -ne $launcherProcess) {
            if (-not $launcherProcess.HasExited) {
                $launcherProcess.StandardInput.Close()
                if (-not $launcherProcess.WaitForExit(3000)) {
                    $launcherProcess.Kill()
                    [void]$launcherProcess.WaitForExit(3000)
                }
            }
            $launcherProcess.Dispose()
        }
        if ($guiPid -gt 0) {
            $ownedGui =
                Get-Process `
                    -Id $guiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $ownedGui) {
                [void]$ownedGui.CloseMainWindow()
                if (-not $ownedGui.WaitForExit(3000)) {
                    Stop-Process `
                        -Id $guiPid `
                        -Force `
                        -ErrorAction SilentlyContinue
                }
                $ownedGui.Dispose()
            }
        }
    }
}

function Start-InteractiveAutomationLauncher {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $LauncherPath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $utf8WithoutBom =
        [System.Text.UTF8Encoding]::new($false)
    $start.StandardOutputEncoding = $utf8WithoutBom
    $start.StandardErrorEncoding = $utf8WithoutBom
    $start.Arguments =
        '--app "' + $AppPath +
        '" --state-root "' + $Root + '"'
    $process =
        [System.Diagnostics.Process]::Start($start)
    $guiPid = 0
    while ($true) {
        $line = Read-LauncherLine -Process $process
        if ($line -match '^SpecForge PID: ([0-9]+)$') {
            $guiPid = [int]$Matches[1]
        }
        if ($line -eq (
                'Harness controls: pipeline begin ... pipeline end; ' +
                'disconnect after accepted <next command>')) {
            break
        }
    }
    Assert-True `
        -Condition ($guiPid -gt 0) `
        -Message 'Interactive scenario should report its owned GUI PID.'
    return [pscustomobject]@{
        Process = $process
        GuiPid = $guiPid
    }
}

function Wait-ForMainGuiWindow {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId
    )

    $windowHandle = [IntPtr]::Zero
    $windowDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $windowDeadline) {
        $guiProcess =
            Get-Process `
                -Id $ProcessId `
                -ErrorAction SilentlyContinue
        if ($null -ne $guiProcess) {
            $guiProcess.Refresh()
            $windowHandle =
                [IntPtr]$guiProcess.MainWindowHandle
        }
        if ($windowHandle -ne [IntPtr]::Zero -and
            [SpecForgeAutomationWindowTestNative]::
                IsWindowVisible($windowHandle) -and
            -not [SpecForgeAutomationWindowTestNative]::
                IsIconic($windowHandle)) {
            return $windowHandle
        }
        Start-Sleep -Milliseconds 25
    }
    throw 'Interactive scenario could not find a renderable main GUI HWND.'
}

function Write-InteractiveLauncherLines {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string[]]$Lines
    )

    $payload =
        [System.Text.UTF8Encoding]::new($false).GetBytes(
            ($Lines -join "`n") + "`n")
    $inputStream = $Process.StandardInput.BaseStream
    $inputStream.Write($payload, 0, $payload.Length)
    $inputStream.Flush()
}

function Invoke-PanelShutdownRollbackScenario {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $launcherProcess = $null
    $guiPid = 0
    $suspendedUiThreadId = [uint32]0
    try {
        $launch =
            Start-InteractiveAutomationLauncher `
                -LauncherPath $LauncherPath `
                -AppPath $AppPath `
                -Root $Root
        $launcherProcess = $launch.Process
        $guiPid = $launch.GuiPid
        $windowHandle =
            Wait-ForMainGuiWindow -ProcessId $guiPid

        $uiThreadId =
            [SpecForgeAutomationWindowTestNative]::
                WindowThreadId($windowHandle)
        $uiThreadWaiting = $false
        $uiThreadWaitDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while ([DateTime]::UtcNow -lt
            $uiThreadWaitDeadline) {
            $guiProcess =
                Get-Process `
                    -Id $guiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $guiProcess) {
                $guiProcess.Refresh()
                $uiThread = @(
                    $guiProcess.Threads |
                        Where-Object {
                            [uint32]$_.Id -eq
                                $uiThreadId
                        }
                ) | Select-Object -First 1
                if ($null -ne $uiThread -and
                    [string]$uiThread.ThreadState -eq
                        'Wait') {
                    $uiThreadWaiting = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 1
        }
        Assert-True `
            -Condition $uiThreadWaiting `
            -Message 'Shutdown rollback scenario should observe the real HWND UI thread idle before suspension.'
        $suspendedUiThreadId =
            [SpecForgeAutomationWindowTestNative]::
                SuspendWindowThread($windowHandle)
        Assert-True `
            -Condition ($suspendedUiThreadId -ne 0) `
            -Message 'Shutdown rollback scenario should suspend the real HWND UI thread.'

        Write-InteractiveLauncherLines `
            -Process $launcherProcess `
            -Lines @('panel set files false')
        $messages = @()
        while ($true) {
            try {
                $line =
                    Read-LauncherLine `
                        -Process $launcherProcess
            }
            catch {
                throw (
                    'Timed out before panel.set acceptance while the real GUI UI thread was suspended: ' +
                    $_.Exception.Message)
            }
            if ($line -notmatch '^\{') {
                continue
            }
            $message = $line | ConvertFrom-Json
            Assert-True `
                -Condition (
                    [string]$message.request_id -eq
                        'request-1') `
                -Message "Shutdown rollback returned an unexpected request: $line"
            $messages += $message
            if ([string]$message.status -eq 'accepted') {
                break
            }
        }
        Assert-True `
            -Condition (
                [SpecForgeAutomationWindowTestNative]::
                    PostMessageW(
                        $windowHandle,
                        0x0010,
                        [IntPtr]::Zero,
                        [IntPtr]::Zero)) `
            -Message 'Shutdown rollback scenario should post WM_CLOSE to the real GUI HWND.'
        Assert-True `
            -Condition (
                [SpecForgeAutomationWindowTestNative]::
                    ResumeWindowThread(
                        $suspendedUiThreadId)) `
            -Message 'Shutdown rollback scenario should resume the real HWND UI thread.'
        $suspendedUiThreadId = [uint32]0

        while ($true) {
            try {
                $line =
                    Read-LauncherLine `
                        -Process $launcherProcess
            }
            catch {
                throw (
                    'Timed out waiting for shutdown settlement after resuming the real GUI UI thread: ' +
                    $_.Exception.Message)
            }
            if ($line -notmatch '^\{') {
                continue
            }
            $message = $line | ConvertFrom-Json
            Assert-True `
                -Condition (
                    [string]$message.request_id -eq
                        'request-1') `
                -Message "Shutdown rollback returned an unexpected terminal: $line"
            $messages += $message
            if ([string]$message.status -in @(
                    'completed',
                    'failed',
                    'canceled')) {
                break
            }
        }
        $terminal =
            Get-ProtocolTerminalMessage `
                -Messages $messages `
                -RequestId 'request-1'
        $launcherProcess.StandardInput.Close()
        Assert-True `
            -Condition (
                [string]$terminal.status -eq 'failed' -and
                [string]$terminal.error.code -eq
                    'app_shutdown' -and
                $launcherProcess.WaitForExit(15000) -and
                $launcherProcess.ExitCode -eq 2) `
            -Message (
                'WM_CLOSE must settle a claimed panel.set with a factual app_shutdown terminal before the launcher reports the failed command. Messages: ' +
                ($messages | ConvertTo-Json -Compress -Depth 20))

        $savedPanelPath =
            Join-Path $Root 'state\panel-visibility.json'
        Assert-True `
            -Condition (
                (Test-Path `
                    -LiteralPath $savedPanelPath `
                    -PathType Leaf) -and
                (Test-JsonBooleanProperty `
                    -Object (
                        Get-Content `
                            -Raw `
                            -LiteralPath $savedPanelPath |
                            ConvertFrom-Json) `
                    -Name 'files' `
                    -Expected $true)) `
            -Message 'Shutdown must flush the restored pre-chain Files baseline, not the unpresented mutation.'

    }
    finally {
        if ($suspendedUiThreadId -ne 0) {
            [void][SpecForgeAutomationWindowTestNative]::
                ResumeWindowThread(
                    $suspendedUiThreadId)
            $suspendedUiThreadId = [uint32]0
        }
        if ($null -ne $launcherProcess) {
            if (-not $launcherProcess.HasExited) {
                $launcherProcess.StandardInput.Close()
                if (-not $launcherProcess.WaitForExit(3000)) {
                    $launcherProcess.Kill()
                    [void]$launcherProcess.WaitForExit(3000)
                }
            }
            $launcherProcess.Dispose()
        }
        if ($guiPid -gt 0) {
            $ownedGui =
                Get-Process `
                    -Id $guiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $ownedGui) {
                [void]$ownedGui.CloseMainWindow()
                if (-not $ownedGui.WaitForExit(3000)) {
                    Stop-Process `
                        -Id $guiPid `
                        -Force `
                        -ErrorAction SilentlyContinue
                }
                $ownedGui.Dispose()
            }
        }
    }
}

$resolvedLauncher = (Resolve-Path -LiteralPath $Launcher).Path
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedCleanupFixture =
    (Resolve-Path -LiteralPath $CleanupFixture).Path
$resolvedTimeoutFixture =
    (Resolve-Path -LiteralPath $TimeoutFixture).Path
$resolvedStateFixture =
    (Resolve-Path -LiteralPath $StateFixture).Path

$fixtureParent = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-automation-launcher-tests-' +
        [Guid]::NewGuid().ToString('N'))
$portableAppRoot =
    Join-Path $fixtureParent 'portable-app'
$fixtureExecutable =
    Join-Path `
        $portableAppRoot `
        ([System.IO.Path]::GetFileName(
            $resolvedExecutable))
$stateRoot = Join-Path $fixtureParent 'state'
[System.IO.Directory]::CreateDirectory($fixtureParent) | Out-Null
$incompatibleEnvironmentVariables = @(
    'SPECFORGE_PROFILE',
    'SPECFORGE_PROFILE_DIR',
    'SPECFORGE_RUNTIME_RESOURCE_WORKLOAD',
    'SPECFORGE_RUNTIME_RESOURCE_STATE_DIR'
)
$originalAutomationEnvironment = @{}
foreach ($name in $incompatibleEnvironmentVariables) {
    $originalAutomationEnvironment[$name] =
        [System.Environment]::GetEnvironmentVariable(
            $name,
            [System.EnvironmentVariableTarget]::Process)
}
$cleanupFixtureEnvironment =
    'SPECFORGE_AUTOMATION_LAUNCHER_FIXTURE'
$originalCleanupFixtureEnvironment =
    [System.Environment]::GetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        [System.EnvironmentVariableTarget]::Process)
$timeoutFixtureEnvironment =
    'SPECFORGE_AUTOMATION_TIMEOUT_FIXTURE'
$originalTimeoutFixtureEnvironment =
    [System.Environment]::GetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        [System.EnvironmentVariableTarget]::Process)
$bystander = $null
$timeoutFixtureBudgetMilliseconds = 25000

try {
    [System.IO.Directory]::CreateDirectory(
        $portableAppRoot) | Out-Null
    Copy-Item `
        -LiteralPath $resolvedExecutable `
        -Destination $fixtureExecutable
    $buildRoot =
        Split-Path -Parent $resolvedExecutable
    foreach ($runtimeFile in @(
            'cfitsio.dll',
            'pthreadVC3d.dll',
            'zd.dll',
            'yaml-cppd.dll')) {
        $runtimeDll = Join-Path $buildRoot $runtimeFile
        if (Test-Path -LiteralPath $runtimeDll -PathType Leaf) {
            Copy-Item `
                -LiteralPath $runtimeDll `
                -Destination (
                    Join-Path $portableAppRoot $runtimeFile)
        }
    }
    $runtimeConfig = Join-Path $buildRoot 'config'
    if (Test-Path -LiteralPath $runtimeConfig -PathType Container) {
        Copy-Item `
            -LiteralPath $runtimeConfig `
            -Destination (
                Join-Path $portableAppRoot 'config') `
            -Recurse
    }
    $metadataPath =
        Join-Path $buildRoot 'spectiary_metadata.json'
    $portableMetadata =
        Get-Content -Raw -LiteralPath $metadataPath |
            ConvertFrom-Json
    $portableMetadata |
        Add-Member `
            -NotePropertyName deployment `
            -NotePropertyValue ([pscustomobject][ordered]@{
                distribution = 'portable'
                storage_profile = 'portable'
            }) `
            -Force
    [System.IO.File]::WriteAllText(
        (Join-Path $portableAppRoot 'spectiary_metadata.json'),
        ($portableMetadata | ConvertTo-Json -Depth 10),
        [System.Text.UTF8Encoding]::new($false))

    $ordinaryRoot =
        Get-OrdinaryStateRoot -AppPath $fixtureExecutable
    Assert-True `
        -Condition (
            $ordinaryRoot -eq (
                $portableAppRoot)) `
        -Message 'Integration fixture requires Portable user-state resolution.'
    [System.IO.Directory]::CreateDirectory($ordinaryRoot) |
        Out-Null

    foreach ($role in @('config', 'state', 'logs', 'unsaved')) {
        [System.IO.Directory]::CreateDirectory((Join-Path $ordinaryRoot $role)) | Out-Null
    }
    $ordinarySource =
        Join-Path $fixtureParent 'ordinary-session-marker.csv'
    [System.IO.File]::WriteAllText(
        $ordinarySource,
        "wavelength,flux`n5000,1`n5001,2`n",
        [System.Text.UTF8Encoding]::new($false))
    $ordinarySession = [ordered]@{
        format_kind = 'specforge.source_collection_session.cache'
        schema_version = 2
        active_source_index = 0
        sources = @(
            [ordered]@{
                path = $ordinarySource
                last_index = 0
            }
        )
    } | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText(
        (Join-Path $ordinaryRoot 'state\source-session.json'),
        $ordinarySession,
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        (Join-Path $ordinaryRoot 'config\ui-language.json'),
        '{"format_kind":"specforge.ui_language.settings","schema_version":1,"language":"zh-Hans"}',
        [System.Text.UTF8Encoding]::new($false))
    # Pin fixture directory timestamps after population; NTFS can otherwise
    # finish its creation-time directory updates after the first snapshot.
    $fixtureDirectoryTime = [DateTime]::UtcNow.AddDays(-1)
    foreach ($directory in @(Get-ChildItem -LiteralPath $ordinaryRoot -Directory -Recurse -Force)) {
        [IO.Directory]::SetLastWriteTimeUtc($directory.FullName, $fixtureDirectoryTime)
    }
    $ordinaryBefore =
        Get-TreeFingerprint -Path $ordinaryRoot

    $sourceRoot = Join-Path $fixtureParent 'source-fixture'
    [System.IO.Directory]::CreateDirectory(
        $sourceRoot) | Out-Null
    foreach ($name in @(
            'alpha.csv',
            'bravo.csv',
            'charlie.csv')) {
        [System.IO.File]::WriteAllText(
            (Join-Path $sourceRoot $name),
            "wavelength,flux`n5000,1`n5001,2`n",
            [System.Text.UTF8Encoding]::new($false))
    }
    $labelSeed =
        Join-Path $fixtureParent 'labeling-seed.json'
    & $resolvedStateFixture `
        --write-labeling-seed $labelSeed `
        --source $sourceRoot
    if ($LASTEXITCODE -ne 0) {
        throw (
            'Production labeling fixture generation failed with exit code ' +
            $LASTEXITCODE)
    }
    $seedBefore =
        Get-FileSha256 -Path $labelSeed
    $capturePath =
        Join-Path $stateRoot 'artifacts\labeled-spectrum.png'

    $externalSentinelRoot =
        Join-Path $fixtureParent 'poisoned-external-output'
    [System.IO.Directory]::CreateDirectory(
        $externalSentinelRoot) | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $externalSentinelRoot 'sentinel.txt'),
        'must remain unchanged',
        [System.Text.UTF8Encoding]::new($false))
    $poisonedWorkload =
        Join-Path $externalSentinelRoot 'workload.json'
    [System.IO.File]::WriteAllText(
        $poisonedWorkload,
        '{"poisoned":true}',
        [System.Text.UTF8Encoding]::new($false))
    $externalBefore =
        Get-TreeFingerprint -Path $externalSentinelRoot
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_PROFILE',
        '1',
        [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_PROFILE_DIR',
        $externalSentinelRoot,
        [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_RUNTIME_RESOURCE_WORKLOAD',
        $poisonedWorkload,
        [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_RUNTIME_RESOURCE_STATE_DIR',
        $externalSentinelRoot,
        [System.EnvironmentVariableTarget]::Process)

    $savedMainErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(
            @(
                'profile start',
                "source open $sourceRoot",
                'wait idle',
                'spectrum goto 2',
                'label assign 5 spectrum 1',
                'spectrum goto 1',
                'wait idle',
                "frame capture $capturePath",
                'profile stop',
                'state get',
                'app quit'
            ) |
                & $resolvedLauncher `
                    --app $fixtureExecutable `
                    --state-root $stateRoot `
                    --labeling-state-seed $labelSeed 2>&1
        )
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedMainErrorActionPreference
    }
    if ($exitCode -ne 0) {
        throw (
            "Automation launcher exited with $exitCode.`n" +
            ($output -join [Environment]::NewLine))
    }

    $messages = @(
        $output |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    Assert-True `
        -Condition ($messages.Count -eq 23) `
        -Message 'Launcher should emit hello plus accepted/completed pairs for the eleven-command GUI and profile workflow.'

    $hello = $messages[0]
    Assert-True `
        -Condition (
            [string]$hello.type -eq 'hello' -and
            [string]$hello.status -eq 'completed' -and
            [int]$hello.protocol_version -eq 1 -and
            @($hello.capabilities).Count -eq 13 -and
            @($hello.capabilities) -contains 'setting.get' -and
            @($hello.capabilities) -contains 'setting.set' -and
            @($hello.capabilities) -contains 'panel.get' -and
            @($hello.capabilities) -contains 'panel.set' -and
            @($hello.capabilities) -contains 'profile.start' -and
            @($hello.capabilities) -contains 'profile.stop' -and
            [int]$hello.max_message_bytes -eq 65536 -and
            [int]$hello.queue_capacity -eq 32) `
        -Message 'Hello should expose the fixed protocol limits and bounded capabilities.'

    $profileStartAccepted = $messages[1]
    $profileStartCompleted = $messages[2]
    $sourceAccepted = $messages[3]
    $sourceCompleted = $messages[4]
    $waitAccepted = $messages[5]
    $waitCompleted = $messages[6]
    $gotoAccepted = $messages[7]
    $gotoCompleted = $messages[8]
    $labelAccepted = $messages[9]
    $labelCompleted = $messages[10]
    $returnAccepted = $messages[11]
    $returnCompleted = $messages[12]
    $secondWaitAccepted = $messages[13]
    $secondWaitCompleted = $messages[14]
    $captureAccepted = $messages[15]
    $captureCompleted = $messages[16]
    $profileStopAccepted = $messages[17]
    $profileStopCompleted = $messages[18]
    $stateAccepted = $messages[19]
    $stateCompleted = $messages[20]
    $quitAccepted = $messages[21]
    $quitCompleted = $messages[22]
    $sourcePathProperty =
        $stateCompleted.state.source.PSObject.Properties['path']
    $ordinarySourceNotImported =
        $null -eq $sourcePathProperty -or
        [string]$sourcePathProperty.Value -ne $ordinarySource
    Assert-True `
        -Condition (
            [string]$stateAccepted.command -eq 'state.get' -and
            [string]$stateAccepted.status -eq 'accepted' -and
            [string]$stateCompleted.status -eq 'completed' -and
            [bool]$stateCompleted.state.shell.idle -and
            [bool]$stateCompleted.state.shell.source_load_idle -and
            [bool]$stateCompleted.state.shell.pending_completion_idle -and
            [bool]$stateCompleted.state.shell.background_retirement_idle -and
            [bool]$stateCompleted.state.window.visible -and
            -not [bool]$stateCompleted.state.window.minimized -and
            [bool]$stateCompleted.state.runtime.running -and
            [bool]$stateCompleted.state.source.present -and
            [string]$stateCompleted.state.source.path -eq $sourceRoot -and
            [bool]$stateCompleted.state.presented_source.present -and
            [string]$stateCompleted.state.presented_source.path -eq
                $sourceRoot -and
            [string]$stateCompleted.state.presented_source.id -eq
                [string]$stateCompleted.state.source.id -and
            [int]$stateCompleted.state.spectrum.index -eq 1 -and
            [string]$stateCompleted.state.spectrum.name -eq 'bravo.csv' -and
            [int]$stateCompleted.state.labeling.current_spectrum_label.code -eq 5 -and
            -not [bool]$stateCompleted.state.capture.pending -and
            [string]$stateCompleted.state.capture.last_result -eq 'succeeded' -and
            [string]$stateCompleted.state.capture.last_path -eq $capturePath -and
            [string]$stateCompleted.state.profile.status -eq 'succeeded' -and
            [string]$stateCompleted.state.profile.path -eq
                [string]$profileStopCompleted.result.path -and
            [string]$stateCompleted.state.profile.stop_reason -eq
                'explicit' -and
            [uint64]$stateCompleted.state.profile.dropped_events -eq
                [uint64]$profileStopCompleted.result.dropped_events -and
            $ordinarySourceNotImported) `
        -Message 'state.get should return the stable instance, Shell, capture, profile, window and runtime contract.'
    Assert-True `
        -Condition (
            [string]$profileStartAccepted.command -eq 'profile.start' -and
            [string]$profileStartCompleted.status -eq 'completed' -and
            [string]$profileStartCompleted.result.status -eq 'recording' -and
            [string]$profileStartCompleted.result.path -eq
                [string]$profileStopCompleted.result.path -and
            [string]$profileStopAccepted.command -eq 'profile.stop' -and
            [string]$profileStopCompleted.status -eq 'completed' -and
            [string]$profileStopCompleted.result.status -eq 'succeeded' -and
            [string]$profileStopCompleted.result.stop_reason -eq 'explicit') `
        -Message 'Profile start should expose the live production path and profile stop should terminal only with the finalized writer outcome.'
    Assert-True `
        -Condition (
            [string]$waitAccepted.command -eq 'wait.idle' -and
            [string]$waitAccepted.status -eq 'accepted' -and
            [string]$waitCompleted.status -eq 'completed') `
        -Message 'wait.idle should complete after prior controls and Shell activity are idle.'
    Assert-True `
        -Condition (
            [string]$sourceAccepted.command -eq 'source.open' -and
            [string]$sourceCompleted.status -eq 'completed' -and
            [int]$sourceCompleted.result.source.spectrum_count -eq 3 -and
            [string]$gotoCompleted.status -eq 'completed' -and
            [int]$gotoCompleted.result.spectrum.index -eq 2 -and
            [string]$labelAccepted.command -eq 'label.assign' -and
            [string]$labelCompleted.status -eq 'completed' -and
            [int]$labelCompleted.result.assignment.spectrum.index -eq 1 -and
            [int]$labelCompleted.result.assignment.previous_code -eq -1 -and
            [int]$labelCompleted.result.assignment.new_code -eq 5 -and
            [bool]$labelCompleted.result.assignment.changed -and
            [string]$labelCompleted.result.persistence.status -eq
                'state_save_scheduled' -and
            [bool]$labelCompleted.result.persistence.state_save_scheduled -and
            -not [bool]$labelCompleted.result.persistence.state_save_attempted -and
            [int]$labelCompleted.result.current_spectrum_after.index -eq 2 -and
            [string]$returnAccepted.command -eq 'spectrum.goto' -and
            [string]$returnCompleted.status -eq 'completed' -and
            [int]$returnCompleted.result.spectrum.index -eq 1 -and
            [string]$secondWaitAccepted.command -eq 'wait.idle' -and
            [string]$secondWaitCompleted.status -eq 'completed') `
        -Message 'Source, navigation and auto-advancing label assignment should complete through the real session seam.'
    Assert-True `
        -Condition (
            [string]$captureAccepted.command -eq 'frame.capture' -and
            [string]$captureCompleted.status -eq 'completed' -and
            [string]$captureCompleted.result.path -eq $capturePath -and
            [string]$captureCompleted.result.format -eq 'png' -and
            [string]$captureCompleted.result.scope -eq 'main_viewport' -and
            [int]$captureCompleted.result.width -gt 0 -and
            [int]$captureCompleted.result.height -gt 0) `
        -Message 'frame.capture should report the requested in-root application-rendered PNG.'
    Assert-True `
        -Condition (
            [string]$quitAccepted.command -eq 'app.quit' -and
            [string]$quitAccepted.status -eq 'accepted' -and
            [string]$quitCompleted.status -eq 'completed') `
        -Message 'app.quit should complete through the normal process shutdown path.'

    Assert-True `
        -Condition (
            (Test-Path -LiteralPath $capturePath -PathType Leaf) -and
            (Get-Item -LiteralPath $capturePath).Length -gt 8) `
        -Message 'Captured PNG should exist and be non-empty.'
    $pngIhdr = Get-PngIhdr -Path $capturePath
    Assert-True `
        -Condition (
            [uint32]$pngIhdr.Width -eq
                [uint32]$captureCompleted.result.width -and
            [uint32]$pngIhdr.Height -eq
                [uint32]$captureCompleted.result.height) `
        -Message 'Captured PNG IHDR dimensions must match the terminal dimensions from the application render target.'

    $profilePath = [string]$profileStopCompleted.result.path
    $profileRoot = [System.IO.Path]::GetFullPath($stateRoot)
    $normalizedProfilePath = [System.IO.Path]::GetFullPath($profilePath)
    $profileLines = @(
        Get-Content -LiteralPath $profilePath |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    $profileSummary =
        [string]$profileLines[-1] | ConvertFrom-Json
    Assert-True `
        -Condition (
            $normalizedProfilePath.StartsWith(
                $profileRoot + [System.IO.Path]::DirectorySeparatorChar,
                [System.StringComparison]::OrdinalIgnoreCase) -and
            (Test-Path -LiteralPath $profilePath -PathType Leaf) -and
            $profileLines.Count -gt 2 -and
            [string]$profileSummary.event -eq 'profile_recorder_summary' -and
            [string]$profileSummary.stop_reason -eq 'explicit' -and
            [uint64]$profileSummary.dropped_events -eq
                [uint64]$profileStopCompleted.result.dropped_events) `
        -Message 'profile.stop completion must correspond to a complete JSONL summary inside the isolated automation root.'

    & $resolvedStateFixture `
        --verify-labeling-state (
            Join-Path $stateRoot 'state\sample-labeling-state.json') `
        --source $sourceRoot `
        --spectrum-index 1 `
        --expected-code 5
    $productionReloadExitCode = $LASTEXITCODE
    Assert-True `
        -Condition ($productionReloadExitCode -eq 0) `
        -Message 'Normal app.quit should persist the assigned label and the production loader should reload the expected source-row value.'
    Assert-True `
        -Condition (
            (Get-FileSha256 -Path $labelSeed) -ceq $seedBefore) `
        -Message 'Launcher-owned materialization must not modify the read-only seed fixture.'

    Assert-True `
        -Condition (
            (Test-Path -LiteralPath $stateRoot -PathType Container) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path $stateRoot 'automation-startup-error.txt') `
                -PathType Leaf)) `
        -Message 'Launcher should create a clean isolated state root with no startup failure.'

    $guiPid = Get-LauncherProcessId -Output $output
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $guiPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Normal app.quit should leave no GUI process.'

    $recordingQuitRoot =
        Join-Path $fixtureParent 'recording-quit-state'
    $recordingQuitOutput = @(
        @(
            'profile start',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $recordingQuitRoot 2>&1
    )
    $recordingQuitExitCode = $LASTEXITCODE
    if ($recordingQuitExitCode -ne 0) {
        throw (
            "Recording app.quit workflow exited with $recordingQuitExitCode.`n" +
            ($recordingQuitOutput -join [Environment]::NewLine))
    }
    $recordingQuitMessages = @(
        $recordingQuitOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $recordingQuitStart =
        Get-ProtocolMessage `
            -Messages $recordingQuitMessages `
            -RequestId 'request-1' `
            -Status 'completed'
    $recordingQuitTerminal =
        Get-ProtocolMessage `
            -Messages $recordingQuitMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    $recordingQuitProfilePath =
        [string]$recordingQuitStart.result.path
    $recordingQuitProfileLines = @(
        Get-Content -LiteralPath $recordingQuitProfilePath |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    $recordingQuitSummary =
        [string]$recordingQuitProfileLines[-1] |
            ConvertFrom-Json
    $recordingQuitPid =
        Get-LauncherProcessId -Output $recordingQuitOutput
    Assert-True `
        -Condition (
            [string]$recordingQuitTerminal.command -eq 'app.quit' -and
            [string]$recordingQuitSummary.event -eq
                'profile_recorder_summary' -and
            [string]$recordingQuitSummary.stop_reason -eq 'explicit' -and
            $null -eq (
                Get-Process `
                    -Id $recordingQuitPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'app.quit while recording should drain the production writer, preserve its summary, and leave no GUI process.'

    $stoppingQuitRoot =
        Join-Path $fixtureParent 'stopping-quit-state'
    $stoppingQuitOutput = @(
        @(
            'profile start',
            'pipeline begin',
            'profile stop',
            'app quit',
            'pipeline end'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $stoppingQuitRoot 2>&1
    )
    $stoppingQuitExitCode = $LASTEXITCODE
    if ($stoppingQuitExitCode -ne 0) {
        throw (
            "Stopping app.quit workflow exited with $stoppingQuitExitCode.`n" +
            ($stoppingQuitOutput -join [Environment]::NewLine))
    }
    $stoppingQuitMessages = @(
        $stoppingQuitOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $stoppingQuitStart =
        Get-ProtocolMessage `
            -Messages $stoppingQuitMessages `
            -RequestId 'request-1' `
            -Status 'completed'
    $stoppingQuitStop =
        Get-ProtocolMessage `
            -Messages $stoppingQuitMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    $stoppingQuitTerminal =
        Get-ProtocolMessage `
            -Messages $stoppingQuitMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $stoppingQuitStopIndex =
        [array]::IndexOf(
            $stoppingQuitMessages,
            $stoppingQuitStop)
    $stoppingQuitTerminalIndex =
        [array]::IndexOf(
            $stoppingQuitMessages,
            $stoppingQuitTerminal)
    $stoppingQuitProfilePath =
        [string]$stoppingQuitStart.result.path
    $stoppingQuitProfileLines = @(
        Get-Content -LiteralPath $stoppingQuitProfilePath |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    $stoppingQuitSummary =
        [string]$stoppingQuitProfileLines[-1] |
            ConvertFrom-Json
    $stoppingQuitPid =
        Get-LauncherProcessId -Output $stoppingQuitOutput
    Assert-True `
        -Condition (
            $stoppingQuitStopIndex -ge 0 -and
            $stoppingQuitStopIndex -lt $stoppingQuitTerminalIndex -and
            [string]$stoppingQuitStop.result.status -eq 'succeeded' -and
            [string]$stoppingQuitSummary.event -eq
                'profile_recorder_summary' -and
            [string]$stoppingQuitTerminal.command -eq 'app.quit' -and
            $null -eq (
                Get-Process `
                    -Id $stoppingQuitPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'app.quit should wait behind an earlier claimed profile.stop terminal and leave no unfinished output or GUI process.'

    $profileConflictRoot =
        Join-Path $fixtureParent 'profile-conflict-state'
    $profileConflictOutput = @(
        @(
            'pipeline begin',
            'profile stop',
            'profile start',
            'state get',
            'profile start',
            'profile stop',
            'state get',
            'profile stop',
            'app quit',
            'pipeline end'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $profileConflictRoot 2>&1
    )
    $profileConflictExitCode = $LASTEXITCODE
    if ($profileConflictExitCode -ne 0) {
        throw (
            "Profile conflict workflow exited with $profileConflictExitCode.`n" +
            ($profileConflictOutput -join [Environment]::NewLine))
    }
    $profileConflictMessages = @(
        $profileConflictOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $profileConflictTerminals = @{}
    foreach ($message in $profileConflictMessages) {
        if ([string]$message.status -in @(
                'completed',
                'failed',
                'canceled')) {
            $profileConflictTerminals[
                [string]$message.request_id] = $message
        }
    }
    $profileConflictExpectedRoot =
        [System.IO.Path]::GetFullPath($profileConflictRoot)
    $profileConflictReportedRoot = ''
    foreach ($outputLine in $profileConflictOutput) {
        if ([string]$outputLine -match '^Automation state root: (.+)$') {
            $profileConflictReportedRoot = $Matches[1]
            break
        }
    }
    $profileConflictRootPrefix =
        $profileConflictExpectedRoot +
        [System.IO.Path]::DirectorySeparatorChar
    $profileConflictStartPath =
        [string]$profileConflictTerminals['request-2'].result.path
    $profileConflictStatePath =
        [string]$profileConflictTerminals['request-3'].state.profile.path
    $profileConflictStopPath =
        [string]$profileConflictTerminals['request-5'].result.path
    $profileConflictPathsAreIsolated =
        $profileConflictReportedRoot -eq
            $profileConflictExpectedRoot -and
        $profileConflictStartPath.StartsWith(
            $profileConflictRootPrefix,
            [System.StringComparison]::OrdinalIgnoreCase) -and
        $profileConflictStatePath -eq
            $profileConflictStartPath -and
        $profileConflictStopPath -eq
            $profileConflictStartPath
    $profileConflictQuitTerminalCount = @(
        $profileConflictMessages |
            Where-Object {
                $commandProperty =
                    $_.PSObject.Properties['command']
                $null -ne $commandProperty -and
                    [string]$commandProperty.Value -eq 'app.quit' -and
                    [string]$_.status -in @(
                        'completed',
                        'failed',
                        'canceled')
            }
    ).Count
    $profileConflictContractPassed =
        $profileConflictMessages.Count -eq 17 -and
        $profileConflictPathsAreIsolated -and
        $profileConflictQuitTerminalCount -eq 1 -and
        [string]$profileConflictTerminals['request-1'].error.code -eq
            'profile_not_recording' -and
        [string]$profileConflictTerminals['request-2'].status -eq
            'completed' -and
        [string]$profileConflictTerminals['request-3'].state.profile.status -eq
            'recording' -and
        [string]$profileConflictTerminals['request-4'].error.code -eq
            'profile_recording_active' -and
        [string]$profileConflictTerminals['request-5'].status -eq
            'completed' -and
        [string]$profileConflictTerminals['request-6'].state.profile.status -in @(
            'stopping',
            'succeeded') -and
        [string]$profileConflictTerminals['request-7'].error.code -eq
            'profile_stop_in_progress' -and
        [string]$profileConflictTerminals['request-8'].status -eq
            'completed'
    if (-not $profileConflictContractPassed) {
        $profileConflictDiagnostic = [ordered]@{
            state_root = $profileConflictRoot
            reported_state_root = $profileConflictReportedRoot
            profile_paths_are_isolated = $profileConflictPathsAreIsolated
            message_count = $profileConflictMessages.Count
            terminals = $profileConflictTerminals
            messages = @($profileConflictMessages)
            raw_output = @($profileConflictOutput | ForEach-Object { [string]$_ })
        }
        Write-Host (
            '[profile-conflict-diagnostic] ' +
            ($profileConflictDiagnostic | ConvertTo-Json -Compress -Depth 20))
    }
    Assert-True `
        -Condition $profileConflictContractPassed `
        -Message 'Profile conflict workflow should expose not-recording, recording-active and stop-in-progress errors plus stable recording/stopping state without leaving the GUI running.'

    $settingsControlRoot =
        Join-Path $fixtureParent 'settings-control-state'
    $settingsCapturePath =
        Join-Path $settingsControlRoot 'captures\settings.png'
    $panelNames = @(
        'files',
        'navigation',
        'annotations',
        'labeling',
        'filters',
        'sorting',
        'smoothing',
        'information',
        'spectral_lines')
    $settingsCommands = @(
        'pipeline begin',
        'setting get ui.language',
        'setting set ui.language zh-Hans',
        'setting get ui.language',
        'setting set ui.scale 125',
        'setting set ui.scale +125',
        'setting set ui.scale not-an-integer',
        'setting set ui.scale 151',
        'setting get unsupported.setting',
        'state get',
        'panel get files',
        'panel get navigation',
        'panel get annotations',
        'panel get labeling',
        'panel get filters',
        'panel get sorting',
        'panel get smoothing',
        'panel get information',
        'panel get spectral_lines',
        'panel set files false',
        'panel set files false',
        'panel set spectral_lines false',
        'panel get spectral_lines',
        'panel get unsupported.panel',
        'panel set navigation false',
        'panel set navigation true',
        'panel set navigation false',
        'wait idle',
        'pipeline end',
        'setting get ui.scale',
        'state get',
        'panel set files true',
        'panel set spectral_lines true',
        'panel set navigation true')
    foreach ($panelName in $panelNames) {
        $settingsCommands += @(
            "panel set $panelName false",
            'state get',
            "panel set $panelName true")
    }
    $settingsCommands += @(
        "frame capture $settingsCapturePath",
        'app quit')
    $settingsOutput = @(
        $settingsCommands |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $settingsControlRoot 2>&1
    )
    $settingsExitCode = $LASTEXITCODE
    if ($settingsExitCode -ne 0) {
        throw (
            "Settings automation launcher exited with $settingsExitCode.`n" +
            ($settingsOutput -join [Environment]::NewLine))
    }
    $settingsMessages = @(
        $settingsOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    Assert-True `
        -Condition ($settingsMessages.Count -eq 123) `
        -Message 'Settings and panel workflow should emit hello plus accepted/terminal pairs for sixty-one requests.'
    $settingsHello = $settingsMessages[0]
    Assert-True `
        -Condition (
            @($settingsHello.capabilities).Count -eq 13 -and
            @($settingsHello.capabilities) -contains
                'setting.get' -and
            @($settingsHello.capabilities) -contains
                'setting.set' -and
            @($settingsHello.capabilities) -contains
                'panel.get' -and
            @($settingsHello.capabilities) -contains
                'panel.set' -and
            @($settingsHello.capabilities) -contains
                'profile.start' -and
            @($settingsHello.capabilities) -contains
                'profile.stop') `
        -Message 'Settings workflow hello should advertise the fixed setting, panel and profile capabilities.'

    $settingsTerminals = @{}
    foreach ($message in $settingsMessages) {
        if (
            [string]$message.type -eq 'response' -and
            [string]$message.status -in @(
                'completed',
                'failed',
                'canceled')) {
            $settingsTerminals[
                [string]$message.request_id] = $message
        }
    }
    Assert-True `
        -Condition (
            $settingsTerminals.Count -eq 61 -and
            [string]$settingsTerminals['request-1'].command -eq
                'setting.get' -and
            [string]$settingsTerminals['request-1'].result.name -eq
                'ui.language' -and
            [string]$settingsTerminals['request-1'].result.value -eq
                'en' -and
            [string]$settingsTerminals['request-2'].status -eq
                'completed' -and
            [string]$settingsTerminals['request-2'].result.value -eq
                'zh-Hans' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-2'].result `
                -Name 'changed' `
                -Expected $true) -and
            [string]$settingsTerminals['request-3'].result.value -eq
                'zh-Hans' -and
            [int]$settingsTerminals['request-4'].result.value -eq
                125 -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-4'].result `
                -Name 'changed' `
                -Expected $true) -and
            [string]$settingsTerminals['request-5'].status -eq
                'completed' -and
            [int]$settingsTerminals['request-5'].result.value -eq
                125 -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-5'].result `
                -Name 'changed' `
                -Expected $false)) `
        -Message 'Supported setting reads and writes should use stable names, types and values, with a leading-plus same-value write reporting changed:false.'
    Assert-True `
        -Condition (
            [string]$settingsTerminals['request-6'].status -eq
                'failed' -and
            [string]$settingsTerminals['request-6'].error.code -eq
                'setting_type_mismatch' -and
            [string]$settingsTerminals['request-7'].status -eq
                'failed' -and
            [string]$settingsTerminals['request-7'].error.code -eq
                'setting_value_rejected' -and
            [string]$settingsTerminals['request-8'].status -eq
                'failed' -and
            [string]$settingsTerminals['request-8'].error.code -eq
                'unsupported_setting' -and
            [string]$settingsTerminals['request-27'].command -eq
                'wait.idle' -and
            [string]$settingsTerminals['request-27'].status -eq
                'completed') `
        -Message 'Invalid setting name, scalar type and value should fail stably without breaking the following idle barrier.'
    Assert-True `
        -Condition (
            [string]$settingsTerminals['request-9'].status -eq
                'completed' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-9'].state.panels `
                -Name 'files' `
                -Expected $true) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-9'].state.panels `
                -Name 'spectral_lines' `
                -Expected $true)) `
        -Message 'Initial state.get should project the isolated production panel defaults before writes.'
    for ($panelIndex = 0;
         $panelIndex -lt $panelNames.Count;
         ++$panelIndex) {
        $panelRequestId =
            'request-' + (10 + $panelIndex)
        Assert-True `
            -Condition (
                [string]$settingsTerminals[$panelRequestId].status -eq
                    'completed' -and
                [string]$settingsTerminals[$panelRequestId].result.name -eq
                    $panelNames[$panelIndex] -and
                (Test-JsonBooleanProperty `
                    -Object $settingsTerminals[$panelRequestId].result `
                    -Name 'visible' `
                    -Expected $true)) `
            -Message (
                'Initial panel.get should expose visible production state for ' +
                $panelNames[$panelIndex] + '.')
    }
    Assert-True `
        -Condition (
            [string]$settingsTerminals['request-19'].status -eq
                'completed' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-19'].result `
                -Name 'visible' `
                -Expected $false) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-19'].result `
                -Name 'changed' `
                -Expected $true) -and
            [uint64]$settingsTerminals['request-19'].result.frame_index -gt
                [uint64]$settingsTerminals['request-9'].state.runtime.frame_index -and
            [string]$settingsTerminals['request-20'].status -eq
                'completed' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-20'].result `
                -Name 'visible' `
                -Expected $false) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-20'].result `
                -Name 'changed' `
                -Expected $false) -and
            [uint64]$settingsTerminals['request-20'].result.frame_index -gt
                [uint64]$settingsTerminals['request-9'].state.runtime.frame_index -and
            [string]$settingsTerminals['request-21'].result.name -eq
                'spectral_lines' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-21'].result `
                -Name 'visible' `
                -Expected $false) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-21'].result `
                -Name 'changed' `
                -Expected $true) -and
            [uint64]$settingsTerminals['request-21'].result.frame_index -gt
                [uint64]$settingsTerminals['request-9'].state.runtime.frame_index -and
            [string]$settingsTerminals['request-22'].result.name -eq
                'spectral_lines' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-22'].result `
                -Name 'visible' `
                -Expected $false) -and
            [string]$settingsTerminals['request-23'].status -eq
                'failed' -and
            [string]$settingsTerminals['request-23'].error.code -eq
                'unsupported_panel') `
        -Message 'Panel reads and writes should use stable identifiers, wait for a presented frame and distinguish changed from unchanged writes.'
    Assert-True `
        -Condition (
            [string]$settingsTerminals['request-24'].status -eq
                'failed' -and
            [string]$settingsTerminals['request-24'].error.code -eq
                'operation_canceled' -and
            [string]$settingsTerminals['request-25'].status -eq
                'failed' -and
            [string]$settingsTerminals['request-25'].error.code -eq
                'operation_canceled' -and
            [string]$settingsTerminals['request-26'].status -eq
                'completed' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-26'].result `
                -Name 'visible' `
                -Expected $false)) `
        -Message 'Every opposite accepted panel write should permanently supersede older unpresented generations, even when a later write returns to the first value.'
    Assert-True `
        -Condition (
            [string]$settingsTerminals['request-28'].status -eq
                'completed' -and
            [string]$settingsTerminals['request-28'].result.name -eq
                'ui.scale' -and
            [int]$settingsTerminals['request-28'].result.value -eq
                125 -and
            [string]$settingsTerminals['request-29'].status -eq
                'completed' -and
            [string]$settingsTerminals['request-29'].state.settings.language -eq
                'zh-Hans' -and
            [int]$settingsTerminals['request-29'].state.settings.ui_scale_percentage -eq
                125 -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-29'].state.panels `
                -Name 'files' `
                -Expected $false) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-29'].state.panels `
                -Name 'spectral_lines' `
                -Expected $false) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-29'].state.panels `
                -Name 'navigation' `
                -Expected $false)) `
        -Message 'Later reads should reflect the last valid setting values and the final superseding panel generation.'
    Assert-True `
        -Condition (
            [string]$settingsTerminals['request-30'].status -eq
                'completed' -and
            [string]$settingsTerminals['request-31'].status -eq
                'completed' -and
            [string]$settingsTerminals['request-32'].status -eq
                'completed' -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-30'].result `
                -Name 'visible' `
                -Expected $true) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-31'].result `
                -Name 'visible' `
                -Expected $true) -and
            (Test-JsonBooleanProperty `
                -Object $settingsTerminals['request-32'].result `
                -Name 'visible' `
                -Expected $true)) `
        -Message 'Panel mapping fixture should restore the three earlier hidden panels before one-hot checks.'
    for ($panelIndex = 0;
         $panelIndex -lt $panelNames.Count;
         ++$panelIndex) {
        $hideRequestId =
            'request-' + (33 + 3 * $panelIndex)
        $stateRequestId =
            'request-' + (34 + 3 * $panelIndex)
        $showRequestId =
            'request-' + (35 + 3 * $panelIndex)
        Assert-True `
            -Condition (
                [string]$settingsTerminals[$hideRequestId].status -eq
                    'completed' -and
                [string]$settingsTerminals[$hideRequestId].result.name -eq
                    $panelNames[$panelIndex] -and
                (Test-JsonBooleanProperty `
                    -Object $settingsTerminals[$hideRequestId].result `
                    -Name 'visible' `
                    -Expected $false) -and
                (Test-JsonBooleanProperty `
                    -Object $settingsTerminals[$hideRequestId].result `
                    -Name 'changed' `
                    -Expected $true) -and
                [string]$settingsTerminals[$showRequestId].status -eq
                    'completed' -and
                [string]$settingsTerminals[$showRequestId].result.name -eq
                    $panelNames[$panelIndex] -and
                (Test-JsonBooleanProperty `
                    -Object $settingsTerminals[$showRequestId].result `
                    -Name 'visible' `
                    -Expected $true) -and
                (Test-JsonBooleanProperty `
                    -Object $settingsTerminals[$showRequestId].result `
                    -Name 'changed' `
                    -Expected $true)) `
            -Message (
                'One-hot panel mapping writes should change and restore only ' +
                $panelNames[$panelIndex] + '.')
        for ($observedPanelIndex = 0;
             $observedPanelIndex -lt $panelNames.Count;
             ++$observedPanelIndex) {
            $observedPanel =
                $panelNames[$observedPanelIndex]
            Assert-True `
                -Condition (
                    Test-JsonBooleanProperty `
                        -Object $settingsTerminals[$stateRequestId].state.panels `
                        -Name $observedPanel `
                        -Expected ($observedPanelIndex -ne $panelIndex)) `
                -Message (
                    'One-hot state projection for ' +
                    $panelNames[$panelIndex] +
                    ' should expose an exact boolean for ' +
                    $observedPanel + '.')
        }
    }
    Assert-True `
        -Condition (
            [string]$settingsTerminals['request-60'].status -eq
                'completed' -and
            [string]$settingsTerminals['request-60'].result.path -eq
                $settingsCapturePath -and
            [string]$settingsTerminals['request-61'].command -eq
                'app.quit' -and
            [string]$settingsTerminals['request-61'].status -eq
                'completed') `
        -Message 'Application capture and normal quit should remain usable after exhaustive panel mapping checks.'

    $savedLanguage =
        Get-Content -Raw -LiteralPath (
            Join-Path $settingsControlRoot 'config\ui-language.json') |
            ConvertFrom-Json
    $savedScale =
        Get-Content -Raw -LiteralPath (
            Join-Path $settingsControlRoot 'config\ui-scale.json') |
            ConvertFrom-Json
    $savedPanels =
        Get-Content -Raw -LiteralPath (
            Join-Path $settingsControlRoot 'state\panel-visibility.json') |
            ConvertFrom-Json
    Assert-True `
        -Condition (
            [string]$savedLanguage.language -eq
                'zh-Hans' -and
            [int]$savedScale.percentage -eq 125 -and
            (Test-Path `
                -LiteralPath $settingsCapturePath `
                -PathType Leaf) -and
            (Get-Item -LiteralPath $settingsCapturePath).Length -gt
                8) `
        -Message 'Settings and panel visibility should persist through production files only inside the automation state root, with invalid writes leaving the last valid value intact.'
    foreach ($panelName in $panelNames) {
        Assert-True `
            -Condition (
                Test-JsonBooleanProperty `
                    -Object $savedPanels `
                    -Name $panelName `
                    -Expected $true) `
            -Message (
                'The final production panel cache should contain an explicit true boolean for ' +
                $panelName + '.')
    }

    $windowContractRoot =
        Join-Path $fixtureParent 'window-contract-state'
    $visibleCapturePath =
        Join-Path $windowContractRoot 'captures\visible.png'
    $hiddenCapturePath =
        Join-Path $windowContractRoot 'captures\hidden.png'
    $minimizedCapturePath =
        Join-Path $windowContractRoot 'captures\minimized.png'
    $unicodeSourceRoot =
        Join-Path $fixtureParent (
            [string][char]0x6570 +
            [string][char]0x636E +
            [string][char]0x6E90)
    [System.IO.Directory]::CreateDirectory(
        $unicodeSourceRoot) | Out-Null
    $unicodeFirstSpectrum =
        [string][char]0x5149 +
        [string][char]0x8C31 +
        [string][char]0x7532 +
        '.csv'
    $unicodeSecondSpectrum =
        [string][char]0x5149 +
        [string][char]0x8C31 +
        [string][char]0x4E59 +
        '.csv'
    foreach ($name in @(
            $unicodeFirstSpectrum,
            $unicodeSecondSpectrum)) {
        [System.IO.File]::WriteAllText(
            (Join-Path $unicodeSourceRoot $name),
            "wavelength,flux`n5000,1`n5001,2`n",
            [System.Text.UTF8Encoding]::new($false))
    }
    $interactiveLauncher = $null
    $interactiveGuiPid = 0
    $profileDirectoryJunctionActive = $false
    $profileDirectoryBlockerCreated = $false
    $successfulProfileDirectoryMoved = $false
    $foregroundBefore =
        [SpecForgeAutomationWindowTestNative]::
            GetForegroundWindow()
    try {
        $start = [System.Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $resolvedLauncher
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardInput = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $utf8WithoutBom =
            [System.Text.UTF8Encoding]::new($false)
        $start.StandardOutputEncoding =
            $utf8WithoutBom
        $start.StandardErrorEncoding =
            $utf8WithoutBom
        $start.Arguments =
            '--app "' + $fixtureExecutable +
            '" --state-root "' +
            $windowContractRoot + '"'
        $interactiveLauncher =
            [System.Diagnostics.Process]::Start($start)

        $initialOutput = @()
        while ($true) {
            $line =
                Read-LauncherLine `
                    -Process $interactiveLauncher
            $initialOutput += $line
            if ($line -match '^SpecForge PID: ([0-9]+)$') {
                $interactiveGuiPid =
                    [int]$Matches[1]
            }
            if ($line -eq (
                    'Harness controls: pipeline begin ... pipeline end; ' +
                    'disconnect after accepted <next command>')) {
                break
            }
        }
        Assert-True `
            -Condition ($interactiveGuiPid -gt 0) `
            -Message 'Interactive launcher should report its owned GUI PID.'

        $windowHandle = [IntPtr]::Zero
        $windowDeadline =
            [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $windowDeadline) {
            $guiProcess =
                Get-Process `
                    -Id $interactiveGuiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $guiProcess) {
                $guiProcess.Refresh()
                $windowHandle =
                    [IntPtr]$guiProcess.MainWindowHandle
            }
            if ($windowHandle -ne [IntPtr]::Zero -and
                [SpecForgeAutomationWindowTestNative]::
                    IsWindowVisible($windowHandle)) {
                break
            }
            Start-Sleep -Milliseconds 25
        }
        $foregroundAfterLaunch =
            [SpecForgeAutomationWindowTestNative]::
                GetForegroundWindow()
        Assert-True `
            -Condition (
                $windowHandle -ne [IntPtr]::Zero -and
                [SpecForgeAutomationWindowTestNative]::
                    IsWindowVisible($windowHandle) -and
                -not [SpecForgeAutomationWindowTestNative]::
                    IsIconic($windowHandle) -and
                $foregroundAfterLaunch -ne $windowHandle -and
                ($foregroundBefore -eq [IntPtr]::Zero -or
                 $foregroundAfterLaunch -eq
                    $foregroundBefore)) `
            -Message 'The real SpecForge HWND must become visible and renderable without activation or foreground theft.'

        $movedWindowRoot =
            Join-Path $fixtureParent 'window-contract-state-moved'
        $rootMoved =
            [SpecForgeAutomationWindowTestNative]::
                MoveFile(
                    $windowContractRoot,
                    $movedWindowRoot)
        $rootMoveError =
            [Runtime.InteropServices.Marshal]::
                GetLastWin32Error()
        Assert-True `
            -Condition (
                -not $rootMoved -and
                $rootMoveError -in @(5, 32, 33) -and
                (Test-Path `
                    -LiteralPath $windowContractRoot `
                    -PathType Container) -and
                -not (Test-Path `
                    -LiteralPath $movedWindowRoot)) `
            -Message 'The launcher-owned identity lease must prevent state-root replacement for the complete GUI child lifetime.'

        $unicodeOpenMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    "source open $unicodeSourceRoot") `
                -RequestId 'request-1'
        )
        $unicodeWaitMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('wait idle') `
                -RequestId 'request-2'
        )
        $unicodeGotoMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    "spectrum goto name $unicodeSecondSpectrum") `
                -RequestId 'request-3'
        )
        $unicodeStateMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('state get') `
                -RequestId 'request-4'
        )
        $unicodeStateTerminal =
            $unicodeStateMessages[1]
        Assert-True `
            -Condition (
                [string]$unicodeOpenMessages[1].status -eq
                    'completed' -and
                [string]$unicodeWaitMessages[1].status -eq
                    'completed' -and
                [string]$unicodeGotoMessages[1].status -eq
                    'completed' -and
                [string]$unicodeGotoMessages[1].result.spectrum.name -eq
                    $unicodeSecondSpectrum -and
                [string]$unicodeStateTerminal.state.source.path -eq
                    $unicodeSourceRoot -and
                [string]$unicodeStateTerminal.state.presented_source.path -eq
                    $unicodeSourceRoot -and
                [string]$unicodeStateTerminal.state.spectrum.name -eq
                    $unicodeSecondSpectrum) `
            -Message 'Explicit UTF-8 redirected stdin must preserve a Unicode source path and exact Unicode spectrum name through the real GUI protocol.'

        $visibleCaptureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    "frame capture $visibleCapturePath") `
                -RequestId 'request-5'
        )
        $visibleCaptureTerminal =
            $visibleCaptureMessages[1]
        $visibleIhdr =
            Get-PngIhdr -Path $visibleCapturePath
        Assert-True `
            -Condition (
                [string]$visibleCaptureTerminal.status -eq
                    'completed' -and
                [uint32]$visibleIhdr.Width -eq
                    [uint32]$visibleCaptureTerminal.result.width -and
                [uint32]$visibleIhdr.Height -eq
                    [uint32]$visibleCaptureTerminal.result.height -and
                [SpecForgeAutomationWindowTestNative]::
                    GetForegroundWindow() -ne $windowHandle) `
            -Message 'A visible but non-activated real HWND should produce an application-rendered PNG whose IHDR matches its terminal.'

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 0)
        $visibilityDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            [SpecForgeAutomationWindowTestNative]::
                IsWindowVisible($windowHandle) -and
            [DateTime]::UtcNow -lt $visibilityDeadline) {
            Start-Sleep -Milliseconds 25
        }
        $hiddenCaptureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    "frame capture $hiddenCapturePath",
                    'pipeline end') `
                -RequestId 'request-6'
        )
        Assert-True `
            -Condition (
                -not [SpecForgeAutomationWindowTestNative]::
                    IsWindowVisible($windowHandle) -and
                [string]$hiddenCaptureMessages[1].status -eq
                    'failed' -and
                [string]$hiddenCaptureMessages[1].error.code -eq
                    'window_not_renderable' -and
                -not (Test-Path -LiteralPath $hiddenCapturePath) -and
                [SpecForgeAutomationWindowTestNative]::
                    GetForegroundWindow() -ne $windowHandle) `
            -Message 'A hidden real HWND must fail capture without restoring, activating, or publishing a PNG.'

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 4)
        $restoreDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            (-not [SpecForgeAutomationWindowTestNative]::
                IsWindowVisible($windowHandle) -or
             [SpecForgeAutomationWindowTestNative]::
                IsIconic($windowHandle)) -and
            [DateTime]::UtcNow -lt $restoreDeadline) {
            Start-Sleep -Milliseconds 25
        }
        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 7)
        $minimizeDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            -not [SpecForgeAutomationWindowTestNative]::
                IsIconic($windowHandle) -and
            [DateTime]::UtcNow -lt $minimizeDeadline) {
            Start-Sleep -Milliseconds 25
        }
        $minimizedCaptureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    "frame capture $minimizedCapturePath",
                    'pipeline end') `
                -RequestId 'request-7'
        )
        Assert-True `
            -Condition (
                [SpecForgeAutomationWindowTestNative]::
                    IsIconic($windowHandle) -and
                [string]$minimizedCaptureMessages[1].status -eq
                    'failed' -and
                [string]$minimizedCaptureMessages[1].error.code -eq
                    'window_not_renderable' -and
                -not (Test-Path -LiteralPath $minimizedCapturePath) -and
                [SpecForgeAutomationWindowTestNative]::
                    GetForegroundWindow() -ne $windowHandle) `
            -Message 'A minimized real HWND must fail capture without restoring, activating, or publishing a PNG.'

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 4)
        $uiScaleWriteBlocker =
            Join-Path $windowContractRoot 'config\ui-scale.json'
        [System.IO.Directory]::CreateDirectory(
            $uiScaleWriteBlocker) | Out-Null
        $failedScaleMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    'setting set ui.scale 125',
                    'pipeline end') `
                -RequestId 'request-8'
        )
        $scaleAfterFailureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    'setting get ui.scale') `
                -RequestId 'request-9'
        )
        $stateAfterScaleFailureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('state get') `
                -RequestId 'request-10'
        )
        $waitAfterScaleFailureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('wait idle') `
                -RequestId 'request-11'
        )
        $failedScaleTerminal =
            $failedScaleMessages[1]
        $scaleAfterFailureTerminal =
            $scaleAfterFailureMessages[1]
        $stateAfterScaleFailureTerminal =
            $stateAfterScaleFailureMessages[1]
        $waitAfterScaleFailureTerminal =
            $waitAfterScaleFailureMessages[1]
        $appliedScaleAfterFailure =
            [int]$stateAfterScaleFailureTerminal.
                state.settings.ui_scale_percentage
        Assert-True `
            -Condition (
                [string]$failedScaleTerminal.status -eq
                    'failed' -and
                [string]$failedScaleTerminal.error.code -eq
                    'setting_persistence_failed' -and
                [string]$scaleAfterFailureTerminal.status -eq
                    'completed' -and
                [int]$scaleAfterFailureTerminal.result.value -eq
                    100 -and
                $appliedScaleAfterFailure -eq 100 -and
                [string]$waitAfterScaleFailureTerminal.status -eq
                    'completed' -and
                (Test-Path `
                    -LiteralPath $uiScaleWriteBlocker `
                    -PathType Container)) `
            -Message 'A production UI-scale write failure must map to setting_persistence_failed, retain the old model and live UI value, emit no setting notification, and preserve idle-barrier semantics.'
        [System.IO.Directory]::Delete(
            $uiScaleWriteBlocker)
        Start-Sleep -Milliseconds 5000

        $profileDirectoryBlocker =
            Join-Path $windowContractRoot 'logs'
        $successfulProfileDirectory =
            Join-Path $windowContractRoot 'logs-successful'
        $externalProfileDirectory =
            Join-Path $fixtureParent 'outside-profile-directory'
        [System.IO.Directory]::CreateDirectory(
            $externalProfileDirectory) | Out-Null
        $externalProfileSentinel =
            Join-Path $externalProfileDirectory 'sentinel.txt'
        [System.IO.File]::WriteAllText(
            $externalProfileSentinel,
            'must remain unchanged by rejected profile starts',
            [System.Text.UTF8Encoding]::new($false))
        $externalProfileFingerprint =
            Get-TreeFingerprint -Path $externalProfileDirectory

        New-TestDirectoryJunction `
            -Path $profileDirectoryBlocker `
            -Target $externalProfileDirectory
        $profileDirectoryJunctionActive = $true
        $initialOutsideProfileBatch = @(
            Send-InteractiveLauncherBatch `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    'profile start',
                    'state get',
                    'pipeline end') `
                -ExpectedRequestIds @(
                    'request-12',
                    'request-13')
        )
        $initialOutsideProfileStart =
            Get-ProtocolMessage `
                -Messages $initialOutsideProfileBatch `
                -RequestId 'request-12' `
                -Status 'failed'
        $initialOutsideProfileState =
            Get-ProtocolMessage `
                -Messages $initialOutsideProfileBatch `
                -RequestId 'request-13' `
                -Status 'completed'
        $initialOutsideProfilePath =
            $initialOutsideProfileState.state.profile.
                PSObject.Properties['path']
        Assert-True `
            -Condition (
                [string]$initialOutsideProfileStart.error.code -eq
                    'profile_output_outside_state_root' -and
                [string]$initialOutsideProfileState.state.profile.status -eq
                    'failed' -and
                [string]$initialOutsideProfileState.state.profile.stop_reason -eq
                    'none' -and
                [uint64]$initialOutsideProfileState.state.profile.dropped_events -eq
                    0 -and
                [int]$initialOutsideProfileState.state.settings.
                    ui_scale_percentage -eq 100 -and
                $null -eq $initialOutsideProfilePath -and
                (Get-TreeFingerprint -Path $externalProfileDirectory) -eq
                    $externalProfileFingerprint) `
            -Message 'An initial production profile resolver outside the isolated root must publish a fresh failed/none/0/no-path state without changing the external directory, while a repaired UI-scale path must not publish the earlier failed setting after its retry deadline.'
        Remove-TestDirectoryJunction `
            -Path $profileDirectoryBlocker
        $profileDirectoryJunctionActive = $false

        $successfulProfileStartMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('profile start') `
                -RequestId 'request-14'
        )
        $successfulProfileStopMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('profile stop') `
                -RequestId 'request-15'
        )
        $successfulProfilePath =
            [string]$successfulProfileStopMessages[1].result.path
        $successfulProfileCompleted =
            Test-Path `
                -LiteralPath $successfulProfilePath `
                -PathType Leaf
        [System.IO.Directory]::Move(
            $profileDirectoryBlocker,
            $successfulProfileDirectory)
        $successfulProfileDirectoryMoved = $true
        New-TestDirectoryJunction `
            -Path $profileDirectoryBlocker `
            -Target $externalProfileDirectory
        $profileDirectoryJunctionActive = $true
        $outsideRestartBatchMessages = @(
            Send-InteractiveLauncherBatch `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    'profile start',
                    'state get',
                    'pipeline end') `
                -ExpectedRequestIds @(
                    'request-16',
                    'request-17')
        )
        $outsideProfileRestart =
            Get-ProtocolMessage `
                -Messages $outsideRestartBatchMessages `
                -RequestId 'request-16' `
                -Status 'failed'
        $sameBatchStateAfterOutsideFailure =
            Get-ProtocolMessage `
                -Messages $outsideRestartBatchMessages `
                -RequestId 'request-17' `
                -Status 'completed'
        $laterStateAfterOutsideFailureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('state get') `
                -RequestId 'request-18'
        )
        $laterStateAfterOutsideFailure =
            $laterStateAfterOutsideFailureMessages[1]
        $sameBatchOutsideFailedPath =
            $sameBatchStateAfterOutsideFailure.state.profile.
                PSObject.Properties['path']
        $laterOutsideFailedPath =
            $laterStateAfterOutsideFailure.state.profile.
                PSObject.Properties['path']
        Assert-True `
            -Condition (
                [string]$successfulProfileStartMessages[1].status -eq
                    'completed' -and
                [string]$successfulProfileStopMessages[1].status -eq
                    'completed' -and
                $successfulProfileCompleted -and
                [string]$outsideProfileRestart.error.code -eq
                    'profile_output_outside_state_root' -and
                [string]$sameBatchStateAfterOutsideFailure.state.profile.status -eq
                    'failed' -and
                [string]$sameBatchStateAfterOutsideFailure.state.profile.stop_reason -eq
                    'none' -and
                [uint64]$sameBatchStateAfterOutsideFailure.state.profile.dropped_events -eq
                    0 -and
                $null -eq $sameBatchOutsideFailedPath -and
                [string]$laterStateAfterOutsideFailure.state.profile.status -eq
                    'failed' -and
                [string]$laterStateAfterOutsideFailure.state.profile.stop_reason -eq
                    'none' -and
                [uint64]$laterStateAfterOutsideFailure.state.profile.dropped_events -eq
                    0 -and
                $null -eq $laterOutsideFailedPath -and
                (Get-TreeFingerprint -Path $externalProfileDirectory) -eq
                    $externalProfileFingerprint) `
            -Message (
                'A production resolver outside the isolated root after a successful recording must replace the prior terminal with failed/none/0/no-path and remain stable across later refreshes. Same batch: ' +
                ($sameBatchStateAfterOutsideFailure.state.profile |
                    ConvertTo-Json -Compress -Depth 8) +
                '; later: ' +
                ($laterStateAfterOutsideFailure.state.profile |
                    ConvertTo-Json -Compress -Depth 8))
        Remove-TestDirectoryJunction `
            -Path $profileDirectoryBlocker
        $profileDirectoryJunctionActive = $false

        [System.IO.File]::WriteAllText(
            $profileDirectoryBlocker,
            'blocks the production profile directory',
            [System.Text.UTF8Encoding]::new($false))
        $profileDirectoryBlockerCreated = $true
        $failedRestartBatchMessages = @(
            Send-InteractiveLauncherBatch `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    'profile start',
                    'state get',
                    'pipeline end') `
                -ExpectedRequestIds @(
                    'request-19',
                    'request-20')
        )
        $failedProfileRestart =
            Get-ProtocolMessage `
                -Messages $failedRestartBatchMessages `
                -RequestId 'request-19' `
                -Status 'failed'
        $sameBatchStateAfterFailure =
            Get-ProtocolMessage `
                -Messages $failedRestartBatchMessages `
                -RequestId 'request-20' `
                -Status 'completed'
        $laterStateAfterFailureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('state get') `
                -RequestId 'request-21'
        )
        $laterStateAfterFailure =
            $laterStateAfterFailureMessages[1]
        $sameBatchFailedPath =
            $sameBatchStateAfterFailure.state.profile.
                PSObject.Properties['path']
        $laterFailedPath =
            $laterStateAfterFailure.state.profile.
                PSObject.Properties['path']
        Assert-True `
            -Condition (
                [string]$successfulProfileStartMessages[1].status -eq
                    'completed' -and
                [string]$successfulProfileStopMessages[1].status -eq
                    'completed' -and
                $successfulProfileCompleted -and
                [string]$failedProfileRestart.status -eq
                    'failed' -and
                [string]$failedProfileRestart.error.code -eq
                    'profile_start_failed' -and
                [string]$sameBatchStateAfterFailure.state.profile.status -eq
                    'failed' -and
                [string]$sameBatchStateAfterFailure.state.profile.stop_reason -eq
                    'none' -and
                [uint64]$sameBatchStateAfterFailure.state.profile.dropped_events -eq
                    0 -and
                $null -eq $sameBatchFailedPath -and
                [string]$laterStateAfterFailure.state.profile.status -eq
                    'failed' -and
                [string]$laterStateAfterFailure.state.profile.stop_reason -eq
                    'none' -and
                [uint64]$laterStateAfterFailure.state.profile.dropped_events -eq
                    0 -and
                $null -eq $laterFailedPath -and
                (Test-Path `
                    -LiteralPath $profileDirectoryBlocker `
                    -PathType Leaf)) `
            -Message (
                'A blocked restart after a successful recording must own a fresh failed state without inheriting or later restoring the previous path, stop reason, dropped count or success terminal. Same batch: ' +
                ($sameBatchStateAfterFailure.state.profile |
                    ConvertTo-Json -Compress -Depth 8) +
                '; later: ' +
                ($laterStateAfterFailure.state.profile |
                    ConvertTo-Json -Compress -Depth 8))
        [System.IO.File]::Delete(
            $profileDirectoryBlocker)
        $profileDirectoryBlockerCreated = $false
        [System.IO.Directory]::Move(
            $successfulProfileDirectory,
            $profileDirectoryBlocker)
        $successfulProfileDirectoryMoved = $false

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 7)
        $titleMinimizeDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            -not [SpecForgeAutomationWindowTestNative]::
                IsIconic($windowHandle) -and
            [DateTime]::UtcNow -lt
                $titleMinimizeDeadline) {
            Start-Sleep -Milliseconds 25
        }
        $titleOpenAccepted =
            Begin-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Line "source open $sourceRoot" `
                -RequestId 'request-22'
        $expectedTitleSuffix =
            ' | source-fixture | 1/3 | alpha.csv'
        $minimizedTitle = ''
        $titleDeadline =
            [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $titleDeadline) {
            $minimizedTitle =
                [SpecForgeAutomationWindowTestNative]::
                    WindowTitle($windowHandle)
            if ($minimizedTitle.EndsWith(
                    $expectedTitleSuffix,
                    [StringComparison]::Ordinal)) {
                break
            }
            Start-Sleep -Milliseconds 25
        }
        Assert-True `
            -Condition (
                [string]$titleOpenAccepted.status -eq
                    'accepted' -and
                [SpecForgeAutomationWindowTestNative]::
                    IsIconic($windowHandle) -and
                $minimizedTitle.EndsWith(
                    $expectedTitleSuffix,
                    [StringComparison]::Ordinal)) `
            -Message (
                'A minimized HWND must reconcile a completed source load into its taskbar/Alt-Tab title before rendering resumes. Actual title: ' +
                $minimizedTitle)

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 4)
        $titleRestoreDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            [SpecForgeAutomationWindowTestNative]::
                IsIconic($windowHandle) -and
            [DateTime]::UtcNow -lt
                $titleRestoreDeadline) {
            Start-Sleep -Milliseconds 25
        }
        $titleOpenTerminal =
            Complete-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -RequestId 'request-22'
        Assert-True `
            -Condition (
                [string]$titleOpenTerminal.status -eq
                    'completed') `
            -Message (
                'The minimized title source.open should complete after rendering resumes.')

        $quitMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('app quit') `
                -RequestId 'request-23'
        )
        Assert-True `
            -Condition (
                [string]$quitMessages[1].status -eq
                    'completed' -and
                $interactiveLauncher.WaitForExit(15000) -and
                $interactiveLauncher.ExitCode -eq 0) `
            -Message 'Interactive HWND contract workflow should quit normally.'
        Assert-True `
            -Condition (
                $null -eq (
                    Get-Process `
                        -Id $interactiveGuiPid `
                        -ErrorAction SilentlyContinue)) `
            -Message 'Interactive HWND contract workflow should leave no GUI process.'
        Assert-True `
            -Condition (
                -not (Test-Path -LiteralPath (
                    Join-Path $windowContractRoot (
                        '.specforge-automation-root.lock')))) `
            -Message 'The launcher must release and remove its state-root identity lock after the owned GUI exits.'
    }
    finally {
        if ($profileDirectoryJunctionActive -and
            (Test-Path -LiteralPath $profileDirectoryBlocker)) {
            Remove-TestDirectoryJunction `
                -Path $profileDirectoryBlocker
            $profileDirectoryJunctionActive = $false
        }
        if ($profileDirectoryBlockerCreated -and
            (Test-Path `
                -LiteralPath $profileDirectoryBlocker `
                -PathType Leaf)) {
            [System.IO.File]::Delete(
                $profileDirectoryBlocker)
            $profileDirectoryBlockerCreated = $false
        }
        if ($successfulProfileDirectoryMoved -and
            (Test-Path `
                -LiteralPath $successfulProfileDirectory `
                -PathType Container) -and
            -not (Test-Path -LiteralPath $profileDirectoryBlocker)) {
            [System.IO.Directory]::Move(
                $successfulProfileDirectory,
                $profileDirectoryBlocker)
            $successfulProfileDirectoryMoved = $false
        }
        if ($null -ne $interactiveLauncher) {
            if (-not $interactiveLauncher.HasExited) {
                $interactiveLauncher.StandardInput.Close()
                if (-not $interactiveLauncher.WaitForExit(3000)) {
                    $interactiveLauncher.Kill()
                    [void]$interactiveLauncher.WaitForExit(3000)
                }
            }
            $interactiveLauncher.Dispose()
        }
        if ($interactiveGuiPid -gt 0) {
            $ownedGui =
                Get-Process `
                    -Id $interactiveGuiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $ownedGui) {
                [void]$ownedGui.CloseMainWindow()
                if (-not $ownedGui.WaitForExit(3000)) {
                    Stop-Process `
                        -Id $interactiveGuiPid `
                        -Force `
                        -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Invoke-UnrenderablePanelQuitScenario `
        -LauncherPath $resolvedLauncher `
        -AppPath $fixtureExecutable `
        -Root (Join-Path $fixtureParent 'hidden-panel-quit-state') `
        -Mode 'hidden'
    Invoke-UnrenderablePanelQuitScenario `
        -LauncherPath $resolvedLauncher `
        -AppPath $fixtureExecutable `
        -Root (Join-Path $fixtureParent 'minimized-panel-quit-state') `
        -Mode 'minimized'
    Invoke-UnrenderablePanelQuitScenario `
        -LauncherPath $resolvedLauncher `
        -AppPath $fixtureExecutable `
        -Root (Join-Path $fixtureParent 'immersive-panel-quit-state') `
        -Mode 'immersive'
    Invoke-PanelShutdownRollbackScenario `
        -LauncherPath $resolvedLauncher `
        -AppPath $fixtureExecutable `
        -Root (Join-Path $fixtureParent 'panel-shutdown-rollback-state')

    $repeatStateRoot =
        Join-Path $fixtureParent 'state-repeat'
    $repeatCapturePath =
        Join-Path $repeatStateRoot 'repeat\labeled.png'
    $repeatOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'spectrum goto 2',
            'label assign 5 spectrum 1',
            'spectrum goto 1',
            'wait idle',
            "frame capture $repeatCapturePath",
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $repeatStateRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $repeatExitCode = $LASTEXITCODE
    if ($repeatExitCode -ne 0) {
        throw (
            "Repeated automation workflow exited with $repeatExitCode.`n" +
            ($repeatOutput -join [Environment]::NewLine))
    }
    $repeatMessages = @(
        $repeatOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $repeatState = $repeatMessages[16]
    Assert-True `
        -Condition (
            $repeatMessages.Count -eq 19 -and
            [string]$repeatMessages[8].status -eq 'completed' -and
            [int]$repeatMessages[8].result.assignment.spectrum.index -eq 1 -and
            [int]$repeatState.state.spectrum.index -eq 1 -and
            [int]$repeatState.state.labeling.current_spectrum_label.code -eq 5 -and
            (Test-Path -LiteralPath $repeatCapturePath -PathType Leaf) -and
            (Get-Item -LiteralPath $repeatCapturePath).Length -gt 8) `
        -Message 'The complete isolated GUI workflow should be stable on a second fresh root.'
    $repeatPid =
        Get-LauncherProcessId -Output $repeatOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $repeatPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Repeated workflow should leave no GUI process.'

    $signedIndexRoot =
        Join-Path $fixtureParent 'signed-index-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $signedIndexOutput = @(
            @(
                'spectrum goto -1',
                'label assign 5 spectrum -1',
                'app quit'
            ) |
                & $resolvedLauncher `
                    --app $fixtureExecutable `
                    --state-root $signedIndexRoot 2>&1
        )
        $signedIndexExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $signedIndexMessages = @(
        $signedIndexOutput |
            Where-Object {
                [string]$_ -match '^\{'
            } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $signedIndexPid =
        Get-LauncherProcessId `
            -Output $signedIndexOutput
    Assert-True `
        -Condition (
            $signedIndexExitCode -eq 0 -and
            $signedIndexMessages.Count -eq 3 -and
            [string]$signedIndexMessages[1].command -eq
                'app.quit' -and
            [string]$signedIndexMessages[2].status -eq
                'completed' -and
            @(
                $signedIndexOutput |
                    Where-Object {
                        [string]$_ -match
                            'must be an unsigned decimal integer'
                    }
            ).Count -eq 2 -and
            $null -eq (
                Get-Process `
                    -Id $signedIndexPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'CLI index parsing must reject signed spectrum targets locally without consuming request IDs, while the owned GUI still quits normally.'

    $sourceRootB =
        Join-Path $fixtureParent 'source-fixture-b'
    [System.IO.Directory]::CreateDirectory(
        $sourceRootB) | Out-Null
    foreach ($name in @('delta.csv', 'echo.csv')) {
        [System.IO.File]::WriteAllText(
            (Join-Path $sourceRootB $name),
            "wavelength,flux`n6000,3`n6001,4`n",
            [System.Text.UTF8Encoding]::new($false))
    }

    $concurrentRoot =
        Join-Path $fixtureParent 'concurrent-open-state'
    $concurrentOutput = @(
        @(
            'pipeline begin',
            "source open $sourceRoot",
            "source open $sourceRootB",
            'wait idle',
            'pipeline end',
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $concurrentRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $concurrentExitCode = $LASTEXITCODE
    if ($concurrentExitCode -ne 0) {
        throw (
            "Concurrent source workflow exited with $concurrentExitCode.`n" +
            ($concurrentOutput -join [Environment]::NewLine))
    }
    $concurrentMessages = @(
        $concurrentOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $firstOpenCanceled =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-1' `
            -Status 'failed'
    $secondOpenCompleted =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    $concurrentWaitCompleted =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $concurrentState =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    Assert-True `
        -Condition (
            [string]$firstOpenCanceled.error.code -eq
                'operation_canceled' -and
            [string]$secondOpenCompleted.result.source.path -eq
                $sourceRootB -and
            [string]$concurrentState.state.source.path -eq
                $sourceRootB -and
            [string]$concurrentWaitCompleted.command -eq
                'wait.idle') `
        -Message 'Two pipelined source opens should leave the older request terminal canceled, activate the newer source, and release wait.idle.'
    $concurrentPid =
        Get-LauncherProcessId -Output $concurrentOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $concurrentPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Concurrent source-open workflow should leave no GUI process.'

    $earlierBarrierRoot =
        Join-Path $fixtureParent 'earlier-wait-barrier-state'
    $earlierBarrierOutput = @(
        @(
            'pipeline begin',
            "source open $sourceRoot",
            'wait idle',
            "source open $sourceRootB",
            'pipeline end',
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $earlierBarrierRoot 2>&1
    )
    if ($LASTEXITCODE -ne 0) {
        throw (
            "Earlier-only wait barrier exited with $LASTEXITCODE.`n" +
            ($earlierBarrierOutput -join [Environment]::NewLine))
    }
    $earlierBarrierMessages = @(
        $earlierBarrierOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $earlierWaitTerminal =
        Get-ProtocolMessage `
            -Messages $earlierBarrierMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    $laterOpenTerminal =
        Get-ProtocolMessage `
            -Messages $earlierBarrierMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $earlierBarrierState =
        Get-ProtocolMessage `
            -Messages $earlierBarrierMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    Assert-True `
        -Condition (
            [array]::IndexOf(
                $earlierBarrierMessages,
                $earlierWaitTerminal) -lt
                [array]::IndexOf(
                    $earlierBarrierMessages,
                    $laterOpenTerminal) -and
            [string]$earlierBarrierState.state.source.path -eq
                $sourceRootB) `
        -Message 'wait.idle must complete after its earlier sequence retires without waiting for a later accepted source.open.'
    $earlierBarrierPid =
        Get-LauncherProcessId -Output $earlierBarrierOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $earlierBarrierPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Earlier-only wait barrier should leave no GUI process.'

    $captureStateRoot =
        Join-Path $fixtureParent 'capture-state'
    $captureStatePath =
        Join-Path $captureStateRoot 'captures\projection.png'
    $captureBusyPath =
        Join-Path $captureStateRoot 'captures\busy.png'
    $captureStateOutput = @(
        @(
            'pipeline begin',
            "frame capture $captureStatePath",
            "frame capture $captureBusyPath",
            'state get',
            'pipeline end',
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $captureStateRoot 2>&1
    )
    $captureStateExitCode = $LASTEXITCODE
    if ($captureStateExitCode -ne 0) {
        throw (
            "Capture state workflow exited with $captureStateExitCode.`n" +
            ($captureStateOutput -join [Environment]::NewLine))
    }
    $captureStateMessages = @(
        $captureStateOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $capturePendingState =
        Get-ProtocolMessage `
            -Messages $captureStateMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $captureSucceededState =
        Get-ProtocolMessage `
            -Messages $captureStateMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    $captureBusyTerminal =
        Get-ProtocolMessage `
            -Messages $captureStateMessages `
            -RequestId 'request-2' `
            -Status 'failed'
    Assert-True `
        -Condition (
            [string]$captureBusyTerminal.error.code -eq
                'capture_busy' -and
            [bool]$capturePendingState.state.capture.pending -and
            [string]$capturePendingState.state.capture.current_path -eq
                $captureStatePath -and
            [string]$capturePendingState.state.capture.last_result -eq
                'failed' -and
            [string]$capturePendingState.state.capture.last_path -eq
                $captureBusyPath -and
            -not [bool]$captureSucceededState.state.capture.pending -and
            [string]$captureSucceededState.state.capture.last_result -eq
                'succeeded' -and
            [string]$captureSucceededState.state.capture.last_path -eq
                $captureStatePath -and
            (Test-Path -LiteralPath $captureStatePath -PathType Leaf) -and
            (Get-Item -LiteralPath $captureStatePath).Length -gt 8 -and
            -not (Test-Path -LiteralPath $captureBusyPath)) `
        -Message 'capture_busy should update the last failed attempt while preserving the active pending path, followed by the first capture success.'

    $captureFailureRoot =
        Join-Path $fixtureParent 'capture-failure-state'
    $captureFailurePath =
        Join-Path $fixtureParent 'capture-failure-outside.png'
    $captureFailureOutput = @(
        @(
            'pipeline begin',
            "frame capture $captureFailurePath",
            'state get',
            'pipeline end',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $captureFailureRoot 2>&1
    )
    $captureFailureExitCode = $LASTEXITCODE
    if ($captureFailureExitCode -ne 0) {
        throw (
            "Capture failure projection exited with $captureFailureExitCode.`n" +
            ($captureFailureOutput -join [Environment]::NewLine))
    }
    $captureFailureMessages = @(
        $captureFailureOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $captureRejected =
        Get-ProtocolMessage `
            -Messages $captureFailureMessages `
            -RequestId 'request-1' `
            -Status 'failed'
    $captureFailedState =
        Get-ProtocolMessage `
            -Messages $captureFailureMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    Assert-True `
        -Condition (
            [string]$captureRejected.error.code -eq
                'capture_path_outside_state_root' -and
            -not [bool]$captureFailedState.state.capture.pending -and
            [string]$captureFailedState.state.capture.last_result -eq
                'failed' -and
            [string]$captureFailedState.state.capture.last_path -eq
                $captureFailurePath -and
            -not (Test-Path -LiteralPath $captureFailurePath)) `
        -Message 'state.capture should distinguish a stable failed terminal attempt without creating the rejected file.'

    $disconnectCaptureRoot =
        Join-Path $fixtureParent 'capture-disconnect-state'
    $disconnectCapturePath =
        Join-Path $disconnectCaptureRoot 'captures\disconnected.png'
    $disconnectCaptureOutput = @(
        @(
            'disconnect after accepted',
            "frame capture $disconnectCapturePath"
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $disconnectCaptureRoot 2>&1
    )
    $disconnectCaptureExitCode = $LASTEXITCODE
    if ($disconnectCaptureExitCode -ne 0) {
        throw (
            "Accepted capture disconnect exited with $disconnectCaptureExitCode.`n" +
            ($disconnectCaptureOutput -join [Environment]::NewLine))
    }
    $disconnectCaptureMessages = @(
        $disconnectCaptureOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $disconnectCaptureAccepted =
        Get-ProtocolMessage `
            -Messages $disconnectCaptureMessages `
            -RequestId 'request-1' `
            -Status 'accepted'
    $disconnectCapturePid =
        Get-LauncherProcessId -Output $disconnectCaptureOutput
    $disconnectTemporaryArtifacts = @(
        Get-ChildItem `
            -LiteralPath (
                Split-Path `
                    -Parent `
                    $disconnectCapturePath) `
            -Filter '.specforge-capture-*.tmp' `
            -Force `
            -ErrorAction SilentlyContinue
    )
    Assert-True `
        -Condition (
            [string]$disconnectCaptureAccepted.command -eq
                'frame.capture' -and
            -not (Test-Path -LiteralPath $disconnectCapturePath) -and
            -not (Test-Path -LiteralPath ($disconnectCapturePath + '.tmp')) -and
            $disconnectTemporaryArtifacts.Count -eq 0 -and
            $null -eq (
                Get-Process `
                    -Id $disconnectCapturePid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'A capture disconnected after acceptance must not encode a PNG or resurrect after launcher cleanup.'

    $canceledBusinessRoot =
        Join-Path $fixtureParent 'canceled-business-state'
    $canceledBusinessOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'spectrum goto 2',
            "source open $sourceRootB",
            'wait idle',
            'pipeline end',
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'label assign 5 spectrum 2',
            "source open $sourceRootB",
            'wait idle',
            'pipeline end',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $canceledBusinessRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $canceledBusinessExitCode = $LASTEXITCODE
    if ($canceledBusinessExitCode -ne 0) {
        throw (
            "Canceled business workflow exited with $canceledBusinessExitCode.`n" +
            ($canceledBusinessOutput -join [Environment]::NewLine))
    }
    $canceledBusinessMessages = @(
        $canceledBusinessOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $gotoCanceled =
        Get-ProtocolMessage `
            -Messages $canceledBusinessMessages `
            -RequestId 'request-3' `
            -Status 'failed'
    $labelCanceled =
        Get-ProtocolMessage `
            -Messages $canceledBusinessMessages `
            -RequestId 'request-8' `
            -Status 'failed'
    $canceledPersisted =
        Get-Content -Raw -LiteralPath (
            Join-Path $canceledBusinessRoot 'unsaved\sample-labeling-drafts.json') |
            ConvertFrom-Json
    $canceledQualityTask = @(
        $canceledPersisted.sources |
            ForEach-Object { $_.draft } |
            Where-Object {
                [string]$_.task_id -eq $labelingTaskId
            }
    )[0]
    Assert-True `
        -Condition (
            [string]$gotoCanceled.error.code -eq
                'operation_canceled' -and
            [string]$labelCanceled.error.code -eq
                'operation_canceled' -and
            [int]$canceledQualityTask.values[2] -eq
                -1) `
        -Message 'Changed goto and targeted label should inherit operation_canceled when a newer source activation wins, without writing the label.'

    $sameActivationRoot =
        Join-Path $fixtureParent 'same-activation-state'
    $sameActivationOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'spectrum goto 2',
            'wait idle',
            'pipeline begin',
            'spectrum goto 2',
            "source open $sourceRoot",
            'wait idle',
            'pipeline end',
            'pipeline begin',
            'label assign 5 spectrum 2',
            "source open $sourceRoot",
            'wait idle',
            'pipeline end',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $sameActivationRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $sameActivationExitCode = $LASTEXITCODE
    if ($sameActivationExitCode -ne 0) {
        throw (
            "Same-activation workflow exited with $sameActivationExitCode.`n" +
            ($sameActivationOutput -join [Environment]::NewLine))
    }
    $sameActivationMessages = @(
        $sameActivationOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $sameGotoTerminal =
        Get-ProtocolTerminalMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-5'
    $sameGotoLatestOpen =
        Get-ProtocolMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-6' `
            -Status 'completed'
    $sameLabelTerminal =
        Get-ProtocolTerminalMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-8'
    $sameLabelLatestOpen =
        Get-ProtocolMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-9' `
            -Status 'completed'
    # No accepted mutation need not create the new owners during read-only import.
    $sameCheckpoint = Join-Path $sameActivationRoot 'unsaved\sample-labeling-drafts.json'
    if (Test-Path -LiteralPath $sameCheckpoint) {
        $sameActivationPersisted = Get-Content -Raw -LiteralPath $sameCheckpoint | ConvertFrom-Json
        $sameActivationQualityTask = @($sameActivationPersisted.sources | ForEach-Object { $_.draft } |
            Where-Object { [string]$_.task_id -eq $labelingTaskId })[0]
    } else {
        Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $sameActivationRoot 'state\sample-labeling-state.json'))) `
            -Message 'An imported draft must not lose its checkpoint after owner cutover.'
        $sameActivationPersisted = Get-Content -Raw -LiteralPath (Join-Path $sameActivationRoot 'sample-labeling-tasks.json') | ConvertFrom-Json
        $sameActivationQualityTask = @($sameActivationPersisted.sources | ForEach-Object { $_.tasks } |
            Where-Object { [string]$_.task_id -eq $labelingTaskId })[0]
    }
    $sameGotoTerminalIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameGotoTerminal)
    $sameGotoLatestOpenIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameGotoLatestOpen)
    $sameLabelTerminalIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameLabelTerminal)
    $sameLabelLatestOpenIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameLabelLatestOpen)
    $sameGotoCanceled =
        [string]$sameGotoTerminal.status -eq 'failed' -and
        [string]$sameGotoTerminal.error.code -eq
            'operation_canceled'
    $sameGotoCompletedFirst =
        [string]$sameGotoTerminal.status -eq 'completed' -and
        $sameGotoTerminalIndex -lt
            $sameGotoLatestOpenIndex -and
        [int]$sameGotoTerminal.result.spectrum.index -eq
            2
    $sameLabelCanceled =
        [string]$sameLabelTerminal.status -eq 'failed' -and
        [string]$sameLabelTerminal.error.code -eq
            'operation_canceled' -and
        [int]$sameActivationQualityTask.values[2] -eq
            -1
    $sameLabelCompletedFirst =
        [string]$sameLabelTerminal.status -eq 'completed' -and
        $sameLabelTerminalIndex -lt
            $sameLabelLatestOpenIndex -and
        [int]$sameLabelTerminal.result.assignment.spectrum.index -eq
            2 -and
        [int]$sameLabelTerminal.result.assignment.new_code -eq
            5 -and
        [int]$sameActivationQualityTask.values[2] -eq
            5
    Assert-True `
        -Condition (
            ($sameGotoCanceled -or
             $sameGotoCompletedFirst) -and
            [string]$sameGotoLatestOpen.result.source.path -eq
                $sourceRoot -and
            [int]$sameGotoLatestOpen.result.current_spectrum.index -eq
                2 -and
            ($sameLabelCanceled -or
             $sameLabelCompletedFirst) -and
            [string]$sameLabelLatestOpen.result.source.path -eq
                $sourceRoot -and
            [int]$sameLabelLatestOpen.result.current_spectrum.index -eq
                2) `
        -Message (
            'A newer same-path activation landing on the same row must either cancel an operation it supersedes or follow an already completed factual terminal; a canceled label cannot write and a written label must complete before replacement. Output: ' +
            ($sameActivationOutput -join [Environment]::NewLine))
    $sameActivationPid =
        Get-LauncherProcessId -Output $sameActivationOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $sameActivationPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Same-activation generation workflow should leave no GUI process.'

    $labelThenQuitRoot =
        Join-Path $fixtureParent 'label-then-quit-state'
    $labelThenQuitOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'label assign 5',
            'app quit',
            'pipeline end'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $labelThenQuitRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $labelThenQuitExitCode = $LASTEXITCODE
    if ($labelThenQuitExitCode -ne 0) {
        throw (
            "Label-then-quit workflow exited with $labelThenQuitExitCode.`n" +
            ($labelThenQuitOutput -join [Environment]::NewLine))
    }
    $labelThenQuitMessages = @(
        $labelThenQuitOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $claimedLabelTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenQuitMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $waitingQuitTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenQuitMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    $claimedLabelIndex =
        [array]::IndexOf(
            $labelThenQuitMessages,
            $claimedLabelTerminal)
    $waitingQuitIndex =
        [array]::IndexOf(
            $labelThenQuitMessages,
            $waitingQuitTerminal)
    $labelThenQuitPersisted =
        Get-Content -Raw -LiteralPath (
            Join-Path $labelThenQuitRoot 'unsaved\sample-labeling-drafts.json') |
            ConvertFrom-Json
    Assert-True `
        -Condition (
            $claimedLabelIndex -ge 0 -and
            $claimedLabelIndex -lt $waitingQuitIndex -and
            [int]$claimedLabelTerminal.result.assignment.new_code -eq
                5 -and
            [int]$labelThenQuitPersisted.sources[0].draft.values[0] -eq
                5) `
        -Message 'A label write that occurs must report completed before the later app.quit terminal and persist the same fact.'

    $labelThenReplaceRoot =
        Join-Path $fixtureParent 'label-then-replace-state'
    $labelThenReplaceOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'label assign 5',
            "source open $sourceRootB",
            'app quit',
            'pipeline end'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $labelThenReplaceRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $labelThenReplaceExitCode = $LASTEXITCODE
    if ($labelThenReplaceExitCode -ne 0) {
        throw (
            "Label-then-replace workflow exited with $labelThenReplaceExitCode.`n" +
            ($labelThenReplaceOutput -join [Environment]::NewLine))
    }
    $labelThenReplaceMessages = @(
        $labelThenReplaceOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $labelThenReplaceInitialOpen =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-1' `
            -Status 'completed'
    $writtenLabelTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $replacementSourceTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    $replacementQuitTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-5' `
            -Status 'completed'
    $writtenLabelIndex =
        [array]::IndexOf(
            $labelThenReplaceMessages,
            $writtenLabelTerminal)
    $replacementQuitIndex =
        [array]::IndexOf(
            $labelThenReplaceMessages,
            $replacementQuitTerminal)
    $labelThenReplacePersisted =
        Get-Content -Raw -LiteralPath (
            Join-Path $labelThenReplaceRoot 'unsaved\sample-labeling-drafts.json') |
            ConvertFrom-Json
    $labelThenReplaceQualityTask = @(
        $labelThenReplacePersisted.sources |
            ForEach-Object { $_.draft } |
            Where-Object {
                [string]$_.task_id -eq $labelingTaskId
            }
    )[0]
    Assert-True `
        -Condition (
            $writtenLabelIndex -ge 0 -and
            $writtenLabelIndex -lt $replacementQuitIndex -and
            [string]$writtenLabelTerminal.result.assignment.source_id -eq
                [string]$labelThenReplaceInitialOpen.result.source.id -and
            [int]$writtenLabelTerminal.result.assignment.spectrum.index -eq
                0 -and
            [int]$writtenLabelTerminal.result.assignment.previous_code -eq
                -1 -and
            [int]$writtenLabelTerminal.result.assignment.new_code -eq
                5 -and
            [string]$replacementSourceTerminal.result.source.path -eq
                $sourceRootB -and
            [int]$labelThenReplaceQualityTask.values[0] -eq
                5) `
        -Message (
            'A label already written must report its factual completed terminal before app.quit even when a later source activation replaces its pending Present. Output: ' +
            ($labelThenReplaceOutput -join [Environment]::NewLine))
    $labelThenReplacePid =
        Get-LauncherProcessId -Output $labelThenReplaceOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $labelThenReplacePid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Label-then-replace workflow should leave no GUI process.'

    $malformedRoot =
        Join-Path $fixtureParent 'malformed-source'
    [System.IO.Directory]::CreateDirectory(
        $malformedRoot) | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $malformedRoot 'alpha.csv'),
        "wavelength,flux`n5000,1`n5001,2`n",
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllBytes(
        (Join-Path $malformedRoot 'bravo.csv'),
        [byte[]]@())

    $gotoLoadFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $malformedRoot",
                'wait idle',
                'spectrum goto 1'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'goto-load-failure-state') `
            -Seed $labelSeed
    $labelLoadFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $malformedRoot",
                'wait idle',
                'label assign 5 spectrum 1'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'label-load-failure-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            [string]$gotoLoadFailure.Messages[6].error.code -eq
                'spectrum_load_failed' -and
            [string]$labelLoadFailure.Messages[6].error.code -eq
                'spectrum_load_failed' -and
            $null -eq (
                Get-Process `
                    -Id $gotoLoadFailure.Pid `
                    -ErrorAction SilentlyContinue) -and
            $null -eq (
                Get-Process `
                    -Id $labelLoadFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Changed goto and targeted label should both publish the documented spectrum_load_failed terminal code.'

    $missingSource =
        Join-Path $fixtureParent 'missing-source'
    $missingSourceFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $missingSource"
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'missing-source-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            $missingSourceFailure.ExitCode -ne 0 -and
            [string]$missingSourceFailure.Messages[2].status -eq
                'failed' -and
            [string]$missingSourceFailure.Messages[2].error.code -eq
                'source_not_found' -and
            $null -eq (
                Get-Process `
                    -Id $missingSourceFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'source.open should reject a missing absolute source and clean up its owned visible GUI.'

    $missingSpectrumFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $sourceRoot",
                'wait idle',
                'spectrum goto name missing.csv'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'missing-spectrum-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            $missingSpectrumFailure.ExitCode -ne 0 -and
            [string]$missingSpectrumFailure.Messages[6].status -eq
                'failed' -and
            [string]$missingSpectrumFailure.Messages[6].error.code -eq
                'spectrum_not_found' -and
            $null -eq (
                Get-Process `
                    -Id $missingSpectrumFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'spectrum.goto should distinguish a missing exact spectrum and clean up its owned GUI.'

    $invalidLabelFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $sourceRoot",
                'wait idle',
                'label assign 999'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'invalid-label-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            $invalidLabelFailure.ExitCode -ne 0 -and
            [string]$invalidLabelFailure.Messages[6].status -eq
                'failed' -and
            [string]$invalidLabelFailure.Messages[6].error.code -eq
                'label_not_found' -and
            $null -eq (
                Get-Process `
                    -Id $invalidLabelFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'label.assign should reject a code absent from the active production task and clean up its owned GUI.'

    $invalidSeed =
        Join-Path $fixtureParent 'invalid-labeling-seed.json'
    [System.IO.File]::WriteAllText(
        $invalidSeed,
        '{ invalid json',
        [System.Text.UTF8Encoding]::new($false))
    $invalidSeedRoot =
        Join-Path $fixtureParent 'invalid-seed-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $invalidSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $invalidSeedRoot `
                --labeling-state-seed $invalidSeed 2>&1
        )
        $invalidSeedExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $invalidSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $invalidSeedRoot) -and
            -not (@($invalidSeedOutput) -match '^SpecForge PID:')) `
        -Message 'Invalid production seed data must fail before creating the GUI process or retaining a partial root.'

    $ordinarySeedRoot =
        Join-Path $fixtureParent 'ordinary-seed-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $ordinarySeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $ordinarySeedRoot `
                --labeling-state-seed (
                    Join-Path $ordinaryRoot 'config\ui-language.json') 2>&1
        )
        $ordinarySeedExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $ordinarySeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $ordinarySeedRoot) -and
            -not (@($ordinarySeedOutput) -match '^SpecForge PID:')) `
        -Message 'Launcher must reject a seed inside the ordinary user state root before GUI startup.'

    $ordinaryOutputSeed =
        Join-Path $fixtureParent 'ordinary-output-seed.json'
    $ordinaryOutputPath =
        Join-Path $ordinaryRoot 'seed-output.csv'
    & $resolvedStateFixture `
        --write-labeling-seed $ordinaryOutputSeed `
        --source $sourceRoot `
        --output-path $ordinaryOutputPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production ordinary-output seed fixture.'
    }
    $ordinaryOutputSeedBefore =
        Get-FileSha256 -Path $ordinaryOutputSeed
    $ordinaryOutputSeedRoot =
        Join-Path $fixtureParent 'ordinary-output-seed-state'
    $ordinaryBeforeOutputSeed =
        Get-TreeFingerprint -Path $ordinaryRoot
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $ordinaryOutputSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $ordinaryOutputSeedRoot `
                --labeling-state-seed $ordinaryOutputSeed 2>&1
        )
        $ordinaryOutputSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $ordinaryOutputSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $ordinaryOutputSeedRoot) -and
            -not (@($ordinaryOutputSeedOutput) -match '^SpecForge PID:') -and
            -not (Test-Path -LiteralPath $ordinaryOutputPath) -and
            (Get-TreeFingerprint -Path $ordinaryRoot) -ceq
                $ordinaryBeforeOutputSeed -and
            (Get-FileSha256 -Path $ordinaryOutputSeed) -ceq
                $ordinaryOutputSeedBefore) `
        -Message 'A production seed output_path overlapping ordinary state must fail before GUI startup without changing either state or seed.'

    $internalOutputSeed =
        Join-Path $fixtureParent 'internal-output-seed.json'
    $internalOutputSeedRoot =
        Join-Path $fixtureParent 'internal-output-seed-state'
    $internalOutputPath =
        Join-Path $internalOutputSeedRoot 'labels\quality.npy'
    & $resolvedStateFixture `
        --write-labeling-seed $internalOutputSeed `
        --source $sourceRoot `
        --output-path $internalOutputPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production root-contained output seed fixture.'
    }
    $internalOutputSeedBefore =
        Get-FileSha256 -Path $internalOutputSeed
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $internalOutputSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $internalOutputSeedRoot `
                --labeling-state-seed $internalOutputSeed 2>&1
        )
        $internalOutputSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $internalOutputSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $internalOutputSeedRoot) -and
            -not (Test-Path -LiteralPath $internalOutputPath) -and
            -not (@($internalOutputSeedOutput) -match '^SpecForge PID:') -and
            (@($internalOutputSeedOutput) -match
                'Persistent labeling output paths are not permitted').Count -gt
                    0 -and
            (Get-FileSha256 -Path $internalOutputSeed) -ceq
                $internalOutputSeedBefore) `
        -Message 'Automation seeding must reject every persistent output_path, including a path lexically inside the future isolated root, before GUI startup.'

    $externalOutputSeed =
        Join-Path $fixtureParent 'external-output-seed.json'
    $externalOutputPath =
        Join-Path $externalSentinelRoot 'seed-output.csv'
    & $resolvedStateFixture `
        --write-labeling-seed $externalOutputSeed `
        --source $sourceRoot `
        --output-path $externalOutputPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production external-output seed fixture.'
    }
    $externalOutputSeedBefore =
        Get-FileSha256 -Path $externalOutputSeed
    $externalOutputSeedRoot =
        Join-Path $fixtureParent 'external-output-seed-state'
    $externalBeforeOutputSeed =
        Get-TreeFingerprint -Path $externalSentinelRoot
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $externalOutputSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $externalOutputSeedRoot `
                --labeling-state-seed $externalOutputSeed 2>&1
        )
        $externalOutputSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $externalOutputSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $externalOutputSeedRoot) -and
            -not (@($externalOutputSeedOutput) -match '^SpecForge PID:') -and
            -not (Test-Path -LiteralPath $externalOutputPath) -and
            (Get-TreeFingerprint -Path $externalSentinelRoot) -ceq
                $externalBeforeOutputSeed -and
            (Get-FileSha256 -Path $externalOutputSeed) -ceq
                $externalOutputSeedBefore) `
        -Message 'A production seed output_path outside the automation root must fail before GUI startup without external writes.'

    $reparseTarget =
        Join-Path $fixtureParent 'reparse-target'
    $reparseParent =
        Join-Path $fixtureParent 'reparse-parent'
    [System.IO.Directory]::CreateDirectory(
        $reparseTarget) | Out-Null
    $reparseCommandOutput = @(
        & $env:ComSpec /d /c `
            "mklink /J `"$reparseParent`" `"$reparseTarget`"" 2>&1
    )
    if ($LASTEXITCODE -ne 0) {
        throw (
            'Could not create the launcher reparse regression fixture: ' +
            ($reparseCommandOutput -join [Environment]::NewLine))
    }
    $reparseSeedRoot =
        Join-Path $reparseParent 'state'
    $reparseOutputSeed =
        Join-Path $fixtureParent 'reparse-output-seed.json'
    & $resolvedStateFixture `
        --write-labeling-seed $reparseOutputSeed `
        --source $sourceRoot `
        --output-path (
            Join-Path $reparseSeedRoot 'labels\quality.csv')
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production reparse-output seed fixture.'
    }
    $reparseTargetBefore =
        Get-TreeFingerprint -Path $reparseTarget
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $reparseSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $reparseSeedRoot `
                --labeling-state-seed $reparseOutputSeed 2>&1
        )
        $reparseSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $reparseSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath (
                Join-Path $reparseTarget 'state')) -and
            -not (@($reparseSeedOutput) -match '^SpecForge PID:') -and
            (Get-TreeFingerprint -Path $reparseTarget) -ceq
                $reparseTargetBefore) `
        -Message 'A reparse-point state parent must reject seeded persistent output references before GUI startup or target writes.'

    $existingRoot =
        Join-Path $fixtureParent 'existing-state'
    [System.IO.Directory]::CreateDirectory(
        $existingRoot) | Out-Null
    $existingMarker =
        Join-Path $existingRoot 'marker.txt'
    [System.IO.File]::WriteAllText(
        $existingMarker,
        'preserve',
        [System.Text.UTF8Encoding]::new($false))
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $existingRootOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $existingRoot `
                --labeling-state-seed $labelSeed 2>&1
        )
        $existingRootExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $existingRootExitCode -ne 0 -and
            (Get-Content -Raw -LiteralPath $existingMarker) -ceq 'preserve' -and
            -not (@($existingRootOutput) -match '^SpecForge PID:')) `
        -Message 'Launcher must reject every existing target root without modifying it or starting the GUI.'

    $outsideCapture =
        Join-Path $fixtureParent 'outside-capture.png'
    $captureRejectRoot =
        Join-Path $fixtureParent 'capture-reject-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $captureRejectOutput = @(
            "frame capture $outsideCapture" |
                & $resolvedLauncher `
                    --app $fixtureExecutable `
                    --state-root $captureRejectRoot `
                    --labeling-state-seed $labelSeed 2>&1
        )
        $captureRejectExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $captureRejectMessages = @(
        $captureRejectOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $captureRejectPid =
        Get-LauncherProcessId `
            -Output $captureRejectOutput
    Assert-True `
        -Condition (
            $captureRejectExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $outsideCapture) -and
            [string]$captureRejectMessages[2].status -eq 'failed' -and
            [string]$captureRejectMessages[2].error.code -eq
                'capture_path_outside_state_root' -and
            $null -eq (
                Get-Process `
                    -Id $captureRejectPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'frame.capture must reject an absolute path outside the current automation root and clean up its owned GUI.'

    $ordinaryAfter = Get-TreeFingerprint -Path $ordinaryRoot
    if ($ordinaryAfter -cne $ordinaryBefore) {
        Compare-Object ($ordinaryBefore -split "`n") ($ordinaryAfter -split "`n") | Out-Host
    }
    Assert-True `
        -Condition ($ordinaryAfter -ceq $ordinaryBefore) `
        -Message 'Real automation startup, state, idle and quit must not read/import or modify ordinary user state.'

    $externalAfter =
        Get-TreeFingerprint -Path $externalSentinelRoot
    Assert-True `
        -Condition ($externalAfter -ceq $externalBefore) `
        -Message 'Launcher must strip inherited legacy workload/profile variables before starting automation.'

    $invalidNestedStateRoot =
        Join-Path $ordinaryRoot 'automation-child'
    $ordinaryBeforeRejectedLaunch =
        Get-TreeFingerprint -Path $ordinaryRoot
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $rejectedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $invalidNestedStateRoot 2>&1
        )
        $rejectedExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $rejectedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $invalidNestedStateRoot) -and
            (Get-TreeFingerprint -Path $ordinaryRoot) -ceq
                $ordinaryBeforeRejectedLaunch) `
        -Message (
            'Launcher must reject an ordinary-root child before creation or diagnostic writes. Output: ' +
            ($rejectedOutput -join [Environment]::NewLine))

    $bystander = Start-Process `
        -FilePath (Join-Path $PSHOME 'powershell.exe') `
        -ArgumentList @(
            '-NoProfile',
            '-Command',
            'Start-Sleep -Seconds 300'
        ) `
        -WindowStyle Hidden `
        -PassThru

    $helloTimeoutRoot =
        Join-Path $fixtureParent 'hello-timeout-state'
    [System.Environment]::SetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        'hello-no-response',
        [System.EnvironmentVariableTarget]::Process)
    $helloTimeout = Invoke-LauncherWithInput `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedTimeoutFixture `
        -Root $helloTimeoutRoot `
        -Lines @('state get') `
        -TimeoutMilliseconds $timeoutFixtureBudgetMilliseconds
    $helloFixturePid =
        Get-TimeoutFixturePid -Root $helloTimeoutRoot
    Assert-ProcessGoneWithin `
        -ProcessId $helloFixturePid `
        -Message 'A hello response timeout must terminate the owned fake GUI within the launcher cleanup budget.'
    Assert-True `
        -Condition (
            $helloTimeout.ExitCode -ne 0 -and
            $helloTimeout.ElapsedMilliseconds -lt $timeoutFixtureBudgetMilliseconds -and
            [string]($helloTimeout.Output -join "`n") -match
                'Automation hello response timed out after 5 seconds\.' -and
            (Test-Path `
                -LiteralPath $helloTimeoutRoot `
                -PathType Container) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path $helloTimeoutRoot 'hello-request-observed.txt') `
                -PathType Leaf)) `
        -Message (
            'A missing hello response must fail quickly, retain the state root, and preserve fixture evidence. Output: ' +
            ($helloTimeout.Output -join [Environment]::NewLine))
    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Hello timeout cleanup must not affect a non-owned bystander process.'

    $unobservedAcceptanceRoot =
        Join-Path $fixtureParent 'unobserved-acceptance-timeout-state'
    [System.Environment]::SetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        'no-accepted-no-terminal',
        [System.EnvironmentVariableTarget]::Process)
    $unobservedAcceptanceTimeout = Invoke-LauncherWithInput `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedTimeoutFixture `
        -Root $unobservedAcceptanceRoot `
        -Lines @('state get') `
        -TimeoutMilliseconds $timeoutFixtureBudgetMilliseconds
    $unobservedAcceptanceFixturePid =
        Get-TimeoutFixturePid -Root $unobservedAcceptanceRoot
    Assert-ProcessGoneWithin `
        -ProcessId $unobservedAcceptanceFixturePid `
        -Message 'A sent request without an observed accepted response must terminate the exact owned fake GUI.'
    Assert-True `
        -Condition (
            $unobservedAcceptanceTimeout.ExitCode -ne 0 -and
            $unobservedAcceptanceTimeout.ElapsedMilliseconds -lt $timeoutFixtureBudgetMilliseconds -and
            [string]($unobservedAcceptanceTimeout.Output -join "`n") -match
                'Automation command response timed out after 10 seconds; terminal outcome was not observed\.' -and
            (Test-Path `
                -LiteralPath $unobservedAcceptanceRoot `
                -PathType Container) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path $unobservedAcceptanceRoot 'request-observed.txt') `
                -PathType Leaf)) `
        -Message (
            'A sent command without observed acceptance must still report an ambiguous terminal outcome and retain the state root. Output: ' +
            ($unobservedAcceptanceTimeout.Output -join [Environment]::NewLine))
    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Unobserved-acceptance cleanup must not affect a non-owned bystander process.'

    $singleTimeoutRoot =
        Join-Path $fixtureParent 'single-command-timeout-state'
    [System.Environment]::SetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        'accepted-no-terminal',
        [System.EnvironmentVariableTarget]::Process)
    $singleTimeout = Invoke-LauncherWithInput `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedTimeoutFixture `
        -Root $singleTimeoutRoot `
        -Lines @('state get') `
        -TimeoutMilliseconds $timeoutFixtureBudgetMilliseconds
    $singleFixturePid =
        Get-TimeoutFixturePid -Root $singleTimeoutRoot
    Assert-ProcessGoneWithin `
        -ProcessId $singleFixturePid `
        -Message 'An accepted single-command timeout must terminate the owned fake GUI within the launcher cleanup budget.'
    Assert-True `
        -Condition (
            $singleTimeout.ExitCode -ne 0 -and
            $singleTimeout.ElapsedMilliseconds -lt $timeoutFixtureBudgetMilliseconds -and
            [string]($singleTimeout.Output -join "`n") -match
                'Automation command response timed out after 10 seconds; terminal outcome was not observed\.' -and
            (Test-Path `
                -LiteralPath $singleTimeoutRoot `
                -PathType Container) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path $singleTimeoutRoot 'accepted-request-observed.txt') `
                -PathType Leaf) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path $singleTimeoutRoot 'extra-request-after-accepted.txt') `
                -PathType Leaf)) `
        -Message (
            'An accepted command without a terminal must fail with an ambiguity diagnostic and retain the state root. Output: ' +
            ($singleTimeout.Output -join [Environment]::NewLine))
    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Single-command timeout cleanup must not affect a non-owned bystander process.'

    $pipelineTimeoutRoot =
        Join-Path $fixtureParent 'pipeline-timeout-state'
    [System.Environment]::SetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        'accepted-no-terminal',
        [System.EnvironmentVariableTarget]::Process)
    $pipelineTimeout = Invoke-LauncherWithInput `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedTimeoutFixture `
        -Root $pipelineTimeoutRoot `
        -Lines @(
            'pipeline begin'
            'state get'
            'pipeline end'
        ) `
        -TimeoutMilliseconds $timeoutFixtureBudgetMilliseconds
    $pipelineFixturePid =
        Get-TimeoutFixturePid -Root $pipelineTimeoutRoot
    Assert-ProcessGoneWithin `
        -ProcessId $pipelineFixturePid `
        -Message 'An accepted pipeline timeout must terminate the owned fake GUI within the launcher cleanup budget.'
    Assert-True `
        -Condition (
            $pipelineTimeout.ExitCode -ne 0 -and
            $pipelineTimeout.ElapsedMilliseconds -lt $timeoutFixtureBudgetMilliseconds -and
            [string]($pipelineTimeout.Output -join "`n") -match
                'Automation pipeline response timed out after 10 seconds; terminal outcome was not observed\.' -and
            (Test-Path `
                -LiteralPath $pipelineTimeoutRoot `
                -PathType Container) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path $pipelineTimeoutRoot 'accepted-request-observed.txt') `
                -PathType Leaf) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path $pipelineTimeoutRoot 'extra-request-after-accepted.txt') `
                -PathType Leaf)) `
        -Message (
            'An accepted pipeline without terminals must fail with an ambiguity diagnostic and retain the state root. Output: ' +
            ($pipelineTimeout.Output -join [Environment]::NewLine))
    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Pipeline timeout cleanup must not affect a non-owned bystander process.'

    $pipelineWriteStallRoot =
        Join-Path $fixtureParent 'pipeline-write-stall-state'
    [System.Environment]::SetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        'pipeline-write-stall',
        [System.EnvironmentVariableTarget]::Process)
    $largePipelineValue = 'x' * 5000
    $pipelineWriteStallLines = @(
        'pipeline begin'
        'state get'
    )
    foreach ($index in 1..31) {
        $pipelineWriteStallLines +=
            'setting set ui.scale ' + $largePipelineValue
    }
    $pipelineWriteStallLines += 'pipeline end'
    $pipelineWriteStall = Invoke-LauncherWithInput `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedTimeoutFixture `
        -Root $pipelineWriteStallRoot `
        -Lines $pipelineWriteStallLines `
        -TimeoutMilliseconds $timeoutFixtureBudgetMilliseconds
    $pipelineWriteStallFixturePid =
        Get-TimeoutFixturePid -Root $pipelineWriteStallRoot
    Assert-ProcessGoneWithin `
        -ProcessId $pipelineWriteStallFixturePid `
        -Message 'A pipeline write deadline must terminate the exact owned fake GUI after the first request is accepted.'
    Assert-True `
        -Condition (
            $pipelineWriteStall.ExitCode -ne 0 -and
            $pipelineWriteStall.ElapsedMilliseconds -lt $timeoutFixtureBudgetMilliseconds -and
            [string]($pipelineWriteStall.Output -join "`n") -match
                'Automation pipeline response timed out after 10 seconds; terminal outcome was not observed\.' -and
            (Test-Path `
                -LiteralPath $pipelineWriteStallRoot `
                -PathType Container) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path $pipelineWriteStallRoot 'accepted-request-observed.txt') `
                -PathType Leaf) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path $pipelineWriteStallRoot 'pipeline-first-accepted.txt') `
                -PathType Leaf)) `
        -Message (
            'A pipeline write that stalls after the first accepted request must time out with an ambiguity diagnostic and retain the state root. Output: ' +
            ($pipelineWriteStall.Output -join [Environment]::NewLine))
    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Pipeline write deadline cleanup must not affect a non-owned bystander process.'

    $eofQuitTimeoutRoot =
        Join-Path $fixtureParent 'eof-quit-timeout-state'
    [System.Environment]::SetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        'quit-no-terminal',
        [System.EnvironmentVariableTarget]::Process)
    $eofQuitTimeout = Invoke-LauncherWithInput `
        -LauncherPath $resolvedLauncher `
        -AppPath $resolvedTimeoutFixture `
        -Root $eofQuitTimeoutRoot `
        -Lines @('state get') `
        -TimeoutMilliseconds $timeoutFixtureBudgetMilliseconds
    $eofQuitFixturePid =
        Get-TimeoutFixturePid -Root $eofQuitTimeoutRoot
    Assert-ProcessGoneWithin `
        -ProcessId $eofQuitFixturePid `
        -Message 'A missing implicit app.quit terminal must still clean up the exact owned fake GUI.'
    Assert-True `
        -Condition (
            $eofQuitTimeout.ExitCode -ne 0 -and
            $eofQuitTimeout.ElapsedMilliseconds -lt $timeoutFixtureBudgetMilliseconds -and
            [string]($eofQuitTimeout.Output -join "`n") -match
                'Automation command response ended before terminal outcome was observed\.' -and
            (Test-Path `
                -LiteralPath $eofQuitTimeoutRoot `
                -PathType Container) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path $eofQuitTimeoutRoot 'quit-request-observed.txt') `
                -PathType Leaf)) `
        -Message (
            'A lost implicit app.quit terminal must remain a nonzero, ambiguous result even when the GUI exits 0. Output: ' +
            ($eofQuitTimeout.Output -join [Environment]::NewLine))
    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Implicit app.quit response cleanup must not affect a non-owned bystander process.'

    $pipelineLimitRoot =
        Join-Path $fixtureParent 'pipeline-limit-state'
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        'pipeline-limit',
        [System.EnvironmentVariableTarget]::Process)
    $pipelineLimitInput = @(
        'pipeline begin'
        foreach ($index in 1..33) {
            'state get'
        }
        'pipeline end'
    )
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $pipelineLimitOutput = @(
            $pipelineLimitInput |
                & $resolvedLauncher `
                    --app $resolvedCleanupFixture `
                    --state-root $pipelineLimitRoot 2>&1
        )
        $pipelineLimitExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $pipelineLimitPid =
        Get-LauncherProcessId `
            -Output $pipelineLimitOutput
    Assert-True `
        -Condition (
            $pipelineLimitExitCode -ne 0 -and
            [string]($pipelineLimitOutput -join "`n") -match
                'Pipeline accepts at most 32 commands' -and
            $null -eq (
                Get-Process `
                    -Id $pipelineLimitPid `
                    -ErrorAction SilentlyContinue) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $pipelineLimitRoot `
                        'pipeline-command-observed.txt')) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $pipelineLimitRoot `
                        'launcher-cleanup-completed.txt') `
                -PathType Leaf)) `
        -Message (
            'An oversized pipeline must fail before sending business requests and still clean up its owned child. Output: ' +
            ($pipelineLimitOutput -join [Environment]::NewLine))

    $gracefulCleanupRoot =
        Join-Path $fixtureParent 'cleanup-graceful-state'
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        'graceful',
        [System.EnvironmentVariableTarget]::Process)
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $gracefulCleanupOutput = @(
            'state get' |
                & $resolvedLauncher `
                    --app $resolvedCleanupFixture `
                    --state-root $gracefulCleanupRoot 2>&1
        )
        $gracefulCleanupExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $gracefulCleanupPid =
        Get-LauncherProcessId `
            -Output $gracefulCleanupOutput
    Assert-True `
        -Condition (
            $gracefulCleanupExitCode -ne 0 -and
            $null -eq (
                Get-Process `
                    -Id $gracefulCleanupPid `
                    -ErrorAction SilentlyContinue) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $gracefulCleanupRoot `
                        'launcher-cleanup-completed.txt') `
                -PathType Leaf)) `
        -Message (
            'A post-handshake launcher error must request normal app.quit and leave no owned process. Output: ' +
            ($gracefulCleanupOutput -join [Environment]::NewLine))

    $forcedCleanupRoot =
        Join-Path $fixtureParent 'cleanup-forced-state'
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        'disconnect',
        [System.EnvironmentVariableTarget]::Process)
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $forcedCleanupOutput = @(
            'state get' |
                & $resolvedLauncher `
                    --app $resolvedCleanupFixture `
                    --state-root $forcedCleanupRoot 2>&1
        )
        $forcedCleanupExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $forcedCleanupPid =
        Get-LauncherProcessId `
            -Output $forcedCleanupOutput
    Assert-True `
        -Condition (
            $forcedCleanupExitCode -ne 0 -and
            $null -eq (
                Get-Process `
                    -Id $forcedCleanupPid `
                    -ErrorAction SilentlyContinue) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $forcedCleanupRoot `
                        'launcher-cleanup-completed.txt') `
                -PathType Leaf)) `
        -Message (
            'A disconnected launcher-owned child must be terminated within the cleanup bound. Output: ' +
            ($forcedCleanupOutput -join [Environment]::NewLine))

    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Launcher cleanup must not affect a non-owned bystander process.'
}
finally {
    foreach ($name in $incompatibleEnvironmentVariables) {
        [System.Environment]::SetEnvironmentVariable(
            $name,
            $originalAutomationEnvironment[$name],
            [System.EnvironmentVariableTarget]::Process)
    }
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        $originalCleanupFixtureEnvironment,
        [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        $timeoutFixtureEnvironment,
        $originalTimeoutFixtureEnvironment,
        [System.EnvironmentVariableTarget]::Process)
    if ($null -ne $bystander) {
        $bystander.Refresh()
        if (-not $bystander.HasExited) {
            Stop-Process `
                -Id $bystander.Id `
                -Force
            [void]$bystander.WaitForExit(5000)
        }
        $bystander.Dispose()
    }
    if (Test-Path -LiteralPath $fixtureParent) {
        $resolvedFixture = (Resolve-Path -LiteralPath $fixtureParent).Path
        $tempRoot = [System.IO.Path]::GetFullPath(
            [System.IO.Path]::GetTempPath())
        if (-not $resolvedFixture.StartsWith(
                $tempRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove fixture outside temp: $resolvedFixture"
        }
        $ownedFixtureProcesses = @(
            Get-Process `
                -Name 'SpecForge','SpecForgeAutomation' `
                -ErrorAction SilentlyContinue |
                Where-Object {
                    $null -ne $_.Path -and
                    $_.Path.StartsWith(
                        $resolvedFixture +
                            [System.IO.Path]::DirectorySeparatorChar,
                        [System.StringComparison]::OrdinalIgnoreCase)
                }
        )
        foreach ($ownedFixtureProcess in $ownedFixtureProcesses) {
            Stop-Process `
                -Id $ownedFixtureProcess.Id `
                -Force `
                -ErrorAction SilentlyContinue
            [void]$ownedFixtureProcess.WaitForExit(5000)
            $ownedFixtureProcess.Dispose()
        }
        $removed = $false
        for ($attempt = 0;
             $attempt -lt 20 -and -not $removed;
             ++$attempt) {
            try {
                Remove-Item `
                    -LiteralPath $resolvedFixture `
                    -Recurse `
                    -Force `
                    -ErrorAction Stop
                $removed = $true
            }
            catch {
                if ($attempt -eq 19) {
                    throw
                }
                Start-Sleep -Milliseconds 50
            }
        }
    }
}

Write-Host 'automation launcher integration tests passed'
