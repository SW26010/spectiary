[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Wrapper
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$wrapperText = Get-Content -Raw -LiteralPath $Wrapper

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)] [string]$Expected,
        [Parameter(Mandatory = $true)] [string]$Message
    )

    if (-not $wrapperText.Contains($Expected)) {
        throw $Message
    }
}

Assert-Contains `
    -Expected '$processStartTicks = $process.StartTime.ToUniversalTime().Ticks' `
    -Message 'Build wrapper must capture the launched process start identity.'
Assert-Contains `
    -Expected "('`$expectedStartTicks = ' + `$processStartTicks)" `
    -Message 'Independent watchdog must receive the launched process start identity.'
Assert-Contains `
    -Expected "'function Test-WatchedProcessIdentity {'" `
    -Message 'Independent watchdog must define a process identity guard.'
Assert-Contains `
    -Expected "'try { `$heldProcessHandle = `$processToWatch.Handle } catch {'" `
    -Message 'Independent watchdog must hold the original native process handle.'
Assert-Contains `
    -Expected "'        return (-not `$Process.HasExited -and `$Process.StartTime.ToUniversalTime().Ticks -eq `$expectedStartTicks)'" `
    -Message 'Watchdog identity guard must compare the held process creation time.'
$heldHandleFallback = @'
'    try { $processToWatch.Kill() } catch { Add-WatchdogLog ("held-handle kill failed: " + $_.Exception.Message) }'
'@
Assert-Contains `
    -Expected $heldHandleFallback `
    -Message 'Watchdog fallback must kill through its held process handle.'

$watchdogStart = $wrapperText.IndexOf(
    '    $watchdogLines = @(',
    [StringComparison]::Ordinal)
$watchdogEnd = $wrapperText.IndexOf(
    '    Set-Content -LiteralPath $watchdogPath',
    $watchdogStart,
    [StringComparison]::Ordinal)
if ($watchdogStart -lt 0 -or $watchdogEnd -le $watchdogStart) {
    throw 'Could not locate the generated independent watchdog body.'
}

$watchdogText = $wrapperText.Substring(
    $watchdogStart,
    $watchdogEnd - $watchdogStart)
$handleAcquisition = $watchdogText.IndexOf(
    "'try { `$heldProcessHandle = `$processToWatch.Handle } catch {'",
    [StringComparison]::Ordinal)
$watchdogSleep = $watchdogText.IndexOf(
    "'Start-Sleep -Seconds `$timeoutSec'",
    [StringComparison]::Ordinal)
$taskkill = $watchdogText.IndexOf(
    "'`$taskkillOutput = @(& taskkill.exe",
    [StringComparison]::Ordinal)
$finalIdentityCheck = $watchdogText.LastIndexOf(
    "'if (-not (Test-WatchedProcessIdentity -Process `$currentProcess)) {'",
    [StringComparison]::Ordinal)

if ($handleAcquisition -lt 0 -or $handleAcquisition -gt $watchdogSleep) {
    throw 'Watchdog must acquire and hold the original process before waiting.'
}
if ($finalIdentityCheck -lt $watchdogSleep -or $finalIdentityCheck -gt $taskkill) {
    throw 'Watchdog must revalidate PID creation identity immediately before taskkill.'
}
if ($watchdogText.Contains("'    Stop-Process -Id `$pidToWatch")) {
    throw 'Watchdog fallback must not target a PID that may have been reused.'
}

Write-Host 'Ninja/MSVC build watchdog identity contract passed.'
