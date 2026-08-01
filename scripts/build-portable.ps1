[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset = 'vs2022-x64-release-static',
    [string]$Configuration = 'Release',
    [string]$PackageName = 'SpecForge-portable',
    [string]$BuildRoot,
    [string]$DistRoot,
    [Parameter(DontShow = $true)]
    [string]$SourceMode = 'working_tree',
    [Parameter(DontShow = $true)]
    [AllowEmptyString()]
    [string]$SourceRevision = '',
    [Parameter(DontShow = $true)]
    [switch]$PackageUnverifiedTestFixture
)

$ErrorActionPreference = 'Stop'
Import-Module `
    (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') `
    -Force `
    -ErrorAction Stop

function Get-NormalizedFullPath {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $separators = [char[]]@(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    return $fullPath.TrimEnd($separators)
}

function Assert-DirectChildPath {
    param(
        [Parameter(Mandatory = $true)] [string]$Child,
        [Parameter(Mandatory = $true)] [string]$Parent,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $childFullPath = Get-NormalizedFullPath -Path $Child
    $parentFullPath = Get-NormalizedFullPath -Path $Parent
    $childParent = Get-NormalizedFullPath -Path ([System.IO.Path]::GetDirectoryName($childFullPath))

    if (-not [string]::Equals($childParent, $parentFullPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description must resolve to a direct child of ${parentFullPath}: $childFullPath"
    }

    return $childFullPath
}

function Test-PathIsWithinRoot {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Root
    )

    $fullPath = Get-NormalizedFullPath -Path $Path
    $fullRoot = Get-NormalizedFullPath -Path $Root
    $rootPrefix = $fullRoot + [System.IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith(
        $rootPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SingleNoticeHeading {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string[]]$Lines,
        [Parameter(Mandatory = $true)] [string]$ComponentName,
        [Parameter(Mandatory = $true)] [string]$ExpectedHeading
    )

    $escapedComponentName = [regex]::Escape($ComponentName)
    $matchingLines = @($Lines | Where-Object { $_ -match "^$escapedComponentName [0-9]" })
    if ($matchingLines.Count -ne 1 -or $matchingLines[0] -ne $ExpectedHeading) {
        $actual = if ($matchingLines.Count -eq 0) { '<missing>' } else { $matchingLines -join ' | ' }
        throw "THIRD_PARTY_NOTICES.txt is stale for ${ComponentName}. Expected '$ExpectedHeading'; found '$actual'."
    }
}

function Get-RequiredMetadataString {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Metadata,
        [Parameter(Mandatory = $true)] [string]$PropertyName
    )

    $property = $Metadata.PSObject.Properties[$PropertyName]
    if ($null -eq $property -or $property.Value -isnot [string] -or
        [string]::IsNullOrWhiteSpace($property.Value)) {
        throw "Build metadata is missing non-empty string '$PropertyName'."
    }
    if ($property.Value -cne $property.Value.Trim()) {
        throw "Build metadata '$PropertyName' must not have leading or trailing whitespace."
    }
    return $property.Value
}

function Get-RequiredMetadataObject {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Metadata,
        [Parameter(Mandatory = $true)] [string]$PropertyName
    )

    $property = $Metadata.PSObject.Properties[$PropertyName]
    if ($null -eq $property -or $null -eq $property.Value -or
        $property.Value -isnot [pscustomobject]) {
        throw "SpecForge metadata is missing object '$PropertyName'."
    }
    return $property.Value
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path

if ($SourceMode -cnotin @('working_tree', 'head')) {
    throw "SourceMode must be exactly 'working_tree' or 'head'."
}
if ($SourceMode -ceq 'working_tree') {
    if (-not [string]::IsNullOrEmpty($SourceRevision)) {
        throw "SourceRevision must be empty when SourceMode is 'working_tree'."
    }
}
else {
    if ($SourceRevision -cnotmatch '^[0-9a-f]{40}$') {
        throw "SourceRevision must be a full 40-character lowercase hexadecimal Git object ID when SourceMode is 'head'."
    }
    if (Test-Path -LiteralPath (Join-Path $repoRoot '.git')) {
        throw 'Head source mode is reserved for an isolated snapshot without Git repository metadata.'
    }
}

if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $repoRoot "build\$Preset"
}
else {
    $BuildRoot = Get-NormalizedFullPath -Path $BuildRoot
}
if ([string]::IsNullOrWhiteSpace($DistRoot)) {
    $DistRoot = Join-Path $repoRoot 'dist'
}
else {
    $DistRoot = Get-NormalizedFullPath -Path $DistRoot
}
if ($PackageUnverifiedTestFixture) {
    # Release-artifact tests intentionally mutate sidecars without rebuilding the
    # EXE. Keep that unverified seam outside every repository artifact directory.
    $temporaryRoot = Get-NormalizedFullPath -Path ([System.IO.Path]::GetTempPath())
    if (-not (Test-PathIsWithinRoot -Path $BuildRoot -Root $temporaryRoot) -or
        -not (Test-PathIsWithinRoot -Path $DistRoot -Root $temporaryRoot)) {
        throw 'PackageUnverifiedTestFixture is reserved for release-artifact tests and may only use BuildRoot and DistRoot beneath the system temporary directory.'
    }
}
$packageRoot = Join-Path $distRoot $PackageName
$zipPath = Join-Path $distRoot "$PackageName.zip"
$legalSourceRoot = Join-Path $repoRoot 'legal'
$thirdPartyNoticesPath = Join-Path `
    $legalSourceRoot `
    'THIRD_PARTY_NOTICES.txt'

if (-not $PackageUnverifiedTestFixture) {
    Write-Host 'build-portable.ps1 invokes CMake directly; run it from a normal developer shell or an approved unsandboxed agent run.'
    Push-Location -LiteralPath $repoRoot
    try {
        $configureArguments = @(
            '--preset',
            $Preset,
            "-DSPECFORGE_BUILD_SOURCE_MODE=$SourceMode",
            "-DSPECFORGE_BUILD_SOURCE_REVISION=$SourceRevision"
        )
        & cmake @configureArguments
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed for preset $Preset."
        }

        & cmake --build --preset $Preset --config $Configuration
        if ($LASTEXITCODE -ne 0) {
            throw "CMake build failed for preset $Preset."
        }
    }
    finally {
        Pop-Location
    }
}

$candidateExecutables = @(
    (Join-Path $buildRoot "$Configuration\SpecForge.exe"),
    (Join-Path $buildRoot 'SpecForge.exe')
)
$sourceExecutable = $candidateExecutables | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $sourceExecutable) {
    throw "SpecForge.exe was not found under $buildRoot."
}

$sourceExecutableDirectory = Split-Path -Parent $sourceExecutable
$sourceMetadataPath = Join-Path $sourceExecutableDirectory 'specforge_metadata.json'
if (-not (Test-Path -LiteralPath $sourceMetadataPath -PathType Leaf)) {
    throw "SpecForge metadata was not found beside SpecForge.exe: $sourceMetadataPath"
}
$sourceMetadata = Get-Content -Raw -LiteralPath $sourceMetadataPath | ConvertFrom-Json
$schemaVersionProperty =
    $sourceMetadata.PSObject.Properties['schema_version']
if ($null -eq $schemaVersionProperty -or
    $sourceMetadata.PSObject.Properties.Name -cnotcontains 'schema_version') {
    throw 'SpecForge metadata schema_version must be the integer 4.'
}
$schemaVersion = $schemaVersionProperty.Value
if (($schemaVersion -isnot [int] -and
     $schemaVersion -isnot [long]) -or
    $schemaVersion -ne 4) {
    throw 'SpecForge metadata schema_version must be the integer 4.'
}
if ($sourceMetadata.PSObject.Properties.Name -ccontains 'deployment') {
    throw 'Build-output SpecForge metadata must not contain deployment; the packaging flow owns distribution identity.'
}
$productMetadata = Get-RequiredMetadataObject `
    -Metadata $sourceMetadata `
    -PropertyName 'product'
$buildMetadata = Get-RequiredMetadataObject `
    -Metadata $sourceMetadata `
    -PropertyName 'build'
$productName = Get-RequiredMetadataString `
    -Metadata $productMetadata `
    -PropertyName 'name'
[void](Get-RequiredMetadataString `
    -Metadata $productMetadata `
    -PropertyName 'version')
if ($productName -cne 'SpecForge') {
    throw "SpecForge metadata has product name '$productName'; expected 'SpecForge'."
}
foreach ($propertyName in @(
    'source_mode',
    'configuration',
    'compiler_id',
    'compiler_version',
    'cmake_version',
    'generator',
    'target_architecture',
    'windows_sdk_version',
    'dear_imgui',
    'implot',
    'zlib'
)) {
    [void](Get-RequiredMetadataString `
        -Metadata $buildMetadata `
        -PropertyName $propertyName)
}
if ($buildMetadata.source_mode -cne $SourceMode) {
    throw "SpecForge metadata has source mode '$($buildMetadata.source_mode)'; expected '$SourceMode'."
}
$sourceRevisionProperty =
    $buildMetadata.PSObject.Properties['source_revision']
if ($null -eq $sourceRevisionProperty -or
    $buildMetadata.PSObject.Properties.Name -cnotcontains 'source_revision') {
    throw "Build metadata is missing 'source_revision'."
}
if ($SourceMode -ceq 'working_tree') {
    if ($null -ne $sourceRevisionProperty.Value) {
        throw "Working-tree build metadata must use null source_revision."
    }
}
else {
    if ($sourceRevisionProperty.Value -isnot [string]) {
        throw 'Head build metadata must use a single string source_revision.'
    }
    if ($sourceRevisionProperty.Value -cne $SourceRevision) {
        throw "Head build metadata has source revision '$($sourceRevisionProperty.Value)'; expected '$SourceRevision'."
    }
}
if ($buildMetadata.configuration -cne $Configuration) {
    throw "SpecForge.exe has configuration '$($buildMetadata.configuration)'; expected '$Configuration'."
}
if ($buildMetadata.compiler_id -cne 'MSVC') {
    throw "SpecForge.exe has compiler '$($buildMetadata.compiler_id)'; expected 'MSVC'."
}
if ($buildMetadata.compiler_version -cnotmatch '^[0-9]+(?:\.[0-9]+){1,3}$') {
    throw "Build metadata has invalid compiler_version '$($buildMetadata.compiler_version)'."
}
if ($buildMetadata.cmake_version -cnotmatch '^[0-9]+(?:\.[0-9]+){2,3}$') {
    throw "Build metadata has invalid cmake_version '$($buildMetadata.cmake_version)'."
}
if ($buildMetadata.generator -match '[\x00-\x1f]') {
    throw "Build metadata has invalid generator '$($buildMetadata.generator)'."
}
if ($buildMetadata.target_architecture -cne 'amd64') {
    throw "SpecForge.exe has target architecture '$($buildMetadata.target_architecture)'; expected 'amd64'."
}
if ($buildMetadata.windows_sdk_version -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?$') {
    throw "Build metadata has invalid windows_sdk_version '$($buildMetadata.windows_sdk_version)'."
}

if (-not (Test-Path -LiteralPath $thirdPartyNoticesPath -PathType Leaf)) {
    throw "Required third-party notices source was not found: $thirdPartyNoticesPath"
}

$thirdPartyNoticeLines = @(
    Get-Content -LiteralPath $thirdPartyNoticesPath
)
Assert-SingleNoticeHeading `
    -Lines $thirdPartyNoticeLines `
    -ComponentName 'Dear ImGui' `
    -ExpectedHeading "Dear ImGui $($buildMetadata.dear_imgui), including docking, Win32, and DirectX 11 backends"
