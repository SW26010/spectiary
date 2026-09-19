[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CMakeCommand,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$BuiltExecutable,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [string]$BuildTarget = 'spectiary_metadata',

    [string]$BuildWrapper = '',

    [string]$BuildPreset = '',

    [int]$BuildTimeoutSec = 180
)

$ErrorActionPreference = 'Stop'

Import-Module `
    (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') `
    -Force `
    -ErrorAction Stop

$resolvedBuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$resolvedBuiltExecutable = [IO.Path]::GetFullPath($BuiltExecutable)
$executableDirectory = Split-Path -Parent $resolvedBuiltExecutable
$metadataPath = Join-Path $executableDirectory 'spectiary_metadata.json'
$resolvedBuildWrapper = ''
if (-not [string]::IsNullOrWhiteSpace($BuildWrapper)) {
    $resolvedBuildWrapper = (Resolve-Path -LiteralPath $BuildWrapper).Path
    if ([string]::IsNullOrWhiteSpace($BuildPreset)) {
        throw 'BuildPreset is required when BuildWrapper is provided.'
    }
    if ($BuildTimeoutSec -lt 1) {
        throw 'BuildTimeoutSec must be positive.'
    }

    $wrapperRepoRoot = [IO.Path]::GetFullPath(
        (Join-Path (Split-Path -Parent $resolvedBuildWrapper) '..'))
    $wrapperBuildDirectory = [IO.Path]::GetFullPath(
        (Join-Path $wrapperRepoRoot "build\$BuildPreset"))
    if (-not $wrapperBuildDirectory.Equals(
            $resolvedBuildDirectory,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "BuildWrapper preset '$BuildPreset' resolves to '$wrapperBuildDirectory', not '$resolvedBuildDirectory'."
    }
}

function Invoke-NativeBuild {
    if (-not [string]::IsNullOrWhiteSpace($resolvedBuildWrapper)) {
        $wrapperArguments = @(
            '-NoProfile',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            $resolvedBuildWrapper,
            '-Preset',
            $BuildPreset,
            '-Target',
            $BuildTarget,
            '-TimeoutSec',
            $BuildTimeoutSec
        )
        Write-Host (
            "Running bounded MSVC build wrapper: powershell " +
            ($wrapperArguments -join ' '))
        & powershell @wrapperArguments
        if ($LASTEXITCODE -ne 0) {
            throw "MSVC build wrapper failed with exit code $LASTEXITCODE."
        }
        return
    }

    $buildArguments = @(
        '--build',
        $resolvedBuildDirectory,
        '--config',
        $Configuration,
        '--target',
        $BuildTarget
    )
    Write-Host "Running real build: $CMakeCommand $($buildArguments -join ' ')"
    & $CMakeCommand @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE."
    }
}

function Get-ExecutableHash {
    param([Parameter(Mandatory = $true)] [string]$Path)

    return (
        Get-FileHash -Algorithm SHA256 -LiteralPath $Path
    ).Hash.ToLowerInvariant()
}

function Read-FileBytes {
    param([Parameter(Mandatory = $true)] [string]$Path)

    return ,([IO.File]::ReadAllBytes($Path))
}

function Test-ByteArraysEqual {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Left,
        [Parameter(Mandatory = $true)] [byte[]]$Right
    )

    return [Convert]::ToBase64String($Left) -ceq [Convert]::ToBase64String($Right)
}

function Read-Metadata {
    return Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-Json
}

function ConvertFrom-MetadataTimestamp {
    param([Parameter(Mandatory = $true)] [string]$Value)

    return [DateTimeOffset]::ParseExact(
        $Value,
        "yyyy-MM-dd'T'HH:mm:ss'Z'",
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal -bor
            [Globalization.DateTimeStyles]::AdjustToUniversal)
}

function Assert-FreshMetadata {
    param([Parameter(Mandatory = $true)] [string]$Description)

    if (-not (Test-Path -LiteralPath $resolvedBuiltExecutable -PathType Leaf)) {
        throw "$Description is missing the built executable: $resolvedBuiltExecutable"
    }
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        throw "$Description is missing metadata: $metadataPath"
    }

    $metadata = Read-Metadata
    if (($metadata.schema_version -isnot [int] -and
         $metadata.schema_version -isnot [long]) -or
        $metadata.schema_version -ne 6) {
        throw "$Description did not produce schema 6 metadata."
    }
    if ($null -eq $metadata.build -or
        $null -eq $metadata.build.completed_at_utc -or
        [string]$metadata.build.completed_at_utc -notmatch
            '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') {
        throw "$Description did not produce a finalized UTC timestamp."
    }
    if ($null -eq $metadata.artifact -or
        $metadata.artifact.file -cne 'Spectiary.exe') {
        throw "$Description did not produce canonical artifact metadata."
    }

    $executableHash = Get-ExecutableHash -Path $resolvedBuiltExecutable
    if ($metadata.artifact.sha256 -cne $executableHash) {
        throw "$Description metadata hash '$($metadata.artifact.sha256)' does not match executable hash '$executableHash'."
    }
    return $executableHash
}

if (-not (Test-Path -LiteralPath $resolvedBuildDirectory -PathType Container)) {
    throw "Build directory does not exist: $resolvedBuildDirectory"
}
if (-not (Test-Path -LiteralPath $resolvedBuiltExecutable -PathType Leaf)) {
    throw "Expected an existing executable before the repeated-build test: $resolvedBuiltExecutable"
}
if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
    throw "Expected existing finalized metadata before the repeated-build test: $metadataPath"
}

$originalExecutableHash = Assert-FreshMetadata `
    -Description 'Initial native build'
