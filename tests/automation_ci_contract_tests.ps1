[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Workflow,
    [Parameter(Mandatory = $true)]
    [string]$CMakeLists,
    [Parameter(Mandatory = $true)]
    [string]$RunnerScript
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-JobBody {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$JobId
    )

    $pattern = '(?ms)^  ' + [regex]::Escape($JobId) + ':\r?\n(?<body>.*?)(?=^  [A-Za-z0-9_-]+:\r?\n|\z)'
    $match = [regex]::Match($Text, $pattern)
    Assert-True -Condition $match.Success -Message "Workflow must define job '$JobId'."
    return $match.Groups['body'].Value
}

function Get-StepBody {
    param(
        [Parameter(Mandatory = $true)]
        [string]$JobBody,
        [Parameter(Mandatory = $true)]
        [string]$StepName
    )

    $pattern = '(?ms)^      - name: ' + [regex]::Escape($StepName) + '\r?\n(?<body>.*?)(?=^      - name:|\z)'
    $match = [regex]::Match($JobBody, $pattern)
    Assert-True -Condition $match.Success -Message "Workflow job must define step '$StepName'."
    return $match.Groups['body'].Value
}

function Assert-ContainsInOrder {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string[]]$Needles,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $offset = 0
    foreach ($needle in $Needles) {
        $position = $Text.IndexOf($needle, $offset, [System.StringComparison]::Ordinal)
        Assert-True -Condition ($position -ge 0) -Message "$Description must contain '$needle' after the preceding step."
        $offset = $position + $needle.Length
    }
}

function Assert-CMakeTargetClosure {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Target,
        [Parameter(Mandatory = $true)] [string[]]$Dependencies
    )

    Assert-True `
        -Condition ($Text -match (
            '(?ms)add_custom_target\s*\(\s*' +
            [regex]::Escape($Target) + '\s*\)')) `
        -Message "CMake must define the explicit target '$Target'."
    $dependencyMatches = @(
        [regex]::Matches(
            $Text,
            '(?ms)add_dependencies\s*\(\s*' +
                [regex]::Escape($Target) +
                '\s+(?<dependencies>.*?)\)')
    )
    Assert-True `
        -Condition ($dependencyMatches.Count -eq 1) `
        -Message "CMake must define one dependency closure for '$Target'."
    $actualDependencies = @(
        $dependencyMatches[0].Groups['dependencies'].Value -split '\s+' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    Assert-True `
        -Condition (
            $actualDependencies.Count -eq $Dependencies.Count -and
            @($Dependencies | Where-Object {
                $actualDependencies -cnotcontains $_
            }).Count -eq 0
        ) `
        -Message (
            "CMake target '$Target' must retain only its automation-owned closure."
        )
}

