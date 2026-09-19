[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$DearImGuiVersion,

    [Parameter(Mandatory = $true)]
    [string]$ImPlotVersion,

    [Parameter(Mandatory = $true)]
    [string]$CfitsioVersion,

    [Parameter(Mandatory = $true)]
    [string]$PthreadsVersion,

    [Parameter(Mandatory = $true)]
    [string]$YamlCppVersion,

    [Parameter(Mandatory = $true)]
    [string]$ZlibVersion,

    [Parameter(Mandatory = $true)]
    [string]$BuiltExecutable,

    [Parameter(Mandatory = $true)]
    [string]$GeneratedManifest,

    [Parameter(Mandatory = $true)]
    [string]$GeneratedBuildIdentity,

    [Parameter(Mandatory = $true)]
    [string]$SpectiaryVersion,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$SourceMode,

    [Parameter(Mandatory = $true)]
    [AllowEmptyString()]
    [string]$SourceRevision,

    [Parameter(Mandatory = $true)]
    [string]$CompilerId,

    [Parameter(Mandatory = $true)]
    [string]$CompilerVersion,

    [Parameter(Mandatory = $true)]
    [string]$CMakeVersion,

    [Parameter(Mandatory = $true)]
    [string]$Generator,

    [Parameter(Mandatory = $true)]
    [string]$TargetArchitecture,

    [Parameter(Mandatory = $true)]
    [AllowEmptyString()]
    [string]$WindowsSdkVersion
)

$ErrorActionPreference = 'Stop'

Import-Module `
    (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') `
    -Force `
    -ErrorAction Stop

. (Join-Path $RepoRoot 'scripts\lib\portable_pe_helpers.ps1')
. (Join-Path $RepoRoot 'scripts\lib\bounded_process.ps1')

$script:FastExternalCaseTimeoutSec = 60
$script:DeepArchiveCaseTimeoutSec = 120
$script:CMakeCaseTimeoutSec = 30

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SpectiaryReleaseArtifactResources
{
    [DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", EntryPoint = "FindResourceW", SetLastError = true)]
    public static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll")]
    public static extern IntPtr LockResource(IntPtr resourceData);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Expected,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    if (-not $Text.Contains($Expected)) {
        throw "$Description is missing '$Expected'."
    }
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Unexpected,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    if ($Text.Contains($Unexpected)) {
        throw "$Description unexpectedly contains '$Unexpected'."
    }
}

function Assert-SchemaVersionFive {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Metadata,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $property = $Metadata.PSObject.Properties['schema_version']
    if ($null -eq $property -or
        $Metadata.PSObject.Properties.Name -cnotcontains 'schema_version') {
        throw "$Description schema_version must be the integer 6."
    }
    $schemaVersion = $property.Value
    if (($schemaVersion -isnot [int] -and
         $schemaVersion -isnot [long]) -or
        $schemaVersion -ne 6) {
        throw "$Description schema_version must be the integer 6."
    }
}

function Assert-FilesMatch {
    param(
        [Parameter(Mandatory = $true)] [string]$ExpectedPath,
        [Parameter(Mandatory = $true)] [string]$ActualPath,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    if (-not (Test-Path -LiteralPath $ActualPath -PathType Leaf)) {
        throw "$Description is missing: $ActualPath"
    }

    $expectedBytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes($ExpectedPath))
    $actualBytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes($ActualPath))
    if ($actualBytes -cne $expectedBytes) {
        throw "$Description is stale: expected $ExpectedPath to match $ActualPath."
    }
}

function Get-ExecutableResourceBytes {
    param(
        [Parameter(Mandatory = $true)] [string]$ExecutablePath,
        [Parameter(Mandatory = $true)] [int]$ResourceId
    )

    $loadLibraryAsDataFile = 0x00000002
    $loadLibraryAsImageResource = 0x00000020
    $module = [SpectiaryReleaseArtifactResources]::LoadLibraryExW(
        $ExecutablePath,
        [IntPtr]::Zero,
        $loadLibraryAsDataFile -bor $loadLibraryAsImageResource)
    if ($module -eq [IntPtr]::Zero) {
        throw "Could not open executable resources: $ExecutablePath"
    }

    try {
        $resource = [SpectiaryReleaseArtifactResources]::FindResourceW(
            $module,
            [IntPtr]$ResourceId,
            [IntPtr]10)
        if ($resource -eq [IntPtr]::Zero) {
            throw "Executable resource $ResourceId is missing."
        }
        $size = [SpectiaryReleaseArtifactResources]::SizeofResource(
            $module,
            $resource)
        $loaded = [SpectiaryReleaseArtifactResources]::LoadResource(
            $module,
            $resource)
        $data = if ($loaded -ne [IntPtr]::Zero) {
            [SpectiaryReleaseArtifactResources]::LockResource($loaded)
        }
        else {
            [IntPtr]::Zero
        }
        if ($size -eq 0 -or $data -eq [IntPtr]::Zero) {
            throw "Executable resource $ResourceId is empty or unreadable."
        }

        $bytes = [byte[]]::new([int]$size)
        [Runtime.InteropServices.Marshal]::Copy(
            $data,
            $bytes,
            0,
            [int]$size)
        return ,$bytes
    }
    finally {
        [void][SpectiaryReleaseArtifactResources]::FreeLibrary($module)
    }
}

function Assert-BuildSourceContract {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Metadata,
        [Parameter(Mandatory = $true)] [string]$ExpectedMode,
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$ExpectedRevision,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $sourceModeProperty = $Metadata.PSObject.Properties['source_mode']
    if ($null -eq $sourceModeProperty -or
        $Metadata.PSObject.Properties.Name -cnotcontains 'source_mode' -or
        $sourceModeProperty.Value -isnot [string] -or
        $sourceModeProperty.Value -cne $ExpectedMode) {
        throw "$Description source_mode expected '$ExpectedMode'; found '$($Metadata.source_mode)'."
    }
    $sourceRevisionProperty = $Metadata.PSObject.Properties['source_revision']
    if ($null -eq $sourceRevisionProperty -or
        $Metadata.PSObject.Properties.Name -cnotcontains 'source_revision') {
        throw "$Description is missing 'source_revision'."
    }
    if ($ExpectedMode -ceq 'working_tree') {
        if ($null -ne $sourceRevisionProperty.Value) {
            throw "$Description working-tree source_revision must be null."
        }
    }
    else {
        if ($sourceRevisionProperty.Value -isnot [string]) {
            throw "$Description head source_revision must be a single string."
        }
        if ($sourceRevisionProperty.Value -cne $ExpectedRevision) {
            throw "$Description source_revision expected '$ExpectedRevision'; found '$($sourceRevisionProperty.Value)'."
        }
    }
}

function Assert-BuildToolchainContract {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Metadata,
        [Parameter(Mandatory = $true)] [System.Collections.IDictionary]$Expected,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    foreach ($expectedProperty in $Expected.GetEnumerator()) {
        $property = $Metadata.PSObject.Properties[$expectedProperty.Key]
        if ($null -eq $property) {
            throw "$Description is missing '$($expectedProperty.Key)'."
        }
        if ($null -eq $expectedProperty.Value) {
            if ($null -ne $property.Value) {
                throw "$Description '$($expectedProperty.Key)' must be null; found '$($property.Value)'."
            }
            continue
        }
        if ($property.Value -isnot [string] -or
            [string]::IsNullOrWhiteSpace($property.Value)) {
            throw "$Description is missing non-empty string '$($expectedProperty.Key)'."
        }
        if ($property.Value -cne [string]$expectedProperty.Value) {
            throw "$Description '$($expectedProperty.Key)' expected '$($expectedProperty.Value)'; found '$($property.Value)'."
        }
    }
}

function Assert-ArtifactHashContract {
    param(
        [Parameter(Mandatory = $true)] [psobject]$Metadata,
        [Parameter(Mandatory = $true)] [string]$ExecutablePath,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $artifactProperty = $Metadata.PSObject.Properties['artifact']
    if ($null -eq $artifactProperty -or
        $Metadata.PSObject.Properties.Name -cnotcontains 'artifact' -or
        $artifactProperty.Value -isnot [pscustomobject]) {
        throw "$Description is missing artifact metadata."
    }
    $artifact = $artifactProperty.Value
    $fileProperty = $artifact.PSObject.Properties['file']
    $shaProperty = $artifact.PSObject.Properties['sha256']
    if ($null -eq $fileProperty -or
        $artifact.PSObject.Properties.Name -cnotcontains 'file' -or
        $fileProperty.Value -isnot [string] -or
        $fileProperty.Value -cne 'Spectiary.exe') {
        throw "$Description artifact.file must be 'Spectiary.exe'."
    }
    if ($null -eq $shaProperty -or
        $artifact.PSObject.Properties.Name -cnotcontains 'sha256' -or
        $shaProperty.Value -isnot [string] -or
        $shaProperty.Value -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Description artifact.sha256 must be a lowercase SHA-256 digest."
    }
    $executableHash = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $ExecutablePath
    ).Hash.ToLowerInvariant()
    if ($shaProperty.Value -cne $executableHash) {
        throw "$Description artifact.sha256 '$($shaProperty.Value)' does not match executable hash '$executableHash'."
    }
    return $executableHash
}

function Assert-PortablePackage {
    param(
        [Parameter(Mandatory = $true)] [string]$ExpectedExecutablePath,
        [Parameter(Mandatory = $true)] [string]$PackageRoot,
        [Parameter(Mandatory = $true)] [string]$ZipPath,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $expectedPackageEntries = @(
        'config', 'state', 'logs', 'unsaved',
        'Spectiary.exe',
        'spectiary_metadata.json'
    )
    $actualPackageItems = @(Get-ChildItem -LiteralPath $PackageRoot -Force)
    $actualPackageEntries = @(
        $actualPackageItems | ForEach-Object { $_.Name } | Sort-Object
    )
    if (($actualPackageEntries -join "`n") -cne (($expectedPackageEntries | Sort-Object) -join "`n")) {
        throw "$Description package root entries are wrong: $($actualPackageEntries -join ', ')."
    }
    $executableEntry = @(
        $actualPackageItems | Where-Object { $_.Name -ceq 'Spectiary.exe' }
    )
    $metadataEntry = @(
        $actualPackageItems |
            Where-Object { $_.Name -ceq 'spectiary_metadata.json' }
    )
    foreach ($role in @('config', 'state', 'logs', 'unsaved')) {
        $entry = @($actualPackageItems | Where-Object { $_.Name -ceq $role })
        if ($entry.Count -ne 1 -or -not $entry[0].PSIsContainer) {
            throw "$Description package root $role must be a directory."
        }
    }
    if ($executableEntry.Count -ne 1 -or $executableEntry[0].PSIsContainer) {
        throw "$Description package root Spectiary.exe must be a file."
    }
    if ($metadataEntry.Count -ne 1 -or $metadataEntry[0].PSIsContainer) {
        throw "$Description package root spectiary_metadata.json must be a file."
    }
    $packagedExecutablePath = Join-Path $PackageRoot 'Spectiary.exe'
    Assert-FilesMatch `
        -ExpectedPath $ExpectedExecutablePath `
        -ActualPath $packagedExecutablePath `
        -Description "$Description packaged executable"
    $packageMetadataPath = Join-Path $PackageRoot 'spectiary_metadata.json'
    $packageMetadata = Get-Content -Raw -LiteralPath $packageMetadataPath |
        ConvertFrom-Json
    Assert-SchemaVersionFive `
        -Metadata $packageMetadata `
        -Description "$Description package metadata"
    $packagedExecutableHash = Assert-ArtifactHashContract `
        -Metadata $packageMetadata `
        -ExecutablePath $packagedExecutablePath `
        -Description "$Description package artifact"
    $expectedExecutableHash = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $ExpectedExecutablePath
    ).Hash.ToLowerInvariant()
    if ($packagedExecutableHash -cne $expectedExecutableHash) {
        throw "$Description package artifact hash '$packagedExecutableHash' does not match build executable hash '$expectedExecutableHash'."
    }
    $deploymentProperty =
        $packageMetadata.PSObject.Properties['deployment']
    if ($null -eq $deploymentProperty -or
        $packageMetadata.PSObject.Properties.Name -cnotcontains 'deployment' -or
        $deploymentProperty.Value -isnot [pscustomobject]) {
        throw "$Description package metadata does not declare Portable deployment."
    }
    $deployment = $deploymentProperty.Value
    $distributionProperty =
        $deployment.PSObject.Properties['distribution']
    $storageProfileProperty =
        $deployment.PSObject.Properties['storage_profile']
    if ($null -eq $distributionProperty -or
        $deployment.PSObject.Properties.Name -cnotcontains 'distribution' -or
        $distributionProperty.Value -isnot [string] -or
        $distributionProperty.Value -cne 'portable' -or
        $null -eq $storageProfileProperty -or
        $deployment.PSObject.Properties.Name -cnotcontains 'storage_profile' -or
        $storageProfileProperty.Value -isnot [string] -or
        $storageProfileProperty.Value -cne 'portable') {
        throw "$Description package metadata does not declare Portable deployment."
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $expectedZipEntries = @(
            'config/', 'state/', 'logs/', 'unsaved/',
            'Spectiary.exe',
            'spectiary_metadata.json'
        )
        $actualZipEntries = @($archive.Entries.FullName | Sort-Object)
        $cfitsioDllEntries = @(
            $actualZipEntries | Where-Object {
                $_ -match '(?i)(?:^|/)[^/]*cfitsio[^/]*\.dll$'
            }
        )
        if ($cfitsioDllEntries.Count -ne 0) {
            throw "$Description ZIP must not contain a CFITSIO DLL: $($cfitsioDllEntries -join ', ')."
        }
        if (($actualZipEntries -join "`n") -cne (($expectedZipEntries | Sort-Object) -join "`n")) {
            throw "$Description ZIP entries are wrong: $($actualZipEntries -join ', ')."
        }

        $metadataEntry = $archive.GetEntry('spectiary_metadata.json')
        if ($null -eq $metadataEntry) {
            throw "$Description ZIP is missing spectiary_metadata.json."
        }
        $metadataStream = $metadataEntry.Open()
        try {
            $memoryStream = [IO.MemoryStream]::new()
            try {
                $metadataStream.CopyTo($memoryStream)
                $zipMetadataBytes = [Convert]::ToBase64String($memoryStream.ToArray())
            }
            finally {
                $memoryStream.Dispose()
            }
        }
        finally {
            $metadataStream.Dispose()
        }
        $expectedMetadataBytes = [Convert]::ToBase64String(
            [IO.File]::ReadAllBytes($packageMetadataPath))
        if ($zipMetadataBytes -cne $expectedMetadataBytes) {
            throw "$Description ZIP metadata does not byte-match package metadata."
        }

        $executableEntry = $archive.GetEntry('Spectiary.exe')
        if ($null -eq $executableEntry) {
            throw "$Description ZIP is missing Spectiary.exe."
        }
        $executableStream = $executableEntry.Open()
        try {
            $memoryStream = [IO.MemoryStream]::new()
            try {
                $executableStream.CopyTo($memoryStream)
                $zipExecutableBytes = [Convert]::ToBase64String(
                    $memoryStream.ToArray())
            }
            finally {
                $memoryStream.Dispose()
            }
        }
        finally {
            $executableStream.Dispose()
        }
        $expectedExecutableBytes = [Convert]::ToBase64String(
            [IO.File]::ReadAllBytes($ExpectedExecutablePath))
        if ($zipExecutableBytes -cne $expectedExecutableBytes) {
            throw "$Description ZIP executable does not byte-match the build output."
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-ScriptFails {
    param(
        [Parameter(Mandatory = $true)] [string]$CaseId,
        [Parameter(Mandatory = $true)] [string]$ScriptPath,
        [Parameter(Mandatory = $true)] [object[]]$Arguments,
        [Parameter(Mandatory = $true)] [string]$Description,
        [string]$ExpectedMessage,
        [int]$TimeoutSec = $script:FastExternalCaseTimeoutSec
    )

    $null = Invoke-SpectiaryBoundedValidationCase `
        -CaseId $CaseId `
        -Description $Description `
        -FilePath 'powershell.exe' `
        -Arguments (@(
            '-NoProfile',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            $ScriptPath
        ) + $Arguments) `
        -TimeoutSec $TimeoutSec `
        -ExpectedOutcome Failure `
        -ExpectedMessage $ExpectedMessage
}

