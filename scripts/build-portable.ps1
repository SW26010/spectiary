[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset = 'vs2022-x64-portable-release-static',
    [string]$Configuration = 'Release',
    [string]$PackageName = 'SpecForge-portable',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

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

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
$buildRoot = Join-Path $repoRoot "build\$Preset"
$distRoot = Join-Path $repoRoot 'dist'
$packageRoot = Join-Path $distRoot $PackageName
$zipPath = Join-Path $distRoot "$PackageName.zip"
$releaseDocumentSourceRoot = Join-Path $repoRoot 'legal'
$releaseDocumentDirectoryName = 'Legal'
$releaseDocumentNames = @(
    'EULA.txt',
    'THIRD_PARTY_NOTICES.txt',
    'DATA_SOURCES.txt'
)

if (-not $SkipBuild) {
    Write-Host 'build-portable.ps1 invokes CMake directly; run it from a normal developer shell or an approved unsandboxed agent run.'
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for preset $Preset."
    }

    & cmake --build --preset $Preset --config $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed for preset $Preset."
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
$buildMetadataPath = Join-Path $sourceExecutableDirectory 'specforge_build_metadata.json'
if (-not (Test-Path -LiteralPath $buildMetadataPath -PathType Leaf)) {
    throw "Build metadata was not found beside SpecForge.exe: $buildMetadataPath"
}
$buildMetadata = Get-Content -Raw -LiteralPath $buildMetadataPath | ConvertFrom-Json
foreach ($propertyName in @(
    'specforge_version',
    'release_profile',
    'configuration',
    'dear_imgui',
    'implot',
    'zlib'
)) {
    if ([string]::IsNullOrWhiteSpace([string]$buildMetadata.$propertyName)) {
        throw "Build metadata is missing '$propertyName'."
    }
}
if ($buildMetadata.schema_version -ne 1) {
    throw "Unsupported build metadata schema version '$($buildMetadata.schema_version)'."
}
if ($buildMetadata.release_profile -cne 'Portable') {
    throw "SpecForge.exe has release profile '$($buildMetadata.release_profile)'; expected 'Portable'."
}
if ($buildMetadata.configuration -cne $Configuration) {
    throw "SpecForge.exe has configuration '$($buildMetadata.configuration)'; expected '$Configuration'."
}

foreach ($documentName in $releaseDocumentNames) {
    $sourceDocument = Join-Path $releaseDocumentSourceRoot $documentName
    if (-not (Test-Path -LiteralPath $sourceDocument -PathType Leaf)) {
        throw "Required release document was not found: $sourceDocument"
    }
}

$thirdPartyNoticeLines = @(
    Get-Content -LiteralPath (Join-Path $releaseDocumentSourceRoot 'THIRD_PARTY_NOTICES.txt')
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
$releaseDocumentPackageRoot = Join-Path $packageRoot $releaseDocumentDirectoryName
New-Item -ItemType Directory -Path $releaseDocumentPackageRoot -Force | Out-Null
Copy-Item -LiteralPath $sourceExecutable -Destination (Join-Path $packageRoot 'SpecForge.exe') -Force
foreach ($documentName in $releaseDocumentNames) {
    Copy-Item `
        -LiteralPath (Join-Path $releaseDocumentSourceRoot $documentName) `
        -Destination (Join-Path $releaseDocumentPackageRoot $documentName) `
        -Force
}

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
        [void]$archive.CreateEntry('Data/')
        [void]$archive.CreateEntry("$releaseDocumentDirectoryName/")
        foreach ($documentName in $releaseDocumentNames) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive,
                (Join-Path $releaseDocumentPackageRoot $documentName),
                "$releaseDocumentDirectoryName/$documentName",
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
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
