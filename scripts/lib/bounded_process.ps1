# Bounded external-process runner for repository validation scripts.
# A kill-on-close Job Object owns each process tree so a per-case timeout can
# terminate descendants as well as the directly launched process.

if (-not ('SpecForgeBoundedProcess.NativeMethods' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace SpecForgeBoundedProcess
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
        private const uint TH32CS_SNAPPROCESS = 0x00000002;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct PROCESSENTRY32
        {
            public uint dwSize;
            public uint cntUsage;
            public uint th32ProcessID;
            public IntPtr th32DefaultHeapID;
            public uint th32ModuleID;
            public uint cntThreads;
            public uint th32ParentProcessID;
            public int pcPriClassBase;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string szExeFile;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateJobObject(IntPtr attributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool SetInformationJobObject(
            IntPtr job,
            int informationClass,
            IntPtr information,
            uint informationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool Process32First(IntPtr snapshot, ref PROCESSENTRY32 entry);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool Process32Next(IntPtr snapshot, ref PROCESSENTRY32 entry);

        public static int[] GetDescendantProcessIds(int rootProcessId)
        {
            IntPtr snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == new IntPtr(-1))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            try
            {
                var entries = new List<PROCESSENTRY32>();
                var entry = new PROCESSENTRY32();
                entry.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32));
                if (Process32First(snapshot, ref entry))
                {
                    do
                    {
                        entries.Add(entry);
                        entry.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32));
                    }
                    while (Process32Next(snapshot, ref entry));
                }

                var tree = new HashSet<uint>();
                tree.Add((uint)rootProcessId);
                var descendants = new List<int>();
                bool changed;
                do
                {
                    changed = false;
                    foreach (PROCESSENTRY32 process in entries)
                    {
                        if (process.th32ProcessID == (uint)rootProcessId ||
                            tree.Contains(process.th32ProcessID) ||
                            !tree.Contains(process.th32ParentProcessID))
                        {
                            continue;
                        }
                        tree.Add(process.th32ProcessID);
                        descendants.Add((int)process.th32ProcessID);
                        changed = true;
                    }
                }
                while (changed);
                return descendants.ToArray();
            }
            finally
            {
                CloseHandle(snapshot);
            }
        }
    }
}
'@
}

function Get-SpecForgeBoundedProcessWin32Error {
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return "${errorCode}: $((New-Object ComponentModel.Win32Exception($errorCode)).Message)"
}

function Close-SpecForgeBoundedProcessJob {
    param(
        [Parameter(Mandatory = $true)] [IntPtr]$Handle
    )

    if ($Handle -ne [IntPtr]::Zero) {
        [void][SpecForgeBoundedProcess.NativeMethods]::CloseHandle($Handle)
    }
}

function New-SpecForgeBoundedProcessJob {
    $jobHandle = [SpecForgeBoundedProcess.NativeMethods]::CreateJobObject(
        [IntPtr]::Zero,
        $null)
    if ($jobHandle -eq [IntPtr]::Zero) {
        throw "CreateJobObject failed: $(Get-SpecForgeBoundedProcessWin32Error)"
    }

    $information =
        New-Object SpecForgeBoundedProcess.JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    # BasicLimitInformation is a value-type field. PowerShell returns a copy
    # when a nested struct is read, so update the copy and explicitly assign
    # it back before marshaling the enclosing structure.
    $basicLimitInformation = $information.BasicLimitInformation
    $basicLimitInformation.LimitFlags =
        [SpecForgeBoundedProcess.NativeMethods]::JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    $information.BasicLimitInformation = $basicLimitInformation
    $length = [Runtime.InteropServices.Marshal]::SizeOf($information)
    $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal($length)
    try {
        [Runtime.InteropServices.Marshal]::StructureToPtr(
            $information,
            $buffer,
            $false)
        $configured =
            [SpecForgeBoundedProcess.NativeMethods]::SetInformationJobObject(
                $jobHandle,
                [SpecForgeBoundedProcess.NativeMethods]::JobObjectExtendedLimitInformation,
                $buffer,
                [uint32]$length)
        if (-not $configured) {
            $message = Get-SpecForgeBoundedProcessWin32Error
            Close-SpecForgeBoundedProcessJob -Handle $jobHandle
            throw "SetInformationJobObject failed: $message"
        }
    }
    finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    }
    return $jobHandle
}