function Assert-ScriptSucceeds {
    param(
        [Parameter(Mandatory = $true)] [string]$CaseId,
        [Parameter(Mandatory = $true)] [string]$ScriptPath,
        [Parameter(Mandatory = $true)] [object[]]$Arguments,
        [Parameter(Mandatory = $true)] [string]$Description,
        [int]$TimeoutSec = $script:DeepArchiveCaseTimeoutSec
    )

    $null = Invoke-SpectiaryBoundedValidationCase `
        -CaseId $CaseId `
        -Description $Description `
        -FilePath 'powershell.exe' `
        -Arguments (@(
            '-NoProfile',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            $ScriptPath
        ) + $Arguments) `
        -TimeoutSec $TimeoutSec `
        -ExpectedOutcome Success `
        -EchoOutput
}

function Copy-ZipWithMutations {
    param(
        [Parameter(Mandatory = $true)] [string]$SourcePath,
        [Parameter(Mandatory = $true)] [string]$DestinationPath,
        [AllowEmptyString()] [string]$TamperedEntryName = '',
        [AllowEmptyString()] [string]$AdditionalEntryName = ''
    )

    if ([string]::IsNullOrEmpty($TamperedEntryName) -and
        [string]::IsNullOrEmpty($AdditionalEntryName)) {
        throw 'ZIP mutation fixture must tamper with or add an entry.'
    }

    Add-Type -AssemblyName System.IO.Compression
    $sourceArchive = [IO.Compression.ZipFile]::OpenRead($SourcePath)
    $destinationStream = $null
    $destinationArchive = $null
    try {
        $destinationStream = [IO.File]::Open(
            $DestinationPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        $destinationArchive = [IO.Compression.ZipArchive]::new(
            $destinationStream,
            [IO.Compression.ZipArchiveMode]::Create,
            $false)
        foreach ($sourceEntry in $sourceArchive.Entries) {
            $destinationEntry = $destinationArchive.CreateEntry(
                $sourceEntry.FullName)
            $inputStream = $sourceEntry.Open()
            try {
                $memoryStream = [IO.MemoryStream]::new()
                try {
                    $inputStream.CopyTo($memoryStream)
                    $entryBytes = $memoryStream.ToArray()
                }
                finally {
                    $memoryStream.Dispose()
                }
            }
            finally {
                $inputStream.Dispose()
            }

            if ($sourceEntry.FullName -ceq $TamperedEntryName) {
                if ($entryBytes.Length -eq 0) {
                    throw "Cannot tamper with empty ZIP entry '$TamperedEntryName'."
                }
                $entryBytes[0] = [byte]($entryBytes[0] -bxor 1)
            }

            $outputStream = $destinationEntry.Open()
            try {
                $outputStream.Write(
                    $entryBytes,
                    0,
                    $entryBytes.Length)
            }
            finally {
                $outputStream.Dispose()
            }
        }
        if (-not [string]::IsNullOrEmpty($AdditionalEntryName)) {
            $additionalEntry = $destinationArchive.CreateEntry(
                $AdditionalEntryName)
            $additionalStream = $additionalEntry.Open()
            try {
                $additionalBytes = [Text.Encoding]::ASCII.GetBytes(
                    'forbidden runtime dependency')
                $additionalStream.Write(
                    $additionalBytes,
                    0,
                    $additionalBytes.Length)
            }
            finally {
                $additionalStream.Dispose()
            }
        }
    }
    finally {
        if ($null -ne $destinationArchive) {
            $destinationArchive.Dispose()
        }
        if ($null -ne $destinationStream) {
            $destinationStream.Dispose()
        }
        $sourceArchive.Dispose()
    }
}

function Assert-BuildSourceCMakeContract {
    param(
        [Parameter(Mandatory = $true)] [string]$CaseId,
        [Parameter(Mandatory = $true)] [string]$ContractPath,
        [Parameter(Mandatory = $true)] [string]$Mode,
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Revision,
        [Parameter(Mandatory = $true)] [bool]$ShouldSucceed,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $expectedOutcome = if ($ShouldSucceed) { 'Success' } else { 'Failure' }
    $null = Invoke-SpectiaryBoundedValidationCase `
        -CaseId $CaseId `
        -Description $Description `
        -FilePath 'cmake.exe' `
        -Arguments @(
            "-DSPECTIARY_BUILD_SOURCE_MODE=$Mode",
            "-DSPECTIARY_BUILD_SOURCE_REVISION=$Revision",
            '-P',
            $ContractPath
        ) `
        -TimeoutSec $script:CMakeCaseTimeoutSec `
        -ExpectedOutcome $expectedOutcome
}

function Assert-BuildIdentityHeader {
    param(
        [Parameter(Mandatory = $true)] [string]$CaseId,
        [Parameter(Mandatory = $true)] [string]$FixturePath,
        [Parameter(Mandatory = $true)] [string]$SourceRoot,
        [Parameter(Mandatory = $true)] [string]$OutputPath,
        [Parameter(Mandatory = $true)] [string]$Mode,
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Revision,
        [Parameter(Mandatory = $true)] [string]$Version,
        [Parameter(Mandatory = $true)] [string]$Configuration,
        [Parameter(Mandatory = $true)] [string]$Architecture,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $null = Invoke-SpectiaryBoundedValidationCase `
        -CaseId $CaseId `
        -Description $Description `
        -FilePath 'cmake.exe' `
        -Arguments @(
            "-DSOURCE_ROOT=$SourceRoot",
            "-DOUTPUT=$OutputPath",
            "-DEXPECTED_MODE=$Mode",
            "-DEXPECTED_REVISION=$Revision",
            "-DEXPECTED_VERSION=$Version",
            "-DEXPECTED_CONFIGURATION=$Configuration",
            "-DEXPECTED_ARCHITECTURE=$Architecture",
            '-P',
            $FixturePath
        ) `
        -TimeoutSec $script:CMakeCaseTimeoutSec `
        -ExpectedOutcome Success
}

function Assert-NoticeSectionContains {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Heading,
        [Parameter(Mandatory = $true)] [string[]]$Expected
    )

    $separator = '=============================================================================='
    $headingIndex = $Text.IndexOf($Heading, [StringComparison]::Ordinal)
    if ($headingIndex -lt 0) {
        throw "Third-party notices is missing section '$Heading'."
    }

    $licenseStart = $Text.IndexOf($separator, $headingIndex, [StringComparison]::Ordinal)
    if ($licenseStart -lt 0) {
        throw "Third-party notices section '$Heading' has no license separator."
    }

    $sectionEnd = $Text.IndexOf(
        $separator,
        $licenseStart + $separator.Length,
        [StringComparison]::Ordinal)
    if ($sectionEnd -lt 0) {
        $sectionEnd = $Text.Length
    }
    $section = $Text.Substring($headingIndex, $sectionEnd - $headingIndex)

    foreach ($expectedText in $Expected) {
        Assert-Contains $section $expectedText "Third-party notices section '$Heading'"
    }
}

$resolvedBuiltExecutable = (Resolve-Path -LiteralPath $BuiltExecutable).Path
$buildMetadataPath = Join-Path `
    (Split-Path -Parent $resolvedBuiltExecutable) `
    'spectiary_metadata.json'
if (-not (Test-Path -LiteralPath $buildMetadataPath -PathType Leaf)) {
    throw "Built executable metadata is missing beside Spectiary.exe: $buildMetadataPath"
}

$buildMetadata = Get-Content -Raw -LiteralPath $buildMetadataPath | ConvertFrom-Json
Assert-SchemaVersionFive `
    -Metadata $buildMetadata `
    -Description 'Built executable metadata'
Assert-ArtifactHashContract `
    -Metadata $buildMetadata `
    -ExecutablePath $resolvedBuiltExecutable `
    -Description 'Built executable artifact'
if ($buildMetadata.PSObject.Properties.Name -ccontains 'deployment') {
    throw 'Build-output metadata must not contain a deployment declaration.'
}
Assert-BuildSourceContract `
    -Metadata $buildMetadata.build `
    -ExpectedMode $SourceMode `
    -ExpectedRevision $SourceRevision `
    -Description 'Built executable metadata'
$expectedToolchainMetadata = [ordered]@{
    compiler_id = $CompilerId
    compiler_version = $CompilerVersion
    cmake_version = $CMakeVersion
    generator = $Generator
    target_architecture = $TargetArchitecture
    windows_sdk_version = if ([string]::IsNullOrEmpty($WindowsSdkVersion)) {
        $null
    }
    else {
        $WindowsSdkVersion
    }
}
Assert-BuildToolchainContract `
    -Metadata $buildMetadata.build `
    -Expected $expectedToolchainMetadata `
    -Description 'Built executable metadata'
$metadataCfitsioVersion = $buildMetadata.build.cfitsio
if ($metadataCfitsioVersion -isnot [string] -or
    [string]::IsNullOrWhiteSpace($metadataCfitsioVersion) -or
    $metadataCfitsioVersion -cne $metadataCfitsioVersion.Trim()) {
    throw 'Built executable metadata cfitsio must be a non-empty unpadded string.'
}
$expectedBuildMetadata = [ordered]@{
    configuration = $Configuration
    dear_imgui = $DearImGuiVersion
    implot = $ImPlotVersion
    cfitsio = $CfitsioVersion
    yaml_cpp = $YamlCppVersion
    zlib = $ZlibVersion
}
foreach ($expectedProperty in $expectedBuildMetadata.GetEnumerator()) {
    $actualValue = [string]$buildMetadata.build.($expectedProperty.Key)
    if ($actualValue -cne [string]$expectedProperty.Value) {
        throw "Built executable metadata '$($expectedProperty.Key)' expected '$($expectedProperty.Value)'; found '$actualValue'."
    }
}
if ($buildMetadata.product.name -cne 'Spectiary' -or
    $buildMetadata.product.version -cne $SpectiaryVersion) {
    throw "Built executable product metadata does not match Spectiary $SpectiaryVersion."
}
if ($Configuration -ceq 'Release') {
    Assert-StaticCfitsioPeImports `
        -ExecutablePath $resolvedBuiltExecutable `
        -Description 'Release build Spectiary.exe'
}

$legalRoot = Join-Path $RepoRoot 'legal'
$noticesPath = Join-Path $legalRoot 'THIRD_PARTY_NOTICES.txt'
$dataSourcesPath = Join-Path $legalRoot 'DATA_SOURCES.txt'
$catalogPath = Join-Path $RepoRoot 'config\spectral_lines.public.tsv'
$packageScriptPath = Join-Path $RepoRoot 'scripts\build-portable.ps1'
$portableVerifierPath = Join-Path $RepoRoot 'scripts\verify-portable.ps1'
$aboutSourcePath = Join-Path $RepoRoot 'src\ui\settings_panel.cpp'
$aboutTextSourcePath = Join-Path $RepoRoot 'src\ui\ui_text.cpp'
$mainSourcePath = Join-Path $RepoRoot 'src\main.cpp'
$cmakeSourcePath = Join-Path $RepoRoot 'CMakeLists.txt'
$vcpkgManifestPath = Join-Path $RepoRoot 'vcpkg.json'
$metadataEnsureScriptPath = Join-Path `
    $RepoRoot `
    'cmake\ensure_spectiary_metadata.cmake'
$buildIdentityTemplatePath = Join-Path $RepoRoot 'cmake\spectiary_build_identity.h.in'
$buildSourceContractPath = Join-Path $RepoRoot 'cmake\spectiary_build_source.cmake'
$buildIdentityFixturePath = Join-Path $RepoRoot 'tests\fixtures\configure_build_identity_header.cmake'
$manifestTemplatePath = Join-Path $RepoRoot 'src\platform\spectiary.exe.manifest.in'
$resourceHeaderPath = Join-Path $RepoRoot 'src\platform\spectiary_resource.h'
$resourceTemplatePath = Join-Path $RepoRoot 'src\platform\spectiary_resources.rc.in'
$embeddedLegalSourcePath = Join-Path $RepoRoot 'src\app\embedded_legal_documents.cpp'
$metadataBuildRegressionTestPath = Join-Path `
    $RepoRoot `
    'tests\spectiary_metadata_build_regression_tests.ps1'

foreach ($requiredPath in @(
    $noticesPath,
    $dataSourcesPath,
    $catalogPath,
    $packageScriptPath,
    $portableVerifierPath,
    $aboutSourcePath,
    $aboutTextSourcePath,
    $mainSourcePath,
    $cmakeSourcePath,
    $vcpkgManifestPath,
    $metadataEnsureScriptPath,
    $buildIdentityTemplatePath,
    $buildSourceContractPath,
    $buildIdentityFixturePath,
    $manifestTemplatePath,
    $resourceHeaderPath,
    $resourceTemplatePath,
    $embeddedLegalSourcePath,
    $metadataBuildRegressionTestPath,
    $GeneratedManifest,
    $GeneratedBuildIdentity
)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required release source is missing: $requiredPath"
    }
}

$vcpkgManifest = Get-Content -Raw -LiteralPath $vcpkgManifestPath |
    ConvertFrom-Json
$cfitsioDependencies = @(
    $vcpkgManifest.dependencies |
        Where-Object {
            $null -ne $_.PSObject.Properties['name'] -and
            $_.name -ceq 'cfitsio'
        }
)
if ($cfitsioDependencies.Count -ne 1) {
    throw "vcpkg.json must declare exactly one structured cfitsio dependency; found $($cfitsioDependencies.Count)."
}
$cfitsioDependency = $cfitsioDependencies[0]
$defaultFeaturesProperty =
    $cfitsioDependency.PSObject.Properties['default-features']
if ($null -eq $defaultFeaturesProperty -or
    $defaultFeaturesProperty.Value -ne $false) {
    throw 'vcpkg.json cfitsio dependency must set default-features to false.'
}
$cfitsioFeaturesProperty = $cfitsioDependency.PSObject.Properties['features']
if ($null -eq $cfitsioFeaturesProperty -or
    (@($cfitsioFeaturesProperty.Value) -join "`n") -cne 'pthreads') {
    throw 'vcpkg.json cfitsio dependency must enable exactly the pthreads feature.'
}

