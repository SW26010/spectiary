[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Launcher,
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$LayoutSeed
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

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
        throw 'Interactive launcher closed stdout unexpectedly.'
    }
    return [string]$read.Result
}

function Start-InteractiveLauncher {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$StateRoot,
        [Parameter(Mandatory = $true)]
        [string]$SeedPath
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
        '" --state-root "' + $StateRoot +
        '" --imgui-layout-seed "' + $SeedPath + '"'

    $process = $null
    $stderrTask = $null
    $guiProcessId = 0
    try {
        $process = [System.Diagnostics.Process]::Start($start)
        $stderrTask = $process.StandardError.ReadToEndAsync()
        while ($true) {
            $line = Read-LauncherLine -Process $process
            if ($line -match '^SpecForge PID: ([0-9]+)$') {
                $guiProcessId = [int]$Matches[1]
            }
            if ($line -eq (
                    'Harness controls: pipeline begin ... pipeline end; ' +
                    'disconnect after accepted <next command>')) {
                break
            }
        }
        Assert-True `
            -Condition ($guiProcessId -gt 0) `
            -Message 'Launcher did not report the owned SpecForge PID.'
        return [pscustomobject]@{
            Process = $process
            StderrTask = $stderrTask
            GuiProcessId = $guiProcessId
        }
    }
    catch {
        $startupError = $_
        if ($null -ne $process) {
            try {
                $process.StandardInput.Close()
            }
            catch {
            }
            try {
                if (-not $process.HasExited -and
                    -not $process.WaitForExit(3000)) {
                    $process.Kill()
                    [void]$process.WaitForExit(5000)
                }
            }
            catch {
            }
        }
        if ($guiProcessId -gt 0) {
            $ownedGui =
                Get-Process `
                    -Id $guiProcessId `
                    -ErrorAction SilentlyContinue
            if ($null -ne $ownedGui) {
                try {
                    if (-not $ownedGui.HasExited) {
                        $ownedGui.Kill()
                        [void]$ownedGui.WaitForExit(5000)
                    }
                }
                catch {
                }
                finally {
                    $ownedGui.Dispose()
                }
            }
        }
        if ($null -ne $process) {
            $process.Dispose()
        }
        throw $startupError
    }
}

