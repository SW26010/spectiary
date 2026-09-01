[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildExecutable,

    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [Parameter(Mandatory = $true)]
    [string]$ZipPath
)

$ErrorActionPreference = 'Stop'

Import-Module `
    (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') `
    -Force `
    -ErrorAction Stop

. (Join-Path $PSScriptRoot 'lib\portable_pe_helpers.ps1')

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Object,
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or
        $Object.PSObject.Properties.Name -cnotcontains $Name) {
        throw "$Description is missing '$Name'."
    }
    return $property.Value
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)] [string]$Path)

    return (
        Get-FileHash -Algorithm SHA256 -LiteralPath $Path
    ).Hash.ToLowerInvariant()
}

function Get-StreamSha256 {
    param([Parameter(Mandatory = $true)] [IO.Stream]$Stream)

    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return (
            [BitConverter]::ToString($algorithm.ComputeHash($Stream)) -replace '-', ''
        ).ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Assert-ExactEntries {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Expected,
        [Parameter(Mandatory = $true)] [string[]]$Actual,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    if (($Actual | Sort-Object) -join "`n" -cne
        (($Expected | Sort-Object) -join "`n")) {
        throw "$Description entries are wrong: $($Actual -join ', ')."
    }
}

function Assert-ZipEntryMatchesFile {
    param(
        [Parameter(Mandatory = $true)] [IO.Compression.ZipArchive]$Archive,
        [Parameter(Mandatory = $true)] [string]$EntryName,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $entry = $Archive.GetEntry($EntryName)
    if ($null -eq $entry) {
        throw "$Description is missing ZIP entry '$EntryName'."
    }
    $stream = $entry.Open()
    try {
        $entryHash = Get-StreamSha256 -Stream $stream
    }
    finally {
        $stream.Dispose()
    }
    $fileHash = Get-Sha256 -Path $FilePath
    if ($entryHash -cne $fileHash) {
        throw "$Description ZIP entry '$EntryName' hash '$entryHash' does not match '$FilePath' hash '$fileHash'."
    }
}

if (-not (Test-Path -LiteralPath $BuildExecutable -PathType Leaf)) {
    throw "Build executable is missing: $BuildExecutable"
}
if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
    throw "Portable package root is missing: $PackageRoot"
}
if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) {
    throw "Portable ZIP is missing: $ZipPath"
}

$expectedPackageEntries = @(
    'Data',
    'SpecForge.exe',
    'specforge_metadata.json'
)
$actualPackageItems = @(Get-ChildItem -LiteralPath $PackageRoot -Force)
$actualPackageEntries = @($actualPackageItems | ForEach-Object { $_.Name })
Assert-ExactEntries `
    -Expected $expectedPackageEntries `
    -Actual $actualPackageEntries `
    -Description 'Portable package root'

$packageDataEntry = @(
    $actualPackageItems | Where-Object { $_.Name -ceq 'Data' }
)
$packageExecutableEntry = @(
    $actualPackageItems | Where-Object { $_.Name -ceq 'SpecForge.exe' }
)
$packageMetadataEntry = @(
    $actualPackageItems |
        Where-Object { $_.Name -ceq 'specforge_metadata.json' }
)
if ($packageDataEntry.Count -ne 1 -or
    -not $packageDataEntry[0].PSIsContainer) {
    throw 'Portable package root Data must be a directory.'
}
if ($packageExecutableEntry.Count -ne 1 -or
    $packageExecutableEntry[0].PSIsContainer) {
    throw 'Portable package root SpecForge.exe must be a file.'
}
if ($packageMetadataEntry.Count -ne 1 -or
    $packageMetadataEntry[0].PSIsContainer) {
    throw 'Portable package root specforge_metadata.json must be a file.'
}

$packageExecutable = Join-Path $PackageRoot 'SpecForge.exe'
$packageMetadataPath = Join-Path $PackageRoot 'specforge_metadata.json'
$metadata = Get-Content -Raw -LiteralPath $packageMetadataPath | ConvertFrom-Json
$schemaVersion = Get-RequiredProperty `
    -Object $metadata `
    -Name 'schema_version' `
    -Description 'Portable metadata'
if (($schemaVersion -isnot [int] -and
     $schemaVersion -isnot [long]) -or
    $schemaVersion -ne 5) {
    throw 'Portable metadata schema_version must be the integer 5.'
}

$build = Get-RequiredProperty `
    -Object $metadata `
    -Name 'build' `
    -Description 'Portable metadata'
if ($build -isnot [pscustomobject]) {
    throw 'Portable metadata build must be an object.'
}
$windowsSdkVersion = Get-RequiredProperty `
    -Object $build `
    -Name 'windows_sdk_version' `
    -Description 'Portable build metadata'