$builtLegalRoot = Join-Path `
    (Split-Path -Parent $resolvedBuiltExecutable) `
    'Legal'
if (Test-Path -LiteralPath $builtLegalRoot) {
    throw "Build output retains obsolete Legal directory: $builtLegalRoot"
}
foreach ($documentName in @(
        'EULA.txt',
        'THIRD_PARTY_NOTICES.txt',
        'DATA_SOURCES.txt')) {
    $obsoleteDocumentPath = Join-Path `
        (Split-Path -Parent $resolvedBuiltExecutable) `
        $documentName
    if (Test-Path -LiteralPath $obsoleteDocumentPath) {
        throw "Build output retains obsolete legal document: $obsoleteDocumentPath"
    }
}

$embeddedDocuments = @(
    [pscustomobject]@{ Name = 'THIRD_PARTY_NOTICES.txt'; Id = 101; Symbol = 'SPECTIARY_RESOURCE_THIRD_PARTY_NOTICES' },
    [pscustomobject]@{ Name = 'DATA_SOURCES.txt'; Id = 102; Symbol = 'SPECTIARY_RESOURCE_DATA_SOURCES' }
)
foreach ($document in $embeddedDocuments) {
    $expectedBytes = [Convert]::ToBase64String(
        [IO.File]::ReadAllBytes((Join-Path $legalRoot $document.Name)))
    $actualBytes = [Convert]::ToBase64String(
        (Get-ExecutableResourceBytes `
            -ExecutablePath $resolvedBuiltExecutable `
            -ResourceId $document.Id))
    if ($actualBytes -cne $expectedBytes) {
        throw "Embedded $($document.Name) does not byte-match its source file."
    }
}

