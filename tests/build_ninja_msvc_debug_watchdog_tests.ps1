[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Wrapper
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$wrapperText = Get-Content -Raw -LiteralPath $Wrapper
$resolvedWrapper = (Resolve-Path -LiteralPath $Wrapper).Path

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

$tokens = $null
$parseErrors = $null
$wrapperAst = [Management.Automation.Language.Parser]::ParseFile(
    $resolvedWrapper,
    [ref]$tokens,
    [ref]$parseErrors)
if ($parseErrors.Count -gt 0) {
    throw (
        'Build wrapper must parse before its resolver can be tested: ' +
        (($parseErrors | ForEach-Object Message) -join '; ')
    )
}
$resolverAst = $wrapperAst.Find(
    {
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Resolve-SpecForgeVcvarsPath'
    },
    $true)
if ($null -eq $resolverAst) {
    throw 'Build wrapper must define Resolve-SpecForgeVcvarsPath.'
}
. ([scriptblock]::Create($resolverAst.Extent.Text))

function Assert-PathEqual {
    param(
        [Parameter(Mandatory = $true)] [string]$Actual,
        [Parameter(Mandatory = $true)] [string]$Expected,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $actualFullPath = [IO.Path]::GetFullPath($Actual)
    $expectedFullPath = [IO.Path]::GetFullPath($Expected)
    if (-not $actualFullPath.Equals(
            $expectedFullPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description resolved '$actualFullPath', expected '$expectedFullPath'."
    }
}

$resolverRoot = Join-Path $env:TEMP (
    'specforge-vcvars-resolver-' + [guid]::NewGuid().ToString('N'))
$originalResolverEnvironment = $env:SPECFORGE_VCVARS_PATH
$originalVswhereResult = $env:SPECFORGE_TEST_VSWHERE_RESULT
$originalVswhereArguments = $env:SPECFORGE_TEST_VSWHERE_ARGS_FILE
try {
    New-Item -ItemType Directory -Path $resolverRoot -Force | Out-Null
    $explicitVcvars = Join-Path $resolverRoot 'explicit-vcvars64.bat'
    $environmentVcvars = Join-Path $resolverRoot 'environment-vcvars64.bat'
    $discoveredVcvars = Join-Path $resolverRoot 'discovered-vcvars64.bat'
    $legacyVcvars = Join-Path $resolverRoot 'legacy-vcvars64.bat'
    foreach ($path in @(
            $explicitVcvars,
            $environmentVcvars,
            $discoveredVcvars,
            $legacyVcvars
        )) {
        [IO.File]::WriteAllText($path, '@rem resolver contract fixture')
    }

    $fakeVswhere = Join-Path $resolverRoot 'vswhere.cmd'
    $vswhereArgumentsFile = Join-Path $resolverRoot 'vswhere-arguments.txt'
    [IO.File]::WriteAllText(
        $fakeVswhere,
        (@'
@echo off
if not "%SPECFORGE_TEST_VSWHERE_ARGS_FILE%"=="" echo %*>"%SPECFORGE_TEST_VSWHERE_ARGS_FILE%"
echo %SPECFORGE_TEST_VSWHERE_RESULT%
exit /b 0
'@).TrimStart())
    $env:SPECFORGE_TEST_VSWHERE_RESULT = $discoveredVcvars
    $env:SPECFORGE_TEST_VSWHERE_ARGS_FILE = $vswhereArgumentsFile

    $env:SPECFORGE_VCVARS_PATH = $environmentVcvars
    $resolved = Resolve-SpecForgeVcvarsPath `
        -ExplicitPath $explicitVcvars `
        -VswherePath $fakeVswhere `
        -LegacyPath $legacyVcvars
    Assert-PathEqual `
        -Actual $resolved `
        -Expected $explicitVcvars `
        -Description 'Explicit vcvars candidate'
    if (Test-Path -LiteralPath $vswhereArgumentsFile) {
        throw 'Explicit vcvars resolution must not invoke vswhere.'
    }

    $resolved = Resolve-SpecForgeVcvarsPath `
        -ExplicitPath (Join-Path $resolverRoot 'missing-explicit.bat') `
        -VswherePath $fakeVswhere `
        -LegacyPath $legacyVcvars
    Assert-PathEqual `
        -Actual $resolved `
        -Expected $environmentVcvars `
        -Description 'Environment vcvars fallback'
    if (Test-Path -LiteralPath $vswhereArgumentsFile) {
        throw 'Environment vcvars resolution must not invoke vswhere.'
    }

    $env:SPECFORGE_VCVARS_PATH = Join-Path $resolverRoot 'missing-environment.bat'
    $resolved = Resolve-SpecForgeVcvarsPath `
        -ExplicitPath (Join-Path $resolverRoot 'missing-explicit.bat') `
        -VswherePath $fakeVswhere `
        -LegacyPath $legacyVcvars
    Assert-PathEqual `
        -Actual $resolved `
        -Expected $discoveredVcvars `
        -Description 'vswhere vcvars fallback'
    $vswhereArguments = Get-Content -Raw -LiteralPath $vswhereArgumentsFile
    foreach ($requiredArgument in @(
            '-latest',
            '-products *',
            '-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '-find VC\Auxiliary\Build\vcvars64.bat'
        )) {
        if (-not $vswhereArguments.Contains($requiredArgument)) {
            throw "vswhere invocation must include '$requiredArgument'."
        }
    }

    $env:SPECFORGE_TEST_VSWHERE_RESULT = Join-Path $resolverRoot 'missing-discovered.bat'
    $resolved = Resolve-SpecForgeVcvarsPath `
        -ExplicitPath (Join-Path $resolverRoot 'missing-explicit.bat') `
        -VswherePath $fakeVswhere `
        -LegacyPath $legacyVcvars
    Assert-PathEqual `
        -Actual $resolved `
        -Expected $legacyVcvars `
        -Description 'Legacy vcvars fallback'

    $missingLegacy = Join-Path $resolverRoot 'missing-legacy.bat'
    $failureMessage = ''
    try {
        Resolve-SpecForgeVcvarsPath `
            -ExplicitPath (Join-Path $resolverRoot 'missing-explicit.bat') `
            -VswherePath $fakeVswhere `
            -LegacyPath $missingLegacy | Out-Null
    }
    catch {
        $failureMessage = $_.Exception.Message
    }
    foreach ($expectedDiagnostic in @(
            'Unable to locate vcvars64.bat',
            'explicit -VcvarsPath',
            'SPECFORGE_VCVARS_PATH',
            'vswhere',
            'legacy VS 2022 BuildTools'
        )) {
        if (-not $failureMessage.Contains($expectedDiagnostic)) {
            throw "All-invalid resolver failure must report '$expectedDiagnostic'."
        }
    }
}
finally {
    $env:SPECFORGE_VCVARS_PATH = $originalResolverEnvironment
    $env:SPECFORGE_TEST_VSWHERE_RESULT = $originalVswhereResult
    $env:SPECFORGE_TEST_VSWHERE_ARGS_FILE = $originalVswhereArguments
    if (Test-Path -LiteralPath $resolverRoot) {
        Remove-Item -LiteralPath $resolverRoot -Recurse -Force
    }
}

Write-Host 'Ninja/MSVC build watchdog identity contract passed.'
