[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$DearImGuiVersion,

    [Parameter(Mandatory = $true)]
    [string]$ImPlotVersion,

    [Parameter(Mandatory = $true)]
    [string]$ZlibVersion,

    [Parameter(Mandatory = $true)]
    [string]$BuiltExecutable,

    [Parameter(Mandatory = $true)]
    [string]$SpecForgeVersion,

    [Parameter(Mandatory = $true)]
    [string]$ReleaseProfile,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$SourceMode
)

$ErrorActionPreference = 'Stop'

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
    'specforge_build_metadata.json'
if (-not (Test-Path -LiteralPath $buildMetadataPath -PathType Leaf)) {
    throw "Built executable metadata is missing beside SpecForge.exe: $buildMetadataPath"
}

$buildMetadata = Get-Content -Raw -LiteralPath $buildMetadataPath | ConvertFrom-Json
if ($buildMetadata.schema_version -ne 2) {
    throw "Built executable metadata has unsupported schema version '$($buildMetadata.schema_version)'."
}
$sourceRevisionProperty = $buildMetadata.PSObject.Properties['source_revision']
if ($null -eq $sourceRevisionProperty) {
    throw "Built executable metadata is missing 'source_revision'."
}
if ($null -ne $sourceRevisionProperty.Value) {
    throw "Working-tree build metadata must use null source_revision."
}
$expectedBuildMetadata = [ordered]@{
    source_mode = $SourceMode
    specforge_version = $SpecForgeVersion
    release_profile = $ReleaseProfile
    configuration = $Configuration
    dear_imgui = $DearImGuiVersion
    implot = $ImPlotVersion
    zlib = $ZlibVersion
}
foreach ($expectedProperty in $expectedBuildMetadata.GetEnumerator()) {
    $actualValue = [string]$buildMetadata.($expectedProperty.Key)
    if ($actualValue -cne [string]$expectedProperty.Value) {
        throw "Built executable metadata '$($expectedProperty.Key)' expected '$($expectedProperty.Value)'; found '$actualValue'."
    }
}

$legalRoot = Join-Path $RepoRoot 'legal'
$eulaPath = Join-Path $legalRoot 'EULA.txt'
$noticesPath = Join-Path $legalRoot 'THIRD_PARTY_NOTICES.txt'
$dataSourcesPath = Join-Path $legalRoot 'DATA_SOURCES.txt'
$catalogPath = Join-Path $RepoRoot 'config\spectral_lines.public.tsv'
$packageScriptPath = Join-Path $RepoRoot 'scripts\build-portable.ps1'
$aboutSourcePath = Join-Path $RepoRoot 'src\ui\settings_panel.cpp'
$cmakeSourcePath = Join-Path $RepoRoot 'CMakeLists.txt'
$buildMetadataTemplatePath = Join-Path $RepoRoot 'cmake\specforge_build_metadata.json.in'

foreach ($requiredPath in @(
    $eulaPath,
    $noticesPath,
    $dataSourcesPath,
    $catalogPath,
    $packageScriptPath,
    $aboutSourcePath,
    $cmakeSourcePath,
    $buildMetadataTemplatePath
)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required release source is missing: $requiredPath"
    }
}

$builtLegalRoot = Join-Path `
    (Split-Path -Parent $resolvedBuiltExecutable) `
    'Legal'
