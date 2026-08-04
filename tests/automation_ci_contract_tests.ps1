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

$resolvedWorkflow = (Resolve-Path -LiteralPath $Workflow).Path
$resolvedCMakeLists = (Resolve-Path -LiteralPath $CMakeLists).Path
$resolvedRunnerScript = (Resolve-Path -LiteralPath $RunnerScript).Path
$workflowText = Get-Content -Raw -LiteralPath $resolvedWorkflow
$cmakeText = Get-Content -Raw -LiteralPath $resolvedCMakeLists
$runnerText = Get-Content -Raw -LiteralPath $resolvedRunnerScript

foreach ($requiredText in @(
        'pull_request:',
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

$nativeBody = Get-JobBody -Text $workflowText -JobId 'native-headless'
Assert-True -Condition ($nativeBody.Contains('runs-on: windows-latest') -and $nativeBody.Contains('timeout-minutes: 60')) -Message 'Native/headless job must use the hosted Windows runner and a 60-minute total budget.'
Assert-ContainsInOrder -Text $nativeBody -Needles @('- name: Check out repository', '- name: Prepare run-scoped automation evidence root', '- name: Configure Ninja/MSVC Debug', '- name: Build native automation targets', '- name: Run required native/headless CTest gate', '- name: Upload headless logs and failure evidence') -Description 'Native/headless job steps'
$nativeCheckout = Get-StepBody -JobBody $nativeBody -StepName 'Check out repository'
$nativeConfigure = Get-StepBody -JobBody $nativeBody -StepName 'Configure Ninja/MSVC Debug'
$nativeBuild = Get-StepBody -JobBody $nativeBody -StepName 'Build native automation targets'
$nativeGate = Get-StepBody -JobBody $nativeBody -StepName 'Run required native/headless CTest gate'
$nativeArtifacts = Get-StepBody -JobBody $nativeBody -StepName 'Upload headless logs and failure evidence'
Assert-True -Condition $nativeCheckout.Contains('uses: actions/checkout@v4') -Message 'Native/headless job must check out its source before building.'
Assert-True -Condition ($nativeConfigure.Contains('scripts\build-ninja-msvc-debug.ps1') -and $nativeConfigure.Contains('-Configure') -and $nativeConfigure.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $nativeConfigure.Contains('-TimeoutSec 600') -and $nativeConfigure.Contains('timeout-minutes: 10')) -Message 'Native/headless configure step must use the bounded repository wrapper and run-scoped logs.'
Assert-True -Condition ($nativeBuild.Contains('scripts\build-ninja-msvc-debug.ps1') -and $nativeBuild.Contains('-Target all') -and $nativeBuild.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $nativeBuild.Contains('-TimeoutSec 1200') -and $nativeBuild.Contains('timeout-minutes: 20')) -Message 'Native/headless build step must use the bounded repository wrapper and run-scoped logs.'
Assert-True -Condition ($nativeGate.Contains('scripts\run-automation-ci.ps1') -and $nativeGate.Contains('-Mode Headless') -and $nativeGate.Contains('-ArtifactsDirectory "$env:AUTOMATION_RUN_ROOT\ctest"') -and $nativeGate.Contains('-SuiteTimeoutSec 300') -and $nativeGate.Contains('-TestTimeoutSec 120') -and $nativeGate.Contains('timeout-minutes: 10')) -Message 'Native/headless gate must select the ci-headless label with both suite and per-test budgets in a run-scoped artifact root.'
Assert-True -Condition ($nativeArtifacts.Contains('always()') -and $nativeArtifacts.Contains('ci-artifacts/${{ github.run_id }}-${{ github.run_attempt }}/native-headless/**') -and -not $nativeArtifacts.Contains('Testing/Temporary') -and -not $nativeArtifacts.Contains('logs/build')) -Message 'Native/headless failure artifacts must upload only the current workflow run root.'
$nativePrepare = Get-StepBody -JobBody $nativeBody -StepName 'Prepare run-scoped automation evidence root'
Assert-True -Condition ($nativePrepare.Contains('AUTOMATION_RUN_ROOT') -and $nativePrepare.Contains('New-Item')) -Message 'Native/headless must create a dedicated run-scoped evidence root before build/test steps.'

$preflightBody = Get-JobBody -Text $workflowText -JobId 'real-gui-preflight'
Assert-True -Condition ($preflightBody.Contains('needs: native-headless') -and $preflightBody.Contains('runs-on: windows-latest') -and $preflightBody.Contains('actions: read') -and $preflightBody.Contains('actions/runners') -and $preflightBody.Contains('runner_available') -and $preflightBody.Contains('outputs:') -and $preflightBody.Contains('steps.runner_preflight.outputs.runner_available') -and $preflightBody.Contains('self-hosted') -and $preflightBody.Contains('windows') -and $preflightBody.Contains('desktop') -and $preflightBody.Contains('preflight.json')) -Message 'Real-GUI preflight must verify an online idle runner, expose its decision to dependent jobs, and retain its diagnostic.'
Assert-True -Condition (-not $preflightBody.Contains('actions/checkout@v4')) -Message 'Real-GUI preflight must not check out or execute repository code.'

$realGuiBody = Get-JobBody -Text $workflowText -JobId 'real-gui'
Assert-ContainsInOrder -Text $realGuiBody -Needles @('- name: Check out repository', '- name: Prepare run-scoped automation evidence root', '- name: Configure Ninja/MSVC Debug', '- name: Build real-GUI automation targets', '- name: Run real-GUI CTest group', '- name: Upload real-GUI logs and failure evidence') -Description 'Real-GUI job steps'
Assert-True -Condition ($realGuiBody.Contains('needs: [native-headless, real-gui-preflight]') -and $realGuiBody.Contains('runs-on: [self-hosted, windows, desktop]') -and $realGuiBody.Contains('environment:') -and $realGuiBody.Contains('name: real-gui') -and $realGuiBody.Contains("needs.real-gui-preflight.outputs.runner_available == 'true'") -and $realGuiBody.Contains('github.ref_protected == true') -and $realGuiBody.Contains('timeout-minutes: 75') -and -not $realGuiBody.Contains('pull_request')) -Message 'Real-GUI execution must remain protected, preflight-gated, separate from pull_request, and have a 75-minute total budget.'
$realGuiCheckout = Get-StepBody -JobBody $realGuiBody -StepName 'Check out repository'
$realGuiConfigure = Get-StepBody -JobBody $realGuiBody -StepName 'Configure Ninja/MSVC Debug'
$realGuiBuild = Get-StepBody -JobBody $realGuiBody -StepName 'Build real-GUI automation targets'
$realGuiGate = Get-StepBody -JobBody $realGuiBody -StepName 'Run real-GUI CTest group'
$realGuiArtifacts = Get-StepBody -JobBody $realGuiBody -StepName 'Upload real-GUI logs and failure evidence'
Assert-True -Condition ($realGuiCheckout.Contains('uses: actions/checkout@v4') -and $realGuiConfigure.Contains('-Configure') -and $realGuiConfigure.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $realGuiBuild.Contains('-Target all') -and $realGuiBuild.Contains('-LogDir "$env:AUTOMATION_RUN_ROOT\build"') -and $realGuiGate.Contains('-Mode RealGui') -and $realGuiGate.Contains('-ArtifactsDirectory "$env:AUTOMATION_RUN_ROOT\ctest"') -and $realGuiGate.Contains('-SuiteTimeoutSec 900') -and $realGuiGate.Contains('-TestTimeoutSec 300') -and $realGuiArtifacts.Contains('ci-artifacts/${{ github.run_id }}-${{ github.run_attempt }}/real-gui/**') -and -not $realGuiArtifacts.Contains('Testing/Temporary') -and -not $realGuiArtifacts.Contains('logs/build')) -Message 'Real-GUI job must retain checkout, build, test and unique sample failure artifact dependencies without historical logs.'
$realGuiPrepare = Get-StepBody -JobBody $realGuiBody -StepName 'Prepare run-scoped automation evidence root'
Assert-True -Condition ($realGuiPrepare.Contains('AUTOMATION_RUN_ROOT') -and $realGuiPrepare.Contains('New-Item')) -Message 'Real-GUI must create a dedicated run-scoped evidence root before build/test steps.'

$watchdogBody = Get-JobBody -Text $workflowText -JobId 'real-gui-watchdog'
Assert-True -Condition ($watchdogBody.Contains('needs: [native-headless, real-gui-preflight]') -and $watchdogBody.Contains('runs-on: windows-latest') -and $watchdogBody.Contains('actions: write') -and $watchdogBody.Contains('timeout-minutes: 15') -and $watchdogBody.Contains('outputs:') -and $watchdogBody.Contains('watchdog_status:') -and $watchdogBody.Contains('id: queue_watchdog') -and $watchdogBody.Contains('actions/runs') -and $watchdogBody.Contains('/cancel') -and $watchdogBody.Contains('$runCancelUri') -and -not $watchdogBody.Contains('actions/jobs/$($target.id)/cancel') -and -not [regex]::IsMatch($watchdogBody, 'actions/jobs/[^\r\n]*/cancel') -and $watchdogBody.Contains('github.ref_protected == true') -and $watchdogBody.Contains('target_not_found') -and $watchdogBody.Contains('api_error') -and $watchdogBody.Contains('cancel_failed') -and $watchdogBody.Contains('request_timeout_sec') -and $watchdogBody.Contains('$apiTimeoutSec = 8') -and $watchdogBody.Contains('-TimeoutSec $apiTimeoutSec') -and $watchdogBody.Contains('watchdog_status=') -and $watchdogBody.Contains('watchdog.json') -and $watchdogBody.Contains('throw "Real-GUI queue watchdog failed')) -Message 'Real-GUI watchdog must bound queued and environment-waiting jobs, bound every API request below the poll period, retain diagnostics, expose status, and fail on API/target/cancel problems.'

$statusBody = Get-JobBody -Text $workflowText -JobId 'real-gui-status-diagnostic'
Assert-True -Condition ($statusBody.Contains('always()') -and $statusBody.Contains('needs: [native-headless, real-gui-preflight, real-gui-watchdog, real-gui]') -and $statusBody.Contains('github.ref_protected == true') -and $statusBody.Contains('REF_PROTECTED: ${{ github.ref_protected }}') -and $statusBody.Contains('unprotected_ref') -and $statusBody.Contains('status:') -and $statusBody.Contains('runner_unavailable') -and $statusBody.Contains('approval_timeout') -and $statusBody.Contains('watchdog_failure') -and $statusBody.Contains('cancel_failed') -and $statusBody.Contains('failed') -and $statusBody.Contains('preflight_result:') -and $statusBody.Contains('watchdog_result:') -and $statusBody.Contains('real_gui_job_result:') -and $statusBody.Contains('authorization_policy:') -and $statusBody.Contains("exit 1") -and $statusBody.Contains('real-gui-status/status.json')) -Message 'Real-GUI unavailable, skipped, unprotected, approval-waiting, cancellation/watchdog, and actual failure paths must retain distinct diagnostics without masking execution failures.'

Assert-True -Condition ($runnerText.Contains('if ($Mode -eq ''Headless'') { ''ci-headless'' } else { ''real-gui'' }') -and $runnerText.Contains('Invoke-BoundedCTest') -and $runnerText.Contains('WaitForExit') -and $runnerText.Contains('suite_timeout_sec') -and $runnerText.Contains('test_timeout_sec') -and $runnerText.Contains('ctest_start_ticks') -and $runnerText.Contains('ProcessHandle') -and $runnerText.Contains('New-AutomationKillOnCloseJob') -and $runnerText.Contains('Assign-AutomationProcessToJob') -and $runnerText.Contains('Stop-AutomationProcessHandle') -and -not $runnerText.Contains('taskkill.exe') -and -not $runnerText.Contains('Stop-Process -Id')) -Message 'The CI runner must select the labels used by the workflow, capture CTest start identity, and bound process cleanup through the shared handle/Job seam.'
Assert-True -Condition ($runnerText.Contains('SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS') -and $runnerText.Contains('automation-samples') -and $runnerText.Contains('ctest-temporary') -and $runnerText.Contains('current_ctest_run_started_utc') -and $runnerText.Contains('historical Testing/Temporary files are excluded') -and $runnerText.Contains('failed with exit code')) -Message 'The CI runner must route sample artifacts and actionable CTest evidence into the current run-scoped artifact root and make nonzero CTest summaries diagnostic.'
Assert-True -Condition ([regex]::Matches($cmakeText, 'LABELS "automation;ci-headless;required"').Count -ge 4 -and [regex]::Matches($cmakeText, 'LABELS "automation;real-gui"').Count -ge 3) -Message 'CTest registrations must expose the ci-headless and real-gui labels selected by the CI runner.'

Write-Host 'Automation CI structural contract passed.'