$notices = Get-Content -Raw -LiteralPath $noticesPath
foreach ($expected in @(
    "Dear ImGui $DearImGuiVersion",
    'Copyright (c) 2014-2026 Omar Cornut',
    "ImPlot $ImPlotVersion",
    'Copyright (c) 2020 Evan Pezent',
    "CFITSIO $metadataCfitsioVersion",
    'Permission to freely use, copy, modify, and distribute this software',
    "PThreads4W $PthreadsVersion",
    'Apache License',
    'Version 2.0, January 2004',
    "yaml-cpp $YamlCppVersion",
    'Copyright (c) 2008-2015 Jesse Beder',
    "zlib $ZlibVersion",
    '(C) 1995-2026 Jean-loup Gailly and Mark Adler',
    'Copyright (c) 2017 Sean Barrett',
    'ProggyClean',
    'Copyright (c) 2004, 2005 Tristan Grimmer',
    'https://github.com/bluescan/proggyfonts',
    'ProggyForever',
    'Copyright (c) 2026 Disco Hello',
    'Copyright (c) 2019,2023 Tristan Grimmer',
    'https://github.com/ocornut/proggyforever'
)) {
    Assert-Contains $notices $expected 'Third-party notices'
}
Assert-NoticeSectionContains `
    -Text $notices `
    -Heading "CFITSIO $metadataCfitsioVersion" `
    -Expected @(
        'U.S. Government as represented by the Administrator',
        'Permission to freely use, copy, modify, and distribute this software',
        "THE SOFTWARE IS PROVIDED 'AS IS' WITHOUT ANY WARRANTY OF ANY KIND",
        'IN NO EVENT SHALL NASA BE LIABLE FOR ANY',
        'DAMAGES, INCLUDING, BUT NOT LIMITED TO, DIRECT, INDIRECT, SPECIAL OR'
    )
Assert-NoticeSectionContains `
    -Text $notices `
    -Heading "PThreads4W $PthreadsVersion" `
    -Expected @(
        'PThreads4W - POSIX threads for Windows',
        'Copyright 1998 John E. Bossom',
        'Copyright 1999-2018, Pthreads4w contributors',
        'This product includes software developed through the colaborative',
        'effort of several individuals, each of whom is listed in the file',
        'CONTRIBUTORS included with this software.',
        'Apache License',
        'Version 2.0, January 2004',
        'TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION',
        'Licensed under the Apache License, Version 2.0'
    )
Assert-NoticeSectionContains `
    -Text $notices `
    -Heading 'ProggyClean (embedded with Dear ImGui)' `
    -Expected @(
        'MIT License',
        'Copyright (c) 2004, 2005 Tristan Grimmer',
        'Permission is hereby granted, free of charge',
        'THE SOFTWARE IS PROVIDED "AS IS"'
    )
Assert-NoticeSectionContains `
    -Text $notices `
    -Heading 'ProggyForever (partial font embedded with Dear ImGui)' `
    -Expected @(
        'MIT License',
        'Copyright (c) 2026 Disco Hello',
        'Copyright (c) 2019,2023 Tristan Grimmer',
        'Permission is hereby granted, free of charge',
        'THE SOFTWARE IS PROVIDED "AS IS"'
    )

$dataSources = Get-Content -Raw -LiteralPath $dataSourcesPath
foreach ($expected in @(
    'Atomic Line List v3.00b5',
    'Creative Commons Attribution 4.0 International',
    '5707fb5304021392540c66bbf4a76535373d48cf008164f9f22a1f5343d49097',
    '6709.613 and',
    '6709.764 Angstrom into one unresolved band marker',
    'H I, C I, C II, Li I',
    '4075.631, 4075.694, and 4075.991 Angstrom for C II 4076',
    '6463.736 and 6463.915 Angstrom for C II 6464',
    'beta release offered for testing',
    'uses these values for visual line identification',
    'wavelength-calibration',
    'https://doi.org/10.1093/mnras/stad3230',
    'approximate 4078.9 Angstrom vacuum'
)) {
    Assert-Contains $dataSources $expected 'Data-source notice'
}

$catalogLines = Get-Content -LiteralPath $catalogPath -Encoding UTF8 |
    Where-Object { -not $_.StartsWith('#') }
$catalog = @($catalogLines | ConvertFrom-Csv -Delimiter "`t")
if ($catalog.Count -eq 0) {
    throw 'Public spectral-line catalog did not parse any rows.'
}

if (@($catalog | Where-Object { $_.source_ref -match '(?i)nist' }).Count -ne 0) {
    throw 'The release catalog still contains a direct NIST source handle.'
}

$sr = @($catalog | Where-Object { $_.id -eq 'sr_ii_4078' })
if ($sr.Count -ne 1 -or $sr[0].vacuum_angstrom -ne '4078.9' -or
    $sr[0].source_ref -ne 'spectiary_curated_approximate') {
    throw 'Sr II must remain the curated approximate 4078.9 Angstrom marker.'
}

foreach ($markerId in @(
    'c_i_5382',
    'c_ii_3920',
    'c_ii_4268_multiplet',
    'c_ii_6580',
    'c_ii_6585',
    'li_i_6708',
    'k_i_7667',
    'k_i_7701',
    'mg_i_8809'
)) {
    $markerRows = @($catalog | Where-Object { $_.id -eq $markerId })
    if ($markerRows.Count -ne 1) {
        throw "Expected exactly one release-catalog row for '$markerId', found $($markerRows.Count)."
    }
    if ($markerRows[0].source_ref -ne 'atll_v3_00b5') {
        throw "Release-catalog marker '$markerId' must use source_ref 'atll_v3_00b5'."
    }
}

$atllRows = @($catalog | Where-Object { $_.source_ref -eq 'atll_v3_00b5' })
if ($atllRows.Count -ne 43) {
    throw "Expected 43 Atomic Line List rows, found $($atllRows.Count)."
}

$baRows = @($catalog | Where-Object { $_.source_ref -eq 'ferrara_et_al_2024_air_to_vacuum' })
if ($baRows.Count -ne 2) {
    throw "Expected two Ferrara et al. Ba II rows, found $($baRows.Count)."
}

$packageScript = Get-Content -Raw -LiteralPath $packageScriptPath
$aboutSource = @(
    Get-Content -Raw -LiteralPath $aboutSourcePath
    Get-Content -Raw -LiteralPath $aboutTextSourcePath
) -join "`n"
$mainSource = Get-Content -Raw -LiteralPath $mainSourcePath
$cmakeSource = Get-Content -Raw -LiteralPath $cmakeSourcePath
$metadataEnsureScript = Get-Content -Raw -LiteralPath $metadataEnsureScriptPath
$buildIdentityTemplate = Get-Content -Raw -LiteralPath $buildIdentityTemplatePath
$manifestTemplate = Get-Content -Raw -LiteralPath $manifestTemplatePath
$resourceHeader = Get-Content -Raw -LiteralPath $resourceHeaderPath
$resourceTemplate = Get-Content -Raw -LiteralPath $resourceTemplatePath
$embeddedLegalSource = Get-Content -Raw -LiteralPath $embeddedLegalSourcePath
$generatedManifestText = Get-Content -Raw -LiteralPath $GeneratedManifest
[xml]$generatedManifest = $generatedManifestText
$generatedAssemblyVersion = [string]$generatedManifest.assembly.assemblyIdentity.version
$expectedAssemblyVersion = "$SpectiaryVersion.0"
if ($generatedAssemblyVersion -cne $expectedAssemblyVersion) {
    throw "Generated manifest assembly version expected '$expectedAssemblyVersion'; found '$generatedAssemblyVersion'."
}
Assert-Contains $manifestTemplate `
    'version="@PROJECT_VERSION_MAJOR@.@PROJECT_VERSION_MINOR@.@PROJECT_VERSION_PATCH@.0"' `
    'Manifest version template'
Assert-NotContains $manifestTemplate 'version="0.1.0.0"' 'Manifest version template'
if ($manifestTemplate -match 'version="[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"') {
    throw 'Manifest template contains a handwritten numeric assembly version.'
}
if (Test-Path -LiteralPath (Join-Path $RepoRoot 'src\platform\spectiary.exe.manifest')) {
    throw 'Tracked final manifest still exists beside the manifest template.'
}
Assert-Contains $cmakeSource `
    'set(SPECTIARY_MANIFEST_TEMPLATE' `
    'CMake manifest template binding'
Assert-Contains $cmakeSource `
    '"${CMAKE_BINARY_DIR}/generated/spectiary/spectiary.exe.manifest"' `
    'CMake generated manifest path'
Assert-Contains $cmakeSource `
    '"${SPECTIARY_MANIFEST_TEMPLATE}"' `
    'CMake manifest configure input'
Assert-Contains $cmakeSource `
    '"${SPECTIARY_MANIFEST}"' `
    'CMake manifest configure output and target source'
$nativeTargetStart = $cmakeSource.IndexOf(
    'add_executable(spectiary_native WIN32',
    [StringComparison]::Ordinal)
if ($nativeTargetStart -lt 0) {
    throw 'Could not locate the spectiary_native source declaration.'
}
$nativeTargetEnd = $cmakeSource.IndexOf(
    'spectiary_configure_production_target(spectiary_native)',
    $nativeTargetStart,
    [StringComparison]::Ordinal)
if ($nativeTargetEnd -lt 0) {
    throw 'Could not locate the spectiary_native source declaration.'
}
$nativeTargetSources = $cmakeSource.Substring(
    $nativeTargetStart,
    $nativeTargetEnd - $nativeTargetStart)
Assert-Contains $nativeTargetSources '${SPECTIARY_MANIFEST}' 'spectiary_native sources'
Assert-NotContains `
    $nativeTargetSources `
    'src/platform/spectiary.exe.manifest' `
    'spectiary_native sources'
Assert-Contains $nativeTargetSources '${SPECTIARY_WINDOWS_RESOURCES}' 'spectiary_native resources'
Assert-NotContains $packageScript '$releaseDocumentDirectoryName' 'Portable packaging script'
Assert-NotContains $packageScript '$releaseDocumentPackageRoot' 'Portable packaging script'
Assert-Contains $packageScript `
    "`$sourceExecutableDirectory = Split-Path -Parent `$sourceExecutable" `
    'Portable packaging executable binding'
Assert-Contains $packageScript `
    '& cmake --build --preset $Preset --config $Configuration --target spectiary_metadata' `
    'Portable packaging configuration binding'
Assert-Contains $packageScript `
    '"-DSPECTIARY_BUILD_SOURCE_MODE=$SourceMode"' `
    'Portable packaging source-mode binding'
Assert-Contains $packageScript `
    '"-DSPECTIARY_BUILD_SOURCE_REVISION=$SourceRevision"' `
    'Portable packaging source-revision binding'
Assert-Contains $packageScript `
    'Join-Path $sourceExecutableDirectory $MetadataFileName' `
    'Portable packaging metadata binding'
Assert-Contains $packageScript `
    "Join-Path `$scriptRoot 'verify-portable.ps1'" `
    'Portable artifact verifier binding'
Assert-Contains $packageScript `
    "distribution = 'portable'" `
    'Portable deployment identity'
Assert-Contains $packageScript `
    "storage_profile = 'portable'" `
    'Portable storage selection'
Assert-Contains $packageScript `
    'Get-FileHash -Algorithm SHA256 -LiteralPath $sourceExecutable' `
    'Portable executable hash verification'
Assert-Contains $packageScript `
    "artifactSha256 -cne `$buildExecutableHash" `
    'Portable metadata/build executable hash verification'
Assert-Contains $packageScript `
    "packagedArtifactSha256 -cne `$packagedExecutableHash" `
    'Portable packaged metadata/executable hash verification'
Assert-Contains (Get-Content -Raw -LiteralPath $portableVerifierPath) `
    'build executable hash' `
    'Portable artifact verifier build hash check'
Assert-NotContains $packageScript `
    '[switch]$SkipBuild' `
    'Portable packaging public parameters'
Assert-Contains $packageScript `
    '[switch]$PackageUnverifiedTestFixture' `
    'Portable packaging test seam'
Assert-NotContains $packageScript `
    "Join-Path `$buildRoot 'generated\spectiary\third_party_versions.json'" `
    'Portable packaging script'
Assert-Contains $cmakeSource '${PROJECT_metadata_filename}' 'CMake metadata'
Assert-NotContains $cmakeSource 'spectiary_metadata.json.in' 'CMake obsolete pre-link metadata template'
Assert-Contains $cmakeSource 'spectiary_build_identity.h.in' 'CMake build identity'
Assert-NotContains `
    $cmakeSource `
    'third_party_versions.h.in' `
    'CMake obsolete compile-time dependency versions'
