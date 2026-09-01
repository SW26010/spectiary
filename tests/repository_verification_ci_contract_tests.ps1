[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Workflow,

    [Parameter(Mandatory = $true)]
    [string]$AutomationWorkflow,

    [Parameter(Mandatory = $true)]
    [string]$PresetsFile,

    [Parameter(Mandatory = $true)]
    [string]$RootCMakeLists,

    [Parameter(Mandatory = $true)]
    [string]$AsdfCMakeLists
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

function Get-TriggerBody {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $match = [regex]::Match(
        $Text,
        '(?ms)^on:\r?\n(?<body>.*?)(?=^[A-Za-z0-9_-]+:\r?\n|\z)')
    Assert-True `
        -Condition $match.Success `
        -Message "$Description must define an on block."
    return $match.Groups['body'].Value
}

function Assert-ManualOnlyTrigger {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $triggerBody = Get-TriggerBody -Text $Text -Description $Description
    $eventNames = @(
        [regex]::Matches(
            $triggerBody,
            "(?m)^  (?<event>[A-Za-z0-9_-]+|'[A-Za-z0-9_-]+'|`"[A-Za-z0-9_-]+`")\s*:") |
            ForEach-Object {
                $_.Groups['event'].Value.Trim(
                    [char[]]@([char]39, [char]34))
            }
    )
    Assert-True `
        -Condition (
            $eventNames.Count -eq 1 -and
            $eventNames[0] -ceq 'workflow_dispatch'
        ) `
        -Message (
            "$Description must whitelist only workflow_dispatch; found: " +
            ($eventNames -join ', ')
        )
}

function Get-JobBody {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$JobId
    )

    $pattern =
        '(?ms)^  ' + [regex]::Escape($JobId) +
        ':\r?\n(?<body>.*?)(?=^  [A-Za-z0-9_-]+:\r?\n|\z)'
    $match = [regex]::Match($Text, $pattern)
    Assert-True `
        -Condition $match.Success `
        -Message "Workflow must define job '$JobId'."
    return $match.Groups['body'].Value
}

function Get-StepBody {
    param(
        [Parameter(Mandatory = $true)] [string]$JobBody,
        [Parameter(Mandatory = $true)] [string]$StepName
    )

    $pattern =
        '(?ms)^      - name: ' + [regex]::Escape($StepName) +
        '\r?\n(?<body>.*?)(?=^      - name:|\z)'
    $match = [regex]::Match($JobBody, $pattern)
    Assert-True `
        -Condition $match.Success `
        -Message "Workflow job must define step '$StepName'."
    return $match.Groups['body'].Value
}

function Assert-FailClosedJob {
    param(
        [Parameter(Mandatory = $true)] [string]$JobBody,
        [Parameter(Mandatory = $true)] [string[]]$AllowedAlwaysStepBodies,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $yamlIfKey = '(?:if|''if''|"if")'
    $yamlContinueKey =
        '(?:continue-on-error|''continue-on-error''|"continue-on-error")'
    Assert-True `
        -Condition (
            $JobBody -notmatch "(?m)^    $yamlIfKey\s*:" -and
            $JobBody -notmatch "(?m)^    $yamlContinueKey\s*:"
        ) `
        -Message "$Description must not be conditional or continue on error."
    Assert-True `
        -Condition ($JobBody -notmatch "(?m)^        $yamlContinueKey\s*:") `
        -Message "$Description steps must fail closed."

    $stepIfKeys = @(
        [regex]::Matches($JobBody, "(?m)^        $yamlIfKey\s*:")
    )
    Assert-True `
        -Condition ($stepIfKeys.Count -eq $AllowedAlwaysStepBodies.Count) `
        -Message (
            "$Description must keep only its explicit always-run evidence steps conditional."
        )
    foreach ($stepBody in $AllowedAlwaysStepBodies) {
        Assert-True `
            -Condition $stepBody.Contains('        if: ${{ always() }}') `
            -Message (
                "$Description evidence steps must use only the explicit always() condition."
            )
    }
}

function Assert-ContainsInOrder {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string[]]$Needles,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $offset = 0
    foreach ($needle in $Needles) {
        $position = $Text.IndexOf(
            $needle,
            $offset,
            [StringComparison]::Ordinal)
        Assert-True `
            -Condition ($position -ge 0) `
            -Message (
                "$Description must contain '$needle' after the preceding step."
            )
        $offset = $position + $needle.Length
    }
}

$workflowText = Get-Content -Raw -LiteralPath (
    (Resolve-Path -LiteralPath $Workflow).Path)
$automationWorkflowText = Get-Content -Raw -LiteralPath (
    (Resolve-Path -LiteralPath $AutomationWorkflow).Path)
$presets = Get-Content -Raw -LiteralPath (
    (Resolve-Path -LiteralPath $PresetsFile).Path) | ConvertFrom-Json
$rootCMakeText = Get-Content -Raw -LiteralPath (
    (Resolve-Path -LiteralPath $RootCMakeLists).Path)
$asdfCMakeText = Get-Content -Raw -LiteralPath (
    (Resolve-Path -LiteralPath $AsdfCMakeLists).Path)

Assert-True `
    -Condition ($workflowText.Contains('name: SpecForge repository verification')) `
    -Message 'Repository verification workflow must keep its explicit owner name.'
Assert-ManualOnlyTrigger `
    -Text $workflowText `
    -Description 'Repository verification workflow'
Assert-ManualOnlyTrigger `
    -Text $automationWorkflowText `
    -Description 'Automation workflow'

$jobsMatch = [regex]::Match(
    $workflowText,
    '(?ms)^jobs:\r?\n(?<body>.*)\z')
Assert-True `
    -Condition $jobsMatch.Success `
    -Message 'Repository verification workflow must define jobs.'
$jobIds = @(
    [regex]::Matches(
        $jobsMatch.Groups['body'].Value,
        '(?m)^  (?<id>[A-Za-z0-9_-]+):\r?$') |
        ForEach-Object { $_.Groups['id'].Value }
)
Assert-True `
    -Condition (
        $jobIds.Count -eq 2 -and
        $jobIds -ccontains 'debug-and-static-release' -and
        $jobIds -ccontains 'asdf-specialized'
    ) `
    -Message 'Repository verification must have exactly core and ASDF owners.'

$coreBody = Get-JobBody `
    -Text $workflowText `
    -JobId 'debug-and-static-release'
Assert-True `
    -Condition (
        $coreBody.Contains('runs-on: windows-latest') -and
        $coreBody.Contains('timeout-minutes: 90') -and
        $coreBody.Contains('VERIFICATION_RUN_ROOT:') -and
        -not $coreBody.Contains('scripts\run-automation-ci.ps1') -and
        -not $coreBody.Contains("-L '^ci-headless$'")
    ) `
    -Message 'Core verification must be hosted, bounded, and repository-owned.'
Assert-ContainsInOrder `
    -Text $coreBody `
    -Needles @(
        '- name: Check out repository',
        '- name: Prepare run-scoped verification root',
        '- name: Select runner vcpkg and MSVC toolchain',
        '- name: Configure Ninja/MSVC Debug',
        '- name: Build Debug repository targets',
        '- name: Build ASDF production benchmark target',
        '- name: Run Debug fast and extended headless CTest presets',
        '- name: Configure Ninja/MSVC static Release',
        '- name: Build static Release repository targets',
        '- name: Run static Release CTest verification',
        '- name: Upload repository verification logs'
    ) `
    -Description 'Core repository verification steps'

$coreToolchain = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Select runner vcpkg and MSVC toolchain'
$debugConfigure = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Configure Ninja/MSVC Debug'
$debugBuild = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Build Debug repository targets'
$benchmarkBuild = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Build ASDF production benchmark target'
$debugTests = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Run Debug fast and extended headless CTest presets'
$releaseConfigure = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Configure Ninja/MSVC static Release'
$releaseBuild = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Build static Release repository targets'
$releaseTests = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Run static Release CTest verification'
$coreArtifacts = Get-StepBody `
    -JobBody $coreBody `
    -StepName 'Upload repository verification logs'
Assert-FailClosedJob `
    -JobBody $coreBody `
    -AllowedAlwaysStepBodies @($coreArtifacts) `
    -Description 'Core repository verification job'

Assert-True `
    -Condition (
        $coreToolchain.Contains('VCPKG_ROOT=') -and
        $coreToolchain.Contains('VCPKG_DEFAULT_BINARY_CACHE=') -and
        $coreToolchain.Contains('SPECFORGE_VCVARS_PATH=')
    ) `
    -Message 'Core verification must resolve hosted vcpkg and MSVC explicitly.'
Assert-True `
    -Condition (
        $debugConfigure.Contains('scripts\build-ninja-msvc-debug.ps1') -and
        $debugConfigure.Contains('-Configure') -and
        $debugConfigure.Contains('-LogDir "$env:VERIFICATION_RUN_ROOT\debug-build"') -and
        $debugConfigure.Contains('-TimeoutSec 600') -and
        $debugConfigure.Contains('Debug configure failed with exit code')
    ) `
    -Message 'Debug configure must use the bounded repository wrapper.'
Assert-True `
    -Condition (
        $debugBuild.Contains('scripts\build-ninja-msvc-debug.ps1') -and
        $debugBuild.Contains('-Target all') -and
        $debugBuild.Contains('-TimeoutSec 1200') -and
        $debugBuild.Contains('Debug all-target build failed with exit code')
    ) `
    -Message 'Debug repository verification must build the complete graph.'
Assert-True `
    -Condition (
        $benchmarkBuild.Contains('scripts\build-ninja-msvc-debug.ps1') -and
        $benchmarkBuild.Contains(
            '-Target specforge_asdf_labeling_production_benchmark') -and
        $benchmarkBuild.Contains('-TimeoutSec 600') -and
        $benchmarkBuild.Contains('ASDF production benchmark build failed')
    ) `
    -Message 'Repository verification must own the excluded ASDF benchmark.'
Assert-True `
    -Condition (
        $debugTests.Contains('ctest --preset fast') -and
        $debugTests.Contains('ctest --preset extended') -and
        $debugTests.Contains('SPECFORGE_ASDF_MUTATION_ARTIFACT_DIRECTORY') -and
        $debugTests.Contains('asdf-mutation-failure') -and
        $debugTests.Contains('fast.log') -and
        $debugTests.Contains('extended.log') -and
        $debugTests.Contains('Debug fast CTest preset failed') -and
        $debugTests.Contains('Debug extended CTest preset failed')
    ) `
    -Message 'Debug verification must own both developer headless tiers.'
Assert-True `
    -Condition (
        $releaseConfigure.Contains('scripts\build-ninja-msvc-debug.ps1') -and
        $releaseConfigure.Contains('-Configure') -and
        $releaseConfigure.Contains('-Preset ninja-msvc-release-static') -and
        $releaseConfigure.Contains('-TimeoutSec 600') -and
        $releaseConfigure.Contains('Static Release configure failed')
    ) `
    -Message 'Static Release configure must use the bounded repository wrapper.'
Assert-True `
    -Condition (
        $releaseBuild.Contains('scripts\build-ninja-msvc-debug.ps1') -and
        $releaseBuild.Contains('-Preset ninja-msvc-release-static') -and
        $releaseBuild.Contains('-Target all') -and
        $releaseBuild.Contains('-TimeoutSec 1500') -and
        $releaseBuild.Contains('Static Release all-target build failed')
    ) `
    -Message 'Static Release verification must build the complete graph.'
Assert-True `
    -Condition (
        $releaseTests.Contains('build\ninja-msvc-release-static') -and
        $releaseTests.Contains("-L '^release$'") -and
        $releaseTests.Contains('--no-tests=error') -and
        [regex]::IsMatch(
            $releaseTests,
            '(?m)^\s*--timeout\s+600\s+2>&1\s*$') -and
        [regex]::IsMatch(
            $releaseTests,
            '(?m)^\s*timeout-minutes:\s*15\s*$') -and
        $releaseTests.Contains('release.log') -and
        $releaseTests.Contains('Static Release CTest verification failed')
    ) `
    -Message 'Static Release must run only its explicit release-owned CTests with exact nested and step deadlines.'
Assert-True `
    -Condition (
        $coreArtifacts.Contains('always()') -and
        $coreArtifacts.Contains('actions/upload-artifact@v4') -and
        $coreArtifacts.Contains('if-no-files-found: warn') -and
        $coreArtifacts.Contains(
            'ci-artifacts/${{ github.run_id }}-${{ github.run_attempt }}/repository-core/**')
    ) `
    -Message 'Core verification must retain only its run-scoped evidence.'

$asdfBody = Get-JobBody -Text $workflowText -JobId 'asdf-specialized'
Assert-True `
    -Condition (
        $asdfBody.Contains('runs-on: windows-latest') -and
        $asdfBody.Contains('timeout-minutes: 35') -and
        $asdfBody.Contains('VERIFICATION_RUN_ROOT:') -and
        -not $asdfBody.Contains('scripts\run-automation-ci.ps1') -and
        -not $asdfBody.Contains("-L '^ci-headless$'")
    ) `
    -Message 'ASDF verification must be a separate hosted specialized owner.'
Assert-ContainsInOrder `
    -Text $asdfBody `
    -Needles @(
        '- name: Check out repository',
        '- name: Prepare run-scoped verification root',
        '- name: Select runner vcpkg and MSVC toolchain',
        '- name: Set up pinned ASDF oracle Python',
        '- name: Install pinned ASDF oracle',
        '- name: Configure and build ASDF oracle targets',
        '- name: Run pinned ASDF oracle CTests',
        '- name: Collect final ASDF oracle evidence',
        '- name: Upload ASDF oracle logs and evidence'
    ) `
    -Description 'ASDF specialized verification steps'

$asdfToolchain = Get-StepBody `
    -JobBody $asdfBody `
    -StepName 'Select runner vcpkg and MSVC toolchain'
$asdfPython = Get-StepBody `
    -JobBody $asdfBody `
    -StepName 'Set up pinned ASDF oracle Python'
$asdfInstall = Get-StepBody `
    -JobBody $asdfBody `
    -StepName 'Install pinned ASDF oracle'
$asdfBuild = Get-StepBody `
    -JobBody $asdfBody `
    -StepName 'Configure and build ASDF oracle targets'
$asdfTests = Get-StepBody `
    -JobBody $asdfBody `
    -StepName 'Run pinned ASDF oracle CTests'
$asdfEvidence = Get-StepBody `
    -JobBody $asdfBody `
    -StepName 'Collect final ASDF oracle evidence'
$asdfArtifacts = Get-StepBody `
    -JobBody $asdfBody `
    -StepName 'Upload ASDF oracle logs and evidence'
Assert-FailClosedJob `
    -JobBody $asdfBody `
    -AllowedAlwaysStepBodies @($asdfEvidence, $asdfArtifacts) `
    -Description 'ASDF specialized verification job'

Assert-True `
    -Condition (
        $asdfToolchain.Contains('VCPKG_ROOT=') -and
        $asdfToolchain.Contains('SPECFORGE_VCVARS_PATH=') -and
        $asdfPython.Contains('actions/setup-python@v6') -and
        $asdfPython.Contains("python-version: '3.12'") -and
        $asdfInstall.Contains('tools\asdf_labeling_spike\requirements.txt')
    ) `
    -Message 'ASDF owner must provision its pinned hosted oracle environment.'
Assert-True `
    -Condition (
        $asdfBuild.Contains('scripts\build-ninja-msvc-debug.ps1') -and
        $asdfBuild.Contains('-Configure') -and
        $asdfBuild.Contains('-Preset ninja-msvc-debug-asdf-labeling-spike') -and
        $asdfBuild.Contains(
            '-Target specforge_asdf_labeling_interoperability_targets') -and
        $asdfBuild.Contains('ASDF oracle configure failed') -and
        $asdfBuild.Contains('ASDF oracle target build failed')
    ) `
    -Message 'ASDF owner must configure and build the dedicated target closure.'
Assert-True `
    -Condition (
        $asdfTests.Contains('build\ninja-msvc-debug-asdf-labeling-spike') -and
        $asdfTests.Contains("-L '^asdf-pinned-oracle$'") -and
        $asdfTests.Contains('--no-tests=error') -and
        $asdfTests.Contains('--timeout 120') -and
        $asdfTests.Contains('asdf-pinned-oracle.log') -and
        $asdfTests.Contains('Pinned ASDF oracle CTests failed')
    ) `
    -Message 'ASDF owner must run only the pinned-oracle label with evidence.'
Assert-True `
    -Condition (
        $asdfEvidence.Contains('interoperability.json') -and
        $asdfEvidence.Contains('checksum-interoperability.json') -and
        $asdfArtifacts.Contains('always()') -and
        $asdfArtifacts.Contains('actions/upload-artifact@v4') -and
        $asdfArtifacts.Contains('if-no-files-found: warn') -and
        $asdfArtifacts.Contains(
            'ci-artifacts/${{ github.run_id }}-${{ github.run_attempt }}/asdf-specialized/**')
    ) `
    -Message 'ASDF owner must retain its current-run logs and oracle evidence.'

foreach ($presetName in @('fast', 'extended')) {
    Assert-True `
        -Condition (
            @($presets.testPresets | Where-Object name -CEQ $presetName).Count -eq 1
        ) `
        -Message "Repository verification requires the '$presetName' CTest preset."
}
Assert-True `
    -Condition (
        $rootCMakeText -match
            '(?ms)add_dependencies\s*\(\s*' +
            'specforge_asdf_labeling_interoperability_targets\s+' +
            'specforge_sample_labeling_asdf_codec_tests\s*\)'
    ) `
    -Message (
        'ASDF interoperability target closure must include the production codec tests.'
    )
Assert-True `
    -Condition (
        [regex]::Matches(
            $asdfCMakeText,
            'LABELS "[^"]*asdf-pinned-oracle[^"]*"').Count -eq 3
    ) `
    -Message 'ASDF specialized owner requires three pinned-oracle CTests.'
$expectedAsdfOracleTests = @(
    'specforge_asdf_labeling_spike_native_fixture_test',
    'specforge_asdf_labeling_interoperability',
    'specforge_sample_labeling_asdf_checksum_interoperability'
)
foreach ($testName in $expectedAsdfOracleTests) {
    $propertyMatch = [regex]::Match(
        $asdfCMakeText,
        '(?ms)set_tests_properties\s*\(\s*' +
        [regex]::Escape($testName) +
        '\s+PROPERTIES(?<properties>.*?)\)')
    Assert-True `
        -Condition (
            $propertyMatch.Success -and
            $propertyMatch.Groups['properties'].Value -match
                'LABELS "[^"]*asdf-pinned-oracle[^"]*"'
        ) `
        -Message (
            "ASDF specialized owner must bind pinned-oracle to $testName."
        )
}

Write-Host 'Repository verification CI structural contract passed.'