foreach ($documentName in @('EULA.txt', 'THIRD_PARTY_NOTICES.txt', 'DATA_SOURCES.txt')) {
    Assert-FilesMatch `
        -ExpectedPath (Join-Path $legalRoot $documentName) `
        -ActualPath (Join-Path $builtLegalRoot $documentName) `
        -Description "Executable-adjacent Legal/$documentName"
}

$eula = Get-Content -Raw -LiteralPath $eulaPath
Assert-Contains $eula 'Copyright (c) 2026 SpecForge.' 'EULA'
Assert-Contains $eula 'THIRD_PARTY_NOTICES.txt' 'EULA third-party boundary'
Assert-Contains $eula 'DATA_SOURCES.txt' 'EULA data boundary'

$notices = Get-Content -Raw -LiteralPath $noticesPath
foreach ($expected in @(
    "Dear ImGui $DearImGuiVersion",
    'Copyright (c) 2014-2026 Omar Cornut',
    "ImPlot $ImPlotVersion",
    'Copyright (c) 2020 Evan Pezent',
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
    $sr[0].source_ref -ne 'specforge_curated_approximate') {
    throw 'Sr II must remain the curated approximate 4078.9 Angstrom marker.'
}

foreach ($markerId in @('li_i_6708', 'k_i_7667', 'k_i_7701', 'mg_i_8809')) {
    $markerRows = @($catalog | Where-Object { $_.id -eq $markerId })
    if ($markerRows.Count -ne 1) {
        throw "Expected exactly one release-catalog row for '$markerId', found $($markerRows.Count)."
    }
    if ($markerRows[0].source_ref -ne 'atll_v3_00b5') {
        throw "Release-catalog marker '$markerId' must use source_ref 'atll_v3_00b5'."
    }
}

$atllRows = @($catalog | Where-Object { $_.source_ref -eq 'atll_v3_00b5' })
if ($atllRows.Count -ne 19) {
    throw "Expected 19 Atomic Line List rows, found $($atllRows.Count)."
}

$baRows = @($catalog | Where-Object { $_.source_ref -eq 'ferrara_et_al_2024_air_to_vacuum' })
if ($baRows.Count -ne 2) {
    throw "Expected two Ferrara et al. Ba II rows, found $($baRows.Count)."
}

$packageScript = Get-Content -Raw -LiteralPath $packageScriptPath
$aboutSource = Get-Content -Raw -LiteralPath $aboutSourcePath
$cmakeSource = Get-Content -Raw -LiteralPath $cmakeSourcePath
$buildMetadataTemplate = Get-Content -Raw -LiteralPath $buildMetadataTemplatePath
Assert-Contains $packageScript "`$releaseDocumentDirectoryName = 'Legal'" 'Portable packaging script'
Assert-Contains $packageScript `
    "`$sourceExecutableDirectory = Split-Path -Parent `$sourceExecutable" `
    'Portable packaging executable binding'
Assert-Contains $packageScript `
    '& cmake --build --preset $Preset --config $Configuration' `
    'Portable packaging configuration binding'
Assert-Contains $packageScript `
    '& cmake --preset $Preset -DSPECFORGE_BUILD_SOURCE_MODE=working_tree' `
    'Portable packaging source-mode binding'
Assert-Contains $packageScript `
    "Join-Path `$sourceExecutableDirectory 'specforge_build_metadata.json'" `
    'Portable packaging metadata binding'
Assert-NotContains $packageScript `
    "Join-Path `$buildRoot 'generated\specforge\third_party_versions.json'" `
    'Portable packaging script'
Assert-Contains $cmakeSource 'specforge_build_metadata.json' 'CMake build metadata'
Assert-Contains $cmakeSource `
    'add_custom_target(specforge_release_documents' `
    'CMake release-document target'
Assert-Contains $cmakeSource `
    'DEPENDS ${SPECFORGE_RELEASE_DOCUMENTS}' `
    'CMake release-document source dependencies'
Assert-Contains $cmakeSource `
    'add_dependencies(specforge_native specforge_release_documents)' `
    'CMake executable release-document dependency'
Assert-Contains $cmakeSource `
    'add_dependencies(specforge_native specforge_build_metadata)' `
    'CMake executable build-metadata dependency'
foreach ($propertyName in @(
    'source_mode',
    'source_revision',
    'specforge_version',
    'release_profile',
    'configuration',
    'dear_imgui',
    'implot',
    'zlib'
)) {
    Assert-Contains $buildMetadataTemplate "`"$propertyName`"" 'Build metadata template'
}
Assert-Contains $aboutSource 'build_info::kDearImGuiVersion' 'About Dear ImGui version'
Assert-Contains $aboutSource 'build_info::kImPlotVersion' 'About ImPlot version'
Assert-Contains $aboutSource 'build_info::kZlibVersion' 'About zlib version'
foreach ($documentName in @('EULA.txt', 'THIRD_PARTY_NOTICES.txt', 'DATA_SOURCES.txt')) {
    Assert-Contains $packageScript $documentName 'Portable packaging script'
    Assert-Contains $aboutSource "Legal/$documentName" 'About panel'
}

$testRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    "specforge-release-artifacts-$PID-$([Guid]::NewGuid().ToString('N'))"
$testBuildRoot = Join-Path $testRoot 'build'
$testDistRoot = Join-Path $testRoot 'dist'
$testPackageName = 'SpecForge-portable-test'
$testPackageRoot = Join-Path $testDistRoot $testPackageName
$testZipPath = Join-Path $testDistRoot "$testPackageName.zip"
try {
    New-Item -ItemType Directory -Path $testBuildRoot -Force | Out-Null
    Copy-Item -LiteralPath $resolvedBuiltExecutable -Destination (Join-Path $testBuildRoot 'SpecForge.exe')
    Copy-Item -LiteralPath $buildMetadataPath -Destination (Join-Path $testBuildRoot 'specforge_build_metadata.json')

    & $packageScriptPath `
        -SkipBuild `
        -Configuration $Configuration `
        -PackageName $testPackageName `
        -BuildRoot $testBuildRoot `
        -DistRoot $testDistRoot

    $expectedPackageEntries = @(
        'Data',
        'Legal',
        'SpecForge.exe',
        'specforge_build_metadata.json'
    )
    $actualPackageEntries = @(
        Get-ChildItem -LiteralPath $testPackageRoot |
            ForEach-Object { $_.Name } |
            Sort-Object
    )
    if (($actualPackageEntries -join "`n") -cne (($expectedPackageEntries | Sort-Object) -join "`n")) {
        throw "Portable package root entries are wrong: $($actualPackageEntries -join ', ')."
    }
    Assert-FilesMatch `
        -ExpectedPath $buildMetadataPath `
        -ActualPath (Join-Path $testPackageRoot 'specforge_build_metadata.json') `
        -Description 'Packaged build metadata'

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($testZipPath)
    try {
        $expectedZipEntries = @(
            'Data/',
            'Legal/',
            'Legal/DATA_SOURCES.txt',
            'Legal/EULA.txt',
            'Legal/THIRD_PARTY_NOTICES.txt',
            'SpecForge.exe',
            'specforge_build_metadata.json'
        )
        $actualZipEntries = @($archive.Entries.FullName | Sort-Object)
        if (($actualZipEntries -join "`n") -cne (($expectedZipEntries | Sort-Object) -join "`n")) {
            throw "Portable ZIP entries are wrong: $($actualZipEntries -join ', ')."
        }

        $metadataEntry = $archive.GetEntry('specforge_build_metadata.json')
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
        $buildMetadataBytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes($buildMetadataPath))
        if ($zipMetadataBytes -cne $buildMetadataBytes) {
            throw 'Portable ZIP build metadata does not byte-match executable-adjacent metadata.'
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

Write-Output 'release artifact tests passed'