Assert-Contains $cmakeSource `
    'generated/$<CONFIG>/spectiary/spectiary_build_identity.h' `
    'CMake generated build identity'
Assert-Contains $cmakeSource `
    'include("${CMAKE_SOURCE_DIR}/cmake/spectiary_build_source.cmake")' `
    'CMake build-source contract entry'
Assert-Contains $cmakeSource `
    'find_package(cfitsio CONFIG REQUIRED)' `
    'CMake CFITSIO package discovery'
Assert-Contains $cmakeSource `
    'spectiary_read_vcpkg_package_version(cfitsio SPECTIARY_CFITSIO_VERSION)' `
    'CMake CFITSIO package version resolution'
Assert-Contains $cmakeSource `
    'spectiary_read_vcpkg_package_version(pthreads SPECTIARY_PTHREADS_VERSION)' `
    'CMake PThreads4W package version resolution'
Assert-Contains $cmakeSource `
    'CFITSIO::cfitsio ZLIB::ZLIB' `
    'CMake core CFITSIO and zlib linkage'
Assert-Contains $cmakeSource `
    'OBJECT_DEPENDS' `
    'CMake embedded legal-document dependencies'
Assert-Contains $cmakeSource `
    '${SPECTIARY_LEGAL_DOCUMENTS};${SPECTIARY_RESOURCE_HEADER}' `
    'CMake embedded legal-document source dependencies'
Assert-NotContains $cmakeSource `
    'spectiary_release_documents' `
    'CMake obsolete release-document copy target'
Assert-Contains $cmakeSource `
    'add_dependencies(spectiary_native spectiary_metadata_finalizer_tool)' `
    'CMake post-link finalizer dependency'
Assert-Contains $cmakeSource `
    '/Brepro' `
    'CMake reproducible native link'
Assert-Contains $cmakeSource `
    '/INCREMENTAL:NO' `
    'CMake non-incremental native link'
Assert-Contains $cmakeSource `
    '$<$<CONFIG:Debug>:/DEBUG:FASTLINK>' `
    'CMake deterministic debug PDB link'
Assert-Contains $cmakeSource `
    'add_custom_command(TARGET spectiary_native PRE_LINK' `
    'CMake pre-link metadata invalidation'
Assert-Contains $cmakeSource `
    'Invalidating stale Spectiary metadata before linking' `
    'CMake stale-sidecar invalidation command'
Assert-Contains $cmakeSource `
    'ensure_spectiary_metadata.cmake' `
    'CMake metadata freshness verifier'
Assert-Contains $cmakeSource `
    'BYPRODUCTS' `
    'CMake metadata byproduct declaration'
Assert-Contains $cmakeSource `
    '-BuildTarget spectiary_metadata' `
    'CMake metadata regression target binding'
Assert-Contains $metadataEnsureScript `
    'file(SHA256' `
    'Metadata freshness executable hash check'
Assert-Contains $metadataEnsureScript `
    'execute_process(' `
    'Metadata freshness finalizer invocation'
Assert-Contains $cmakeSource `
    'spectiary_metadata_build_regression_tests' `
    'CMake repeated/no-op metadata build regression test'
Assert-Contains $cmakeSource `
    '-BuildDirectory "${CMAKE_BINARY_DIR}"' `
    'CMake build regression directory binding'
Assert-Contains $cmakeSource `
    'scripts/build-ninja-msvc-debug.ps1' `
    'CMake Ninja/MSVC build regression wrapper binding'
Assert-Contains $cmakeSource `
    'Finalizing schema 6 Spectiary metadata' `
    'CMake post-link finalizer command'
Assert-Contains $cmakeSource `
    'DEPENDS spectiary_native' `
    'CMake metadata target dependency'
$portableMetadataTargetStart = $cmakeSource.IndexOf(
    'add_custom_target(spectiary_portable_test_metadata',
    [StringComparison]::Ordinal)
if ($portableMetadataTargetStart -lt 0) {
    throw 'Could not locate the Portable metadata fixture target.'
}
$portableMetadataTargetEnd = $cmakeSource.IndexOf(
    'function(spectiary_add_test',
    $portableMetadataTargetStart,
    [StringComparison]::Ordinal)
if ($portableMetadataTargetEnd -lt 0) {
    throw 'Could not locate the end of the Portable metadata fixture target.'
}
$portableMetadataTargetSource = $cmakeSource.Substring(
    $portableMetadataTargetStart,
    $portableMetadataTargetEnd - $portableMetadataTargetStart)
if ($portableMetadataTargetSource -notmatch
    '(?ms)DEPENDS\s+spectiary_metadata(?:\s|$)') {
    throw 'Portable metadata fixture must depend on spectiary_metadata freshness target.'
}
if ($portableMetadataTargetSource -match
    '(?ms)DEPENDS\s+spectiary_native(?:\s|$)') {
    throw 'Portable metadata fixture must not bypass spectiary_metadata freshness target.'
}
Assert-NotContains $cmakeSource 'SPECTIARY_RELEASE_PROFILE' 'CMake unified executable'
Assert-NotContains $buildIdentityTemplate 'ReleaseProfile' 'Build identity header template'
Assert-Contains $buildIdentityTemplate `
    '@PROJECT_VERSION@' `
    'Build identity header template'
Assert-Contains $buildIdentityTemplate `
    '$<CONFIG>' `
    'Build identity header template'
Assert-Contains $buildIdentityTemplate `
    '@SPECTIARY_BUILD_TARGET_ARCHITECTURE@' `
    'Build identity header template'
Assert-Contains $buildIdentityTemplate `
    '@SPECTIARY_BUILD_SOURCE_MODE@' `
    'Build identity header template'
Assert-Contains $buildIdentityTemplate `
    '@SPECTIARY_BUILD_SOURCE_REVISION@' `
    'Build identity header template'
Assert-Contains $aboutSource `
    'build_info::kBuildSourceMode' `
    'About build source mode'
Assert-Contains $aboutSource `
    'build_info::kBuildSourceRevision' `
    'About build source revision'
Assert-Contains $aboutSource 'metadata.dear_imgui_version' 'About Dear ImGui metadata version'
Assert-Contains $aboutSource 'metadata.implot_version' 'About ImPlot metadata version'
Assert-Contains $aboutSource 'metadata.cfitsio_version' 'About CFITSIO metadata version'
Assert-Contains $aboutSource 'metadata.yaml_cpp_version' 'About yaml-cpp metadata version'
Assert-Contains $aboutSource 'metadata.zlib_version' 'About zlib metadata version'

