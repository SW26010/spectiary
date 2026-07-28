[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Runner,
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$testExecutable = $Executable
. $Runner

function Assert-True {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

$disabledTier = Resolve-RuntimeResourceCTestTier `
    -Enabled $true `
    -RequestedTier ''
$smokeTier = Resolve-RuntimeResourceCTestTier `
    -Enabled $true `
    -RequestedTier 'smoke'
$soakTier = Resolve-RuntimeResourceCTestTier `
    -Enabled $true `
    -RequestedTier 'soak'
Assert-True `
    -Condition (
        $disabledTier.skip -and
        $smokeTier.measured_cycles -eq 12 -and
        $soakTier.measured_cycles -eq 100) `
    -Message 'CTest tier resolution must default to skip and expose smoke/soak workloads explicitly.'

if (-not ('SpecForge.RuntimeResourceTestNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace SpecForge {
    public static class RuntimeResourceTestNative {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool DeleteFile(string path);
    }
}
'@
}

function New-SampleSet {
    param(
        [double]$PrivateStep = 0,
        [double]$WorkingSetStep = 0,
        [double]$HandleStep = 0,
        [double]$ThreadStep = 0,
        [double]$GdiStep = 0,
        [double]$UserStep = 0
    )

    return @(
        for ($cycle = 1; $cycle -le 8; ++$cycle) {
            [pscustomobject]@{
                elapsed_ms = $cycle * 1000
                phase = 'settling'
                completed_measured_cycles = $cycle
                private_bytes = 100MB + ($PrivateStep * $cycle)
                working_set_bytes = 80MB + ($WorkingSetStep * $cycle)
                handle_count = 120 + ($HandleStep * $cycle)
                thread_count = 8 + ($ThreadStep * $cycle)
                gdi_object_count = 20 + ($GdiStep * $cycle)
                user_object_count = 10 + ($UserStep * $cycle)
            }
        }
    )
}

$expectedSourcePaths = @(
    'baseline-source',
    'stress-source')

function New-MeasuredCyclePresentations {
    return @(
        for ($cycleIndex = 0;
             $cycleIndex -lt 8;
             ++$cycleIndex) {
            $stressSequence =
                1 + (2 * $cycleIndex)
            $baselineSequence =
                $stressSequence + 1
            [pscustomobject]@{
                measured_cycle = $cycleIndex + 1
                stress = [pscustomobject]@{
                    source_index = 1
                    source_id = 'stress-id'
                    source_path = 'stress-source'
                    spectrum_index = 0
                    presentation_sequence =
                        $stressSequence
                    source_load_id =
                        100 + $stressSequence
                    activation_frame =
                        200 + $stressSequence
                    viewport_id = 7
                    render_target_resize_sequence =
                        $stressSequence
                }
                baseline = [pscustomobject]@{
                    source_index = 0
                    source_id = 'baseline-id'
                    source_path = 'baseline-source'
                    spectrum_index = 0
                    presentation_sequence =
                        $baselineSequence
                    source_load_id =
                        100 + $baselineSequence
                    activation_frame =
                        200 + $baselineSequence
                    viewport_id = 7
                    render_target_resize_sequence =
                        $baselineSequence
                }
            }
        }
    )
}

$thresholds = @{
    MaxPrivateBytesSlopeMiBPerCycle = 1.0
    MaxPrivateBytesGrowthMiB = 16.0
    MaxHandleSlopePerCycle = 0.25
    MaxHandleGrowth = 4.0
    MaxThreadSlopePerCycle = 0.10
    MaxThreadGrowth = 2.0
    MaxGdiObjectSlopePerCycle = 0.25
    MaxGdiObjectGrowth = 4.0
    MaxUserObjectSlopePerCycle = 0.25
    MaxUserObjectGrowth = 4.0
}
$status = [pscustomobject]@{
    state = 'completed'
    phase = 'completed'
    warmup_cycles = 2
    configured_source_count = 2
    preconditioned_source_count = 2
    measurement_baseline_recorded = $true
    measured_cycles_with_cancellation = 8
    measured_cycles_with_retirement = 8
    completed_measured_cycles = 8
    measured_cycle_presentations =
        @(New-MeasuredCyclePresentations)
    presentation = [pscustomobject]@{
        successful_source_load_present_count = 16
        last_source_load_id = 116
        last_activation_frame = 216
        last_viewport_id = 7
        render_target_resize_count = 16
    }
    activity = [pscustomobject]@{
        idle = $true
        worker_count = 0
        successful_cancellation_delta = 8
        retired_object_delta = 16
    }
    graphics = [pscustomobject]@{
        requested = $true
        report_recorded = $true
        available = $true
        unexpected_live_object_messages = 0
        detail = 'clean'
    }
}
$profile = [pscustomobject]@{
    valid = $true
    summary_count = 1
    final_event = 'profile_recorder_summary'
    stop_reason = 'explicit'
    dropped_events = 0
    max_duration_seconds = 0
    superseded_source_load_count = 8
    presented_source_load_count = 16
    presented_source_load_ids = @(101..116)
}

$atomicReadRoot = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-runtime-status-read-' + [Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($atomicReadRoot) | Out-Null
$atomicReadTarget = Join-Path $atomicReadRoot 'status.json'
try {
    [System.IO.File]::WriteAllText(
        $atomicReadTarget,
        '{"generation":0}')
    $statusStream =
        Open-WorkloadStatusReadStream -Path $atomicReadTarget
    try {
        $deleteSucceeded =
            [SpecForge.RuntimeResourceTestNative]::DeleteFile(
                $atomicReadTarget)
        Assert-True `
            -Condition $deleteSucceeded `
            -Message 'The workload status reader must grant delete sharing while its stream is open.'
    }
    finally {
        $statusStream.Dispose()
    }
}
finally {
    Remove-Item `
        -LiteralPath $atomicReadRoot `
        -Recurse `
        -Force `
        -ErrorAction SilentlyContinue
}

$utf8SourceListPath =
    [System.IO.Path]::GetTempFileName()
try {
    $expectedUtf8Sources = @(
        'C:\光谱\基线.csv',
        'D:\资料\压力.fits')
    [System.IO.File]::WriteAllText(
        $utf8SourceListPath,
        ($expectedUtf8Sources |
            ConvertTo-Json -Compress),
        [System.Text.UTF8Encoding]::new(
            $false,
            $true))
    $configuredUtf8Sources = @(
        Get-ConfiguredSources `
            -ExplicitSources @() `
            -ListFile $utf8SourceListPath)
    Assert-True `
        -Condition (
            $configuredUtf8Sources.Count -eq 2 -and
            [string]::Equals(
                [string]$configuredUtf8Sources[0],
                $expectedUtf8Sources[0],
                [System.StringComparison]::Ordinal) -and
            [string]::Equals(
                [string]$configuredUtf8Sources[1],
                $expectedUtf8Sources[1],
                [System.StringComparison]::Ordinal)) `
        -Message 'SourceListFile must decode UTF-8 without a BOM, including Chinese paths, under Windows PowerShell 5.1.'
}
finally {
    Remove-Item `
        -LiteralPath $utf8SourceListPath `
        -Force `
        -ErrorAction SilentlyContinue
}

$sampleBoundaryBefore = [pscustomobject]@{
    state = 'running'
    phase = 'settling'
    completed_cycles = 4
    completed_measured_cycles = 2
}
$sampleBoundarySame =
    $sampleBoundaryBefore.PSObject.Copy()
$sampleBoundaryNextPhase =
    $sampleBoundaryBefore.PSObject.Copy()
$sampleBoundaryNextPhase.phase =
    'start_stress_burst'
$sampleBoundaryNextCycle =
    $sampleBoundaryBefore.PSObject.Copy()
$sampleBoundaryNextCycle.completed_cycles = 5
$sampleBoundaryNextCycle.completed_measured_cycles = 3
Assert-True `
    -Condition (
        (Test-WorkloadStatusSampleBoundary `
            -Before $sampleBoundaryBefore `
            -After $sampleBoundarySame) -and
        -not (Test-WorkloadStatusSampleBoundary `
            -Before $sampleBoundaryBefore `
            -After $sampleBoundaryNextPhase) -and
        -not (Test-WorkloadStatusSampleBoundary `
            -Before $sampleBoundaryBefore `
            -After $sampleBoundaryNextCycle) -and
        -not (Test-WorkloadStatusSampleBoundary `
            -Before $null `
            -After $sampleBoundaryBefore)) `
    -Message 'A resource sample must be accepted only when phase and cycle identity are unchanged across the measurement.'

$stable = Invoke-RuntimeResourceEvidenceAnalysis `
    -Samples (New-SampleSet) `
    -FinalStatus $status `
    -ProfileEvidence $profile `
    -ProcessExitCode 0 `
    -ExpectedSourceCount 2 `
    -ExpectedSourcePaths $expectedSourcePaths `
    -ExpectedWarmupCycles 2 `
    -ExpectedMeasuredCycles 8 `
    -Thresholds $thresholds
Assert-True `
    -Condition ($stable.result -eq 'PASS') `
    -Message 'Stable evidence should pass.'

$handleLeak = Invoke-RuntimeResourceEvidenceAnalysis `
    -Samples (New-SampleSet -HandleStep 1) `
    -FinalStatus $status `
    -ProfileEvidence $profile `
    -ProcessExitCode 0 `
    -ExpectedSourceCount 2 `
    -ExpectedSourcePaths $expectedSourcePaths `
    -ExpectedWarmupCycles 2 `
    -ExpectedMeasuredCycles 8 `
    -Thresholds $thresholds
$handleCheck = $handleLeak.checks |
    Where-Object name -eq 'handle_count_trend'
Assert-True `
    -Condition (
        $handleLeak.result -eq 'FAIL' -and
        $handleCheck.status -eq 'FAIL') `
    -Message 'Linear handle growth should fail the handle trend gate.'

$workingSetOnly = Invoke-RuntimeResourceEvidenceAnalysis `
    -Samples (New-SampleSet -WorkingSetStep 8MB) `
    -FinalStatus $status `
    -ProfileEvidence $profile `
    -ProcessExitCode 0 `
    -ExpectedSourceCount 2 `
    -ExpectedSourcePaths $expectedSourcePaths `
    -ExpectedWarmupCycles 2 `
    -ExpectedMeasuredCycles 8 `
    -Thresholds $thresholds
$workingSetCheck = $workingSetOnly.checks |
    Where-Object name -eq 'working_set_trend'
Assert-True `
    -Condition (
        $workingSetOnly.result -eq 'PASS' -and
        $workingSetCheck.status -eq 'INFO') `
    -Message 'Working Set growth alone must remain informational.'

$badProfile = $profile.PSObject.Copy()
$badProfile.valid = $false
$profileFailure = Invoke-RuntimeResourceEvidenceAnalysis `
    -Samples (New-SampleSet) `
    -FinalStatus $status `
    -ProfileEvidence $badProfile `
    -ProcessExitCode 0 `
    -ExpectedSourceCount 2 `
    -ExpectedSourcePaths $expectedSourcePaths `
    -ExpectedWarmupCycles 2 `
    -ExpectedMeasuredCycles 8 `
    -Thresholds $thresholds
Assert-True `
    -Condition ($profileFailure.result -eq 'FAIL') `
    -Message 'An incomplete profile writer result should fail.'

$boundedWorkloadProfile = $profile.PSObject.Copy()
$boundedWorkloadProfile.max_duration_seconds = 300
$boundedWorkloadProfileAnalysis =
    Invoke-RuntimeResourceEvidenceAnalysis `
        -Samples (New-SampleSet) `
        -FinalStatus $status `
        -ProfileEvidence $boundedWorkloadProfile `
        -ProcessExitCode 0 `
        -ExpectedSourceCount 2 `
        -ExpectedSourcePaths $expectedSourcePaths `
        -ExpectedWarmupCycles 2 `
        -ExpectedMeasuredCycles 8 `
        -Thresholds $thresholds
$profileDurationCheck =
    $boundedWorkloadProfileAnalysis.checks |
        Where-Object name -eq 'profile_duration_contract'
Assert-True `
    -Condition (
        $boundedWorkloadProfileAnalysis.result -eq 'FAIL' -and
        $profileDurationCheck.status -eq 'FAIL') `
    -Message 'A workload profile with the ordinary five-minute duration limit must fail the external-timeout contract.'

$incompleteSourceStabilizationStatus = $status.PSObject.Copy()
$incompleteSourceStabilizationStatus.configured_source_count = 15
$incompleteSourceStabilizationStatus.preconditioned_source_count = 2
$incompleteSourceStabilization =
    Invoke-RuntimeResourceEvidenceAnalysis `
        -Samples (New-SampleSet) `
        -FinalStatus $incompleteSourceStabilizationStatus `
        -ProfileEvidence $profile `
        -ProcessExitCode 0 `
        -ExpectedSourceCount 15 `
        -ExpectedSourcePaths $expectedSourcePaths `
        -ExpectedWarmupCycles 2 `
        -ExpectedMeasuredCycles 8 `
        -Thresholds $thresholds
$sourceStabilizationCheck =
    $incompleteSourceStabilization.checks |
        Where-Object name -eq 'source_stabilization'
Assert-True `
    -Condition (
        $incompleteSourceStabilization.result -eq 'FAIL' -and
        $sourceStabilizationCheck.status -eq 'FAIL') `
    -Message 'Measured analysis must fail when not all 15 configured sources were stabilized.'

$missingMeasuredActivityStatus = $status.PSObject.Copy()
$missingMeasuredActivityStatus.measured_cycles_with_cancellation = 0
$missingMeasuredActivityStatus.measured_cycles_with_retirement = 0
$missingMeasuredActivity =
    Invoke-RuntimeResourceEvidenceAnalysis `
        -Samples (New-SampleSet) `
        -FinalStatus $missingMeasuredActivityStatus `
        -ProfileEvidence $profile `
        -ProcessExitCode 0 `
        -ExpectedSourceCount 2 `
        -ExpectedSourcePaths $expectedSourcePaths `
        -ExpectedWarmupCycles 2 `
        -ExpectedMeasuredCycles 8 `
        -Thresholds $thresholds
$measuredActivityCheck =
    $missingMeasuredActivity.checks |
        Where-Object name -eq 'measured_cycle_activity'
Assert-True `
    -Condition (
        $missingMeasuredActivity.result -eq 'FAIL' -and
        $measuredActivityCheck.status -eq 'FAIL') `
    -Message 'Warmup-only cancellation and retirement evidence must not satisfy measured-cycle activity.'

$missingMeasuredPresentationStatus =
    $status.PSObject.Copy()
$missingMeasuredPresentationStatus.measured_cycle_presentations = @(
        $status.measured_cycle_presentations |
            Select-Object -First 7)
$missingMeasuredPresentation =
    Invoke-RuntimeResourceEvidenceAnalysis `
        -Samples (New-SampleSet) `
        -FinalStatus $missingMeasuredPresentationStatus `
        -ProfileEvidence $profile `
        -ProcessExitCode 0 `
        -ExpectedSourceCount 2 `
        -ExpectedSourcePaths $expectedSourcePaths `
        -ExpectedWarmupCycles 2 `
        -ExpectedMeasuredCycles 8 `
        -Thresholds $thresholds
$measuredPresentationCheck =
    $missingMeasuredPresentation.checks |
        Where-Object name -eq `
            'measured_cycle_presentations'
Assert-True `
    -Condition (
        $missingMeasuredPresentation.result -eq 'FAIL' -and
        $measuredPresentationCheck.status -eq 'FAIL') `
    -Message 'Warmup/global Present activity must not hide one missing measured-cycle stress/baseline Present pair.'

$missingGraphicsStatus = $status.PSObject.Copy()
$missingGraphicsStatus.graphics = [pscustomobject]@{
    requested = $true
    report_recorded = $false
    available = $false
    unexpected_live_object_messages = 0
    detail = ''
}
$missingGraphics = Invoke-RuntimeResourceEvidenceAnalysis `
    -Samples (New-SampleSet) `
    -FinalStatus $missingGraphicsStatus `
    -ProfileEvidence $profile `
    -ProcessExitCode 0 `
    -ExpectedSourceCount 2 `
    -ExpectedSourcePaths $expectedSourcePaths `
    -ExpectedWarmupCycles 2 `
    -ExpectedMeasuredCycles 8 `
    -Thresholds $thresholds
$missingGraphicsCheck = $missingGraphics.checks |
    Where-Object name -eq 'd3d11_live_objects'
Assert-True `
    -Condition (
        $missingGraphics.result -eq 'FAIL' -and
        $missingGraphicsCheck.status -eq 'FAIL') `
    -Message 'A requested but missing shutdown graphics report should fail.'

$retainedWorkerStatus = $status.PSObject.Copy()
$retainedWorkerStatus.activity = $status.activity.PSObject.Copy()
$retainedWorkerStatus.activity.worker_count = 1
$retainedWorker = Invoke-RuntimeResourceEvidenceAnalysis `
    -Samples (New-SampleSet) `
    -FinalStatus $retainedWorkerStatus `
    -ProfileEvidence $profile `
    -ProcessExitCode 0 `
    -ExpectedSourceCount 2 `
    -ExpectedSourcePaths $expectedSourcePaths `
    -ExpectedWarmupCycles 2 `
    -ExpectedMeasuredCycles 8 `
    -Thresholds $thresholds
$workloadCheck = $retainedWorker.checks |
    Where-Object name -eq 'workload_completion'
Assert-True `
    -Condition (
        $retainedWorker.result -eq 'FAIL' -and
        $workloadCheck.status -eq 'FAIL') `
    -Message 'A retained source-load worker should fail workload completion.'

$profilePath = [System.IO.Path]::GetTempFileName()
try {
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText(
        $profilePath,
        '{"event":"profile_recorder_summary","stop_reason":"explicit"}' +
            [Environment]::NewLine,
        $encoding)
    $missingDroppedEvents = Read-ProfileEvidence -Path $profilePath
    Assert-True `
        -Condition (
            -not $missingDroppedEvents.valid -and
            $missingDroppedEvents.dropped_events -eq -1) `
        -Message 'A profile summary without dropped_events should fail closed.'
}
finally {
    Remove-Item -LiteralPath $profilePath -Force -ErrorAction SilentlyContinue
}

$startupFailureRoot = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-runtime-startup-failure-' +
        [Guid]::NewGuid().ToString('N'))
$startupStateDirectory =
    Join-Path $startupFailureRoot 'state'
$startupConfiguration =
    Join-Path $startupFailureRoot 'invalid-config.json'
try {
    [System.IO.Directory]::CreateDirectory(
        $startupStateDirectory) | Out-Null
    [System.IO.File]::WriteAllText(
        $startupConfiguration,
        '{}',
        [System.Text.UTF8Encoding]::new($false))
    $startupEnvironment = Set-TemporaryEnvironment -Values @{
        SPECFORGE_RUNTIME_RESOURCE_WORKLOAD =
            $startupConfiguration
        SPECFORGE_RUNTIME_RESOURCE_STATE_DIR =
            $startupStateDirectory
        SPECFORGE_PROFILE = $null
        SPECFORGE_PROFILE_DIR = $null
    }
    $startupProcess = $null
    try {
        $startupProcess = Start-Process `
            -FilePath (Resolve-Path -LiteralPath $testExecutable).Path `
            -PassThru `
            -WindowStyle Hidden
        $exited = $startupProcess.WaitForExit(5000)
        if (-not $exited) {
            Stop-Process `
                -Id $startupProcess.Id `
                -Force `
                -ErrorAction SilentlyContinue
            $startupProcess.WaitForExit()
        }
        Assert-True `
            -Condition $exited `
            -Message 'An automated workload startup error must exit without waiting for a modal dialog.'
        Assert-True `
            -Condition ($startupProcess.ExitCode -eq 1) `
            -Message 'An automated workload startup error must return exit code 1.'
        $startupDiagnostic = Join-Path `
            $startupStateDirectory `
            'runtime-resource-startup-error.txt'
        Assert-True `
            -Condition (
                (Test-Path `
                    -LiteralPath $startupDiagnostic `
                    -PathType Leaf) -and
                -not [string]::IsNullOrWhiteSpace(
                    (Get-Content `
                        -Raw `
                        -LiteralPath $startupDiagnostic))) `
            -Message 'An automated workload startup error must write a diagnostic evidence file.'
    }
    finally {
        if ($null -ne $startupProcess -and
            -not $startupProcess.HasExited) {
            Stop-Process `
                -Id $startupProcess.Id `
                -Force `
                -ErrorAction SilentlyContinue
            $startupProcess.WaitForExit()
        }
        Restore-TemporaryEnvironment `
            -Previous $startupEnvironment
    }
}
finally {
    Remove-Item `
        -LiteralPath $startupFailureRoot `
        -Recurse `
        -Force `
        -ErrorAction SilentlyContinue
}

$presentationGateRoot = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-runtime-presentation-gate-' +
        [Guid]::NewGuid().ToString('N'))
$presentationGateState =
    Join-Path $presentationGateRoot 'state'
$presentationGateProfile =
    Join-Path $presentationGateRoot 'profile'
$presentationGateStatus =
    Join-Path $presentationGateRoot 'status.json'
$presentationGateConfiguration =
    Join-Path $presentationGateRoot 'config.json'
$presentationGateBaseline =
    Join-Path $presentationGateRoot 'baseline.csv'
$presentationGateStress =
    Join-Path $presentationGateRoot 'stress.csv'
try {
    [System.IO.Directory]::CreateDirectory(
        $presentationGateState) | Out-Null
    [System.IO.Directory]::CreateDirectory(
        $presentationGateProfile) | Out-Null
    $csvFixture =
        "wavelength,flux`r`n5000,1`r`n5001,2`r`n"
    $utf8WithoutBom =
        [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText(
        $presentationGateBaseline,
        $csvFixture,
        $utf8WithoutBom)
    [System.IO.File]::WriteAllText(
        $presentationGateStress,
        $csvFixture,
        $utf8WithoutBom)
    $presentationGateConfigValue = [ordered]@{
        format_kind =
            'specforge_runtime_resource_workload'
        schema_version = 1
        status_path = $presentationGateStatus
        source_paths = @(
            $presentationGateBaseline,
            $presentationGateStress)
        warmup_cycles = 1
        measured_cycles = 1
        burst_count = 2
        settle_ms = 50
        final_settle_ms = 50
        poll_interval_ms = 10
        phase_timeout_ms = 1000
        enable_graphics_debug = $false
        require_graphics_diagnostics = $false
    }
    Write-JsonFile `
        -Path $presentationGateConfiguration `
        -Value $presentationGateConfigValue `
        -Depth 6
    $presentationGateEnvironment =
        Set-TemporaryEnvironment -Values @{
            SPECFORGE_RUNTIME_RESOURCE_WORKLOAD =
                $presentationGateConfiguration
            SPECFORGE_RUNTIME_RESOURCE_STATE_DIR =
                $presentationGateState
            SPECFORGE_PROFILE = '1'
            SPECFORGE_PROFILE_DIR =
                $presentationGateProfile
            SPECFORGE_PAN_PACING = $null
        }
    $presentationGateProcess = $null
    try {
        $presentationGateProcess = Start-Process `
            -FilePath (
                Resolve-Path `
                    -LiteralPath $testExecutable).Path `
            -WorkingDirectory (
                Resolve-Path `
                    -LiteralPath (
                        Join-Path $PSScriptRoot '..')).Path `
            -PassThru `
            -WindowStyle Minimized
        $presentationGateExited =
            $presentationGateProcess.WaitForExit(10000)
        if (-not $presentationGateExited) {
            Stop-Process `
                -Id $presentationGateProcess.Id `
                -Force `
                -ErrorAction SilentlyContinue
            $presentationGateProcess.WaitForExit()
        }
        $presentationGateFinalStatus =
            Read-WorkloadStatus `
                -Path $presentationGateStatus
        Assert-True `
            -Condition $presentationGateExited `
            -Message 'A minimized workload must fail its Present gate and close itself without the runner timeout.'
        Assert-True `
            -Condition (
                $presentationGateProcess.ExitCode -eq 2 -and
                $null -ne
                    $presentationGateFinalStatus -and
                $presentationGateFinalStatus.state -eq
                    'failed' -and
                [string]$presentationGateFinalStatus.failure -like
                    '*wait_stress_presented*') `
            -Message 'A zero-Present minimized workload must fail in wait_stress_presented instead of completing cycles.'
    }
    finally {
        if ($null -ne $presentationGateProcess -and
            -not $presentationGateProcess.HasExited) {
            Stop-Process `
                -Id $presentationGateProcess.Id `
                -Force `
                -ErrorAction SilentlyContinue
            $presentationGateProcess.WaitForExit()
        }
        Restore-TemporaryEnvironment `
            -Previous $presentationGateEnvironment
    }

    $initialStatusFailureConfiguration =
        Join-Path $presentationGateRoot `
            'initial-status-failure-config.json'
    $initialStatusFailureConfigValue = [ordered]@{}
    foreach ($entry in
             $presentationGateConfigValue.GetEnumerator()) {
        $initialStatusFailureConfigValue[
            $entry.Key] = $entry.Value
    }
    $initialStatusFailureConfigValue.status_path =
        $presentationGateState
    Write-JsonFile `
        -Path $initialStatusFailureConfiguration `
        -Value $initialStatusFailureConfigValue `
        -Depth 6
    $initialStatusFailureEnvironment =
        Set-TemporaryEnvironment -Values @{
            SPECFORGE_RUNTIME_RESOURCE_WORKLOAD =
                $initialStatusFailureConfiguration
            SPECFORGE_RUNTIME_RESOURCE_STATE_DIR =
                $presentationGateState
            SPECFORGE_PROFILE = '1'
            SPECFORGE_PROFILE_DIR =
                $presentationGateProfile
            SPECFORGE_PAN_PACING = $null
        }
    $initialStatusFailureProcess = $null
    try {
        $initialStatusFailureProcess = Start-Process `
            -FilePath (
                Resolve-Path `
                    -LiteralPath $testExecutable).Path `
            -WorkingDirectory (
                Resolve-Path `
                    -LiteralPath (
                        Join-Path $PSScriptRoot '..')).Path `
            -PassThru `
            -WindowStyle Hidden
        $initialStatusFailureExited =
            $initialStatusFailureProcess.WaitForExit(5000)
        if (-not $initialStatusFailureExited) {
            Stop-Process `
                -Id $initialStatusFailureProcess.Id `
                -Force `
                -ErrorAction SilentlyContinue
            $initialStatusFailureProcess.WaitForExit()
        }
        Assert-True `
            -Condition (
                $initialStatusFailureExited -and
                $initialStatusFailureProcess.ExitCode -eq
                    2) `
            -Message 'An initial workload status write failure must schedule immediate service and close with exit code 2.'
    }
    finally {
        if ($null -ne
                $initialStatusFailureProcess -and
            -not $initialStatusFailureProcess.HasExited) {
            Stop-Process `
                -Id $initialStatusFailureProcess.Id `
                -Force `
                -ErrorAction SilentlyContinue
            $initialStatusFailureProcess.WaitForExit()
        }
        Restore-TemporaryEnvironment `
            -Previous $initialStatusFailureEnvironment
    }
}
finally {
    Remove-Item `
        -LiteralPath $presentationGateRoot `
        -Recurse `
        -Force `
        -ErrorAction SilentlyContinue
}

$savedSamplingParameters = @{
    Source = $Source
    Executable = $Executable
    SampleIntervalMs = $SampleIntervalMs
    SettleMs = $SettleMs
}
try {
    $Source = @('sampling-margin-a', 'sampling-margin-b')
    $Executable = 'deliberately-missing-specforge.exe'
    $SampleIntervalMs = 200
    $SettleMs = 200
    $samplingMarginRejected = $false
    try {
        [void](Invoke-RuntimeResourceStability)
    }
    catch {
        $samplingMarginRejected =
            $_.Exception.Message -like
                '*no more than half of SettleMs*'
    }
    Assert-True `
        -Condition $samplingMarginRejected `
        -Message 'SampleIntervalMs equal to SettleMs must be rejected before process startup.'
}
finally {
    $Source = $savedSamplingParameters.Source
    $Executable = $savedSamplingParameters.Executable
    $SampleIntervalMs =
        $savedSamplingParameters.SampleIntervalMs
    $SettleMs = $savedSamplingParameters.SettleMs
}
