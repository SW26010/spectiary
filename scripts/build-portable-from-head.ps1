[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$PackageName = 'Spectiary-portable'
)

$ErrorActionPreference = 'Stop'

function Get-NormalizedFullPath {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    $separators = [char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
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
    $childParent = Get-NormalizedFullPath -Path ([IO.Path]::GetDirectoryName($childFullPath))
    if (-not [string]::Equals(
        $childParent,
        $parentFullPath,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description must resolve to a direct child of ${parentFullPath}: $childFullPath"
    }
    return $childFullPath
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
$revisionOutput = @(& git -C $repoRoot rev-parse --verify 'HEAD^{commit}')
if ($LASTEXITCODE -ne 0 -or $revisionOutput.Count -ne 1) {
    throw "Could not resolve the repository HEAD commit."
}
$sourceRevision = $revisionOutput[0].Trim()
if ($sourceRevision -cnotmatch '^[0-9a-f]{40}$') {
    throw "HEAD must resolve to a full 40-character lowercase hexadecimal Git object ID."
}

$temporaryParent = Get-NormalizedFullPath -Path ([IO.Path]::GetTempPath())
$temporaryName = "sfh-$PID-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$temporaryRoot = Assert-DirectChildPath `
    -Child (Join-Path $temporaryParent $temporaryName) `
    -Parent $temporaryParent `
    -Description 'Temporary HEAD snapshot path'
$snapshotRoot = Join-Path $temporaryRoot 's'
$archivePath = Join-Path $temporaryRoot 's.zip'
$headDistRoot = Join-Path $repoRoot 'dist\head'

try {
    New-Item -ItemType Directory -Path $snapshotRoot -Force | Out-Null
    & git -C $repoRoot archive --format=zip "--output=$archivePath" $sourceRevision
    if ($LASTEXITCODE -ne 0) {
        throw "git archive failed for HEAD revision $sourceRevision."
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $snapshotRoot)

    $snapshotPackageScript = Join-Path $snapshotRoot 'scripts\build-portable.ps1'
    if (-not (Test-Path -LiteralPath $snapshotPackageScript -PathType Leaf)) {
        throw "HEAD snapshot is missing scripts/build-portable.ps1."
    }

    & powershell `
        -NoProfile `
        -ExecutionPolicy Bypass `
        -File $snapshotPackageScript `
        -PackageName $PackageName `
        -DistRoot $headDistRoot `
        -SourceMode head `
        -SourceRevision $sourceRevision
    if ($LASTEXITCODE -ne 0) {
        throw "Portable HEAD snapshot build failed with exit code $LASTEXITCODE."
    }

    $headPackageRoot = Join-Path $headDistRoot $PackageName
    $headZipPath = Join-Path $headDistRoot "$PackageName.zip"
    $headHashPath = "$headZipPath.sha256"
    if (-not (Test-Path -LiteralPath $headPackageRoot -PathType Container)) {
        throw "Portable HEAD package root is missing: $headPackageRoot"
    }
    foreach ($artifactPath in @($headZipPath, $headHashPath)) {
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            throw "Portable HEAD artifact is missing: $artifactPath"
        }
    }

    Write-Host "HEAD revision: $sourceRevision"
    Write-Host "HEAD package root: $headPackageRoot"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        $resolvedTemporaryRoot = (Resolve-Path -LiteralPath $temporaryRoot).Path
        [void](Assert-DirectChildPath `
            -Child $resolvedTemporaryRoot `
            -Parent $temporaryParent `
            -Description 'Temporary HEAD snapshot cleanup path')
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force
    }
}
