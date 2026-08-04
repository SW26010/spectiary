[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FixtureDirectory,
    [Parameter(Mandatory = $true)]
    [string]$Documentation,
    [Parameter(Mandatory = $true)]
    [string]$Runner,
    [Parameter(Mandatory = $true)]
    [string]$CMakeLists
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

$resolvedFixtureDirectory =
    (Resolve-Path -LiteralPath $FixtureDirectory).Path
$resolvedDocumentation =
    (Resolve-Path -LiteralPath $Documentation).Path
$resolvedRunner = (Resolve-Path -LiteralPath $Runner).Path
$resolvedCMakeLists = (Resolve-Path -LiteralPath $CMakeLists).Path
$expectedFiles = @('alpha.csv', 'bravo.csv', 'charlie.csv')
$actualFiles = @(
    Get-ChildItem -LiteralPath $resolvedFixtureDirectory -File |
        Sort-Object Name |
        ForEach-Object Name
)
Assert-True `
    -Condition (
        (@($actualFiles) -join "`n") -ceq
            (@($expectedFiles) -join "`n")) `
    -Message (
        'Automation sample fixture must contain exactly the checked-in CSV files. ' +
        "Actual: $($actualFiles -join ', ')")

foreach ($fileName in $expectedFiles) {
    $path = Join-Path $resolvedFixtureDirectory $fileName
    $lines = @(Get-Content -LiteralPath $path)
    Assert-True `
        -Condition (
            $lines.Count -eq 5 -and
            $lines[0] -ceq 'wavelength,flux') `
        -Message "$fileName must contain one header and four data rows."
    for ($index = 1; $index -lt $lines.Count; ++$index) {
        $columns = $lines[$index].Split(',')
        Assert-True `
            -Condition (
                $columns.Count -eq 2 -and
                $columns[0] -as [double] -ne $null -and
                $columns[1] -as [double] -ne $null) `
            -Message "$fileName row $index must contain two numeric values."
    }
}

$documentationText = Get-Content -Raw -LiteralPath $resolvedDocumentation
$runnerText = Get-Content -Raw -LiteralPath $resolvedRunner
$cmakeText = Get-Content -Raw -LiteralPath $resolvedCMakeLists
foreach ($requiredText in @(
        'Sequence A',
        'Sequence B',
        'source open',
        'spectrum goto name bravo.csv',
        'label assign 5 spectrum 1',
        'frame capture',
        'app quit',
        'Ownership and cleanup',
        'specforge_multi_instance_labeling_smoke_tests',
        'specforge_automation_launcher',
        'specforge_automation_state_isolation_tests',
        'specforge-automation-samples-<32-hex>',
        'sample-run-<32-hex>',
        '.specforge-automation-samples-ownership.json',
        'temporary_run_root',
        'bounded TEMP retention',
        'owner lease',
        'heartbeat')) {
    Assert-True `
        -Condition $documentationText.Contains($requiredText) `
        -Message "Automation sample documentation must describe '$requiredText'."
}

foreach ($requiredText in @(
        'Invoke-StateFixtureHelper',
        '[System.Diagnostics.ProcessStartInfo]::new()',
        'StateFixtureTimeoutMilliseconds',
        'WaitForExit',
        'helper_pid',
        'OwnedHelperPids',
        'Stop-OwnedLauncherProcessTree',
        'Assert-OwnedLauncherProcessGone',
        'OwnedLauncherPids',
        'launcher_pid',
        'ProcessCleanupRetryCount',
        'Get-TerminalMessages',
        'labelFirstGotoTerminal',
        'labelWaitTerminals',
        'cleanup_failures',
        'preserved_run_root',
        'Prune-FailureEvidenceDirectories',
        'FailureEvidenceRetentionCount',
        'FailureTempRetentionCount',
        'remainingTemporaryRoots',
        'retention capacity is exhausted',
        'Prune-SampleTempRunRoots',
        'Prune-SampleArtifactRunDirectories',
        'Get-SampleArtifactParentLeaseStatus',
        'parentLeaseStatus',
        'artifact-lease',
        "@('active', 'grace', 'unknown')",
        "@('completed', 'stale')",
         'ArtifactRunCapacity',
         'Get-SampleArtifactRunDirectories',
         'Enter-SampleArtifactCapacityLock',
         '[System.IO.FileShare]::None',
         'Write-SampleArtifactCapacityDiagnostic',
        'sample-artifact-capacity-diagnostic.json',
        'artifact capacity is exhausted',
        'No new run artifact directory was created',
        'SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS',
        'Initialize-SampleRunLease',
        'Complete-SampleRunLease',
        'Get-SampleLeaseStatus',
        'Write-AtomicUtf8File',
        'Register-OwnedProcessIdentity',
        'owner_start_ticks',
        'heartbeat_utc',
        'processes',
        'launch_pending',
        'PendingLaunches',
        'New-SampleLaunchToken',
        'Set-SampleLaunchPendingIdentity',
        'Complete-SampleLaunchPending',
        'Commit-SampleLaunchOwnership',
        'Get-OwnedProcessHandleForIdentity',
        'ProcessHandle',
        'ExpectedStartTicks',
        'New-AutomationKillOnCloseJob',
        'Assign-AutomationProcessToJob',
        'Stop-AutomationProcessHandle',
        'automation_process_guard.ps1',
        'resolvedRunArtifactsDirectory',
        'Write-SampleRunRootOwnershipManifest',
        'Test-SampleRunRootHasLiveOwnedProcess',
        'Stop-SampleRunRootOwnedProcesses',
        '.specforge-automation-samples-ownership.json',
        'sample-run-root-',
        'temporary_run_root',
        'temporary_root_retention_count',
        '$successSummary')) {
    Assert-True `
        -Condition $runnerText.Contains($requiredText) `
        -Message "Automation sample runner must contain '$requiredText'."
}
Assert-True `
    -Condition (
        -not $runnerText.Contains('Join-Path $resolvedArtifactsDirectory ''sample-source-navigation.log''') -and
        -not $runnerText.Contains('Join-Path $resolvedArtifactsDirectory ''sample-label-capture.log''') -and
        $runnerText.Contains('Join-Path $resolvedRunArtifactsDirectory ''sample-source-navigation.log''') -and
        $runnerText.Contains('Join-Path $resolvedRunArtifactsDirectory ''sample-label-capture.log''') -and
        $runnerText.Contains('throw $failureMessage')) `
    -Message 'Sample logs and stale-retention failures must use the unique run artifact directory and fail the runner.'
Assert-True `
    -Condition (
        -not $runnerText.Contains('taskkill.exe') -and
        -not $runnerText.Contains('Stop-Process -Id') -and
        $runnerText.Contains('sameIdentity') -and
        $runnerText.Contains('[int64]$_.start_ticks')) `
    -Message 'Sample cleanup must use held process handles and start identity, never PID-only taskkill or old-PID deduplication.'
Assert-True `
    -Condition ($runnerText -notmatch '(?m)^\s*&\s*\$resolvedStateFixture\b') `
    -Message 'Automation sample runner must not invoke the state fixture helper synchronously.'
Assert-True `
    -Condition (-not $runnerText.Contains('Start-Process') -and -not $runnerText.Contains('Add-Type')) `
    -Message 'Automation sample runner must not introduce a competing process or multi-instance runner.'
Assert-True `
    -Condition (
        $cmakeText.Contains('NAME specforge_automation_launcher_integration_tests') -and
        $cmakeText.Contains('NAME specforge_automation_samples_tests') -and
        $cmakeText.Contains('<TARGET_FILE:specforge_automation_launcher>') -and
        $cmakeText.Contains('<TARGET_FILE:specforge_automation_state_isolation_tests>')) `
    -Message 'Automation samples must reuse the existing launcher and state-isolation targets.'

Write-Host 'Automation sample fixture and command documentation contract passed.'