Assert-SingleNoticeHeading `
    -Lines $thirdPartyNoticeLines `
    -ComponentName 'ImPlot' `
    -ExpectedHeading "ImPlot $($buildMetadata.implot)"
Assert-SingleNoticeHeading `
    -Lines $thirdPartyNoticeLines `
    -ComponentName 'zlib' `
    -ExpectedHeading "zlib $($buildMetadata.zlib)"

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null

$resolvedDistRoot = Get-NormalizedFullPath -Path (Resolve-Path -LiteralPath $distRoot).Path
$packageRoot = Assert-DirectChildPath -Child $packageRoot -Parent $resolvedDistRoot -Description 'Portable package path'
$zipPath = Assert-DirectChildPath -Child $zipPath -Parent $resolvedDistRoot -Description 'Portable zip path'
$hashPath = "$zipPath.sha256"

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}

New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'Data') -Force | Out-Null
Copy-Item -LiteralPath $sourceExecutable -Destination (Join-Path $packageRoot 'SpecForge.exe') -Force
$sourceExecutableHash = (
    Get-FileHash -Algorithm SHA256 -LiteralPath $sourceExecutable
).Hash.ToLowerInvariant()
$packagedExecutableHash = (
    Get-FileHash `
        -Algorithm SHA256 `
        -LiteralPath (Join-Path $packageRoot 'SpecForge.exe')
).Hash.ToLowerInvariant()
if ($sourceExecutableHash -cne $packagedExecutableHash) {
    throw "Packaged SpecForge.exe hash '$packagedExecutableHash' does not match source executable hash '$sourceExecutableHash'."
}
$packageMetadataPath = Join-Path $packageRoot 'specforge_metadata.json'
$portableMetadata = $sourceMetadata |
    ConvertTo-Json -Depth 10 |
    ConvertFrom-Json
$portableMetadata |
    Add-Member `
        -MemberType NoteProperty `
        -Name deployment `
        -Value ([ordered]@{
            distribution = 'portable'
            storage_profile = 'portable'
        })
$portableMetadataJson = (
    $portableMetadata |
        ConvertTo-Json -Depth 10
) + [Environment]::NewLine
[IO.File]::WriteAllText(
    $packageMetadataPath,
    $portableMetadataJson,
    (New-Object Text.UTF8Encoding($false)))

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$zipStream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
try {
    $archive = [System.IO.Compression.ZipArchive]::new($zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            (Join-Path $packageRoot 'SpecForge.exe'),
            'SpecForge.exe',
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $packageMetadataPath,
            'specforge_metadata.json',
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        [void]$archive.CreateEntry('Data/')
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    $zipStream.Dispose()
}

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath
Set-Content -LiteralPath $hashPath -Value ("{0}  {1}" -f $hash.Hash.ToLowerInvariant(), (Split-Path -Leaf $zipPath)) -Encoding ASCII

Write-Host "Portable package: $packageRoot"
Write-Host "Portable zip: $zipPath"
Write-Host "SHA256: $hashPath"
Write-Host "SpecForge.exe SHA256: $packagedExecutableHash"
