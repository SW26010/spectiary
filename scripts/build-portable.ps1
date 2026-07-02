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

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
$buildRoot = Join-Path $repoRoot "build\$Preset"
$distRoot = Join-Path $repoRoot 'dist'
$packageRoot = Join-Path $distRoot $PackageName
$zipPath = Join-Path $distRoot "$PackageName.zip"

if (-not $SkipBuild) {
    Write-Host 'build-portable.ps1 invokes CMake directly; run it from a normal developer shell or an approved unsandboxed agent run.'
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for preset $Preset."
    }

    & cmake --build --preset $Preset
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