function Add-SpecForgeProcessToBoundedJob {
    param(
        [Parameter(Mandatory = $true)] [IntPtr]$JobHandle,
        [Parameter(Mandatory = $true)] [Diagnostics.Process]$Process
    )

    if ($Process.HasExited) {
        return
    }
    $assigned =
        [SpecForgeBoundedProcess.NativeMethods]::AssignProcessToJobObject(
            $JobHandle,
            $Process.Handle)
    if (-not $assigned) {
        throw "AssignProcessToJobObject failed: $(Get-SpecForgeBoundedProcessWin32Error)"
    }
}

function Get-SpecForgeDescendantProcessHandles {
    param(
        [Parameter(Mandatory = $true)] [int]$RootProcessId
    )

    foreach ($processId in
        [SpecForgeBoundedProcess.NativeMethods]::GetDescendantProcessIds(
            $RootProcessId)) {
        try {
            $process = [Diagnostics.Process]::GetProcessById($processId)
            # Force acquisition of the process handle before cleanup starts;
            # the held handle, rather than a later PID lookup, remains the
            # identity authority if Windows recycles the PID.
            [void]$process.Handle
            Write-Output $process
        }
        catch [ArgumentException] {
            # The descendant exited between the snapshot and handle lookup.
        }
    }
}

function Stop-SpecForgeDescendantProcessHandles {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [Diagnostics.Process[]]$Processes
    )

    try {
        for ($index = $Processes.Count - 1; $index -ge 0; $index--) {
            $process = $Processes[$index]
            if ($process.HasExited) {
                continue
            }
            $process.Kill()
            if (-not $process.WaitForExit(5000)) {
                throw (
                    'Descendant PID {0} remained after bounded handle-based cleanup.' -f
                        $process.Id)
            }
        }
    }
    finally {
        foreach ($process in $Processes) {
            $process.Dispose()
        }
    }
}

