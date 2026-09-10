[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DirectoryPath,
    [ValidateRange(1, 10000)][int]$Count = 1000,
    [ValidateRange(0, 100)][int]$LifetimeTrials = 20,
    [ValidateRange(5, 60)][int]$TimeoutSec = 20,
    [switch]$Child
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ($Child) {
    Add-Type -Path (Join-Path $PSScriptRoot 'directory-change-registration-probe.cs')
    [DirectoryRegistrationProbe]::Run($DirectoryPath, $Count, $LifetimeTrials) |
        ConvertTo-Json -Depth 4
    exit 0
}

# No preliminary path stat: that could itself block outside the watchdog.
if ($DirectoryPath.Contains('"')) { throw 'DirectoryPath cannot contain quotes.' }
$logRoot = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs\directory-registration'
[void](New-Item -ItemType Directory -Path $logRoot -Force)
$runId = [guid]::NewGuid().ToString('N')
$stdout = Join-Path $logRoot "$runId.json"
$stderr = Join-Path $logRoot "$runId.err.txt"
$childCommand = "& '{0}' -Child -DirectoryPath '{1}' -Count {2} -LifetimeTrials {3}" -f $PSCommandPath.Replace("'", "''"), $DirectoryPath.Replace("'", "''"), $Count, $LifetimeTrials
$encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($childCommand))
$arguments = "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encodedCommand"
$process = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$null = $process.Handle
try {
    if (!$process.WaitForExit($TimeoutSec * 1000)) {
        # Kill only the process created above, never a process selected by name.
        $process.Kill()
        $terminated = $process.WaitForExit(5000)
        throw "Probe exceeded ${TimeoutSec}s; termination confirmed=$terminated; output=$stdout"
    }
    if ($process.ExitCode -ne 0) {
        throw "Probe failed ($($process.ExitCode)): $(Get-Content -LiteralPath $stderr -Raw)"
    }
    Get-Content -LiteralPath $stdout -Raw
    Write-Host "Evidence: $stdout"
}
finally { $process.Dispose() }
