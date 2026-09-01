[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PresetsFile,

    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Assert-True {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-SourceCMakeLists {
    param([Parameter(Mandatory = $true)] [string]$Directory)

    foreach ($path in [IO.Directory]::EnumerateFiles(
            $Directory,
            'CMakeLists.txt',
            [IO.SearchOption]::TopDirectoryOnly)) {
        $path
    }
    foreach ($path in [IO.Directory]::EnumerateDirectories($Directory)) {
        $name = [IO.Path]::GetFileName($path)
        if ($name -cin @('.git', '.scratch', 'build')) {
            continue
        }
        Get-SourceCMakeLists -Directory $path
    }
}

function Add-DeclaredTier {
    param(
        [Parameter(Mandatory = $true)] [hashtable]$DeclaredTiers,
        [Parameter(Mandatory = $true)] [string]$TestName,
        [Parameter(Mandatory = $true)] [string]$Tier
    )

    if (-not $DeclaredTiers.ContainsKey($TestName)) {
        $DeclaredTiers[$TestName] = @()
    }
    if ($DeclaredTiers[$TestName] -cnotcontains $Tier) {
        $DeclaredTiers[$TestName] =
            @($DeclaredTiers[$TestName]) + $Tier
    }
}

function Assert-StaticTierDeclarations {
    param([Parameter(Mandatory = $true)] [string]$Directory)

    $registeredTests = @{}
    $declaredTiers = @{}
    $registrationPattern = [regex]::new(
        '(?ims)\badd_test\s*\(\s*NAME\s+' +
        '(?<name>[A-Za-z0-9_.+-]+)')
    $helperRegistrationPattern = [regex]::new(
        '(?ims)\b(?:specforge_add_test|' +
        'specforge_add_portable_only_test)\s*\(\s*' +
        '(?<name>[A-Za-z0-9_.+-]+)')
    $tierListPattern = [regex]::new(
        '(?ims)\bset\s*\(\s*SPECFORGE_' +
        '(?<tier>FAST|EXTENDED)_TESTS(?<body>.*?)\)')
    $tierListEntryPattern = [regex]::new(
        '(?m)^\s*(?<name>specforge_[A-Za-z0-9_.+-]+)\s*$')
    $tierHelperPattern = [regex]::new(
        '(?ims)\bspecforge_set_test_tier\s*\(\s*' +
        '(?<name>[A-Za-z0-9_.+-]+)\s+' +
        '(?<tier>fast|extended)\s*\)')

    foreach ($cmakeLists in Get-SourceCMakeLists -Directory $Directory) {
        $text = Get-Content -Raw -LiteralPath $cmakeLists
        foreach ($pattern in @(
                $registrationPattern,
                $helperRegistrationPattern)) {
            foreach ($match in $pattern.Matches($text)) {
                $registeredTests[$match.Groups['name'].Value] = $true
            }
        }
        foreach ($match in $tierListPattern.Matches($text)) {
            $tier = $match.Groups['tier'].Value.ToLowerInvariant()
            foreach ($entry in $tierListEntryPattern.Matches(
                    $match.Groups['body'].Value)) {
                Add-DeclaredTier `
                    -DeclaredTiers $declaredTiers `
                    -TestName $entry.Groups['name'].Value `
                    -Tier $tier
            }
        }
        foreach ($match in $tierHelperPattern.Matches($text)) {
            Add-DeclaredTier `
                -DeclaredTiers $declaredTiers `
                -TestName $match.Groups['name'].Value `
                -Tier $match.Groups['tier'].Value
        }
    }

    $staticViolations = @()
    foreach ($testName in @($registeredTests.Keys | Sort-Object)) {
        $tiers = @(
            if ($declaredTiers.ContainsKey($testName)) {
                @($declaredTiers[$testName])
            }
        )
        if ($tiers.Count -ne 1) {
            $displayedTiers = if ($tiers.Count -eq 0) {
                '<none>'
            }
            else {
                $tiers -join ', '
            }
            $staticViolations +=
                "${testName}: expected one static tier; tiers=$displayedTiers"
        }
    }
    foreach ($testName in @($declaredTiers.Keys | Sort-Object)) {
        if (-not $registeredTests.ContainsKey($testName)) {
            $staticViolations +=
                "${testName}: tier declared without a CTest registration"
        }
    }
    Assert-True `
        -Condition ($staticViolations.Count -eq 0) `
        -Message (
            "Static CTest tier contract violations:`n" +
            ($staticViolations -join "`n")
        )
    return $registeredTests.Count
}

$absoluteSourceDirectory = [IO.Path]::GetFullPath($SourceDirectory)
$staticRegistrationCount =
    Assert-StaticTierDeclarations -Directory $absoluteSourceDirectory

$absoluteBuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$ctestOutput = @(
    & ctest `
        --test-dir $absoluteBuildDirectory `
        --show-only=json-v1
)
Assert-True `
    -Condition ($LASTEXITCODE -eq 0) `
    -Message 'CTest could not describe the configured test model.'

$ctestModel = $ctestOutput | ConvertFrom-Json
$tierNames = @('fast', 'extended')
$tierCounts = @{
    fast = 0
    extended = 0
}
$violations = @()
foreach ($test in $ctestModel.tests) {
    $labels = @(
        $test.properties |
            Where-Object { $_.name -ceq 'LABELS' } |
            ForEach-Object { @($_.value) }
    )
    $assignedTiers = @(
        $tierNames | Where-Object { $labels -ccontains $_ }
    )
    if ($assignedTiers.Count -ne 1) {
        $displayedLabels = if ($labels.Count -eq 0) {
            '<none>'
        }
        else {
            $labels -join ', '
        }
        $violations +=
            "$($test.name): expected exactly one tier; labels=$displayedLabels"
    }
    else {
        ++$tierCounts[$assignedTiers[0]]
    }
}
Assert-True `
    -Condition ($ctestModel.tests.Count -gt 0) `
    -Message 'The configured CTest model must contain tests.'
Assert-True `
    -Condition ($violations.Count -eq 0) `
    -Message ("CTest tier contract violations:`n" + ($violations -join "`n"))
foreach ($tierName in $tierNames) {
    Assert-True `
        -Condition ($tierCounts[$tierName] -gt 0) `
        -Message "The '$tierName' CTest tier must not be empty."
}

$presets = Get-Content -Raw -LiteralPath $PresetsFile | ConvertFrom-Json
$specializedLabelPattern =
    '^(real-gui|gui-integration|release|periodic|asdf-pinned-oracle)$'
foreach ($tierName in $tierNames) {
    $matchingPresets = @(
        $presets.testPresets |
            Where-Object { $_.name -ceq $tierName }
    )
    Assert-True `
        -Condition ($matchingPresets.Count -eq 1) `
        -Message "CTest must define exactly one '$tierName' test preset."

    $preset = $matchingPresets[0]
    $specializedExclusionValid = $true
    if ($tierName -ceq 'extended') {
        $specializedExclusionValid =
            $preset.filter.exclude.label -ceq $specializedLabelPattern
    }
    Assert-True `
        -Condition (
            $preset.configurePreset -ceq 'ninja-msvc-debug' -and
            $preset.filter.include.label -ceq "^$tierName`$" -and
            $specializedExclusionValid -and
            $preset.output.outputOnFailure -eq $true -and
            $preset.execution.noTestsAction -ceq 'error'
        ) `
        -Message (
            "The '$tierName' preset must select only its Debug tier, " +
            'show failures, reject an empty suite, and keep specialized ' +
            'verification out of the extended headless suite.'
        )
}

Write-Host (
    "CTest tier contract passed for $($ctestModel.tests.Count) " +
    "configured tests and $staticRegistrationCount source registrations."
)