$originalMetadataBytes = Read-FileBytes -Path $metadataPath
$originalMetadata = Read-Metadata
$originalCompletedAtUtc = ConvertFrom-MetadataTimestamp `
    -Value ([string]$originalMetadata.build.completed_at_utc)

$relinkedMetadataBytes = $null
try {
    # Remove only the target executable. Its object files and the rest of the
    # CTest build remain intact, while the next build must perform a real link
    # and run the post-build finalizer against the new executable.
    Start-Sleep -Seconds 2
    Remove-Item -LiteralPath $resolvedBuiltExecutable -Force
    Invoke-NativeBuild

    $relinkedExecutableHash = Assert-FreshMetadata `
        -Description 'Repeated native build'
    if ($relinkedExecutableHash -cne $originalExecutableHash) {
        throw 'Repeated real link changed the executable hash.'
    }
    $relinkedMetadataBytes = Read-FileBytes -Path $metadataPath
    $relinkedMetadata = Read-Metadata
    $relinkedCompletedAtUtc = ConvertFrom-MetadataTimestamp `
        -Value ([string]$relinkedMetadata.build.completed_at_utc)
    $relinkedMetadataWriteTime =
        (Get-Item -LiteralPath $metadataPath).LastWriteTimeUtc
    if (Test-ByteArraysEqual `
            -Left $originalMetadataBytes `
            -Right $relinkedMetadataBytes) {
        throw 'Repeated native build did not refresh the finalized metadata sidecar.'
    }
    if ($relinkedCompletedAtUtc -le $originalCompletedAtUtc) {
        throw "Repeated native build did not advance completed_at_utc beyond '$($originalMetadata.build.completed_at_utc)'."
    }

    # A second build must be a real CMake no-op. Waiting crosses the one-second
    # schema timestamp granularity so an accidental finalizer rerun is visible
    # in both the JSON bytes and the filesystem write time.
    Start-Sleep -Seconds 2
    Invoke-NativeBuild
    $noOpExecutableHash = Get-ExecutableHash -Path $resolvedBuiltExecutable
    $noOpMetadataBytes = Read-FileBytes -Path $metadataPath
    $noOpMetadataWriteTime =
        (Get-Item -LiteralPath $metadataPath).LastWriteTimeUtc
    if ($noOpExecutableHash -cne $relinkedExecutableHash) {
        throw 'No-op build changed the executable hash.'
    }
    if (-not (Test-ByteArraysEqual `
            -Left $relinkedMetadataBytes `
            -Right $noOpMetadataBytes)) {
        throw 'No-op build refreshed metadata unexpectedly.'
    }
    if ($noOpMetadataWriteTime.Ticks -ne $relinkedMetadataWriteTime.Ticks) {
        throw 'No-op build changed the metadata filesystem write time.'
    }

    # Changing only the metadata completion timestamp must never feed back into
    # the executable. The following no-op also proves that metadata is not an
    # output that causes the post-build finalizer to rerun.
    $timestampChangedMetadata = Read-Metadata
    $timestampChangedMetadata.build.completed_at_utc = '2099-12-31T23:59:59Z'
    $timestampChangedJson =
        ($timestampChangedMetadata | ConvertTo-Json -Depth 10) +
        [Environment]::NewLine
    [IO.File]::WriteAllText(
        $metadataPath,
        $timestampChangedJson,
        [Text.UTF8Encoding]::new($false))
    $timestampChangedMetadataBytes = Read-FileBytes -Path $metadataPath
    $hashBeforeTimestampNoOp = Get-ExecutableHash -Path $resolvedBuiltExecutable
    if ($hashBeforeTimestampNoOp -cne $relinkedExecutableHash) {
        throw 'Changing metadata timestamp changed the executable hash.'
    }

    Start-Sleep -Seconds 2
    Invoke-NativeBuild
    $afterTimestampNoOpHash = Get-ExecutableHash -Path $resolvedBuiltExecutable
    $afterTimestampNoOpMetadataBytes = Read-FileBytes -Path $metadataPath
    $afterTimestampNoOpMetadata = Read-Metadata
    if ($afterTimestampNoOpHash -cne $hashBeforeTimestampNoOp) {
        throw 'No-op build changed the executable after a metadata timestamp change.'
    }
    if (-not (Test-ByteArraysEqual `
            -Left $timestampChangedMetadataBytes `
            -Right $afterTimestampNoOpMetadataBytes)) {
        throw 'No-op build overwrote metadata whose timestamp had changed.'
    }
    if ($afterTimestampNoOpMetadata.build.completed_at_utc -cne
        '2099-12-31T23:59:59Z') {
        throw 'No-op build refreshed the metadata completion timestamp.'
    }

    Write-Host 'Spectiary repeated/no-op metadata build regression passed.'
}
finally {
    # Leave the build directory with the fresh pair produced by the real link,
    # rather than the deliberately modified timestamp fixture.
    if ($null -ne $relinkedMetadataBytes -and
        (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        $currentMetadataBytes = Read-FileBytes -Path $metadataPath
        if (-not (Test-ByteArraysEqual `
                -Left $relinkedMetadataBytes `
                -Right $currentMetadataBytes)) {
            [IO.File]::WriteAllBytes($metadataPath, $relinkedMetadataBytes)
        }
    }
}
