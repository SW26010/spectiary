[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset = 'snapshot-owned-portable-preset',
    [string]$Configuration = 'SnapshotRelease',
    [string]$PackageName,
    [string]$DistRoot,
    [string]$SourceMode,
    [string]$SourceRevision
)

$ErrorActionPreference = 'Stop'
$presetWasBound = $PSBoundParameters.ContainsKey('Preset')
$configurationWasBound = $PSBoundParameters.ContainsKey('Configuration')

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
Set-Content `
    -LiteralPath (Join-Path $packageRoot 'SpecForge.exe') `
    -Value 'fixture executable' `
    -Encoding ASCII
[ordered]@{
    schema_version = 3
    source_mode = $SourceMode
    source_revision = $SourceRevision
    specforge_version = 'fixture'
    release_profile = 'Portable'
    configuration = $Configuration
    compiler_id = 'MSVC'
    compiler_version = '19.0'
    cmake_version = '3.0.0'
    generator = 'fixture'
    target_architecture = 'x64'
    windows_sdk_version = '10.0.0.0'
    dear_imgui = 'fixture'
    implot = 'fixture'
    zlib = 'fixture'
} |
    ConvertTo-Json -Depth 10 |
    Set-Content `
        -LiteralPath (Join-Path $packageRoot 'specforge_build_metadata.json') `
        -Encoding UTF8

$verifiedMetadata = Get-Content `
    -Raw `
    -LiteralPath (Join-Path $packageRoot 'specforge_build_metadata.json') |
    ConvertFrom-Json
$verifiedSchemaVersion =
    $verifiedMetadata.PSObject.Properties['schema_version'].Value
if (($verifiedSchemaVersion -isnot [int] -and
     $verifiedSchemaVersion -isnot [long]) -or
    $verifiedSchemaVersion -ne 3 -or
    $verifiedMetadata.source_mode -isnot [string] -or
    $verifiedMetadata.source_mode -cne 'head' -or
    $verifiedMetadata.source_revision -isnot [string] -or
    $verifiedMetadata.source_revision -cne $SourceRevision -or
    $verifiedMetadata.release_profile -isnot [string] -or
    $verifiedMetadata.release_profile -cne 'Portable') {
    throw 'Snapshot-owned builder rejected its generated legacy package metadata.'
}

$zipPath = Join-Path $DistRoot "$PackageName.zip"
Set-Content -LiteralPath $zipPath -Value 'fixture zip' -Encoding ASCII
Set-Content -LiteralPath "$zipPath.sha256" -Value 'fixture hash' -Encoding ASCII

[ordered]@{
    snapshot_root = $snapshotRoot
    preset = $Preset
    preset_was_bound = $presetWasBound
    configuration = $Configuration
    configuration_was_bound = $configurationWasBound
    package_name = $PackageName
    source_mode = $SourceMode
    source_revision = $SourceRevision
    tracked_value = $trackedValue
    child_schema_version = $verifiedMetadata.schema_version
} |
    ConvertTo-Json |
    Set-Content `
        -LiteralPath (Join-Path $DistRoot 'head-wrapper-invocation.json') `
        -Encoding UTF8