function Send-LauncherRequest {
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
    $input = $Process.StandardInput.BaseStream
    $input.Write($payload, 0, $payload.Length)
    $input.Flush()

    $messages = @()
    while ($true) {
        $outputLine = Read-LauncherLine -Process $Process
        if ($outputLine -notmatch '^\{') {
            continue
        }
        $message = $outputLine | ConvertFrom-Json
        Assert-True `
            -Condition (
                [string]$message.request_id -eq
                    $RequestId) `
            -Message (
                "Unexpected request ID in launcher output: $outputLine")
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
            [string]$messages[0].status -eq
                'accepted') `
        -Message (
            "$RequestId must emit accepted and one terminal response.")
    return $messages[1]
}

if ($null -eq ('SpecForgeViewportOwnershipNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class SpecForgeViewportOwnershipNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
        public int Width { get { return Right - Left; } }
        public int Height { get { return Bottom - Top; } }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    public sealed class WindowInfo
    {
        public IntPtr Handle { get; set; }
        public uint ProcessId { get; set; }
        public string ClassName { get; set; }
        public string Title { get; set; }
    }

    private delegate bool EnumWindowsCallback(
        IntPtr window,
        IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(
        EnumWindowsCallback callback,
        IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window,
        out uint processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindow(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(
        IntPtr window,
        StringBuilder className,
        int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLengthW(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(
        IntPtr window,
        StringBuilder text,
        int maximumCount);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(
        IntPtr window,
        out Rect rect);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(
        IntPtr window,
        out Rect rect);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ClientToScreen(
        IntPtr window,
        ref Point point);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr window,
        IntPtr insertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);

    private static string WindowClassName(IntPtr window)
    {
        StringBuilder text = new StringBuilder(256);
        GetClassNameW(window, text, text.Capacity);
        return text.ToString();
    }

    private static string WindowTitle(IntPtr window)
    {
        int length = GetWindowTextLengthW(window);
        StringBuilder text = new StringBuilder(length + 1);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    public static WindowInfo[] WindowsForProcess(uint processId)
    {
        List<WindowInfo> windows = new List<WindowInfo>();
        EnumWindowsCallback callback = delegate(IntPtr window, IntPtr parameter)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == processId && IsWindowVisible(window))
            {
                windows.Add(new WindowInfo
                {
                    Handle = window,
                    ProcessId = owner,
                    ClassName = WindowClassName(window),
                    Title = WindowTitle(window)
                });
            }
            return true;
        };
        if (!EnumWindows(callback, IntPtr.Zero))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        return windows.ToArray();
    }

    public static uint WindowProcessId(IntPtr window)
    {
        uint processId;
        GetWindowThreadProcessId(window, out processId);
        return processId;
    }

    public static Rect WindowRectangle(IntPtr window)
    {
        Rect rect;
        if (!GetWindowRect(window, out rect))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        return rect;
    }

    public static Rect ClientRectangleOnScreen(IntPtr window)
    {
        Rect client;
        if (!GetClientRect(window, out client))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        Point topLeft = new Point { X = client.Left, Y = client.Top };
        Point bottomRight = new Point { X = client.Right, Y = client.Bottom };
        if (!ClientToScreen(window, ref topLeft) ||
            !ClientToScreen(window, ref bottomRight))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        return new Rect
        {
            Left = topLeft.X,
            Top = topLeft.Y,
            Right = bottomRight.X,
            Bottom = bottomRight.Y
        };
    }

    public static Rect VirtualScreenRectangle()
    {
        int left = GetSystemMetrics(76);
        int top = GetSystemMetrics(77);
        int width = GetSystemMetrics(78);
        int height = GetSystemMetrics(79);
        return new Rect
        {
            Left = left,
            Top = top,
            Right = left + width,
            Bottom = top + height
        };
    }

    public static void MoveWindowNoActivate(
        IntPtr window,
        int x,
        int y)
    {
        const uint SWP_NOSIZE = 0x0001;
        const uint SWP_NOZORDER = 0x0004;
        const uint SWP_NOACTIVATE = 0x0010;
        if (!SetWindowPos(
                window,
                IntPtr.Zero,
                x,
                y,
                0,
                0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
    }
}
'@
}

function Format-Handle {
    param([Parameter(Mandatory = $true)] [IntPtr]$Handle)

    return '0x{0:x16}' -f $Handle.ToInt64()
}

function Wait-ForOwnershipWindows {
    param(
        [Parameter(Mandatory = $true)]
        [int]$GuiProcessId
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        $windows = @(
            [SpecForgeViewportOwnershipNative]::
                WindowsForProcess([uint32]$GuiProcessId)
        )
        $main = @(
            $windows |
                Where-Object {
                    $_.ClassName -ceq
                        'SpecForgeMainWindow'
                })
        $secondary = @(
            $windows |
                Where-Object {
                    $_.ClassName -cne
                        'SpecForgeMainWindow'
                })
        if ($main.Count -eq 1 -and
            $secondary.Count -eq 1) {
            return [pscustomobject]@{
                Main = $main[0]
                Secondary = $secondary[0]
            }
        }
        Start-Sleep -Milliseconds 25
    }
    $diagnostic = @(
        [SpecForgeViewportOwnershipNative]::
            WindowsForProcess([uint32]$GuiProcessId) |
            ForEach-Object {
                (Format-Handle -Handle $_.Handle) +
                " class='$($_.ClassName)' title='$($_.Title)'"
            }
    ) -join '; '
    throw (
        'Expected exactly one SpecForge main HWND and one detached ImGui ' +
        "viewport HWND. Observed: $diagnostic")
}

function Assert-SameOwnership {
    param(
        [Parameter(Mandatory = $true)]
        [int]$GuiProcessId,
        [Parameter(Mandatory = $true)]
        [IntPtr]$MainHandle,
        [Parameter(Mandatory = $true)]
        [IntPtr]$SecondaryHandle,
        [Parameter(Mandatory = $true)]
        [string]$Phase
    )

    Assert-True `
        -Condition (
            [SpecForgeViewportOwnershipNative]::
                IsWindow($SecondaryHandle) -and
            [SpecForgeViewportOwnershipNative]::
                WindowProcessId($SecondaryHandle) -eq
                    [uint32]$GuiProcessId) `
        -Message (
            "$Phase destroyed or reassigned detached HWND " +
            (Format-Handle -Handle $SecondaryHandle) + '.')
    $windows =
        Wait-ForOwnershipWindows `
            -GuiProcessId $GuiProcessId
    Assert-True `
        -Condition (
            $windows.Main.Handle -eq $MainHandle -and
            $windows.Secondary.Handle -eq
                $SecondaryHandle) `
        -Message (
            "$Phase changed viewport ownership. Expected main " +
            (Format-Handle -Handle $MainHandle) +
            ' and detached ' +
            (Format-Handle -Handle $SecondaryHandle) +
            '; observed main ' +
            (Format-Handle -Handle $windows.Main.Handle) +
            ' and detached ' +
            (Format-Handle -Handle $windows.Secondary.Handle) + '.')
}