function Get-CMakeTestLabelMap {
    param([Parameter(Mandatory = $true)] [string]$Text)

    $operations = [System.Collections.Generic.List[object]]::new()
    $commands = [regex]::Matches(
        $Text,
        '(?ms)(?<command>set_tests_properties|set_property)\s*\((?<body>.*?)\)')
    foreach ($commandMatch in $commands) {
        $tokens = @(
            [regex]::Matches(
                $commandMatch.Groups['body'].Value,
                '"[^"]*"|[^\s]+') |
                ForEach-Object { $_.Value.Trim('"') }
        )
        if ($tokens.Count -eq 0) {
            continue
        }

        if ($commandMatch.Groups['command'].Value -ceq
            'set_tests_properties') {
            $propertiesIndex = -1
            for ($index = 0; $index -lt $tokens.Count; ++$index) {
                if ($tokens[$index] -ceq 'PROPERTIES') {
                    $propertiesIndex = $index
                    break
                }
            }
            if ($propertiesIndex -le 0) {
                continue
            }
            $labelsIndex = -1
            for ($index = $propertiesIndex + 1;
                $index -lt $tokens.Count;
                $index += 2) {
                if ($tokens[$index] -ceq 'LABELS') {
                    $labelsIndex = $index
                    break
                }
            }
            if ($labelsIndex -lt 0 -or $labelsIndex + 1 -ge $tokens.Count) {
                continue
            }
            [void]$operations.Add([pscustomobject]@{
                Index = $commandMatch.Index
                TestNames = @($tokens[0..($propertiesIndex - 1)])
                Append = $false
                Labels = @($tokens[$labelsIndex + 1] -split ';' |
                    Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
            })
            continue
        }

        if ($tokens[0] -cne 'TEST') {
            continue
        }
        $propertyIndex = -1
        for ($index = 1; $index -lt $tokens.Count; ++$index) {
            if ($tokens[$index] -ceq 'PROPERTY') {
                $propertyIndex = $index
                break
            }
        }
        if ($propertyIndex -le 1 -or
            $propertyIndex + 1 -ge $tokens.Count -or
            $tokens[$propertyIndex + 1] -cne 'LABELS') {
            continue
        }
        $testNames = @(
            $tokens[1..($propertyIndex - 1)] |
                Where-Object {
                    $_ -cnotin @('APPEND', 'APPEND_STRING')
                }
        )
        $labelTokens = if ($propertyIndex + 2 -lt $tokens.Count) {
            @($tokens[($propertyIndex + 2)..($tokens.Count - 1)])
        }
        else {
            @()
        }
        [void]$operations.Add([pscustomobject]@{
            Index = $commandMatch.Index
            TestNames = $testNames
            Append = ($tokens -ccontains 'APPEND')
            Labels = @($labelTokens | ForEach-Object { $_ -split ';' } |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        })
    }

    $labelsByTest = @{}
    foreach ($operation in @($operations | Sort-Object Index)) {
        foreach ($testName in $operation.TestNames) {
            if (-not $labelsByTest.ContainsKey($testName)) {
                $labelsByTest[$testName] =
                    [System.Collections.Generic.HashSet[string]]::new(
                        [StringComparer]::Ordinal)
            }
            if (-not $operation.Append) {
                $labelsByTest[$testName].Clear()
            }
            foreach ($label in $operation.Labels) {
                [void]$labelsByTest[$testName].Add($label)
            }
        }
    }
    return $labelsByTest
}

$resolvedWorkflow = (Resolve-Path -LiteralPath $Workflow).Path
$resolvedCMakeLists = (Resolve-Path -LiteralPath $CMakeLists).Path
$resolvedRunnerScript = (Resolve-Path -LiteralPath $RunnerScript).Path
$workflowText = Get-Content -Raw -LiteralPath $resolvedWorkflow
$cmakeText = Get-Content -Raw -LiteralPath $resolvedCMakeLists
$runnerText = Get-Content -Raw -LiteralPath $resolvedRunnerScript

foreach ($requiredText in @(
        'workflow_dispatch:',
        'run_real_gui:',
        'type: boolean',
        'name: Native/headless automation gate',
        'name: Real-GUI runner preflight',
        'name: Real-GUI queue watchdog',
        'name: Real-GUI status diagnostic',
        'status.json')) {
    Assert-True -Condition $workflowText.Contains($requiredText) -Message "Workflow must contain '$requiredText'."
}

$triggerMatch = [regex]::Match(
    $workflowText,
    '(?ms)^on:\r?\n(?<body>.*?)(?=^[A-Za-z0-9_-]+:\r?\n|\z)')
Assert-True `
    -Condition $triggerMatch.Success `
    -Message 'Workflow must define active triggers.'
$triggerBody = $triggerMatch.Groups['body'].Value
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
    -Message 'Automation workflow must remain manual-only.'

$nativeBody = Get-JobBody -Text $workflowText -JobId 'native-headless'
Assert-True -Condition ($nativeBody.Contains('runs-on: windows-latest') -and $nativeBody.Contains('timeout-minutes: 90') -and $nativeBody -notmatch '(?m)^    if:' -and $nativeBody -notmatch '(?m)^    continue-on-error:') -Message 'Native/headless job must be an unconditional hosted Windows gate with a 90-minute total budget.'
Assert-ContainsInOrder -Text $nativeBody -Needles @('- name: Check out repository', '- name: Prepare run-scoped automation evidence root', '- name: Configure Ninja/MSVC Debug', '- name: Build native automation targets', '- name: Run required native/headless CTest gate', '- name: Upload headless logs and failure evidence') -Description 'Native/headless job steps'
$nativeCheckout = Get-StepBody -JobBody $nativeBody -StepName 'Check out repository'
$nativeConfigure = Get-StepBody -JobBody $nativeBody -StepName 'Configure Ninja/MSVC Debug'
$nativeBuild = Get-StepBody -JobBody $nativeBody -StepName 'Build native automation targets'
$nativeGate = Get-StepBody -JobBody $nativeBody -StepName 'Run required native/headless CTest gate'
$nativeArtifacts = Get-StepBody -JobBody $nativeBody -StepName 'Upload headless logs and failure evidence'
Assert-True -Condition $nativeCheckout.Contains('uses: actions/checkout@v4') -Message 'Native/headless job must check out its source before building.'
Assert-True -Condition ($nativeConfigure.Contains('scripts\build-ninja-msvc-debug.ps1') -and $nativeConfigure.Contains('-Configure') -and $nativeConfigure.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $nativeConfigure.Contains('-TimeoutSec 600') -and $nativeConfigure.Contains('timeout-minutes: 10') -and $nativeConfigure.Contains('Debug configure failed with exit code')) -Message 'Native/headless Debug configure step must use the bounded repository wrapper, run-scoped logs, and immediate failure propagation.'
Assert-True -Condition ($nativeBuild.Contains('scripts\build-ninja-msvc-debug.ps1') -and $nativeBuild.Contains('-Target specforge_automation_headless_targets') -and $nativeBuild.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $nativeBuild.Contains('-TimeoutSec 1200') -and $nativeBuild.Contains('timeout-minutes: 20') -and $nativeBuild.Contains('Debug automation target build failed with exit code') -and -not $nativeBuild.Contains('-Target all')) -Message 'Native/headless Debug build step must build only the automation-owned target closure and propagate failure immediately.'
Assert-True -Condition ($nativeGate.Contains('scripts\run-automation-ci.ps1') -and $nativeGate.Contains('-Mode Headless') -and $nativeGate.Contains('-BuildDirectory build\ninja-msvc-debug') -and $nativeGate.Contains('-ArtifactsDirectory "$env:AUTOMATION_RUN_ROOT\ctest"') -and $nativeGate.Contains('-SuiteTimeoutSec 300') -and $nativeGate.Contains('-TestTimeoutSec 120') -and $nativeGate.Contains('timeout-minutes: 10') -and $nativeGate.Contains('Debug native/headless CTest gate failed with exit code') -and -not $nativeGate.Contains('SPECFORGE_ASDF')) -Message 'Native/headless Debug gate must select only automation-headless coverage with bounded execution and immediate failure propagation.'
Assert-True -Condition ($nativeArtifacts.Contains('always()') -and $nativeArtifacts.Contains('ci-artifacts/${{ github.run_id }}-${{ github.run_attempt }}/native-headless/**') -and -not $nativeArtifacts.Contains('Testing/Temporary') -and -not $nativeArtifacts.Contains('logs/build')) -Message 'Native/headless failure artifacts must upload only the current workflow run root.'
$nativePrepare = Get-StepBody -JobBody $nativeBody -StepName 'Prepare run-scoped automation evidence root'
Assert-True -Condition ($nativePrepare.Contains('AUTOMATION_RUN_ROOT') -and $nativePrepare.Contains('New-Item')) -Message 'Native/headless must create a dedicated run-scoped evidence root before build/test steps.'
foreach ($removedOwner in @('-Target all', 'ninja-msvc-release-static', 'asdf-pinned-oracle', 'actions/setup-python', 'specforge_asdf_labeling')) {
    Assert-True -Condition (-not $workflowText.Contains($removedOwner)) -Message "Automation workflow must not retain repository-wide owner '$removedOwner'."
}

$preflightBody = Get-JobBody -Text $workflowText -JobId 'real-gui-preflight'
Assert-True -Condition ($preflightBody.Contains('needs: native-headless') -and $preflightBody.Contains('runs-on: windows-latest') -and $preflightBody.Contains('actions: read') -and $preflightBody.Contains('actions/runners') -and $preflightBody.Contains('runner_available') -and $preflightBody.Contains('outputs:') -and $preflightBody.Contains('steps.runner_preflight.outputs.runner_available') -and $preflightBody.Contains('self-hosted') -and $preflightBody.Contains('windows') -and $preflightBody.Contains('desktop') -and $preflightBody.Contains('preflight.json') -and $preflightBody.Contains("github.event_name == 'workflow_dispatch'") -and -not $preflightBody.Contains("github.event_name == 'push'")) -Message 'Real-GUI preflight must use only the protected manual-dispatch policy, verify an online idle runner, expose its decision, and retain its diagnostic.'
Assert-True -Condition (-not $preflightBody.Contains('actions/checkout@v4')) -Message 'Real-GUI preflight must not check out or execute repository code.'

$realGuiBody = Get-JobBody -Text $workflowText -JobId 'real-gui'
Assert-ContainsInOrder -Text $realGuiBody -Needles @('- name: Check out repository', '- name: Prepare run-scoped automation evidence root', '- name: Configure Ninja/MSVC Debug', '- name: Build real-GUI automation targets', '- name: Run real-GUI CTest group', '- name: Upload real-GUI logs and failure evidence') -Description 'Real-GUI job steps'
Assert-True -Condition ($realGuiBody.Contains('needs: [native-headless, real-gui-preflight]') -and $realGuiBody.Contains('runs-on: [self-hosted, windows, desktop]') -and $realGuiBody.Contains('environment:') -and $realGuiBody.Contains('name: real-gui') -and $realGuiBody.Contains("needs.real-gui-preflight.outputs.runner_available == 'true'") -and $realGuiBody.Contains("github.event_name == 'workflow_dispatch'") -and $realGuiBody.Contains('github.ref_protected == true') -and $realGuiBody.Contains('timeout-minutes: 75') -and -not $realGuiBody.Contains("github.event_name == 'push'") -and -not $realGuiBody.Contains('pull_request')) -Message 'Real-GUI execution must remain protected, manual-only, preflight-gated, separate from pull_request, and have a 75-minute total budget.'
$realGuiCheckout = Get-StepBody -JobBody $realGuiBody -StepName 'Check out repository'
$realGuiConfigure = Get-StepBody -JobBody $realGuiBody -StepName 'Configure Ninja/MSVC Debug'
$realGuiBuild = Get-StepBody -JobBody $realGuiBody -StepName 'Build real-GUI automation targets'
$realGuiGate = Get-StepBody -JobBody $realGuiBody -StepName 'Run real-GUI CTest group'
$realGuiArtifacts = Get-StepBody -JobBody $realGuiBody -StepName 'Upload real-GUI logs and failure evidence'
Assert-True -Condition ($realGuiCheckout.Contains('uses: actions/checkout@v4') -and $realGuiConfigure.Contains('-Configure') -and $realGuiConfigure.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $realGuiBuild.Contains('-Target specforge_automation_real_gui_targets') -and -not $realGuiBuild.Contains('-Target all') -and $realGuiBuild.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $realGuiGate.Contains('-Mode RealGui') -and $realGuiGate.Contains('-ArtifactsDirectory "$env:AUTOMATION_RUN_ROOT\ctest"') -and $realGuiGate.Contains('-SuiteTimeoutSec 900') -and $realGuiGate.Contains('-TestTimeoutSec 300') -and $realGuiArtifacts.Contains('ci-artifacts/${{ github.run_id }}-${{ github.run_attempt }}/real-gui/**') -and -not $realGuiArtifacts.Contains('Testing/Temporary') -and -not $realGuiArtifacts.Contains('logs/build')) -Message 'Real-GUI job must build only its automation closure and retain unique sample failure artifacts without historical logs.'
$realGuiPrepare = Get-StepBody -JobBody $realGuiBody -StepName 'Prepare run-scoped automation evidence root'
Assert-True -Condition ($realGuiPrepare.Contains('AUTOMATION_RUN_ROOT') -and $realGuiPrepare.Contains('New-Item')) -Message 'Real-GUI must create a dedicated run-scoped evidence root before build/test steps.'

$watchdogBody = Get-JobBody -Text $workflowText -JobId 'real-gui-watchdog'
Assert-True -Condition ($watchdogBody.Contains('needs: [native-headless, real-gui-preflight]') -and $watchdogBody.Contains('runs-on: windows-latest') -and $watchdogBody.Contains('actions: write') -and $watchdogBody.Contains('timeout-minutes: 15') -and $watchdogBody.Contains('outputs:') -and $watchdogBody.Contains('watchdog_status:') -and $watchdogBody.Contains('id: queue_watchdog') -and $watchdogBody.Contains('actions/runs') -and $watchdogBody.Contains('/cancel') -and $watchdogBody.Contains('$runCancelUri') -and -not $watchdogBody.Contains('actions/jobs/$($target.id)/cancel') -and -not [regex]::IsMatch($watchdogBody, 'actions/jobs/[^\r\n]*/cancel') -and $watchdogBody.Contains("github.event_name == 'workflow_dispatch'") -and -not $watchdogBody.Contains("github.event_name == 'push'") -and $watchdogBody.Contains('github.ref_protected == true') -and $watchdogBody.Contains('target_not_found') -and $watchdogBody.Contains('api_error') -and $watchdogBody.Contains('cancel_failed') -and $watchdogBody.Contains('request_timeout_sec') -and $watchdogBody.Contains('$apiTimeoutSec = 8') -and $watchdogBody.Contains('-TimeoutSec $apiTimeoutSec') -and $watchdogBody.Contains('watchdog_status=') -and $watchdogBody.Contains('watchdog.json') -and $watchdogBody.Contains('throw "Real-GUI queue watchdog failed')) -Message 'Real-GUI watchdog must keep the manual-only authorization policy, bound queued and environment-waiting jobs, retain diagnostics, expose status, and fail on API/target/cancel problems.'

$statusBody = Get-JobBody -Text $workflowText -JobId 'real-gui-status-diagnostic'
Assert-True -Condition ($statusBody.Contains('always()') -and $statusBody.Contains('needs: [native-headless, real-gui-preflight, real-gui-watchdog, real-gui]') -and $statusBody.Contains("github.event_name == 'workflow_dispatch'") -and -not $statusBody.Contains("github.event_name == 'push'") -and -not $statusBody.Contains("EVENT_NAME -eq 'push'") -and -not $statusBody.Contains('protected master push') -and $statusBody.Contains('workflow_dispatch on protected master with run_real_gui=true') -and $statusBody.Contains('github.ref_protected == true') -and $statusBody.Contains('REF_PROTECTED: ${{ github.ref_protected }}') -and $statusBody.Contains('unprotected_ref') -and $statusBody.Contains('status:') -and $statusBody.Contains('runner_unavailable') -and $statusBody.Contains('approval_timeout') -and $statusBody.Contains('watchdog_failure') -and $statusBody.Contains('cancel_failed') -and $statusBody.Contains('failed') -and $statusBody.Contains('preflight_result:') -and $statusBody.Contains('watchdog_result:') -and $statusBody.Contains('real_gui_job_result:') -and $statusBody.Contains('authorization_policy:') -and $statusBody.Contains("exit 1") -and $statusBody.Contains('real-gui-status/status.json')) -Message 'Real-GUI unavailable, skipped, unprotected, approval-waiting, cancellation/watchdog, and actual failure paths must retain a manual-only authorization diagnostic without masking execution failures.'

Assert-True -Condition ($runnerText.Contains('if ($Mode -eq ''Headless'') { ''automation-headless'' } else { ''real-gui'' }') -and -not $runnerText.Contains('''ci-headless''') -and $runnerText.Contains('Invoke-BoundedCTest') -and $runnerText.Contains('WaitForExit') -and $runnerText.Contains('suite_timeout_sec') -and $runnerText.Contains('test_timeout_sec') -and $runnerText.Contains('ctest_start_ticks') -and $runnerText.Contains('ProcessHandle') -and $runnerText.Contains('New-AutomationKillOnCloseJob') -and $runnerText.Contains('Assign-AutomationProcessToJob') -and $runnerText.Contains('Stop-AutomationProcessHandle') -and -not $runnerText.Contains('taskkill.exe') -and -not $runnerText.Contains('Stop-Process -Id')) -Message 'The CI runner must select the automation-specific labels, capture CTest start identity, and bound process cleanup through the shared handle/Job seam.'
Assert-True -Condition ($runnerText.Contains('SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS') -and $runnerText.Contains('automation-samples') -and $runnerText.Contains('ctest-temporary') -and $runnerText.Contains('current_ctest_run_started_utc') -and $runnerText.Contains('historical Testing/Temporary files are excluded') -and $runnerText.Contains('failed with exit code')) -Message 'The CI runner must route sample artifacts and actionable CTest evidence into the current run-scoped artifact root and make nonzero CTest summaries diagnostic.'
Assert-CMakeTargetClosure -Text $cmakeText -Target 'specforge_automation_headless_targets' -Dependencies @('specforge_automation_control_tests', 'specforge_automation_panel_command_coordinator_tests', 'specforge_automation_state_isolation_tests')
Assert-CMakeTargetClosure -Text $cmakeText -Target 'specforge_automation_real_gui_targets' -Dependencies @('specforge_automation_launcher', 'specforge_native', 'specforge_automation_control_tests', 'specforge_automation_launcher_timeout_fixture', 'specforge_automation_state_isolation_tests')
$expectedHeadlessTests = @('specforge_automation_control_tests', 'specforge_automation_panel_command_coordinator_tests', 'specforge_automation_state_isolation_tests', 'specforge_automation_samples_contract_tests', 'specforge_automation_ci_contract_tests')
$labelsByTest = Get-CMakeTestLabelMap -Text $cmakeText
$selectedHeadlessTests = @(
    $labelsByTest.GetEnumerator() |
        Where-Object { $_.Value.Contains('automation-headless') } |
        ForEach-Object { [string]$_.Key } |
        Sort-Object
)
Assert-True -Condition ($selectedHeadlessTests.Count -eq $expectedHeadlessTests.Count -and @($expectedHeadlessTests | Where-Object { $selectedHeadlessTests -cnotcontains $_ }).Count -eq 0) -Message 'The effective automation-headless selector must contain exactly the five automation-owned tests.'
foreach ($testName in $expectedHeadlessTests) {
    Assert-True -Condition ($labelsByTest.ContainsKey($testName) -and @('automation', 'automation-headless', 'ci-headless', 'required' | Where-Object { -not $labelsByTest[$testName].Contains($_) }).Count -eq 0) -Message "CTest '$testName' must retain its automation and repository-wide label tokens."
}
Assert-True -Condition ([regex]::Matches($cmakeText, 'LABELS "automation;real-gui"').Count -ge 3) -Message 'CTest registrations must retain the real-gui labels selected by the CI runner.'

Write-Host 'Automation CI structural contract passed.'
