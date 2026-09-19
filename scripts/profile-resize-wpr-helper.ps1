[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Start', 'Stop')][string]$Action,
    [Parameter(Mandatory)][ValidatePattern('^SpectiaryResize-[a-f0-9]{32}$')][string]$Instance,
    [Parameter(Mandatory)][string]$Directory
)
$ErrorActionPreference = 'Stop'
$Directory = (Resolve-Path -LiteralPath $Directory).Path
$logRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../logs')) + [IO.Path]::DirectorySeparatorChar
if (!$Directory.StartsWith($logRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Trace directory must be inside repository logs.' }
$log = Join-Path $Directory ('wpr-' + $Action.ToLowerInvariant() + '.log')
try {
    $wpr = Join-Path $env:SystemRoot 'System32/wpr.exe'
    if ($Action -eq 'Start') {
        & $wpr -profiledetails CPU 2>&1 | Out-File (Join-Path $Directory 'wpr-profile.txt')
        if ($LASTEXITCODE -ne 0) { throw 'CPU profile unavailable' }
        & $wpr -start CPU.Verbose -filemode -recordtempto $Directory -instancename $Instance 2>&1 | Out-File $log
    } else {
        & $wpr -stop (Join-Path $Directory 'system-trace.etl') -instancename $Instance 2>&1 | Out-File $log
    }
    $result = $LASTEXITCODE
    if ($result -ne 0) { throw "WPR $Action failed: $result. See $log" }
    exit 0
} catch {
    $_ | Out-String | Add-Content -LiteralPath $log
    exit 1
}
