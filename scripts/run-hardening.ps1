[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Asan', 'Analyze')]
    [string]$Mode,
    [string]$ArtifactsDirectory = '',
    [string]$VcvarsPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent $PSScriptRoot
$preset = if ($Mode -eq 'Asan') { 'ninja-msvc-asan' } else { 'ninja-msvc-analyze' }
$target = if ($Mode -eq 'Asan') { 'spectiary_asan_targets' } else { 'spectiary_static_analysis_targets' }
if (-not $ArtifactsDirectory) {
    $ArtifactsDirectory = Join-Path $repoRoot "logs\hardening\$Mode"
}
$ArtifactsDirectory = [IO.Path]::GetFullPath($ArtifactsDirectory)
New-Item -ItemType Directory -Path $ArtifactsDirectory -Force | Out-Null
$buildDirectory = Join-Path $repoRoot "build\$preset"
$originalPath = $env:PATH
$originalAsanOptions = $env:ASAN_OPTIONS
Push-Location $repoRoot
try {
    $buildArguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $PSScriptRoot 'build-ninja-msvc-debug.ps1'),
        '-Preset', $preset, '-LogDir', (Join-Path $ArtifactsDirectory 'build')
    )
    if ($VcvarsPath) { $buildArguments += @('-VcvarsPath', $VcvarsPath) }
    & powershell @buildArguments -Configure -TimeoutSec 600
    if ($LASTEXITCODE -ne 0) { throw "$Mode configure failed ($LASTEXITCODE)." }
    & powershell @buildArguments -Target $target -TimeoutSec 1200
    if ($LASTEXITCODE -ne 0) { throw "$Mode build failed ($LASTEXITCODE)." }

    if ($Mode -eq 'Asan') {
        # vcvars runs in the build wrapper's child process. CTest needs the
        # runtime directory from that exact compiler, not an unrelated LLVM.
        $cache = Get-Content -Raw -LiteralPath (Join-Path $buildDirectory 'CMakeCache.txt')
        $compiler = [regex]::Match($cache, '(?m)^CMAKE_CXX_COMPILER:[^=]+=(.+)\r?$')
        if (-not $compiler.Success) { throw 'Configured MSVC compiler was not recorded.' }
        $runtimeDirectory = Split-Path -Parent $compiler.Groups[1].Value.Trim()
        if (-not (Test-Path -LiteralPath (Join-Path $runtimeDirectory 'clang_rt.asan_dynamic-x86_64.dll'))) {
            throw "Install the Visual Studio C++ AddressSanitizer component for $runtimeDirectory."
        }
        $env:PATH = "$runtimeDirectory;$originalPath"
        # Own the gate's options so caller settings cannot enable recovery.
        # Keep Windows' default mismatch policy: uninstrumented yaml-cpp DLL
        # delete thunks call free, misclassifying valid cross-module new/delete.
        # Ordinary UAF, overflow and double-free detection remains enabled.
        $env:ASAN_OPTIONS = 'halt_on_error=1:abort_on_error=1:alloc_dealloc_mismatch=0'
        $ctest = [regex]::Match($cache, '(?m)^CMAKE_CTEST_COMMAND:[^=]+=(.+)\r?$')
        if (-not $ctest.Success) { throw 'Configured CTest executable was not recorded.' }
        & $ctest.Groups[1].Value.Trim() --preset asan --output-junit (Join-Path $ArtifactsDirectory 'asan.xml') `
            --output-log (Join-Path $ArtifactsDirectory 'asan.log')
        if ($LASTEXITCODE -ne 0) { throw "ASan tests failed ($LASTEXITCODE)." }
    }
}
finally {
    $analysisProbeLog = Join-Path $buildDirectory 'analysis-probe.log'
    if (Test-Path -LiteralPath $analysisProbeLog) {
        Copy-Item -LiteralPath $analysisProbeLog -Destination $ArtifactsDirectory -Force
    }
    $env:PATH = $originalPath
    $env:ASAN_OPTIONS = $originalAsanOptions
    Pop-Location
}