function ConvertTo-SpecForgeCommandLineArgument {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Argument
    )

    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    $builder = [Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $backslashCount = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $backslashCount++
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append('\', (($backslashCount * 2) + 1))
            [void]$builder.Append('"')
            $backslashCount = 0
            continue
        }
        if ($backslashCount -gt 0) {
            [void]$builder.Append('\', $backslashCount)
            $backslashCount = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashCount -gt 0) {
        [void]$builder.Append('\', ($backslashCount * 2))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-SpecForgeCommandLineArguments {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyCollection()] [object[]]$Arguments
    )

    return @(
        foreach ($argument in $Arguments) {
            ConvertTo-SpecForgeCommandLineArgument -Argument ([string]$argument)
        }
    ) -join ' '
}

function Get-SpecForgeBoundedProcessStream {
    param(
        [Parameter(Mandatory = $true)] [object]$Task,
        [Parameter(Mandatory = $true)] [string]$Name,
        [int]$DrainTimeoutMilliseconds = 5000
    )

    try {
        if (-not $Task.Wait($DrainTimeoutMilliseconds)) {
            return "<$Name did not drain within ${DrainTimeoutMilliseconds}ms>"
        }
        return [string]$Task.Result
    }
    catch {
        return "<$Name could not be collected: $($_.Exception.Message)>"
    }
}

function Get-SpecForgeBoundedProcessCaptureFile {
    param(
        [Parameter(Mandatory = $true)] [string]$Path
    )

    if (-not [IO.File]::Exists($Path)) {
        return ''
    }
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        try {
            return $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Join-SpecForgeBoundedProcessOutput {
    param(
        [AllowEmptyString()] [string]$TargetOutput,
        [AllowEmptyString()] [string]$BootstrapOutput
    )

    $parts = [Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrEmpty($TargetOutput)) {
        $parts.Add($TargetOutput.TrimEnd("`r", "`n"))
    }
    if (-not [string]::IsNullOrEmpty($BootstrapOutput)) {
        $parts.Add($BootstrapOutput.TrimEnd("`r", "`n"))
    }
    return $parts -join [Environment]::NewLine
}

function Get-SpecForgeOutputTail {
    param(
        [AllowEmptyString()] [string]$Text,
        [int]$LineCount = 20
    )

    if ([string]::IsNullOrEmpty($Text)) {
        return '<empty>'
    }
    $trimmed = $Text.TrimEnd("`r", "`n")
    if ([string]::IsNullOrEmpty($trimmed)) {
        return '<empty>'
    }
    $lines = @($trimmed -split '\r?\n')
    $first = [Math]::Max(0, $lines.Count - $LineCount)
    return @($lines[$first..($lines.Count - 1)]) -join [Environment]::NewLine
}

function Get-SpecForgeBoundedBootstrapEncodedCommand {
    $bootstrapScript = @'
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$gate = $null
$targetProcess = $null
$stdoutFile = $null
$stderrFile = $null
$stdoutCopyTask = $null
$stderrCopyTask = $null
try {
    $gate = [Threading.EventWaitHandle]::OpenExisting(
        $env:SPECFORGE_BOUNDED_PROCESS_GATE)
    if (-not $gate.WaitOne(30000)) {
        throw 'Bounded process launch gate timed out before Job assignment.'
    }
    $serializedBytes = [Convert]::FromBase64String(
        $env:SPECFORGE_BOUNDED_PROCESS_SPEC)
    $serialized = [Text.Encoding]::Unicode.GetString($serializedBytes)
    $specification =
        [Management.Automation.PSSerializer]::Deserialize($serialized)
    $targetStartInfo = [Diagnostics.ProcessStartInfo]::new()
    $targetStartInfo.FileName = [string]$specification.FilePath
    $targetStartInfo.Arguments = [string]$specification.ArgumentString
    $targetStartInfo.UseShellExecute = $false
    $targetStartInfo.CreateNoWindow = $true
    $targetStartInfo.RedirectStandardOutput = $true
    $targetStartInfo.RedirectStandardError = $true
    $targetProcess = [Diagnostics.Process]::new()
    $targetProcess.StartInfo = $targetStartInfo
    $stdoutFile = [IO.FileStream]::new(
        [string]$specification.StdoutPath,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        [IO.FileShare]::Read,
        1,
        [IO.FileOptions]::WriteThrough)
    $stderrFile = [IO.FileStream]::new(
        [string]$specification.StderrPath,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        [IO.FileShare]::Read,
        1,
        [IO.FileOptions]::WriteThrough)
    if (-not $targetProcess.Start()) {
        throw "Failed to start bounded target '$($specification.FilePath)'."
    }
    $stdoutCopyTask =
        $targetProcess.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
    $stderrCopyTask =
        $targetProcess.StandardError.BaseStream.CopyToAsync($stderrFile)
    $targetProcess.WaitForExit()
    $targetExitCode = $targetProcess.ExitCode
    foreach ($copyTask in @($stdoutCopyTask, $stderrCopyTask)) {
        try {
            [void]$copyTask.Wait(1000)
        }
        catch {
            # The root exit code remains authoritative. A surviving
            # descendant can keep an inherited pipe open until the parent
            # closes the Job immediately after this bootstrap exits.
        }
    }
    exit $targetExitCode
}
catch {
    [Console]::Error.WriteLine(($_ | Out-String))
    exit 125
}
finally {
    if ($null -ne $gate) {
        $gate.Dispose()
    }
    if ($null -ne $targetProcess) {
        if ($null -ne $stdoutCopyTask -and -not $stdoutCopyTask.IsCompleted) {
            $targetProcess.StandardOutput.Dispose()
        }
        if ($null -ne $stderrCopyTask -and -not $stderrCopyTask.IsCompleted) {
            $targetProcess.StandardError.Dispose()
        }
        foreach ($copyTask in @($stdoutCopyTask, $stderrCopyTask)) {
            if ($null -eq $copyTask) {
                continue
            }
            try {
                [void]$copyTask.Wait(1000)
            }
            catch {
                # Closing the reader is the bounded cancellation path for an
                # inherited pipe that remained open after the root exited.
            }
        }
        $targetProcess.Dispose()
    }
    if ($null -ne $stdoutFile) {
        $stdoutFile.Flush()
        $stdoutFile.Dispose()
    }
    if ($null -ne $stderrFile) {
        $stderrFile.Flush()
        $stderrFile.Dispose()
    }
}
'@
    $bytes = [Text.Encoding]::Unicode.GetBytes($bootstrapScript)
    return [Convert]::ToBase64String($bytes)
}

function Invoke-SpecForgeBoundedProcess {
    param(
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [Parameter(Mandatory = $true)] [AllowEmptyCollection()] [object[]]$Arguments,
        [Parameter(Mandatory = $true)] [ValidateRange(1, 2147483)] [int]$TimeoutSec,
        # Focused contract tests use this seam to prove that the controlled
        # target cannot start before the bootstrap is assigned to its Job.
        [scriptblock]$BeforeAssignmentProbe
    )

    $jobHandle = [IntPtr]::Zero
    $launchGate = $null
    $captureRootName =
        'specforge-bounded-process-' + [Guid]::NewGuid().ToString('N')
    $targetStdoutPath = Join-Path `
        ([IO.Path]::GetTempPath()) `
        ($captureRootName + '.stdout.log')
    $targetStderrPath = Join-Path `
        ([IO.Path]::GetTempPath()) `
        ($captureRootName + '.stderr.log')
    $process = $null
    $stdoutTask = $null
    $stderrTask = $null
    $stdout = ''
    $stderr = ''
    $processId = 0
    $exitCode = $null
    $timedOut = $false
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    try {
        $jobHandle = New-SpecForgeBoundedProcessJob
        $gateName = 'Local\SpecForgeBoundedProcess-' + [Guid]::NewGuid().ToString('N')
        $launchGate = [Threading.EventWaitHandle]::new(
            $false,
            [Threading.EventResetMode]::ManualReset,
            $gateName)
        $launchSpecification = [pscustomobject]@{
            FilePath = $FilePath
            ArgumentString =
                Join-SpecForgeCommandLineArguments -Arguments $Arguments
            StdoutPath = $targetStdoutPath
            StderrPath = $targetStderrPath
        }
        $serializedSpecification =
            [Management.Automation.PSSerializer]::Serialize(
                $launchSpecification)
        $encodedSpecification = [Convert]::ToBase64String(
            [Text.Encoding]::Unicode.GetBytes($serializedSpecification))
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = Join-Path $PSHOME 'powershell.exe'
        $startInfo.Arguments = Join-SpecForgeCommandLineArguments -Arguments @(
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy',
            'Bypass',
            '-EncodedCommand',
            (Get-SpecForgeBoundedBootstrapEncodedCommand)
        )
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.EnvironmentVariables['SPECFORGE_BOUNDED_PROCESS_GATE'] =
            $gateName
        $startInfo.EnvironmentVariables['SPECFORGE_BOUNDED_PROCESS_SPEC'] =
            $encodedSpecification

        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Failed to start '$FilePath'."
        }
        $processId = $process.Id
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if ($null -ne $BeforeAssignmentProbe) {
            & $BeforeAssignmentProbe
        }
        Add-SpecForgeProcessToBoundedJob `
            -JobHandle $jobHandle `
            -Process $process
        # The controlled bootstrap cannot launch the requested target before
        # this signal. Assignment therefore precedes target creation, and Job
        # membership is inherited by every target descendant.
        [void]$launchGate.Set()

        if (-not $process.WaitForExit($TimeoutSec * 1000)) {
            $timedOut = $true
            $descendants = @(
                Get-SpecForgeDescendantProcessHandles `
                    -RootProcessId $processId
            )
            Close-SpecForgeBoundedProcessJob -Handle $jobHandle
            $jobHandle = [IntPtr]::Zero
            if (-not $process.WaitForExit(5000)) {
                $process.Kill()
                if (-not $process.WaitForExit(5000)) {
                    throw "Timed-out process PID $processId remained after bounded cleanup."
                }
            }
            Stop-SpecForgeDescendantProcessHandles -Processes $descendants
            # A descendant can be created between the first snapshot and Job
            # closure in a constrained nested-job environment. Repeat the
            # held-handle sweep after the root has stopped.
            for ($cleanupPass = 0; $cleanupPass -lt 3; $cleanupPass++) {
                $escapedDescendants = @(
                    Get-SpecForgeDescendantProcessHandles `
                        -RootProcessId $processId
                )
                if ($escapedDescendants.Count -eq 0) {
                    break
                }
                Stop-SpecForgeDescendantProcessHandles `
                    -Processes $escapedDescendants
                Start-Sleep -Milliseconds 50
            }
        }
        else {
            $process.WaitForExit()
            $exitCode = $process.ExitCode
            # Closing the kill-on-close Job also removes any descendant that
            # improperly survived after the root process exited.
            Close-SpecForgeBoundedProcessJob -Handle $jobHandle
            $jobHandle = [IntPtr]::Zero
        }

        $bootstrapStdout = Get-SpecForgeBoundedProcessStream `
            -Task $stdoutTask `
            -Name 'stdout'
        $bootstrapStderr = Get-SpecForgeBoundedProcessStream `
            -Task $stderrTask `
            -Name 'stderr'
        $stdout = Join-SpecForgeBoundedProcessOutput `
            -TargetOutput (
                Get-SpecForgeBoundedProcessCaptureFile `
                    -Path $targetStdoutPath) `
            -BootstrapOutput $bootstrapStdout
        $stderr = Join-SpecForgeBoundedProcessOutput `
            -TargetOutput (
                Get-SpecForgeBoundedProcessCaptureFile `
                    -Path $targetStderrPath) `
            -BootstrapOutput $bootstrapStderr
    }
    finally {
        $stopwatch.Stop()
        if ($jobHandle -ne [IntPtr]::Zero) {
            Close-SpecForgeBoundedProcessJob -Handle $jobHandle
        }
        if ($null -ne $launchGate) {
            $launchGate.Dispose()
        }
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) {
                    $process.Kill()
                    [void]$process.WaitForExit(5000)
                }
            }
            catch {
                # Preserve the original launch/wait failure. The Job handle
                # has already provided the process-tree cleanup authority.
            }
            $process.Dispose()
        }
        foreach ($capturePath in @($targetStdoutPath, $targetStderrPath)) {
            if ([IO.File]::Exists($capturePath)) {
                [IO.File]::Delete($capturePath)
            }
        }
    }

    return [pscustomobject]@{
        ProcessId = $processId
        ExitCode = $exitCode
        TimedOut = $timedOut
        ElapsedMilliseconds = [long]$stopwatch.Elapsed.TotalMilliseconds
        Stdout = $stdout
        Stderr = $stderr
    }
}

function New-SpecForgeBoundedCaseFailureMessage {
    param(
        [Parameter(Mandatory = $true)] [string]$CaseId,
        [Parameter(Mandatory = $true)] [string]$Reason,
        [Parameter(Mandatory = $true)] [psobject]$Result
    )

    $stdoutTail = Get-SpecForgeOutputTail -Text $Result.Stdout
    $stderrTail = Get-SpecForgeOutputTail -Text $Result.Stderr
    return @(
        "Case '$CaseId' $Reason",
        "PID: $($Result.ProcessId)",
        "Elapsed milliseconds: $($Result.ElapsedMilliseconds)",
        'stdout tail:',
        $stdoutTail,
        'stderr tail:',
        $stderrTail
    ) -join [Environment]::NewLine
}

function Invoke-SpecForgeBoundedValidationCase {
    param(
        [Parameter(Mandatory = $true)] [ValidatePattern('^[a-z0-9][a-z0-9-]*$')] [string]$CaseId,
        [Parameter(Mandatory = $true)] [string]$Description,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [Parameter(Mandatory = $true)] [AllowEmptyCollection()] [object[]]$Arguments,
        [Parameter(Mandatory = $true)] [ValidateRange(1, 2147483)] [int]$TimeoutSec,
        [Parameter(Mandatory = $true)] [ValidateSet('Success', 'Failure')] [string]$ExpectedOutcome,
        [string]$ExpectedMessage,
        [switch]$EchoOutput
    )

    Write-Host (
        'CASE BEGIN id={0} timeout_sec={1} description={2}' -f
            $CaseId,
            $TimeoutSec,
            $Description)
    try {
        $result = Invoke-SpecForgeBoundedProcess `
            -FilePath $FilePath `
            -Arguments $Arguments `
            -TimeoutSec $TimeoutSec
    }
    catch {
        Write-Host "CASE END id=$CaseId status=runner-error"
        throw
    }

    if ($result.TimedOut) {
        Write-Host (
            'CASE END id={0} status=timeout elapsed_ms={1} pid={2}' -f
                $CaseId,
                $result.ElapsedMilliseconds,
                $result.ProcessId)
        throw (New-SpecForgeBoundedCaseFailureMessage `
                -CaseId $CaseId `
                -Reason "timed out after ${TimeoutSec}s." `
                -Result $result)
    }

    $expectsSuccess = $ExpectedOutcome -ceq 'Success'
    $succeeded = $result.ExitCode -eq 0
    if ($succeeded -ne $expectsSuccess) {
        $status = if ($succeeded) { 'unexpected-success' } else { 'unexpected-failure' }
        Write-Host (
            'CASE END id={0} status={1} elapsed_ms={2} pid={3} exit_code={4}' -f
                $CaseId,
                $status,
                $result.ElapsedMilliseconds,
                $result.ProcessId,
                $result.ExitCode)
        $reason = if ($succeeded) {
            'unexpectedly succeeded.'
        }
        else {
            "failed with exit code $($result.ExitCode)."
        }
        throw (New-SpecForgeBoundedCaseFailureMessage `
                -CaseId $CaseId `
                -Reason $reason `
                -Result $result)
    }

    $combinedOutput = @($result.Stdout, $result.Stderr) -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($ExpectedMessage) -and
        $combinedOutput.IndexOf($ExpectedMessage, [StringComparison]::Ordinal) -lt 0) {
        Write-Host (
            'CASE END id={0} status=wrong-failure elapsed_ms={1} pid={2} exit_code={3}' -f
                $CaseId,
                $result.ElapsedMilliseconds,
                $result.ProcessId,
                $result.ExitCode)
        throw (New-SpecForgeBoundedCaseFailureMessage `
                -CaseId $CaseId `
                -Reason "failed without expected message '$ExpectedMessage'." `
                -Result $result)
    }

    if ($EchoOutput) {
        if (-not [string]::IsNullOrEmpty($result.Stdout)) {
            Write-Host $result.Stdout.TrimEnd()
        }
        if (-not [string]::IsNullOrEmpty($result.Stderr)) {
            Write-Host $result.Stderr.TrimEnd()
        }
    }
    Write-Host (
        'CASE END id={0} status=passed elapsed_ms={1} pid={2} exit_code={3}' -f
            $CaseId,
            $result.ElapsedMilliseconds,
            $result.ProcessId,
            $result.ExitCode)
    return $result
}
