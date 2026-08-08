[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Launcher,
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$WindowPeer,
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

function Start-WindowPeer {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $ExecutablePath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $utf8WithoutBom =
        [System.Text.UTF8Encoding]::new($false)
    $start.StandardOutputEncoding = $utf8WithoutBom
    $start.StandardErrorEncoding = $utf8WithoutBom

    $process = $null
    try {
        $process = [System.Diagnostics.Process]::Start($start)
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $ready = Read-LauncherLine -Process $process
        Assert-True `
            -Condition (
                $ready -match
                    '^ready (0x[0-9a-fA-F]+)$') `
            -Message (
                "Win32 test peer returned an invalid ready line: $ready")
        $handle = [IntPtr][Convert]::ToInt64(
            $Matches[1].Substring(2),
            16)
        return [pscustomobject]@{
            Process = $process
            StderrTask = $stderrTask
            Handle = $handle
        }
    }
    catch {
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
            $process.Dispose()
        }
        throw
    }
}

function Send-WindowPeerCommand {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $payload =
        [System.Text.UTF8Encoding]::new($false).GetBytes(
            $Command + "`n")
    $input = $Process.StandardInput.BaseStream
    $input.Write($payload, 0, $payload.Length)
    $input.Flush()
    return Read-LauncherLine -Process $Process
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
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsIconic(IntPtr window);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ShowWindowAsync(
        IntPtr window,
        int command);

    [DllImport("user32.dll")]
    private static extern IntPtr GetWindow(
        IntPtr window,
        uint command);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtrW(
        IntPtr window,
        int index);

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

    public static IntPtr[] VisibleWindowsInZOrder()
    {
        List<IntPtr> windows = new List<IntPtr>();
        EnumWindowsCallback callback = delegate(IntPtr window, IntPtr parameter)
        {
            if (IsWindowVisible(window))
            {
                windows.Add(window);
            }
            return true;
        };
        if (!EnumWindows(callback, IntPtr.Zero))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        return windows.ToArray();
    }

    public static IntPtr WindowOwner(IntPtr window)
    {
        const uint GW_OWNER = 4;
        return GetWindow(window, GW_OWNER);
    }

    public static bool WindowIsTopMost(IntPtr window)
    {
        const int GWL_EXSTYLE = -20;
        const long WS_EX_TOPMOST = 0x00000008L;
        return (
            GetWindowLongPtrW(window, GWL_EXSTYLE).ToInt64() &
            WS_EX_TOPMOST) != 0;
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
        [int]$GuiProcessId,
        [int]$ExpectedSecondaryCount = 2
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
            $secondary.Count -eq
                $ExpectedSecondaryCount) {
            return [pscustomobject]@{
                Main = $main[0]
                Secondary = @($secondary)
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
        'Expected exactly one SpecForge main HWND and ' +
        "$ExpectedSecondaryCount detached ImGui viewport HWNDs. " +
        "Observed: $diagnostic")
}

function Assert-SameViewportWindows {
    param(
        [Parameter(Mandatory = $true)]
        [int]$GuiProcessId,
        [Parameter(Mandatory = $true)]
        [IntPtr]$MainHandle,
        [Parameter(Mandatory = $true)]
        [IntPtr[]]$SecondaryHandles,
        [Parameter(Mandatory = $true)]
        [string]$Phase
    )

    foreach ($secondaryHandle in $SecondaryHandles) {
        Assert-True `
            -Condition (
            [SpecForgeViewportOwnershipNative]::
                    IsWindow($secondaryHandle) -and
            [SpecForgeViewportOwnershipNative]::
                    WindowProcessId($secondaryHandle) -eq
                        [uint32]$GuiProcessId) `
            -Message (
                "$Phase destroyed or reassigned detached HWND " +
                (Format-Handle -Handle $secondaryHandle) + '.')
    }
    $windows =
        Wait-ForOwnershipWindows `
            -GuiProcessId $GuiProcessId `
            -ExpectedSecondaryCount $SecondaryHandles.Count
    $observedSecondaryHandles = @(
        $windows.Secondary |
            ForEach-Object { [IntPtr]$_.Handle }
    )
    $missingHandles = @(
        $SecondaryHandles |
            Where-Object {
                $observedSecondaryHandles -notcontains $_
            }
    )
    $unexpectedHandles = @(
        $observedSecondaryHandles |
            Where-Object {
                $SecondaryHandles -notcontains $_
            }
    )
    Assert-True `
        -Condition (
            $windows.Main.Handle -eq $MainHandle -and
            $missingHandles.Count -eq 0 -and
            $unexpectedHandles.Count -eq 0) `
        -Message (
            "$Phase changed viewport HWND identity. Expected main " +
            (Format-Handle -Handle $MainHandle) +
            ' and detached HWNDs ' +
            (($SecondaryHandles | ForEach-Object {
                Format-Handle -Handle $_
            }) -join ', ') +
            '; observed main ' +
            (Format-Handle -Handle $windows.Main.Handle) +
            ' and detached HWNDs ' +
            (($observedSecondaryHandles | ForEach-Object {
                Format-Handle -Handle $_
            }) -join ', ') + '.')
}

function Assert-Win32Ownership {
    param(
        [Parameter(Mandatory = $true)]
        [IntPtr]$MainHandle,
        [Parameter(Mandatory = $true)]
        [IntPtr[]]$SecondaryHandles,
        [Parameter(Mandatory = $true)]
        [string]$Phase
    )

    foreach ($secondaryHandle in $SecondaryHandles) {
        $owner =
            [SpecForgeViewportOwnershipNative]::
                WindowOwner($secondaryHandle)
        Assert-True `
            -Condition (
                $owner -eq $MainHandle -and
                -not [SpecForgeViewportOwnershipNative]::
                    WindowIsTopMost($secondaryHandle)) `
            -Message (
                "$Phase expected detached HWND " +
                (Format-Handle -Handle $secondaryHandle) +
                ' to be a non-topmost window owned by main HWND ' +
                (Format-Handle -Handle $MainHandle) +
                '; actual owner is ' +
                (Format-Handle -Handle $owner) + '.')
    }
}

function Assert-OwnershipGroupZOrder {
    param(
        [Parameter(Mandatory = $true)]
        [IntPtr]$MainHandle,
        [Parameter(Mandatory = $true)]
        [IntPtr[]]$SecondaryHandles,
        [Parameter(Mandatory = $true)]
        [IntPtr]$PeerHandle
    )

    $visibleWindows = @(
        [SpecForgeViewportOwnershipNative]::
            VisibleWindowsInZOrder()
    )
    $indices = @{}
    for ($index = 0;
         $index -lt $visibleWindows.Count;
         ++$index) {
        $indices[
            (Format-Handle -Handle (
                [IntPtr]$visibleWindows[$index]))] =
                    $index
    }
    $groupHandles = @($SecondaryHandles) +
        @($MainHandle)
    $groupKeys = @(
        $groupHandles |
            ForEach-Object {
                Format-Handle -Handle $_
            }
    )
    $missingKeys = @(
        $groupKeys |
            Where-Object { -not $indices.ContainsKey($_) }
    )
    $peerKey = Format-Handle -Handle $PeerHandle
    Assert-True `
        -Condition (
            $missingKeys.Count -eq 0 -and
            $indices.ContainsKey($peerKey)) `
        -Message (
            'Restored z-order did not contain every SpecForge HWND and the ' +
            'peer HWND. Missing: ' + ($missingKeys -join ', ') + '.')

    $groupIndices = @(
        $groupKeys |
            ForEach-Object { [int]$indices[$_] }
    )
    $mainIndex =
        [int]$indices[
            (Format-Handle -Handle $MainHandle)]
    $minimumGroupIndex =
        ($groupIndices | Measure-Object -Minimum).Minimum
    $maximumGroupIndex =
        ($groupIndices | Measure-Object -Maximum).Maximum
    $secondaryAboveOwner =
        @(
            $SecondaryHandles |
                Where-Object {
                    [int]$indices[
                        (Format-Handle -Handle $_)] -ge
                            $mainIndex
                }
        ).Count -eq 0
    Assert-True `
        -Condition (
            $secondaryAboveOwner -and
            $maximumGroupIndex - $minimumGroupIndex + 1 -eq
                $groupHandles.Count -and
            [int]$indices[$peerKey] -gt
                $maximumGroupIndex) `
        -Message (
            'Restored SpecForge HWNDs must form one contiguous non-topmost ' +
            'ownership group above the peer window. Group indexes: ' +
            ($groupIndices -join ', ') +
            "; peer index: $($indices[$peerKey]).")
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
$resolvedWindowPeer =
    (Resolve-Path -LiteralPath $WindowPeer).Path
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
$peerProcess = $null
$peerStderrTask = $null
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
    $secondaryHandles = @(
        $windows.Secondary |
            ForEach-Object { [IntPtr]$_.Handle }
    )
    $secondaryHandle = $secondaryHandles[0]
    Assert-Win32Ownership `
        -MainHandle $mainHandle `
        -SecondaryHandles $secondaryHandles `
        -Phase 'Initial detached viewport creation'
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
    Assert-SameViewportWindows `
        -GuiProcessId $guiProcessId `
        -MainHandle $mainHandle `
        -SecondaryHandles $secondaryHandles `
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
    Assert-SameViewportWindows `
        -GuiProcessId $guiProcessId `
        -MainHandle $mainHandle `
        -SecondaryHandles $secondaryHandles `
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

    $peer = Start-WindowPeer `
        -ExecutablePath $resolvedWindowPeer
    $peerProcess = $peer.Process
    $peerStderrTask = $peer.StderrTask
    $peerHandle = [IntPtr]$peer.Handle
    Assert-True `
        -Condition (
            [SpecForgeViewportOwnershipNative]::
                WindowOwner($peerHandle) -eq
                    [IntPtr]::Zero -and
            -not [SpecForgeViewportOwnershipNative]::
                WindowIsTopMost($peerHandle)) `
        -Message 'The Win32 test peer must be an ordinary unowned non-topmost window.'

    [void][SpecForgeViewportOwnershipNative]::
        ShowWindowAsync($mainHandle, 7)
    $minimizeDeadline =
        [DateTime]::UtcNow.AddSeconds(10)
    do {
        $visibleSecondaryHandles = @(
            $secondaryHandles |
                Where-Object {
                    [SpecForgeViewportOwnershipNative]::
                        IsWindowVisible($_)
                }
        )
        if ([SpecForgeViewportOwnershipNative]::
                IsIconic($mainHandle) -and
            $visibleSecondaryHandles.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $minimizeDeadline)
    $destroyedWhileMinimized = @(
        $secondaryHandles |
            Where-Object {
                -not [SpecForgeViewportOwnershipNative]::
                    IsWindow($_)
            }
    )
    Assert-True `
        -Condition (
            [SpecForgeViewportOwnershipNative]::
                IsIconic($mainHandle) -and
            $visibleSecondaryHandles.Count -eq 0 -and
            $destroyedWhileMinimized.Count -eq 0) `
        -Message (
            'Minimizing the owner must hide every detached viewport without ' +
            'destroying its HWND identity.')
    Assert-Win32Ownership `
        -MainHandle $mainHandle `
        -SecondaryHandles $secondaryHandles `
        -Phase 'Main-window minimize'

    $peerActivation =
        Send-WindowPeerCommand `
            -Process $peerProcess `
            -Command 'activate'
    Assert-True `
        -Condition (
            $peerActivation -eq (
                'activated ' +
                (Format-Handle -Handle $peerHandle) +
                ' true') -and
            [SpecForgeViewportOwnershipNative]::
                GetForegroundWindow() -eq
                    $peerHandle) `
        -Message (
            'The independent Win32 peer must become the foreground window ' +
            "while SpecForge is minimized. Peer response: $peerActivation")

    $mainActivation =
        Send-WindowPeerCommand `
            -Process $peerProcess `
            -Command (
                'activate-window ' +
                (Format-Handle -Handle $mainHandle))
    $restoreDeadline =
        [DateTime]::UtcNow.AddSeconds(10)
    do {
        $hiddenSecondaryHandles = @(
            $secondaryHandles |
                Where-Object {
                    -not [SpecForgeViewportOwnershipNative]::
                        IsWindowVisible($_)
                }
        )
        if (-not [SpecForgeViewportOwnershipNative]::
                IsIconic($mainHandle) -and
            $hiddenSecondaryHandles.Count -eq 0 -and
            [SpecForgeViewportOwnershipNative]::
                GetForegroundWindow() -eq
                    $mainHandle) {
            break
        }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $restoreDeadline)
    Assert-True `
        -Condition (
            $mainActivation -eq (
                'activated ' +
                (Format-Handle -Handle $mainHandle) +
                ' true') -and
            -not [SpecForgeViewportOwnershipNative]::
                IsIconic($mainHandle) -and
            $hiddenSecondaryHandles.Count -eq 0 -and
            [SpecForgeViewportOwnershipNative]::
                GetForegroundWindow() -eq
                    $mainHandle) `
        -Message (
            'The foreground peer must restore and activate the SpecForge ' +
            'ownership group without leaving a detached viewport hidden. ' +
            "Peer response: $mainActivation")

    $restoredSpectralLines =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'panel set spectral_lines true' `
            -RequestId 'request-6'
    $restoredInformation =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'panel set information true' `
            -RequestId 'request-7'
    Assert-True `
        -Condition (
            [string]$restoredSpectralLines.status -eq
                'completed' -and
            [string]$restoredInformation.status -eq
                'completed' -and
            [uint64]$restoredInformation.result.frame_index -gt
                [uint64]$restoredSpectralLines.result.frame_index) `
        -Message 'Both restored detached viewports must complete later successful Present barriers.'
    Assert-SameViewportWindows `
        -GuiProcessId $guiProcessId `
        -MainHandle $mainHandle `
        -SecondaryHandles $secondaryHandles `
        -Phase 'Main-window restore after peer activation'
    Assert-Win32Ownership `
        -MainHandle $mainHandle `
        -SecondaryHandles $secondaryHandles `
        -Phase 'Main-window restore after peer activation'
    Assert-OwnershipGroupZOrder `
        -MainHandle $mainHandle `
        -SecondaryHandles $secondaryHandles `
        -PeerHandle $peerHandle

    $peerProcess.StandardInput.WriteLine('quit')
    $peerProcess.StandardInput.Flush()
    Assert-True `
        -Condition ($peerProcess.WaitForExit(10000)) `
        -Message 'Win32 test peer did not exit after quit.'
    Assert-True `
        -Condition ($peerProcess.ExitCode -eq 0) `
        -Message (
            'Win32 test peer exited with code ' +
            $peerProcess.ExitCode + '. ' +
            $peerStderrTask.GetAwaiter().GetResult())
    $peerProcess.Dispose()
    $peerProcess = $null

    $quit =
        Send-LauncherRequest `
            -Process $launcherProcess `
            -Line 'app quit' `
            -RequestId 'request-8'
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
        'ImGui viewport ownership integration passed with stable owned detached HWNDs ' +
        (($secondaryHandles | ForEach-Object {
            Format-Handle -Handle $_
        }) -join ', ') + '.')
    $testCompleted = $true
}
finally {
    if ($null -ne $peerProcess) {
        if (-not $peerProcess.HasExited) {
            try {
                $peerProcess.StandardInput.WriteLine('quit')
                $peerProcess.StandardInput.Flush()
            }
            catch {
            }
            if (-not $peerProcess.WaitForExit(3000)) {
                $peerProcess.Kill()
                [void]$peerProcess.WaitForExit(5000)
            }
        }
        $peerProcess.Dispose()
    }
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
