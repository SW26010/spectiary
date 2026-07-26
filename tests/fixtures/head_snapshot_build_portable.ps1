[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset,
    [string]$Configuration,
    [string]$PackageName,
    [string]$DistRoot,
    [string]$SourceMode,
    [string]$SourceRevision
)

$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$snapshotRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
if (Test-Path -LiteralPath (Join-Path $snapshotRoot '.git')) {
    throw 'HEAD snapshot retained Git repository metadata.'
}

$trackedValue = (
    Get-Content -Raw -LiteralPath (Join-Path $snapshotRoot 'tracked-sentinel.txt')
).Trim()
if ($trackedValue -cne 'committed') {
    throw "HEAD snapshot used dirty tracked content: '$trackedValue'."
}
if (Test-Path -LiteralPath (Join-Path $snapshotRoot 'untracked-sentinel.txt')) {
    throw 'HEAD snapshot included an untracked file.'
}

$packageRoot = Join-Path $DistRoot $PackageName
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
[ordered]@{
    schema_version = 3
    source_mode = $SourceMode
    source_revision = $SourceRevision
    compiler_id = 'MSVC'
    compiler_version = '19.0'
    cmake_version = '4.0.0'
    generator = 'fixture'
    target_architecture = 'x64'
    windows_sdk_version = '10.0.0.0'
} |
    ConvertTo-Json |
    Set-Content `
        -LiteralPath (Join-Path $packageRoot 'specforge_build_metadata.json') `
        -Encoding UTF8

[ordered]@{
    snapshot_root = $snapshotRoot
    preset = $Preset
    configuration = $Configuration
    package_name = $PackageName
    source_mode = $SourceMode
    source_revision = $SourceRevision
    tracked_value = $trackedValue
} |
    ConvertTo-Json |
    Set-Content `
        -LiteralPath (Join-Path $DistRoot 'head-wrapper-invocation.json') `
        -Encoding UTF8