function Test-RectangleContained {
    param(
        [Parameter(Mandatory = $true)]$Inner,
        [Parameter(Mandatory = $true)]$Outer
    )

    return (
        $Inner.Left -ge $Outer.Left -and
        $Inner.Top -ge $Outer.Top -and
        $Inner.Right -le $Outer.Right -and
        $Inner.Bottom -le $Outer.Bottom)
}

function Test-RectanglesDisjoint {
    param(
        [Parameter(Mandatory = $true)]$First,
        [Parameter(Mandatory = $true)]$Second
    )

    return (
        $First.Right -le $Second.Left -or
        $First.Left -ge $Second.Right -or
        $First.Bottom -le $Second.Top -or
        $First.Top -ge $Second.Bottom)
}

function Test-RectangleCenterOutside {
    param(
        [Parameter(Mandatory = $true)]$Rectangle,
        [Parameter(Mandatory = $true)]$Outer
    )

    $centerX =
        $Rectangle.Left +
        (($Rectangle.Right - $Rectangle.Left) / 2.0)
    $centerY =
        $Rectangle.Top +
        (($Rectangle.Bottom - $Rectangle.Top) / 2.0)
    return (
        $centerX -lt $Outer.Left -or
        $centerX -ge $Outer.Right -or
        $centerY -lt $Outer.Top -or
        $centerY -ge $Outer.Bottom)
}

function Test-MinimumVisibleArea {
    param(
        [Parameter(Mandatory = $true)]$Rectangle,
        [Parameter(Mandatory = $true)]$VirtualScreen,
        [int]$MinimumExtent = 32
    )

    $visibleWidth =
        [Math]::Min($Rectangle.Right, $VirtualScreen.Right) -
        [Math]::Max($Rectangle.Left, $VirtualScreen.Left)
    $visibleHeight =
        [Math]::Min($Rectangle.Bottom, $VirtualScreen.Bottom) -
        [Math]::Max($Rectangle.Top, $VirtualScreen.Top)
    return (
        $visibleWidth -ge $MinimumExtent -and
        $visibleHeight -ge $MinimumExtent)
}

function Get-OutsidePosition {
    param(
        [Parameter(Mandatory = $true)]$MainClient,
        [Parameter(Mandatory = $true)]$SecondaryRect,
        [Parameter(Mandatory = $true)]$VirtualScreen
    )

    $margin = 32
    $width = $SecondaryRect.Width
    $height = $SecondaryRect.Height
    $alignedX = [Math]::Max(
        $VirtualScreen.Left + $margin,
        [Math]::Min(
            $MainClient.Left + $margin,
            $VirtualScreen.Right - $width - $margin))
    $alignedY = [Math]::Max(
        $VirtualScreen.Top + $margin,
        [Math]::Min(
            $MainClient.Top + $margin,
            $VirtualScreen.Bottom - $height - $margin))
    $candidates = @(
        [pscustomobject]@{
            X = $MainClient.Right + $margin
            Y = $alignedY
        },
        [pscustomobject]@{
            X = $MainClient.Left - $width - $margin
            Y = $alignedY
        },
        [pscustomobject]@{
            X = $alignedX
            Y = $MainClient.Bottom + $margin
        },
        [pscustomobject]@{
            X = $alignedX
            Y = $MainClient.Top - $height - $margin
        })
    foreach ($candidate in $candidates) {
        $rect = [pscustomobject]@{
            Left = [int]$candidate.X
            Top = [int]$candidate.Y
            Right = [int]$candidate.X + $width
            Bottom = [int]$candidate.Y + $height
        }
        if ((Test-RectangleContained `
                -Inner $rect `
                -Outer $VirtualScreen) -and
            (Test-RectanglesDisjoint `
                -First $rect `
                -Second $MainClient)) {
            return $candidate
        }
    }

    $halfWidth = [Math]::Floor($width / 2.0)
    $halfHeight = [Math]::Floor($height / 2.0)
    $centerOutsideCandidates = @(
        [pscustomobject]@{
            X = $MainClient.Right + $margin - $halfWidth
            Y = $alignedY
        },
        [pscustomobject]@{
            X = $MainClient.Left - $margin - $halfWidth
            Y = $alignedY
        },
        [pscustomobject]@{
            X = $alignedX
            Y = $MainClient.Bottom + $margin - $halfHeight
        },
        [pscustomobject]@{
            X = $alignedX
            Y = $MainClient.Top - $margin - $halfHeight
        })
    foreach ($candidate in $centerOutsideCandidates) {
        $rect = [pscustomobject]@{
            Left = [int]$candidate.X
            Top = [int]$candidate.Y
            Right = [int]$candidate.X + $width
            Bottom = [int]$candidate.Y + $height
        }
        if ((Test-RectangleCenterOutside `
                -Rectangle $rect `
                -Outer $MainClient) -and
            (Test-MinimumVisibleArea `
                -Rectangle $rect `
                -VirtualScreen $VirtualScreen)) {
            return $candidate
        }
    }
    throw 'The interactive desktop has no visible area large enough to move the detached viewport center outside the main client rectangle.'
}