$generatedBuildIdentityText = Get-Content -Raw -LiteralPath $GeneratedBuildIdentity
foreach ($expectedIdentityText in @(
    "kSpectiaryVersion[] = `"$SpectiaryVersion`"",
    "kBuildConfiguration[] = `"$Configuration`"",
    "kTargetArchitecture[] = `"$TargetArchitecture`"",
    "kBuildSourceMode[] = `"$SourceMode`"",
    "kBuildSourceRevision[] = `"$SourceRevision`"",
    "kBuildCfitsioVersion[] = `"$CfitsioVersion`"",
    "kBuildYamlCppVersion[] = `"$YamlCppVersion`""
)) {
    Assert-Contains `
        $generatedBuildIdentityText `
        $expectedIdentityText `
        'Generated build identity'
}
Assert-NotContains `
    $generatedBuildIdentityText `
    'ReleaseProfile' `
    'Generated build identity'
Assert-NotContains `
    $generatedBuildIdentityText `
    '$<CONFIG>' `
    'Generated build identity'
foreach ($document in $embeddedDocuments) {
    Assert-Contains `
        $resourceHeader `
        "#define $($document.Symbol) $($document.Id)" `
        'Legal resource identifiers'
    Assert-Contains $resourceTemplate $document.Name 'Legal resource template'
}
Assert-Contains $packageScript 'THIRD_PARTY_NOTICES.txt' 'Portable notice-version check'
Assert-NotContains $packageScript 'DATA_SOURCES.txt' 'Portable external document packaging'
Assert-Contains $embeddedLegalSource 'FindResourceW' 'Embedded legal runtime loader'
Assert-Contains $embeddedLegalSource 'RT_RCDATA' 'Embedded legal resource type'
Assert-Contains $aboutSource 'EmbeddedLegalDocumentContent' 'About embedded legal access'
Assert-NotContains $aboutSource 'Legal/' 'About external legal path'
$testRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    "spectiary-release-artifacts-$PID-$([Guid]::NewGuid().ToString('N'))"
$testDistRoot = Join-Path $testRoot 'dist'
try {
    $headRevision = '0123456789abcdef0123456789abcdef01234567'
    $packageWindowsSdkVersion = if ([string]::IsNullOrEmpty($WindowsSdkVersion)) {
        '10.0.26100.0'
    }
    else {
        $WindowsSdkVersion
    }
    $expectedPackageToolchainMetadata = [ordered]@{
        compiler_id = $CompilerId
        compiler_version = $CompilerVersion
        cmake_version = $CMakeVersion
        generator = $Generator
        target_architecture = $TargetArchitecture
        windows_sdk_version = $packageWindowsSdkVersion
    }
    Assert-BuildIdentityHeader `
        -CaseId 'build-identity-working-tree' `
        -FixturePath $buildIdentityFixturePath `
        -SourceRoot $RepoRoot `
        -OutputPath (Join-Path $testRoot 'working-tree-build-identity.h') `
        -Mode 'working_tree' `
        -Revision '' `
        -Version $SpectiaryVersion `
        -Configuration $Configuration `
        -Architecture $TargetArchitecture `
        -Description 'Working-tree compile-time build identity'
    Assert-BuildIdentityHeader `
        -CaseId 'build-identity-head' `
        -FixturePath $buildIdentityFixturePath `
        -SourceRoot $RepoRoot `
        -OutputPath (Join-Path $testRoot 'head-build-identity.h') `
        -Mode 'head' `
        -Revision $headRevision `
        -Version $SpectiaryVersion `
        -Configuration $Configuration `
        -Architecture $TargetArchitecture `
        -Description 'HEAD compile-time build identity'
    Assert-ScriptFails `
        -CaseId 'reject-unverified-package-outside-temp' `
        -ScriptPath $packageScriptPath `
        -Arguments @(
            '-PackageUnverifiedTestFixture',
            '-BuildRoot',
            (Join-Path $RepoRoot 'build\unverified-fixture'),
            '-DistRoot',
            (Join-Path $RepoRoot 'dist\unverified-fixture')
        ) `
        -ExpectedMessage 'reserved for release-artifact tests' `
        -Description 'Unverified package fixture outside the temporary directory'
    Assert-ScriptFails `
        -CaseId 'reject-direct-head-packaging' `
        -ScriptPath $packageScriptPath `
        -Arguments @(
            '-PackageUnverifiedTestFixture',
            '-BuildRoot',
            (Join-Path $testRoot 'direct-head-build'),
            '-DistRoot',
            $testDistRoot,
            '-SourceMode',
            'head',
            '-SourceRevision',
            $headRevision
        ) `
        -ExpectedMessage 'Head source mode is reserved for an isolated snapshot' `
        -Description 'Direct checkout head-mode packaging'

    $snapshotPackageRoot = Join-Path $testRoot 'package-script-snapshot'
    $snapshotPackageScriptsRoot = Join-Path $snapshotPackageRoot 'scripts'
    $snapshotPackageScriptsLibRoot = Join-Path $snapshotPackageScriptsRoot 'lib'
    $snapshotPackageLegalRoot = Join-Path $snapshotPackageRoot 'legal'
    New-Item -ItemType Directory -Path $snapshotPackageScriptsRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $snapshotPackageScriptsLibRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $snapshotPackageLegalRoot -Force | Out-Null
    $snapshotIdentityRoot = Join-Path $snapshotPackageRoot 'config'
    New-Item -ItemType Directory -Path $snapshotIdentityRoot -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'config\project_identity.json') -Destination $snapshotIdentityRoot
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'scripts\project-identity.ps1') -Destination $snapshotPackageScriptsRoot
    $snapshotPackageScriptPath = Join-Path `
        $snapshotPackageScriptsRoot `
        'build-portable.ps1'
    Copy-Item -LiteralPath $packageScriptPath -Destination $snapshotPackageScriptPath
    Copy-Item `
        -LiteralPath $portableVerifierPath `
        -Destination (Join-Path $snapshotPackageScriptsRoot 'verify-portable.ps1')
    Copy-Item `
        -LiteralPath (Join-Path $RepoRoot 'scripts\lib\portable_pe_helpers.ps1') `
        -Destination (Join-Path $snapshotPackageScriptsLibRoot 'portable_pe_helpers.ps1')
    Copy-Item `
        -LiteralPath $noticesPath `
        -Destination (Join-Path $snapshotPackageLegalRoot 'THIRD_PARTY_NOTICES.txt')

    $packageFixtures = @(
        [pscustomobject]@{
            Mode = 'working_tree'
            Revision = ''
            PackageName = 'Spectiary-portable-working-tree-test'
        },
        [pscustomobject]@{
            Mode = 'head'
            Revision = $headRevision
            PackageName = 'Spectiary-portable-head-test'
        }
    )

    foreach ($fixture in $packageFixtures) {
        $fixturePackageScriptPath = if ($fixture.Mode -ceq 'head') {
            $snapshotPackageScriptPath
        }
        else {
            $packageScriptPath
        }
        $fixtureBuildRoot = Join-Path $testRoot "build-$($fixture.Mode)"
        New-Item -ItemType Directory -Path $fixtureBuildRoot -Force | Out-Null
        Copy-Item `
            -LiteralPath $resolvedBuiltExecutable `
            -Destination (Join-Path $fixtureBuildRoot 'Spectiary.exe')

        $fixtureMetadata = Get-Content -Raw -LiteralPath $buildMetadataPath | ConvertFrom-Json
        $fixtureMetadata.product.name = 'Display-only rename fixture'
        $fixtureMetadata.build.source_mode = $fixture.Mode
        $fixtureMetadata.build.source_revision = if ($fixture.Mode -ceq 'working_tree') {
            $null
        }
        else {
            $fixture.Revision
        }
        $fixtureMetadata.build.windows_sdk_version = $packageWindowsSdkVersion
        $fixtureMetadataPath = Join-Path $fixtureBuildRoot 'spectiary_metadata.json'
        $fixtureMetadata |
            ConvertTo-Json -Depth 10 |
            Set-Content -LiteralPath $fixtureMetadataPath -Encoding UTF8

        Assert-ScriptSucceeds `
            -CaseId "package-$($fixture.Mode.Replace('_', '-'))" `
            -ScriptPath $fixturePackageScriptPath `
            -Arguments @(
                '-PackageUnverifiedTestFixture',
                '-Configuration',
                $Configuration,
                '-PackageName',
                $fixture.PackageName,
                '-BuildRoot',
                $fixtureBuildRoot,
                '-DistRoot',
                $testDistRoot,
                '-SourceMode',
                $fixture.Mode,
                '-SourceRevision',
                $fixture.Revision
            ) `
            -Description "$($fixture.Mode) Portable packaging and verification"

        $fixturePackageRoot = Join-Path $testDistRoot $fixture.PackageName
        $fixtureZipPath = Join-Path $testDistRoot "$($fixture.PackageName).zip"
        Assert-PortablePackage `
            -ExpectedExecutablePath $resolvedBuiltExecutable `
            -PackageRoot $fixturePackageRoot `
            -ZipPath $fixtureZipPath `
            -Description "$($fixture.Mode) Portable fixture"
        $packagedMetadata = Get-Content -Raw `
            -LiteralPath (Join-Path $fixturePackageRoot 'spectiary_metadata.json') |
            ConvertFrom-Json
        Assert-BuildSourceContract `
            -Metadata $packagedMetadata.build `
            -ExpectedMode $fixture.Mode `
            -ExpectedRevision $fixture.Revision `
            -Description "$($fixture.Mode) packaged metadata"
        Assert-BuildToolchainContract `
            -Metadata $packagedMetadata.build `
            -Expected $expectedPackageToolchainMetadata `
            -Description "$($fixture.Mode) packaged metadata"
    }

    $wrongArtifactHash = ('0' * 64) -join ''
    $tamperedMetadataBuildRoot = Join-Path $testRoot 'tampered-metadata-build'
    New-Item -ItemType Directory -Path $tamperedMetadataBuildRoot -Force | Out-Null
    Copy-Item `
        -LiteralPath $resolvedBuiltExecutable `
        -Destination (Join-Path $tamperedMetadataBuildRoot 'Spectiary.exe')
    $tamperedMetadata = Get-Content -Raw -LiteralPath $buildMetadataPath |
        ConvertFrom-Json
    $tamperedMetadata.build.windows_sdk_version = $packageWindowsSdkVersion
    $tamperedMetadata.artifact.sha256 = $wrongArtifactHash
    $tamperedMetadata |
        ConvertTo-Json -Depth 10 |
        Set-Content `
            -LiteralPath (Join-Path $tamperedMetadataBuildRoot 'spectiary_metadata.json') `
            -Encoding UTF8
    Assert-ScriptFails `
        -CaseId 'reject-tampered-build-metadata-hash' `
        -ScriptPath $packageScriptPath `
        -Arguments @(
            '-PackageUnverifiedTestFixture',
            '-BuildRoot',
            $tamperedMetadataBuildRoot,
            '-DistRoot',
            $testDistRoot,
            '-PackageName',
            'Spectiary-portable-tampered-metadata',
            '-Configuration',
            $Configuration
        ) `
        -ExpectedMessage 'does not match build directory Spectiary.exe hash' `
        -Description 'Portable packaging rejects tampered metadata hash'

    $tamperedExecutableBuildRoot = Join-Path $testRoot 'tampered-executable-build'
    New-Item -ItemType Directory -Path $tamperedExecutableBuildRoot -Force | Out-Null
    $tamperedExecutablePath = Join-Path $tamperedExecutableBuildRoot 'Spectiary.exe'
    Copy-Item -LiteralPath $resolvedBuiltExecutable -Destination $tamperedExecutablePath
    Add-Content -LiteralPath $tamperedExecutablePath -Value 'tampered' -Encoding ASCII
    $tamperedExecutableMetadata =
        Get-Content -Raw -LiteralPath $buildMetadataPath |
        ConvertFrom-Json
    $tamperedExecutableMetadata.build.windows_sdk_version = $packageWindowsSdkVersion
    $tamperedExecutableMetadata |
        ConvertTo-Json -Depth 10 |
        Set-Content `
            -LiteralPath (Join-Path $tamperedExecutableBuildRoot 'spectiary_metadata.json') `
            -Encoding UTF8
    Assert-ScriptFails `
        -CaseId 'reject-tampered-build-executable' `
        -ScriptPath $packageScriptPath `
        -Arguments @(
            '-PackageUnverifiedTestFixture',
            '-BuildRoot',
            $tamperedExecutableBuildRoot,
            '-DistRoot',
            $testDistRoot,
            '-PackageName',
            'Spectiary-portable-tampered-executable',
            '-Configuration',
            $Configuration
        ) `
        -ExpectedMessage 'does not match build directory Spectiary.exe hash' `
        -Description 'Portable packaging rejects tampered build executable'

    $verifiedPackage = $packageFixtures[0]
    $verifiedPackageRoot = Join-Path $testDistRoot $verifiedPackage.PackageName
    $verifiedPackageZip = Join-Path $testDistRoot "$($verifiedPackage.PackageName).zip"
    $verifiedPackageMetadataPath = Join-Path $verifiedPackageRoot 'spectiary_metadata.json'

    $hiddenEntryPackageRoot = Join-Path $testRoot 'hidden-entry-package'
    New-Item `
        -ItemType Directory `
        -Path (Join-Path $hiddenEntryPackageRoot 'state') `
        -Force |
        Out-Null
    Copy-Item `
        -LiteralPath (Join-Path $verifiedPackageRoot 'Spectiary.exe') `
        -Destination (Join-Path $hiddenEntryPackageRoot 'Spectiary.exe')
    Copy-Item `
        -LiteralPath $verifiedPackageMetadataPath `
        -Destination (
            Join-Path $hiddenEntryPackageRoot 'spectiary_metadata.json')
    $hiddenEntryPath = Join-Path `
        $hiddenEntryPackageRoot `
        'hidden-cfitsio.dll'
    [IO.File]::WriteAllText($hiddenEntryPath, 'hidden')
    $hiddenEntry = Get-Item -LiteralPath $hiddenEntryPath -Force
    $hiddenEntry.Attributes =
        $hiddenEntry.Attributes -bor [IO.FileAttributes]::Hidden
    Assert-ScriptFails `
        -CaseId 'reject-hidden-extra-package-entry' `
        -ScriptPath $portableVerifierPath `
        -Arguments @(
            '-BuildExecutable',
            $resolvedBuiltExecutable,
            '-PackageRoot',
            $hiddenEntryPackageRoot,
            '-ZipPath',
            $verifiedPackageZip
        ) `
        -ExpectedMessage 'Portable package root entries are wrong' `
        -Description 'Portable verifier rejects hidden extra package entries'

    $wrongDataTypePackageRoot = Join-Path $testRoot 'wrong-data-type-package'
    New-Item `
        -ItemType Directory `
        -Path $wrongDataTypePackageRoot `
        -Force |
        Out-Null
    Copy-Item `
        -LiteralPath (Join-Path $verifiedPackageRoot 'Spectiary.exe') `
        -Destination (Join-Path $wrongDataTypePackageRoot 'Spectiary.exe')
    Copy-Item `
        -LiteralPath $verifiedPackageMetadataPath `
        -Destination (
            Join-Path $wrongDataTypePackageRoot 'spectiary_metadata.json')
    foreach ($role in @('config', 'logs', 'unsaved')) {
        New-Item -ItemType Directory -Path (Join-Path $wrongDataTypePackageRoot $role) | Out-Null
    }
    [IO.File]::WriteAllText(
        (Join-Path $wrongDataTypePackageRoot 'state'),
        'not a directory')
    Assert-ScriptFails `
        -CaseId 'reject-data-file' `
        -ScriptPath $portableVerifierPath `
        -Arguments @(
            '-BuildExecutable',
            $resolvedBuiltExecutable,
            '-PackageRoot',
            $wrongDataTypePackageRoot,
            '-ZipPath',
            $verifiedPackageZip
        ) `
        -ExpectedMessage 'Portable package root state must be a directory' `
        -Description 'Portable verifier rejects a state file in place of the directory'

    $originalPackageMetadataBytes = [IO.File]::ReadAllBytes($verifiedPackageMetadataPath)
    try {
        $tamperedPackageMetadata =
            Get-Content -Raw -LiteralPath $verifiedPackageMetadataPath |
            ConvertFrom-Json
        $tamperedPackageMetadata.artifact.sha256 = $wrongArtifactHash
        [IO.File]::WriteAllText(
            $verifiedPackageMetadataPath,
            (($tamperedPackageMetadata | ConvertTo-Json -Depth 10) + [Environment]::NewLine),
            (New-Object Text.UTF8Encoding($false)))
        Assert-ScriptFails `
            -CaseId 'reject-packaged-metadata-tamper' `
            -ScriptPath $portableVerifierPath `
            -Arguments @(
                '-BuildExecutable',
                $resolvedBuiltExecutable,
                '-PackageRoot',
                $verifiedPackageRoot,
                '-ZipPath',
                $verifiedPackageZip
            ) `
            -ExpectedMessage 'does not match build executable hash' `
            -Description 'Published Portable metadata tamper is detected'
    }
    finally {
        [IO.File]::WriteAllBytes(
            $verifiedPackageMetadataPath,
            $originalPackageMetadataBytes)
    }

    $verifiedPackageExecutablePath = Join-Path $verifiedPackageRoot 'Spectiary.exe'
    $originalPackageExecutableBytes = [IO.File]::ReadAllBytes(
        $verifiedPackageExecutablePath)
    try {
        $tamperedPackageExecutableBytes = New-Object byte[] (
            $originalPackageExecutableBytes.Length + 1)
        [Array]::Copy(
            $originalPackageExecutableBytes,
            $tamperedPackageExecutableBytes,
            $originalPackageExecutableBytes.Length)
        $tamperedPackageExecutableBytes[
            $tamperedPackageExecutableBytes.Length - 1] = 0
        [IO.File]::WriteAllBytes(
            $verifiedPackageExecutablePath,
            $tamperedPackageExecutableBytes)
        Assert-ScriptFails `
            -CaseId 'reject-packaged-executable-tamper' `
            -ScriptPath $portableVerifierPath `
            -Arguments @(
                '-BuildExecutable',
                $resolvedBuiltExecutable,
                '-PackageRoot',
                $verifiedPackageRoot,
                '-ZipPath',
                $verifiedPackageZip
            ) `
            -ExpectedMessage 'does not match packaged executable hash' `
            -Description 'Published Portable executable tamper is detected'
    }
    finally {
        [IO.File]::WriteAllBytes(
            $verifiedPackageExecutablePath,
            $originalPackageExecutableBytes)
    }

    $tamperedZipPath = Join-Path `
        $testDistRoot `
        "$($verifiedPackage.PackageName)-tampered-metadata.zip"
    Copy-ZipWithMutations `
        -SourcePath $verifiedPackageZip `
        -DestinationPath $tamperedZipPath `
        -TamperedEntryName 'spectiary_metadata.json'
    Assert-ScriptFails `
        -CaseId 'reject-zip-metadata-tamper' `
        -ScriptPath $portableVerifierPath `
        -Arguments @(
            '-BuildExecutable',
            $resolvedBuiltExecutable,
            '-PackageRoot',
            $verifiedPackageRoot,
            '-ZipPath',
            $tamperedZipPath
        ) `
        -TimeoutSec $script:DeepArchiveCaseTimeoutSec `
        -ExpectedMessage 'Portable ZIP metadata ZIP entry' `
        -Description 'Published Portable ZIP metadata tamper is detected'

    $tamperedExecutableZipPath = Join-Path `
        $testDistRoot `
        "$($verifiedPackage.PackageName)-tampered-executable.zip"
    Copy-ZipWithMutations `
        -SourcePath $verifiedPackageZip `
        -DestinationPath $tamperedExecutableZipPath `
        -TamperedEntryName 'Spectiary.exe'
    Assert-ScriptFails `
        -CaseId 'reject-zip-executable-tamper' `
        -ScriptPath $portableVerifierPath `
        -Arguments @(
            '-BuildExecutable',
            $resolvedBuiltExecutable,
            '-PackageRoot',
            $verifiedPackageRoot,
            '-ZipPath',
            $tamperedExecutableZipPath
        ) `
        -TimeoutSec $script:DeepArchiveCaseTimeoutSec `
        -ExpectedMessage 'Portable ZIP executable ZIP entry' `
        -Description 'Published Portable ZIP executable tamper is detected'

    $injectedCfitsioZipPath = Join-Path `
        $testDistRoot `
        "$($verifiedPackage.PackageName)-injected-cfitsio.zip"
    Copy-ZipWithMutations `
        -SourcePath $verifiedPackageZip `
        -DestinationPath $injectedCfitsioZipPath `
        -AdditionalEntryName 'cfitsio.dll'
    Assert-ScriptFails `
        -CaseId 'reject-zip-cfitsio-injection' `
        -ScriptPath $portableVerifierPath `
        -Arguments @(
            '-BuildExecutable',
            $resolvedBuiltExecutable,
            '-PackageRoot',
            $verifiedPackageRoot,
            '-ZipPath',
            $injectedCfitsioZipPath
        ) `
        -TimeoutSec $script:DeepArchiveCaseTimeoutSec `
        -ExpectedMessage 'Portable ZIP must not contain a CFITSIO DLL: cfitsio.dll' `
        -Description 'Published Portable ZIP rejects an injected CFITSIO DLL'

    $originalPortableMetadataBytes = [IO.File]::ReadAllBytes(
        $verifiedPackageMetadataPath)
    try {
        $invalidPortableMetadata =
            Get-Content -Raw -LiteralPath $verifiedPackageMetadataPath |
            ConvertFrom-Json
        $invalidPortableMetadata.build.windows_sdk_version = $null
        [IO.File]::WriteAllText(
            $verifiedPackageMetadataPath,
            (($invalidPortableMetadata | ConvertTo-Json -Depth 10) +
                [Environment]::NewLine),
            (New-Object Text.UTF8Encoding($false)))
        Assert-ScriptFails `
            -CaseId 'reject-missing-windows-sdk-version' `
            -ScriptPath $portableVerifierPath `
            -Arguments @(
                '-BuildExecutable',
                $resolvedBuiltExecutable,
                '-PackageRoot',
                $verifiedPackageRoot,
                '-ZipPath',
                $verifiedPackageZip
            ) `
            -ExpectedMessage 'windows_sdk_version must be a non-empty dotted numeric version' `
            -Description 'Portable verifier rejects missing Windows SDK version'
    }
    finally {
        [IO.File]::WriteAllBytes(
            $verifiedPackageMetadataPath,
            $originalPortableMetadataBytes)
    }

    $invalidCfitsioVersions = @(
        [pscustomobject]@{
            CaseId = 'reject-cfitsio-empty'
            Description = 'empty'
            Value = ''
        },
        [pscustomobject]@{
            CaseId = 'reject-cfitsio-blank'
            Description = 'blank'
            Value = '   '
        },
        [pscustomobject]@{
            CaseId = 'reject-cfitsio-leading-padding'
            Description = 'leading padding'
            Value = ' 4.6.4'
        },
        [pscustomobject]@{
            CaseId = 'reject-cfitsio-trailing-padding'
            Description = 'trailing padding'
            Value = '4.6.4 '
        },
        [pscustomobject]@{
            CaseId = 'reject-cfitsio-nonnumeric'
            Description = 'non-numeric'
            Value = '4.6.x'
        }
    )
    foreach ($invalidCfitsioVersion in $invalidCfitsioVersions) {
        try {
            $invalidPortableMetadata =
                Get-Content -Raw -LiteralPath $verifiedPackageMetadataPath |
                ConvertFrom-Json
            $invalidPortableMetadata.build.cfitsio =
                $invalidCfitsioVersion.Value
            [IO.File]::WriteAllText(
                $verifiedPackageMetadataPath,
                (($invalidPortableMetadata | ConvertTo-Json -Depth 10) +
                    [Environment]::NewLine),
                (New-Object Text.UTF8Encoding($false)))
            Assert-ScriptFails `
                -CaseId $invalidCfitsioVersion.CaseId `
                -ScriptPath $portableVerifierPath `
                -Arguments @(
                    '-BuildExecutable',
                    $resolvedBuiltExecutable,
                    '-PackageRoot',
                    $verifiedPackageRoot,
                    '-ZipPath',
                    $verifiedPackageZip
                ) `
                -ExpectedMessage 'cfitsio must be a non-empty, unpadded dotted numeric version' `
                -Description (
                    'Portable verifier rejects ' +
                    $invalidCfitsioVersion.Description +
                    ' CFITSIO version metadata')
        }
        finally {
            [IO.File]::WriteAllBytes(
                $verifiedPackageMetadataPath,
                $originalPortableMetadataBytes)
        }
    }

    $invalidMetadataCases = @(
        [pscustomobject]@{
            CaseId = 'reject-metadata-missing-application-id'
            Description = 'Missing machine application identity'
            PropertyName = 'application_id'
            Remove = $true
            Value = $null
            ExpectedMessage = "missing non-empty string 'application_id'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-foreign-application-id'
            Description = 'Foreign machine application identity'
            PropertyName = 'application_id'
            Remove = $false
            Value = 'Spectiary'
            ExpectedMessage = 'application_id does not match the founding identity'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-schema-string'
            Description = 'Metadata schema as string'
            PropertyName = 'schema_version'
            Remove = $false
            Value = '4'
            ExpectedMessage = 'schema_version must be the integer 6'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-schema-decimal'
            Description = 'Metadata schema as decimal'
            PropertyName = 'schema_version'
            Remove = $false
            Value = 4
            RawSchemaJson = '4.0'
            ExpectedMessage = 'schema_version must be the integer 6'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-schema-array'
            Description = 'Metadata schema as array'
            PropertyName = 'schema_version'
            Remove = $false
            Value = @(4)
            ExpectedMessage = 'schema_version must be the integer 6'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-legacy-schema'
            Description = 'Metadata legacy schema'
            PropertyName = 'schema_version'
            Remove = $false
            Value = 2
            ExpectedMessage = 'schema_version must be the integer 6'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-missing-compiler-id'
            Description = 'Metadata missing compiler ID'
            PropertyName = 'compiler_id'
            Remove = $true
            Value = $null
            ExpectedMessage = "missing non-empty string 'compiler_id'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-blank-generator'
            Description = 'Metadata blank generator'
            PropertyName = 'generator'
            Remove = $false
            Value = ' '
            ExpectedMessage = "missing non-empty string 'generator'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-unsupported-compiler'
            Description = 'Metadata unsupported compiler'
            PropertyName = 'compiler_id'
            Remove = $false
            Value = 'Clang'
            ExpectedMessage = "expected 'MSVC'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-invalid-compiler-version'
            Description = 'Metadata invalid compiler version'
            PropertyName = 'compiler_version'
            Remove = $false
            Value = 'latest'
            ExpectedMessage = 'invalid compiler_version'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-invalid-cmake-version'
            Description = 'Metadata invalid CMake version'
            PropertyName = 'cmake_version'
            Remove = $false
            Value = '4'
            ExpectedMessage = 'invalid cmake_version'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-unsupported-architecture'
            Description = 'Metadata unsupported target architecture'
            PropertyName = 'target_architecture'
            Remove = $false
            Value = 'arm64'
            ExpectedMessage = "expected 'amd64'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-missing-windows-sdk'
            Description = 'Metadata missing Windows SDK version'
            PropertyName = 'windows_sdk_version'
            Remove = $false
            Value = $null
            ExpectedMessage = "missing non-empty string 'windows_sdk_version'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-invalid-windows-sdk'
            Description = 'Metadata invalid Windows SDK version'
            PropertyName = 'windows_sdk_version'
            Remove = $false
            Value = 'current'
            ExpectedMessage = 'invalid windows_sdk_version'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-missing-cfitsio'
            Description = 'Metadata missing CFITSIO provenance'
            PropertyName = 'cfitsio'
            Remove = $true
            Value = $null
            ExpectedMessage = "missing non-empty string 'cfitsio'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-cfitsio-notice-mismatch'
            Description = 'Metadata CFITSIO provenance disagrees with notices'
            PropertyName = 'cfitsio'
            Remove = $false
            Value = '999.0.0'
            ExpectedMessage = 'THIRD_PARTY_NOTICES.txt is stale for CFITSIO'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-missing-yaml-cpp'
            Description = 'Metadata missing yaml-cpp provenance'
            PropertyName = 'yaml_cpp'
            Remove = $true
            Value = $null
            ExpectedMessage = "missing non-empty string 'yaml_cpp'"
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-yaml-cpp-notice-mismatch'
            Description = 'Metadata yaml-cpp provenance disagrees with notices'
            PropertyName = 'yaml_cpp'
            Remove = $false
            Value = '999.0.0'
            ExpectedMessage = 'THIRD_PARTY_NOTICES.txt is stale for yaml-cpp'
        },
        [pscustomobject]@{
            CaseId = 'reject-metadata-working-tree-revision-array'
            Description = 'Working-tree revision as null array'
            PropertyName = 'source_revision'
            Remove = $false
            Value = @($null)
            ExpectedMessage = 'must use null source_revision'
        }
    )
    foreach ($invalidCase in $invalidMetadataCases) {
        $invalidBuildRoot = Join-Path `
            $testRoot `
            "invalid-metadata-$($invalidCase.PropertyName)-$([Guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Path $invalidBuildRoot -Force | Out-Null
        Copy-Item `
            -LiteralPath $resolvedBuiltExecutable `
            -Destination (Join-Path $invalidBuildRoot 'Spectiary.exe')
        $invalidMetadata = Get-Content -Raw -LiteralPath $buildMetadataPath |
            ConvertFrom-Json
        $invalidMetadata.build.windows_sdk_version = $packageWindowsSdkVersion
        $metadataContainer = if ($invalidCase.PropertyName -cin @('schema_version', 'application_id')) {
            $invalidMetadata
        }
        else {
            $invalidMetadata.build
        }
        if ($invalidCase.Remove) {
            $metadataContainer.PSObject.Properties.Remove($invalidCase.PropertyName)
        }
        else {
            $metadataContainer.($invalidCase.PropertyName) = $invalidCase.Value
        }
        $invalidMetadataJson =
            $invalidMetadata |
            ConvertTo-Json -Depth 10
        $rawSchemaJsonProperty =
            $invalidCase.PSObject.Properties['RawSchemaJson']
        if ($null -ne $rawSchemaJsonProperty) {
            $invalidMetadataJson = [regex]::Replace(
                $invalidMetadataJson,
                '(?m)("schema_version"\s*:\s*)4(?=\s*,)',
                '${1}' + $rawSchemaJsonProperty.Value)
        }
        Set-Content `
            -LiteralPath (Join-Path $invalidBuildRoot 'spectiary_metadata.json') `
            -Value $invalidMetadataJson `
            -Encoding UTF8
        Assert-ScriptFails `
            -CaseId $invalidCase.CaseId `
            -ScriptPath $packageScriptPath `
            -Arguments @(
                '-PackageUnverifiedTestFixture',
                '-BuildRoot',
                $invalidBuildRoot,
                '-DistRoot',
                $testDistRoot,
                '-PackageName',
                "Spectiary-portable-invalid-$($invalidCase.PropertyName)",
                '-Configuration',
                $Configuration
            ) `
            -ExpectedMessage $invalidCase.ExpectedMessage `
            -Description $invalidCase.Description
    }

    $invalidHeadRevisionBuildRoot = Join-Path `
        $testRoot `
        'invalid-head-revision-array'
    New-Item `
        -ItemType Directory `
        -Path $invalidHeadRevisionBuildRoot `
        -Force |
        Out-Null
    Copy-Item `
        -LiteralPath $resolvedBuiltExecutable `
        -Destination (Join-Path $invalidHeadRevisionBuildRoot 'Spectiary.exe')
    $invalidHeadRevisionMetadata =
        Get-Content -Raw -LiteralPath $buildMetadataPath |
        ConvertFrom-Json
    $invalidHeadRevisionMetadata.build.source_mode = 'head'
    $invalidHeadRevisionMetadata.build.source_revision = @($headRevision)
    $invalidHeadRevisionMetadata.build.windows_sdk_version =
        $packageWindowsSdkVersion
    $invalidHeadRevisionMetadata |
        ConvertTo-Json -Depth 10 |
        Set-Content `
            -LiteralPath (
                Join-Path `
                    $invalidHeadRevisionBuildRoot `
                    'spectiary_metadata.json'
            ) `
            -Encoding UTF8
    Assert-ScriptFails `
        -CaseId 'reject-head-revision-array' `
        -ScriptPath $snapshotPackageScriptPath `
        -Arguments @(
            '-PackageUnverifiedTestFixture',
            '-BuildRoot',
            $invalidHeadRevisionBuildRoot,
            '-DistRoot',
            $testDistRoot,
            '-PackageName',
            'Spectiary-portable-invalid-head-revision-array',
            '-Configuration',
            $Configuration,
            '-SourceMode',
            'head',
            '-SourceRevision',
            $headRevision
        ) `
        -ExpectedMessage 'must use a single string source_revision' `
        -Description 'Head revision as array'

    $invalidBaseArguments = @(
        '-PackageUnverifiedTestFixture',
        '-BuildRoot',
        (Join-Path $testRoot 'invalid-build'),
        '-DistRoot',
        $testDistRoot
    )
    Assert-ScriptFails `
        -CaseId 'reject-unknown-source-mode' `
        -ScriptPath $packageScriptPath `
        -Arguments ($invalidBaseArguments + @('-SourceMode', 'unknown')) `
        -Description 'Unknown source mode'
    Assert-ScriptFails `
        -CaseId 'reject-head-without-revision' `
        -ScriptPath $packageScriptPath `
        -Arguments ($invalidBaseArguments + @('-SourceMode', 'head')) `
        -Description 'Head mode without revision'
    Assert-ScriptFails `
        -CaseId 'reject-working-tree-with-revision' `
        -ScriptPath $packageScriptPath `
        -Arguments ($invalidBaseArguments + @(
            '-SourceMode',
            'working_tree',
            '-SourceRevision',
            $headRevision
        )) `
        -Description 'Working-tree mode with revision'
    Assert-ScriptFails `
        -CaseId 'reject-head-invalid-revision' `
        -ScriptPath $packageScriptPath `
        -Arguments ($invalidBaseArguments + @(
            '-SourceMode',
            'head',
            '-SourceRevision',
            'not-a-full-object-id'
        )) `
        -Description 'Head mode with invalid revision'

    $cmakeContractCases = @(
        [pscustomobject]@{
            CaseId = 'cmake-working-tree'
            Mode = 'working_tree'
            Revision = ''
            ShouldSucceed = $true
            Description = 'CMake working-tree contract'
        },
        [pscustomobject]@{
            CaseId = 'cmake-head'
            Mode = 'head'
            Revision = $headRevision
            ShouldSucceed = $true
            Description = 'CMake head contract'
        },
        [pscustomobject]@{
            CaseId = 'cmake-reject-unknown-mode'
            Mode = 'unknown'
            Revision = ''
            ShouldSucceed = $false
            Description = 'CMake unknown mode'
        },
        [pscustomobject]@{
            CaseId = 'cmake-reject-head-without-revision'
            Mode = 'head'
            Revision = ''
            ShouldSucceed = $false
            Description = 'CMake head without revision'
        },
        [pscustomobject]@{
            CaseId = 'cmake-reject-working-tree-with-revision'
            Mode = 'working_tree'
            Revision = $headRevision
            ShouldSucceed = $false
            Description = 'CMake working tree with revision'
        },
        [pscustomobject]@{
            CaseId = 'cmake-reject-abbreviated-head'
            Mode = 'head'
            Revision = '0123456789ab'
            ShouldSucceed = $false
            Description = 'CMake abbreviated head revision'
        },
        [pscustomobject]@{
            CaseId = 'cmake-reject-uppercase-head'
            Mode = 'head'
            Revision = '0123456789ABCDEF0123456789ABCDEF01234567'
            ShouldSucceed = $false
            Description = 'CMake uppercase head revision'
        }
    )
    foreach ($contractCase in $cmakeContractCases) {
        Assert-BuildSourceCMakeContract `
            -CaseId $contractCase.CaseId `
            -ContractPath $buildSourceContractPath `
            -Mode $contractCase.Mode `
            -Revision $contractCase.Revision `
            -ShouldSucceed $contractCase.ShouldSucceed `
            -Description $contractCase.Description
    }
}
finally {
    $cleanupStatus = 'passed'
    Write-Host "CASE CLEANUP BEGIN id=release-artifacts-temp-root path=$testRoot"
    try {
        if (Test-Path -LiteralPath $testRoot) {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
        }
        if (Test-Path -LiteralPath $testRoot) {
            throw "Release artifact test root remains after cleanup: $testRoot"
        }
    }
    catch {
        $cleanupStatus = 'failed'
        throw
    }
    finally {
        Write-Host (
            'CASE CLEANUP END id=release-artifacts-temp-root status={0}' -f
                $cleanupStatus)
    }
}

Write-Output 'release artifact tests passed'
