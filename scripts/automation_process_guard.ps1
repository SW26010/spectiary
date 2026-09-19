# Shared bounded process ownership helpers for automation runners.
# Keep the Win32 Job Object definition in one place so the sample runner and
# the CTest wrapper cannot drift into competing process-tree implementations.

if (-not ('SpectiaryAutomationProcessGuard.NativeMethods' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace SpectiaryAutomationProcessGuard
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

function Get-AutomationWin32ErrorMessage {
    $errorCode = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return "${errorCode}: $((New-Object System.ComponentModel.Win32Exception($errorCode)).Message)"
}

function Close-AutomationNativeHandle {
    param(
        [Parameter(Mandatory = $true)]
        [IntPtr]$Handle
    )

    if ($Handle -ne [IntPtr]::Zero) {
        [void][SpectiaryAutomationProcessGuard.NativeMethods]::CloseHandle($Handle)
    }
}

function New-AutomationKillOnCloseJob {
    $jobHandle = [SpectiaryAutomationProcessGuard.NativeMethods]::CreateJobObject(
        [IntPtr]::Zero,
        $null)
    if ($jobHandle -eq [IntPtr]::Zero) {
        throw "CreateJobObject failed: $(Get-AutomationWin32ErrorMessage)"
    }

    $info = New-Object SpectiaryAutomationProcessGuard.JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    $info.BasicLimitInformation.LimitFlags =
        [SpectiaryAutomationProcessGuard.NativeMethods]::JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    $length = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
    $buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($length)
    try {
        [System.Runtime.InteropServices.Marshal]::StructureToPtr($info, $buffer, $false)
        $ok = [SpectiaryAutomationProcessGuard.NativeMethods]::SetInformationJobObject(
            $jobHandle,
            [SpectiaryAutomationProcessGuard.NativeMethods]::JobObjectExtendedLimitInformation,
            $buffer,
            [uint32]$length)
        if (-not $ok) {
            $message = Get-AutomationWin32ErrorMessage
            Close-AutomationNativeHandle -Handle $jobHandle
            throw "SetInformationJobObject failed: $message"
        }
    }
    finally {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    }
    return $jobHandle
}

function Assign-AutomationProcessToJob {
    param(
        [Parameter(Mandatory = $true)]
        [IntPtr]$JobHandle,
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$ProcessHandle
    )

    if ($JobHandle -eq [IntPtr]::Zero) {
        throw 'Cannot assign a process to an empty automation Job Object handle.'
    }
    if ($null -eq $ProcessHandle) {
        throw 'Cannot assign a missing process to the automation Job Object.'
    }
    if ($ProcessHandle.HasExited) {
        # The held process handle proves that the launch completed and the
        # process is already gone; there is no live child left for the Job.
        return
    }
    if (-not [SpectiaryAutomationProcessGuard.NativeMethods]::AssignProcessToJobObject(
            $JobHandle,
            $ProcessHandle.Handle)) {
        throw "AssignProcessToJobObject failed: $(Get-AutomationWin32ErrorMessage)"
    }
}

function Stop-AutomationProcessHandle {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$ProcessHandle,
        [Parameter(Mandatory = $true)]
        [int]$WaitMilliseconds
    )

    if ($ProcessHandle.HasExited) {
        return
    }
    try {
        [void]$ProcessHandle.CloseMainWindow()
    }
    catch {
        # A console/helper process may not have a window. The held process
        # handle remains the ownership authority for the bounded kill below.
    }
    if ($ProcessHandle.WaitForExit($WaitMilliseconds)) {
        return
    }
    $ProcessHandle.Kill()
    if (-not $ProcessHandle.WaitForExit($WaitMilliseconds)) {
        throw "Owned process PID $($ProcessHandle.Id) remained after a bounded handle-based kill."
    }
}