$resolvedLauncher =
    (Resolve-Path -LiteralPath $Launcher).Path
$resolvedExecutable =
    (Resolve-Path -LiteralPath $Executable).Path
$resolvedLayoutSeed =
    (Resolve-Path -LiteralPath $LayoutSeed).Path
$fixtureParent = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-viewport-ownership-' +
     [Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory(
    $fixtureParent) | Out-Null
$stateRoot = Join-Path $fixtureParent 'state'
$invalidStateRoot = Join-Path $fixtureParent 'invalid-state'
$invalidSeed = Join-Path $fixtureParent 'malformed-layout.ini'
$launcherProcess = $null
$guiProcessId = 0
$testCompleted = $false

try {
    [System.IO.File]::WriteAllText(
        $invalidSeed,
        "[Window][truncated`nPos=1,2`n",
        [System.Text.UTF8Encoding]::new($false))
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $invalidOutput = @(
            & $resolvedLauncher `
                --app $resolvedExecutable `
                --state-root $invalidStateRoot `
                --imgui-layout-seed $invalidSeed 2>&1
        )
        $invalidExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $invalidExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $invalidStateRoot) -and
            (@($invalidOutput) -match
                'not a well-formed ImGui layout snapshot')) `
        -Message (
            'Malformed ImGui layout seed must fail before state-root creation ' +
            'or GUI launch. Output: ' +
            ($invalidOutput -join [Environment]::NewLine))

    $launch =
        Start-InteractiveLauncher `
            -LauncherPath $resolvedLauncher `
            -AppPath $resolvedExecutable `
            -StateRoot $stateRoot `
            -SeedPath $resolvedLayoutSeed
    $launcherProcess = $launch.Process
    $guiProcessId = $launch.GuiProcessId

    $baseline =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'panel set spectral_lines true' `
            -RequestId 'request-1'
    Assert-True `
        -Condition (
            [string]$baseline.status -eq 'completed' -and
            [string]$baseline.command -eq 'panel.set' -and
            -not [bool]$baseline.result.changed) `
        -Message 'Initial detached Spectral Lines panel must complete an unchanged Present barrier.'

    $windows =
        Wait-ForOwnershipWindows `
            -GuiProcessId $guiProcessId
    $mainHandle = [IntPtr]$windows.Main.Handle
    $secondaryHandle =
        [IntPtr]$windows.Secondary.Handle
    $mainClient =
        [SpecForgeViewportOwnershipNative]::
            ClientRectangleOnScreen($mainHandle)
    $secondaryRect =
        [SpecForgeViewportOwnershipNative]::
            WindowRectangle($secondaryHandle)
    Assert-True `
        -Condition (
            $secondaryRect.Width + 64 -le
                $mainClient.Width -and
            $secondaryRect.Height + 64 -le
                $mainClient.Height) `
        -Message 'Detached fixture viewport must fit completely inside the main client rectangle with a safety margin.'

    $insideX =
        $mainClient.Left +
        [Math]::Floor(
            ($mainClient.Width -
             $secondaryRect.Width) / 2)
    $insideY =
        $mainClient.Top +
        [Math]::Floor(
            ($mainClient.Height -
             $secondaryRect.Height) / 2)
    [SpecForgeViewportOwnershipNative]::
        MoveWindowNoActivate(
            $secondaryHandle,
            $insideX,
            $insideY)

    $insideFirst =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'panel set spectral_lines true' `
            -RequestId 'request-2'
    $insideSecond =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'panel set spectral_lines true' `
            -RequestId 'request-3'
    Assert-True `
        -Condition (
            [string]$insideFirst.status -eq 'completed' -and
            [string]$insideSecond.status -eq 'completed' -and
            [uint64]$insideFirst.result.frame_index -gt
                [uint64]$baseline.result.frame_index -and
            [uint64]$insideSecond.result.frame_index -gt
                [uint64]$insideFirst.result.frame_index) `
        -Message 'Moving inside must be followed by two later successful detached-viewport Present barriers.'
    Assert-SameOwnership `
        -GuiProcessId $guiProcessId `
        -MainHandle $mainHandle `
        -SecondaryHandle $secondaryHandle `
        -Phase 'Moving the detached viewport inside the main client area'
    $insideRect =
        [SpecForgeViewportOwnershipNative]::
            WindowRectangle($secondaryHandle)
    Assert-True `
        -Condition (
            Test-RectangleContained `
                -Inner $insideRect `
                -Outer $mainClient) `
        -Message 'Detached viewport did not remain fully inside the main client rectangle after Present barriers.'

    $outside =
        Get-OutsidePosition `
            -MainClient $mainClient `
            -SecondaryRect $insideRect `
            -VirtualScreen (
                [SpecForgeViewportOwnershipNative]::
                    VirtualScreenRectangle())
    [SpecForgeViewportOwnershipNative]::
        MoveWindowNoActivate(
            $secondaryHandle,
            [int]$outside.X,
            [int]$outside.Y)
    $outsideFirst =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'panel set spectral_lines true' `
            -RequestId 'request-4'
    $outsideSecond =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'panel set spectral_lines true' `
            -RequestId 'request-5'
    Assert-True `
        -Condition (
            [string]$outsideFirst.status -eq 'completed' -and
            [string]$outsideSecond.status -eq 'completed' -and
            [uint64]$outsideFirst.result.frame_index -gt
                [uint64]$insideSecond.result.frame_index -and
            [uint64]$outsideSecond.result.frame_index -gt
                [uint64]$outsideFirst.result.frame_index) `
        -Message 'Moving outside must be followed by two later successful detached-viewport Present barriers.'
    Assert-SameOwnership `
        -GuiProcessId $guiProcessId `
        -MainHandle $mainHandle `
        -SecondaryHandle $secondaryHandle `
        -Phase 'Moving the detached viewport outside the main client area'
    $outsideRect =
        [SpecForgeViewportOwnershipNative]::
            WindowRectangle($secondaryHandle)
    Assert-True `
        -Condition (
            Test-RectangleCenterOutside `
                -Rectangle $outsideRect `
                -Outer $mainClient) `
        -Message 'Detached viewport center did not remain outside the main client rectangle after Present barriers.'

    $quit =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'app quit' `
            -RequestId 'request-6'
    Assert-True `
        -Condition (
            [string]$quit.status -eq 'completed' -and
            [string]$quit.command -eq 'app.quit') `
        -Message 'Viewport ownership scenario must quit through the normal application path.'
    Assert-True `
        -Condition ($launcherProcess.WaitForExit(10000)) `
        -Message 'Viewport ownership launcher did not exit after app.quit.'
    Assert-True `
        -Condition ($launcherProcess.ExitCode -eq 0) `
        -Message (
            'Viewport ownership launcher exited with code ' +
            $launcherProcess.ExitCode + '. ' +
            $launch.StderrTask.GetAwaiter().GetResult())
    Write-Host (
        'ImGui viewport ownership integration passed with stable detached HWND ' +
        (Format-Handle -Handle $secondaryHandle) + '.')
    $testCompleted = $true
}
finally {
    if ($null -ne $launcherProcess) {
        if (-not $launcherProcess.HasExited) {
            try {
                $launcherProcess.StandardInput.Close()
            }
            catch {
            }
            if (-not $launcherProcess.WaitForExit(3000)) {
                $launcherProcess.Kill()
                [void]$launcherProcess.WaitForExit(5000)
            }
        }
        $launcherProcess.Dispose()
    }
    if ($guiProcessId -gt 0) {
        $ownedGui =
            Get-Process `
                -Id $guiProcessId `
                -ErrorAction SilentlyContinue
        if ($null -ne $ownedGui) {
            Stop-Process `
                -Id $guiProcessId `
                -Force `
                -ErrorAction SilentlyContinue
            $ownedGui.Dispose()
        }
    }
    if (Test-Path -LiteralPath $fixtureParent) {
        try {
            [System.IO.Directory]::Delete(
                $fixtureParent,
                $true)
        }
        catch {
            if ($testCompleted) {
                throw
            }
            Write-Warning (
                'Could not remove failed viewport ownership fixture directory ' +
                "'$fixtureParent': $($_.Exception.Message)")
        }
    }
}