if ($windowsSdkVersion -isnot [string] -or
    [string]::IsNullOrWhiteSpace($windowsSdkVersion) -or
    $windowsSdkVersion -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?$') {
    throw 'Portable build metadata windows_sdk_version must be a non-empty dotted numeric version.'
}
$yamlCppVersion = Get-RequiredProperty `
    -Object $build `
    -Name 'yaml_cpp' `
    -Description 'Portable build metadata'
if ($yamlCppVersion -isnot [string] -or
    [string]::IsNullOrWhiteSpace($yamlCppVersion) -or
    $yamlCppVersion -cne $yamlCppVersion.Trim()) {
    throw 'Portable build metadata yaml_cpp must be a non-empty unpadded string.'
}
$cfitsioVersion = Get-RequiredProperty `
    -Object $build `
    -Name 'cfitsio' `
    -Description 'Portable build metadata'
if ($cfitsioVersion -isnot [string] -or
    [string]::IsNullOrWhiteSpace($cfitsioVersion) -or
    $cfitsioVersion -cne $cfitsioVersion.Trim() -or
    $cfitsioVersion -cnotmatch '^[0-9]+\.[0-9]+(?:\.[0-9]+){0,2}$') {
    throw 'Portable build metadata cfitsio must be a non-empty, unpadded dotted numeric version.'
}
$configuration = Get-RequiredProperty `
    -Object $build `
    -Name 'configuration' `
    -Description 'Portable build metadata'
if ($configuration -isnot [string] -or
    [string]::IsNullOrWhiteSpace($configuration) -or
    $configuration -cne $configuration.Trim()) {
    throw 'Portable build metadata configuration must be a non-empty unpadded string.'
}

$deployment = Get-RequiredProperty `
    -Object $metadata `
    -Name 'deployment' `
    -Description 'Portable metadata'
if ($deployment -isnot [pscustomobject]) {
    throw 'Portable metadata deployment must be an object.'
}
if ((Get-RequiredProperty $deployment 'distribution' 'Portable deployment') -cne 'portable' -or
    (Get-RequiredProperty $deployment 'storage_profile' 'Portable deployment') -cne 'portable') {
    throw 'Portable metadata must declare portable distribution and storage_profile.'
}

$artifact = Get-RequiredProperty `
    -Object $metadata `
    -Name 'artifact' `
    -Description 'Portable metadata'
if ($artifact -isnot [pscustomobject]) {
    throw 'Portable metadata artifact must be an object.'
}
if ((Get-RequiredProperty $artifact 'file' 'Portable artifact') -cne 'SpecForge.exe') {
    throw "Portable artifact file must be 'SpecForge.exe'."
}
$artifactSha256 = [string](Get-RequiredProperty $artifact 'sha256' 'Portable artifact')
if ($artifactSha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'Portable artifact sha256 must be a lowercase SHA-256 digest.'
}

$buildExecutableHash = Get-Sha256 -Path $BuildExecutable
$packageExecutableHash = Get-Sha256 -Path $packageExecutable
if ($artifactSha256 -cne $buildExecutableHash) {
    throw "Portable metadata artifact sha256 '$artifactSha256' does not match build executable hash '$buildExecutableHash'."
}
if ($artifactSha256 -cne $packageExecutableHash) {
    throw "Portable metadata artifact sha256 '$artifactSha256' does not match packaged executable hash '$packageExecutableHash'."
}
if ($configuration -ceq 'Release') {
    Assert-StaticCfitsioPeImports `
        -ExecutablePath $BuildExecutable `
        -Description 'Release build SpecForge.exe'
    Assert-StaticCfitsioPeImports `
        -ExecutablePath $packageExecutable `
        -Description 'Portable Release SpecForge.exe'
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    $expectedZipEntries = @(
        'Data/',
        'SpecForge.exe',
        'specforge_metadata.json'
    )
    $actualZipEntries = @($archive.Entries.FullName)
    $cfitsioDllEntries = @(
        $actualZipEntries | Where-Object {
            $_ -match '(?i)(?:^|/)[^/]*cfitsio[^/]*\.dll$'
        }
    )
    if ($cfitsioDllEntries.Count -ne 0) {
        throw "Portable ZIP must not contain a CFITSIO DLL: $($cfitsioDllEntries -join ', ')."
    }
    Assert-ExactEntries `
        -Expected $expectedZipEntries `
        -Actual $actualZipEntries `
        -Description 'Portable ZIP'
    Assert-ZipEntryMatchesFile `
        -Archive $archive `
        -EntryName 'SpecForge.exe' `
        -FilePath $packageExecutable `
        -Description 'Portable ZIP executable'
    Assert-ZipEntryMatchesFile `
        -Archive $archive `
        -EntryName 'specforge_metadata.json' `
        -FilePath $packageMetadataPath `
        -Description 'Portable ZIP metadata'
}
finally {
    $archive.Dispose()
}

Write-Output 'Portable artifact verification passed'
