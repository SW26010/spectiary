[CmdletBinding()]
param(
    [string]$Executable = '',
    [string[]]$Source = @(),
    [string]$SourceListFile = '',
    [string]$OutputDirectory = '',
    [int]$WarmupCycles = 2,
    [int]$MeasuredCycles = 12,
    [int]$BurstCount = 3,
    [int]$SampleIntervalMs = 200,
    [int]$SettleMs = 750,
    [int]$FinalSettleMs = 2000,
    [int]$PhaseTimeoutMs = 120000,
    [int]$TimeoutSec = 600,
    [double]$MaxPrivateBytesSlopeMiBPerCycle = 1.0,
    [double]$MaxPrivateBytesGrowthMiB = 16.0,
    [double]$MaxHandleSlopePerCycle = 0.25,
    [double]$MaxHandleGrowth = 4.0,
    [double]$MaxThreadSlopePerCycle = 0.10,
    [double]$MaxThreadGrowth = 2.0,
    [double]$MaxGdiObjectSlopePerCycle = 0.25,
    [double]$MaxGdiObjectGrowth = 4.0,
    [double]$MaxUserObjectSlopePerCycle = 0.25,
    [double]$MaxUserObjectGrowth = 4.0,
    [switch]$DisableGraphicsDebug,
    [switch]$RequireGraphicsDiagnostics,
    [switch]$ReportOnly,
    [switch]$SkipIfSourcesUnavailable,
    [switch]$CTestIntegration
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$script:RuntimeResourceFailureResultPath = $null
$script:RuntimeResourceFailureEvidence = $null

function Resolve-RuntimeResourceCTestTier {
    param(
        [Parameter(Mandatory = $true)] [bool]$Enabled,
        [AllowEmptyString()] [string]$RequestedTier = ''
    )

    if (-not $Enabled) {
        return [pscustomobject][ordered]@{
            skip = $false
            name = 'custom'
            warmup_cycles = $null
            measured_cycles = $null
            timeout_sec = $null
        }
    }
    if ([string]::IsNullOrWhiteSpace($RequestedTier)) {
        return [pscustomobject][ordered]@{
            skip = $true
            name = 'disabled'
            warmup_cycles = $null
            measured_cycles = $null
            timeout_sec = $null
        }
    }

    switch ($RequestedTier.Trim().ToLowerInvariant()) {
        'smoke' {
            return [pscustomobject][ordered]@{
                skip = $false
                name = 'smoke'
                warmup_cycles = 2
                measured_cycles = 12
                timeout_sec = 600
            }
        }
        'soak' {
            return [pscustomobject][ordered]@{
                skip = $false
                name = 'soak'
                warmup_cycles = 5
                measured_cycles = 100
                timeout_sec = 1800
            }
        }
        default {
            throw "SPECFORGE_RESOURCE_STABILITY_TIER must be 'smoke' or 'soak', not '$RequestedTier'."
        }
    }
}

function Assert-RuntimeResourceSamplingConfiguration {
    param(
        [Parameter(Mandatory = $true)]
        [int]$IntervalMs,
        [Parameter(Mandatory = $true)]
        [int]$SettlingDurationMs
    )

    if ($IntervalMs -lt 50 -or
        ([long]$IntervalMs * 2) -gt
            [long]$SettlingDurationMs) {
        throw (
            'SampleIntervalMs must be at least 50 ms and no more ' +
            'than half of SettleMs.')
    }
}

function Get-Median {
    param([Parameter(Mandatory = $true)] [double[]]$Values)

    if ($Values.Count -eq 0) {
        throw 'Median requires at least one value.'
    }
    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) {
        return [double]$ordered[$middle]
    }
    return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

function Get-LinearTrend {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Samples,
        [Parameter(Mandatory = $true)] [string]$Property
    )

    if ($Samples.Count -lt 2) {
        throw "Trend '$Property' requires at least two samples."
    }

    $values = @($Samples | ForEach-Object { [double]$_.$Property })
    $count = $values.Count
    $meanX = ($count - 1) / 2.0
    $meanY = ($values | Measure-Object -Average).Average
    $xy = 0.0
    $xx = 0.0
    for ($index = 0; $index -lt $count; ++$index) {
        $dx = $index - $meanX
        $xy += $dx * ($values[$index] - $meanY)
        $xx += $dx * $dx
    }
    $slope = if ($xx -gt 0.0) { $xy / $xx } else { 0.0 }
    $intercept = $meanY - ($slope * $meanX)
    $residualSquares = 0.0
    $totalSquares = 0.0
    for ($index = 0; $index -lt $count; ++$index) {
        $predicted = $intercept + ($slope * $index)
        $residualSquares += [Math]::Pow($values[$index] - $predicted, 2)
        $totalSquares += [Math]::Pow($values[$index] - $meanY, 2)
    }
    $rSquared = if ($totalSquares -gt 0.0) {
        [Math]::Max(0.0, 1.0 - ($residualSquares / $totalSquares))
    }
    else {
        1.0
    }

    $windowSize = [Math]::Min(3, [Math]::Max(1, [int][Math]::Floor($count / 2)))
    $firstValues = [double[]]$values[0..($windowSize - 1)]
    $lastValues = [double[]]$values[($count - $windowSize)..($count - 1)]
    $firstMedian = Get-Median -Values $firstValues
    $lastMedian = Get-Median -Values $lastValues

    return [pscustomobject][ordered]@{
        sample_count = $count
        first_window_median = $firstMedian
        last_window_median = $lastMedian
        growth = $lastMedian - $firstMedian
        slope_per_cycle = $slope
        r_squared = $rSquared
    }
}

function New-TrendCheck {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [object[]]$Samples,
        [Parameter(Mandatory = $true)] [string]$Property,
        [Parameter(Mandatory = $true)] [double]$MaximumSlope,
        [Parameter(Mandatory = $true)] [double]$MaximumGrowth,
        [double]$Scale = 1.0,
        [string]$Unit = '',
        [bool]$Enforced = $true
    )

    $trend = Get-LinearTrend -Samples $Samples -Property $Property
    $scaledSlope = $trend.slope_per_cycle / $Scale
    $scaledGrowth = $trend.growth / $Scale
    $linearGrowth =
        $scaledSlope -gt $MaximumSlope -and
        $scaledGrowth -gt $MaximumGrowth -and
        $trend.r_squared -ge 0.50
    $status = if (-not $Enforced) {
        'INFO'
    }
    elseif ($linearGrowth) {
        'FAIL'
    }
    else {
        'PASS'
    }

    return [pscustomobject][ordered]@{
        name = $Name
        status = $status
        detail = if (-not $Enforced) {
            'Recorded for context; Working Set alone is not a leak gate.'
        }
        elseif ($linearGrowth) {
            "Sustained linear growth exceeded both gates."
        }
        else {
            'No sustained linear growth above both configured gates.'
        }
        observed = [pscustomobject][ordered]@{
            sample_count = $trend.sample_count
            first_window_median = $trend.first_window_median / $Scale
            last_window_median = $trend.last_window_median / $Scale
            growth = $scaledGrowth
            slope_per_cycle = $scaledSlope
            r_squared = $trend.r_squared
            unit = $Unit
        }
        threshold = [pscustomobject][ordered]@{
            enforced = $Enforced
            maximum_growth = $MaximumGrowth
            maximum_slope_per_cycle = $MaximumSlope
            minimum_r_squared = 0.50
            unit = $Unit
        }
    }
}

function Get-CycleEndSamples {
    param([Parameter(Mandatory = $true)] [object[]]$Samples)

    $settling = @(
        $Samples |
            Where-Object {
                $_.phase -eq 'settling' -and
                [int]$_.completed_measured_cycles -gt 0
            } |
            Group-Object completed_measured_cycles |
            ForEach-Object {
                $_.Group |
                    Sort-Object elapsed_ms |
                    Select-Object -Last 1
            } |
            Sort-Object { [int]$_.completed_measured_cycles }
    )
    return $settling
}

function Read-ProfileEvidence {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $eventCount = 0
    $summaryCount = 0
    $summary = $null
    $lastEvent = ''
    $supersededLoads = 0
    $presentedLoads = 0
    $presentedSourceLoadIds =
        [System.Collections.Generic.List[long]]::new()
    $profileMaxDurationSeconds = -1L
    $parseErrors = [System.Collections.Generic.List[string]]::new()

    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        try {
            $event = $line | ConvertFrom-Json
        }
        catch {
            $parseErrors.Add($_.Exception.Message)
            continue
        }
        ++$eventCount
        $lastEvent = [string]$event.event
        if ($event.event -eq 'profile_recorder_summary') {
            ++$summaryCount
            $summary = $event
        }
        if ($event.event -eq 'profile_recording' -and
            $event.action -eq 'start' -and
            $null -ne $event.PSObject.Properties[
                'max_duration_seconds']) {
            $profileMaxDurationSeconds =
                [long]$event.max_duration_seconds
        }
        if ($event.event -eq 'source_load_latency') {
            if ($event.outcome -eq 'superseded') {
                ++$supersededLoads
            }
            elseif ($event.outcome -eq 'presented') {
                ++$presentedLoads
                if ($null -eq $event.PSObject.Properties[
                        'source_load_id'] -or
                    [long]$event.source_load_id -le 0) {
                    $parseErrors.Add(
                        'A presented source-load event is missing a positive source_load_id.')
                }
                else {
                    $presentedSourceLoadIds.Add(
                        [long]$event.source_load_id)
                }
            }
        }
    }

    $hasDroppedEvents =
        $null -ne $summary -and
        $null -ne $summary.PSObject.Properties['dropped_events']
    $droppedEvents = if ($hasDroppedEvents) {
        [long]$summary.dropped_events
    }
    else {
        -1L
    }
    $stopReason = if ($null -ne $summary) {
        [string]$summary.stop_reason
    }
    else {
        ''
    }
    $valid =
        $parseErrors.Count -eq 0 -and
        $summaryCount -eq 1 -and
        $lastEvent -eq 'profile_recorder_summary' -and
        $droppedEvents -eq 0 -and
        $stopReason -eq 'explicit'

    return [pscustomobject][ordered]@{
        valid = $valid
        event_count = $eventCount
        parse_error_count = $parseErrors.Count
        summary_count = $summaryCount
        final_event = $lastEvent
        dropped_events = $droppedEvents
        stop_reason = $stopReason
        max_duration_seconds =
            $profileMaxDurationSeconds
        superseded_source_load_count = $supersededLoads
        presented_source_load_count = $presentedLoads
        presented_source_load_ids =
            @($presentedSourceLoadIds)
    }
}

function Add-ResourceNativeMethods {
    if ('SpecForge.RuntimeResourceNative' -as [type]) {
        return
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace SpecForge {
    public static class RuntimeResourceNative {
        [DllImport("user32.dll", SetLastError = true)]
        public static extern uint GetGuiResources(IntPtr process, uint flags);
    }
}
'@
}

function Open-WorkloadStatusReadStream {
    param([Parameter(Mandatory = $true)] [string]$Path)

    return [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        ([System.IO.FileShare]::ReadWrite -bor
            [System.IO.FileShare]::Delete))
}

function Read-WorkloadStatus {
    param([Parameter(Mandatory = $true)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    $stream = $null
    $reader = $null
    try {
        $stream = Open-WorkloadStatusReadStream -Path $Path
        $reader = [System.IO.StreamReader]::new(
            $stream,
            [System.Text.Encoding]::UTF8,
            $true)
        $text = $reader.ReadToEnd()
        return $text | ConvertFrom-Json
    }
    catch {
        return $null
    }
    finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        }
        elseif ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Get-ProcessResourceSample {
    param(
        [Parameter(Mandatory = $true)] [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)] [long]$ElapsedMilliseconds,
        [Parameter(Mandatory = $true)] [int]$SampleIndex
    )

    $Process.Refresh()
    $gdiObjects = [SpecForge.RuntimeResourceNative]::GetGuiResources($Process.Handle, 0)
    $userObjects = [SpecForge.RuntimeResourceNative]::GetGuiResources($Process.Handle, 1)
    return [pscustomobject][ordered]@{
        sample_index = $SampleIndex
        elapsed_ms = $ElapsedMilliseconds
        private_bytes = [long]$Process.PrivateMemorySize64
        working_set_bytes = [long]$Process.WorkingSet64
        virtual_bytes = [long]$Process.VirtualMemorySize64
        handle_count = [int]$Process.HandleCount
        thread_count = [int]$Process.Threads.Count
        gdi_object_count = [int]$gdiObjects
        user_object_count = [int]$userObjects
    }
}

function Test-WorkloadStatusSampleBoundary {
    param(
        [AllowNull()] [object]$Before,
        [AllowNull()] [object]$After
    )

    if ($null -eq $Before -or $null -eq $After) {
        return $null -eq $Before -and
            $null -eq $After
    }
    return (
        [string]$Before.state -eq
            [string]$After.state -and
        [string]$Before.phase -eq
            [string]$After.phase -and
        [int]$Before.completed_cycles -eq
            [int]$After.completed_cycles -and
        [int]$Before.completed_measured_cycles -eq
            [int]$After.completed_measured_cycles)
}

function Add-WorkloadStatusToResourceSample {
    param(
        [Parameter(Mandatory = $true)] [object]$ResourceSample,
        [AllowNull()] [object]$Status
    )

    return [pscustomobject][ordered]@{
        sample_index = [int]$ResourceSample.sample_index
        elapsed_ms = [long]$ResourceSample.elapsed_ms
        workload_state = if ($null -ne $Status) {
            [string]$Status.state
        }
        else {
            ''
        }
        phase = if ($null -ne $Status) {
            [string]$Status.phase
        }
        else {
            ''
        }
        completed_cycles = if ($null -ne $Status) {
            [int]$Status.completed_cycles
        }
        else {
            -1
        }
        completed_measured_cycles = if (
            $null -ne $Status) {
            [int]$Status.completed_measured_cycles
        }
        else {
            -1
        }
        status_consistent = $true
        private_bytes =
            [long]$ResourceSample.private_bytes
        working_set_bytes =
            [long]$ResourceSample.working_set_bytes
        virtual_bytes =
            [long]$ResourceSample.virtual_bytes
        handle_count =
            [int]$ResourceSample.handle_count
        thread_count =
            [int]$ResourceSample.thread_count
        gdi_object_count =
            [int]$ResourceSample.gdi_object_count
        user_object_count =
            [int]$ResourceSample.user_object_count
    }
}

function New-Check {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [ValidateSet('PASS', 'FAIL', 'SKIP', 'INFO')] [string]$Status,
        [Parameter(Mandatory = $true)] [string]$Detail,
        [AllowNull()] [object]$Observed = $null,
        [AllowNull()] [object]$Threshold = $null
    )

    return [pscustomobject][ordered]@{
        name = $Name
        status = $Status
        detail = $Detail
        observed = $Observed
        threshold = $Threshold
    }
}

function Invoke-RuntimeResourceEvidenceAnalysis {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Samples,
        [Parameter(Mandatory = $true)] [object]$FinalStatus,
        [Parameter(Mandatory = $true)] [object]$ProfileEvidence,
        [Parameter(Mandatory = $true)] [int]$ProcessExitCode,
        [Parameter(Mandatory = $true)] [int]$ExpectedSourceCount,
        [Parameter(Mandatory = $true)] [string[]]$ExpectedSourcePaths,
        [Parameter(Mandatory = $true)] [int]$ExpectedWarmupCycles,
        [Parameter(Mandatory = $true)] [int]$ExpectedMeasuredCycles,
        [Parameter(Mandatory = $true)] [hashtable]$Thresholds,
        [bool]$RequireGraphics = $false
    )

    $checks = [System.Collections.Generic.List[object]]::new()
    $checks.Add((New-Check `
        -Name 'process_exit' `
        -Status $(if ($ProcessExitCode -eq 0) { 'PASS' } else { 'FAIL' }) `
        -Detail "SpecForge exit code: $ProcessExitCode." `
        -Observed $ProcessExitCode `
        -Threshold 0))

    $workerCount = [int]$FinalStatus.activity.worker_count
    $workloadComplete =
        $FinalStatus.state -eq 'completed' -and
        [int]$FinalStatus.completed_measured_cycles -eq $ExpectedMeasuredCycles -and
        [bool]$FinalStatus.activity.idle -and
        $workerCount -eq 0
    $checks.Add((New-Check `
        -Name 'workload_completion' `
        -Status $(if ($workloadComplete) { 'PASS' } else { 'FAIL' }) `
        -Detail $(if ($workloadComplete) {
            'All cycles completed with load/retirement activity idle and no retained load workers.'
        } else {
            "Final workload state was '$($FinalStatus.state)' / '$($FinalStatus.phase)'; retained load workers: $workerCount."
        }) `
        -Observed ([pscustomobject]@{
            state = $FinalStatus.state
            worker_count = $workerCount
        }) `
        -Threshold 'completed, idle, and worker_count == 0'))

    $configuredSourceCount =
        [int]$FinalStatus.configured_source_count
    $preconditionedSourceCount =
        [int]$FinalStatus.preconditioned_source_count
    $measurementBaselineRecorded =
        [bool]$FinalStatus.measurement_baseline_recorded
    $sourceStabilized =
        $configuredSourceCount -eq $ExpectedSourceCount -and
        $preconditionedSourceCount -eq $ExpectedSourceCount -and
        $measurementBaselineRecorded
    $checks.Add((New-Check `
        -Name 'source_stabilization' `
        -Status $(if ($sourceStabilized) { 'PASS' } else { 'FAIL' }) `
        -Detail "Preconditioned sources: $preconditionedSourceCount / $configuredSourceCount; expected: $ExpectedSourceCount; post-warmup baseline recorded: $measurementBaselineRecorded." `
        -Observed ([pscustomobject]@{
            configured_source_count = $configuredSourceCount
            preconditioned_source_count = $preconditionedSourceCount
            measurement_baseline_recorded = $measurementBaselineRecorded
        }) `
        -Threshold 'all configured sources preconditioned before a post-warmup baseline'))

    $measuredCancellationCycles =
        [int]$FinalStatus.measured_cycles_with_cancellation
    $measuredRetirementCycles =
        [int]$FinalStatus.measured_cycles_with_retirement
    $measuredActivityComplete =
        $measuredCancellationCycles -eq $ExpectedMeasuredCycles -and
        $measuredRetirementCycles -eq $ExpectedMeasuredCycles
    $checks.Add((New-Check `
        -Name 'measured_cycle_activity' `
        -Status $(if ($measuredActivityComplete) { 'PASS' } else { 'FAIL' }) `
        -Detail "Measured cycles with cancellation: $measuredCancellationCycles / $ExpectedMeasuredCycles; with retirement: $measuredRetirementCycles / $ExpectedMeasuredCycles." `
        -Observed ([pscustomobject]@{
            cancellation_cycles = $measuredCancellationCycles
            retirement_cycles = $measuredRetirementCycles
        }) `
        -Threshold $ExpectedMeasuredCycles))

    $presentationFailures =
        [System.Collections.Generic.List[string]]::new()
    $cyclePresentations = if (
        $null -ne $FinalStatus.PSObject.Properties[
            'measured_cycle_presentations']) {
        @($FinalStatus.measured_cycle_presentations)
    }
    else {
        @()
    }
    if ($ExpectedSourcePaths.Count -ne
        $ExpectedSourceCount) {
        $presentationFailures.Add(
            'Expected source path count does not match ExpectedSourceCount.')
    }
    if ($cyclePresentations.Count -ne
        $ExpectedMeasuredCycles) {
        $presentationFailures.Add(
            "Measured presentation records: $($cyclePresentations.Count) / $ExpectedMeasuredCycles.")
    }

    $profilePresentedIds =
        [System.Collections.Generic.HashSet[long]]::new()
    if ($null -ne $ProfileEvidence.PSObject.Properties[
            'presented_source_load_ids']) {
        foreach ($sourceLoadId in
                 @($ProfileEvidence.presented_source_load_ids)) {
            if ([long]$sourceLoadId -gt 0) {
                [void]$profilePresentedIds.Add(
                    [long]$sourceLoadId)
            }
        }
    }
    $measuredPresentedIds =
        [System.Collections.Generic.HashSet[long]]::new()
    $previousPresentationSequence = 0L
    $previousResizeSequence = 0L
    $recordsToValidate = [Math]::Min(
        $cyclePresentations.Count,
        $ExpectedMeasuredCycles)
    for ($cycleIndex = 0;
         $cycleIndex -lt $recordsToValidate;
         ++$cycleIndex) {
        $cycle = $cyclePresentations[$cycleIndex]
        $expectedCycle = $cycleIndex + 1
        if ($null -eq $cycle -or
            $null -eq $cycle.PSObject.Properties[
                'measured_cycle'] -or
            [int]$cycle.measured_cycle -ne
                $expectedCycle) {
            $presentationFailures.Add(
                "Measured presentation record $expectedCycle has the wrong cycle identity.")
            continue
        }

        $expectedStressIndex = 1 +
            (($ExpectedWarmupCycles + $cycleIndex) %
                ($ExpectedSourceCount - 1))
        foreach ($kind in @('stress', 'baseline')) {
            $expectedSourceIndex = if (
                $kind -eq 'stress') {
                $expectedStressIndex
            }
            else {
                0
            }
            $property = $cycle.PSObject.Properties[$kind]
            if ($null -eq $property -or
                $null -eq $property.Value) {
                $presentationFailures.Add(
                    "Measured cycle $expectedCycle is missing $kind Present evidence.")
                continue
            }
            $evidence = $property.Value
            $requiredFields = @(
                'source_index',
                'source_id',
                'source_path',
                'spectrum_index',
                'presentation_sequence',
                'source_load_id',
                'activation_frame',
                'viewport_id',
                'render_target_resize_sequence')
            $missingFields = @(
                $requiredFields |
                    Where-Object {
                        $null -eq
                            $evidence.PSObject.Properties[$_]
                    })
            if ($missingFields.Count -gt 0) {
                $presentationFailures.Add(
                    "Measured cycle $expectedCycle $kind evidence is missing: $($missingFields -join ', ').")
                continue
            }

            $sourceIndex = [int]$evidence.source_index
            $sourceLoadId = [long]$evidence.source_load_id
            $presentationSequence =
                [long]$evidence.presentation_sequence
            $activationFrame =
                [long]$evidence.activation_frame
            $viewportId = [long]$evidence.viewport_id
            $resizeSequence =
                [long]$evidence.render_target_resize_sequence
            $pathMatches =
                $expectedSourceIndex -lt
                    $ExpectedSourcePaths.Count -and
                [string]::Equals(
                    [string]$evidence.source_path,
                    [string]$ExpectedSourcePaths[
                        $expectedSourceIndex],
                    [System.StringComparison]::
                        OrdinalIgnoreCase)
            if ($sourceIndex -ne $expectedSourceIndex -or
                -not $pathMatches -or
                [string]::IsNullOrWhiteSpace(
                    [string]$evidence.source_id) -or
                [int]$evidence.spectrum_index -ne 0 -or
                $presentationSequence -le
                    $previousPresentationSequence -or
                $sourceLoadId -le 0 -or
                $activationFrame -le 0 -or
                $viewportId -le 0 -or
                $resizeSequence -le
                    $previousResizeSequence) {
                $presentationFailures.Add(
                    "Measured cycle $expectedCycle $kind evidence has an invalid source identity, ordering, Present, or resize value.")
                continue
            }
            if (-not $measuredPresentedIds.Add(
                    $sourceLoadId)) {
                $presentationFailures.Add(
                    "Measured source_load_id $sourceLoadId is duplicated.")
                continue
            }
            if (-not $profilePresentedIds.Contains(
                    $sourceLoadId)) {
                $presentationFailures.Add(
                    "Measured source_load_id $sourceLoadId is absent from presented profile events.")
                continue
            }
            $previousPresentationSequence =
                $presentationSequence
            $previousResizeSequence = $resizeSequence
        }
    }

    $minimumMeasuredPresentations =
        2 * $ExpectedMeasuredCycles
    if ([int]$ProfileEvidence.presented_source_load_count -lt
        $minimumMeasuredPresentations) {
        $presentationFailures.Add(
            "Presented profile events: $($ProfileEvidence.presented_source_load_count); expected at least $minimumMeasuredPresentations for measured cycles.")
    }
    $presentationComplete =
        $presentationFailures.Count -eq 0
    $checks.Add((New-Check `
        -Name 'measured_cycle_presentations' `
        -Status $(if ($presentationComplete) {
            'PASS'
        } else {
            'FAIL'
        }) `
        -Detail $(if ($presentationComplete) {
            "All $ExpectedMeasuredCycles measured cycles have ordered, identity-matched stress/baseline Present and render-target resize evidence corroborated by the profile."
        } else {
            $presentationFailures -join ' '
        }) `
        -Observed $cyclePresentations.Count `
        -Threshold "$ExpectedMeasuredCycles complete stress/baseline pairs"))

    $cancellationDelta = [long]$FinalStatus.activity.successful_cancellation_delta
    $checks.Add((New-Check `
        -Name 'source_load_cancellation' `
        -Status $(if ($cancellationDelta -gt 0) { 'PASS' } else { 'FAIL' }) `
        -Detail "Successful cancellation delta: $cancellationDelta." `
        -Observed $cancellationDelta `
        -Threshold '> 0'))

    $retirementDelta = [long]$FinalStatus.activity.retired_object_delta
    $retirementIdle = [bool]$FinalStatus.activity.idle
    $retirementComplete =
        $retirementDelta -gt 0 -and
        $retirementIdle
    $checks.Add((New-Check `
        -Name 'background_retirement' `
        -Status $(if ($retirementComplete) { 'PASS' } else { 'FAIL' }) `
        -Detail "Retired object delta: $retirementDelta; final queues are idle: $retirementIdle." `
        -Observed $retirementDelta `
        -Threshold '> 0 and idle'))

    $checks.Add((New-Check `
        -Name 'profile_writer_completion' `
        -Status $(if ($ProfileEvidence.valid) { 'PASS' } else { 'FAIL' }) `
        -Detail "Summary count: $($ProfileEvidence.summary_count); final event: $($ProfileEvidence.final_event); stop reason: $($ProfileEvidence.stop_reason); dropped events: $($ProfileEvidence.dropped_events)." `
        -Observed $ProfileEvidence `
        -Threshold 'one final explicit-stop summary and dropped_events == 0'))

    $profileDurationExternallyManaged =
        [long]$ProfileEvidence.max_duration_seconds -eq 0
    $checks.Add((New-Check `
        -Name 'profile_duration_contract' `
        -Status $(if ($profileDurationExternallyManaged) {
            'PASS'
        } else {
            'FAIL'
        }) `
        -Detail "Workload profile max duration: $($ProfileEvidence.max_duration_seconds) seconds; the runner owns the external timeout." `
        -Observed $ProfileEvidence.max_duration_seconds `
        -Threshold 0))

    $profileCancellation = [int]$ProfileEvidence.superseded_source_load_count
    $checks.Add((New-Check `
        -Name 'profile_cancellation_evidence' `
        -Status $(if ($profileCancellation -gt 0) { 'PASS' } else { 'FAIL' }) `
        -Detail "Superseded source-load traces: $profileCancellation." `
        -Observed $profileCancellation `
        -Threshold '> 0'))

    $graphics = $FinalStatus.graphics
    if ([bool]$graphics.available) {
        $unexpected = [long]$graphics.unexpected_live_object_messages
        $checks.Add((New-Check `
            -Name 'd3d11_live_objects' `
            -Status $(if ($unexpected -eq 0) { 'PASS' } else { 'FAIL' }) `
            -Detail ([string]$graphics.detail) `
            -Observed $unexpected `
            -Threshold 0))
    }
    elseif ([bool]$graphics.requested -and
            -not [bool]$graphics.report_recorded) {
        $checks.Add((New-Check `
            -Name 'd3d11_live_objects' `
            -Status 'FAIL' `
            -Detail 'D3D11 live-object diagnostics were requested but the application did not record its shutdown report.' `
            -Observed $false `
            -Threshold 'shutdown report recorded'))
    }
    elseif ($RequireGraphics) {
        $checks.Add((New-Check `
            -Name 'd3d11_live_objects' `
            -Status 'FAIL' `
            -Detail 'D3D11 live-object diagnostics were required but unavailable.' `
            -Observed $false `
            -Threshold $true))
    }
    else {
        $graphicsDetail = [string]$graphics.detail
        if ([string]::IsNullOrWhiteSpace($graphicsDetail)) {
            $graphicsDetail =
                'D3D11 live-object diagnostics were disabled for this run.'
        }
        $checks.Add((New-Check `
            -Name 'd3d11_live_objects' `
            -Status 'SKIP' `
            -Detail $graphicsDetail `
            -Observed $false `
            -Threshold 'available when Windows Graphics Tools are installed'))
    }

    $cycleSamples = @(Get-CycleEndSamples -Samples $Samples)
    $sampleCountOkay = $cycleSamples.Count -eq $ExpectedMeasuredCycles
    $checks.Add((New-Check `
        -Name 'cycle_end_samples' `
        -Status $(if ($sampleCountOkay) { 'PASS' } else { 'FAIL' }) `
        -Detail "Comparable post-settle samples: $($cycleSamples.Count) / $ExpectedMeasuredCycles." `
        -Observed $cycleSamples.Count `
        -Threshold $ExpectedMeasuredCycles))

    if ($cycleSamples.Count -ge 6) {
        $mib = 1MB
        $checks.Add((New-TrendCheck `
            -Name 'private_bytes_commit_trend' `
            -Samples $cycleSamples `
            -Property 'private_bytes' `
            -MaximumSlope $Thresholds.MaxPrivateBytesSlopeMiBPerCycle `
            -MaximumGrowth $Thresholds.MaxPrivateBytesGrowthMiB `
            -Scale $mib `
            -Unit 'MiB'))
        $checks.Add((New-TrendCheck `
            -Name 'working_set_trend' `
            -Samples $cycleSamples `
            -Property 'working_set_bytes' `
            -MaximumSlope 0 `
            -MaximumGrowth 0 `
            -Scale $mib `
            -Unit 'MiB' `
            -Enforced $false))
        $checks.Add((New-TrendCheck `
            -Name 'handle_count_trend' `
            -Samples $cycleSamples `
            -Property 'handle_count' `
            -MaximumSlope $Thresholds.MaxHandleSlopePerCycle `
            -MaximumGrowth $Thresholds.MaxHandleGrowth `
            -Unit 'objects'))
        $checks.Add((New-TrendCheck `
            -Name 'thread_count_trend' `
            -Samples $cycleSamples `
            -Property 'thread_count' `
            -MaximumSlope $Thresholds.MaxThreadSlopePerCycle `
            -MaximumGrowth $Thresholds.MaxThreadGrowth `
            -Unit 'threads'))
        $checks.Add((New-TrendCheck `
            -Name 'gdi_object_trend' `
            -Samples $cycleSamples `
            -Property 'gdi_object_count' `
            -MaximumSlope $Thresholds.MaxGdiObjectSlopePerCycle `
            -MaximumGrowth $Thresholds.MaxGdiObjectGrowth `
            -Unit 'objects'))
        $checks.Add((New-TrendCheck `
            -Name 'user_object_trend' `
            -Samples $cycleSamples `
            -Property 'user_object_count' `
            -MaximumSlope $Thresholds.MaxUserObjectSlopePerCycle `
            -MaximumGrowth $Thresholds.MaxUserObjectGrowth `
            -Unit 'objects'))
    }
    else {
        $checks.Add((New-Check `
            -Name 'resource_trends' `
            -Status 'FAIL' `
            -Detail 'At least six comparable measured-cycle samples are required for trend analysis.' `
            -Observed $cycleSamples.Count `
            -Threshold 6))
    }

    return [pscustomobject][ordered]@{
        result = if (@($checks | Where-Object status -eq 'FAIL').Count -eq 0) {
            'PASS'
        }
        else {
            'FAIL'
        }
        checks = @($checks)
        cycle_end_samples = $cycleSamples
    }
}

function Get-ConfiguredSources {
    param(
        [string[]]$ExplicitSources,
        [string]$ListFile
    )

    if ($ExplicitSources.Count -gt 0) {
        return @($ExplicitSources)
    }
    if (-not [string]::IsNullOrWhiteSpace($ListFile)) {
        try {
            $parsed = [System.IO.File]::ReadAllText(
                $ListFile,
                [System.Text.UTF8Encoding]::new(
                    $false,
                    $true)) |
                ConvertFrom-Json
            return @($parsed | ForEach-Object { $_ })
        }
        catch {
            throw 'SourceListFile must contain a JSON array of source paths.'
        }
    }
    $json = $env:SPECFORGE_RESOURCE_STABILITY_SOURCES_JSON
    if ([string]::IsNullOrWhiteSpace($json)) {
        return @()
    }
    try {
        $parsed = $json | ConvertFrom-Json
        return @($parsed | ForEach-Object { $_ })
    }
    catch {
        throw 'SPECFORGE_RESOURCE_STABILITY_SOURCES_JSON must be a JSON array of source paths.'
    }
}

function Set-TemporaryEnvironment {
    param([Parameter(Mandatory = $true)] [hashtable]$Values)

    $previous = @{}
    foreach ($name in $Values.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $Values[$name], 'Process')
    }
    return $previous
}

function Restore-TemporaryEnvironment {
    param([Parameter(Mandatory = $true)] [hashtable]$Previous)

    foreach ($name in $Previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $Previous[$name], 'Process')
    }
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [object]$Value,
        [int]$Depth = 8
    )

    $json = $Value | ConvertTo-Json -Depth $Depth
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText(
        $Path,
        $json + [Environment]::NewLine,
        $encoding)
}

function Invoke-RuntimeResourceStability {
    $scriptRoot = $PSScriptRoot
    $repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
    $tier = Resolve-RuntimeResourceCTestTier `
        -Enabled ([bool]$CTestIntegration) `
        -RequestedTier ([string]$env:SPECFORGE_RESOURCE_STABILITY_TIER)
    if ($tier.skip) {
        Write-Host (
            'SKIP: Set SPECFORGE_RESOURCE_STABILITY_TIER to smoke or soak ' +
            'to enable the periodic runtime resource CTest.')
        return 125
    }
    if ($CTestIntegration) {
        $WarmupCycles = [int]$tier.warmup_cycles
        $MeasuredCycles = [int]$tier.measured_cycles
        $TimeoutSec = [int]$tier.timeout_sec
    }
    if (-not $Executable) {
        $Executable = Join-Path $repoRoot 'build\ninja-msvc-debug\SpecForge.exe'
    }
    if (-not $OutputDirectory) {
        $OutputDirectory = Join-Path $repoRoot 'logs\runtime-resource-stability'
    }

    $configuredSources = @(
        Get-ConfiguredSources `
            -ExplicitSources $Source `
            -ListFile $SourceListFile
    )
    if ($configuredSources.Count -lt 2) {
        $message =
            'Provide at least two stable real source paths with -Source, or set SPECFORGE_RESOURCE_STABILITY_SOURCES_JSON.'
        if ($SkipIfSourcesUnavailable) {
            Write-Host "SKIP: $message"
            return 125
        }
        throw $message
    }
    if ($MeasuredCycles -lt 6) {
        throw 'MeasuredCycles must be at least 6 for trend analysis.'
    }
    Assert-RuntimeResourceSamplingConfiguration `
        -IntervalMs $SampleIntervalMs `
        -SettlingDurationMs $SettleMs

    $resolvedExecutable = Resolve-Path -LiteralPath $Executable -ErrorAction Stop
    $resolvedSources = [System.Collections.Generic.List[string]]::new()
    foreach ($path in $configuredSources) {
        $resolved = Resolve-Path -LiteralPath ([string]$path) -ErrorAction Stop
        $resolvedSources.Add($resolved.Path)
    }
    if (@($resolvedSources | Sort-Object -Unique).Count -ne $resolvedSources.Count) {
        throw 'Runtime resource stability source paths must be distinct.'
    }

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $OutputDirectory =
        (Resolve-Path -LiteralPath $OutputDirectory).Path
    $runName = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
    $runDirectory = Join-Path $OutputDirectory $runName
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $stateDirectory = Join-Path $runDirectory 'state'
    New-Item -ItemType Directory -Path $stateDirectory | Out-Null
    $configurationPath = Join-Path $runDirectory 'workload-config.json'
    $statusPath = Join-Path $runDirectory 'workload-status.json'
    $startupErrorPath =
        Join-Path $stateDirectory `
            'runtime-resource-startup-error.txt'
    $samplesPath = Join-Path $runDirectory 'samples.csv'
    $resultPath = Join-Path $runDirectory 'result.json'
    $script:RuntimeResourceFailureResultPath = $resultPath
    $script:RuntimeResourceFailureEvidence = [ordered]@{
        result_json = $resultPath
        samples_csv = $samplesPath
        workload_configuration_json = $configurationPath
        workload_status_json = $statusPath
        startup_error_txt = $startupErrorPath
    }

    $configuration = [ordered]@{
        format_kind = 'specforge_runtime_resource_workload'
        schema_version = 1
        status_path = $statusPath
        source_paths = @($resolvedSources)
        warmup_cycles = $WarmupCycles
        measured_cycles = $MeasuredCycles
        burst_count = $BurstCount
        settle_ms = $SettleMs
        final_settle_ms = $FinalSettleMs
        poll_interval_ms = 25
        phase_timeout_ms = $PhaseTimeoutMs
        enable_graphics_debug = -not $DisableGraphicsDebug
        require_graphics_diagnostics = [bool]$RequireGraphicsDiagnostics
    }
    Write-JsonFile `
        -Path $configurationPath `
        -Value $configuration `
        -Depth 6

    Add-ResourceNativeMethods
    $temporaryEnvironment = @{
        SPECFORGE_RUNTIME_RESOURCE_WORKLOAD = $configurationPath
        SPECFORGE_RUNTIME_RESOURCE_STATE_DIR = $stateDirectory
        SPECFORGE_PROFILE = '1'
        SPECFORGE_PROFILE_DIR = $runDirectory
        SPECFORGE_PAN_PACING = $null
    }
    $previousEnvironment = Set-TemporaryEnvironment -Values $temporaryEnvironment
    $process = $null
    $timedOut = $false
    $samples = [System.Collections.Generic.List[object]]::new()
    $discardedTransitionSamples = 0
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $process = Start-Process `
            -FilePath $resolvedExecutable.Path `
            -WorkingDirectory $repoRoot `
            -PassThru
        $sampleIndex = 0
        while (-not $process.HasExited) {
            if ($stopwatch.Elapsed.TotalSeconds -gt $TimeoutSec) {
                $timedOut = $true
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                break
            }

            $statusBefore =
                Read-WorkloadStatus -Path $statusPath
            try {
                $resourceSample =
                    Get-ProcessResourceSample `
                    -Process $process `
                    -ElapsedMilliseconds $stopwatch.ElapsedMilliseconds `
                    -SampleIndex $sampleIndex
                $statusAfter =
                    Read-WorkloadStatus -Path $statusPath
                if (Test-WorkloadStatusSampleBoundary `
                        -Before $statusBefore `
                        -After $statusAfter) {
                    $samples.Add((
                        Add-WorkloadStatusToResourceSample `
                            -ResourceSample $resourceSample `
                            -Status $statusAfter))
                    ++$sampleIndex
                }
                else {
                    ++$discardedTransitionSamples
                }
            }
            catch {
                if (-not $process.HasExited) {
                    throw
                }
            }
            Start-Sleep -Milliseconds $SampleIntervalMs
        }
        $process.WaitForExit()
    }
    finally {
        if ($null -ne $process -and
            -not $process.HasExited) {
            Stop-Process `
                -Id $process.Id `
                -Force `
                -ErrorAction SilentlyContinue
            $process.WaitForExit()
        }
        Restore-TemporaryEnvironment -Previous $previousEnvironment
    }

    $samples | Export-Csv -LiteralPath $samplesPath -NoTypeInformation -Encoding UTF8
    $finalStatus = Read-WorkloadStatus -Path $statusPath
    if ($null -eq $finalStatus) {
        $startupError = if (
            Test-Path -LiteralPath $startupErrorPath -PathType Leaf) {
            (Get-Content -Raw -LiteralPath $startupErrorPath).Trim()
        }
        else {
            ''
        }
        $startupDetail = if (
            [string]::IsNullOrWhiteSpace($startupError)) {
            ''
        }
        else {
            " Startup diagnostic: $startupError"
        }
        throw (
            "SpecForge did not produce a readable workload status file: " +
            "$statusPath.$startupDetail")
    }
    $profileFiles = @(
        Get-ChildItem -LiteralPath $runDirectory -Filter 'specforge-profile-*.jsonl' |
            Sort-Object LastWriteTime
    )
    if ($profileFiles.Count -ne 1) {
        throw "Expected exactly one profile JSONL file, found $($profileFiles.Count)."
    }
    $profileEvidence = Read-ProfileEvidence -Path $profileFiles[0].FullName
    $thresholds = @{
        MaxPrivateBytesSlopeMiBPerCycle = $MaxPrivateBytesSlopeMiBPerCycle
        MaxPrivateBytesGrowthMiB = $MaxPrivateBytesGrowthMiB
        MaxHandleSlopePerCycle = $MaxHandleSlopePerCycle
        MaxHandleGrowth = $MaxHandleGrowth
        MaxThreadSlopePerCycle = $MaxThreadSlopePerCycle
        MaxThreadGrowth = $MaxThreadGrowth
        MaxGdiObjectSlopePerCycle = $MaxGdiObjectSlopePerCycle
        MaxGdiObjectGrowth = $MaxGdiObjectGrowth
        MaxUserObjectSlopePerCycle = $MaxUserObjectSlopePerCycle
        MaxUserObjectGrowth = $MaxUserObjectGrowth
    }
    $exitCode = if ($timedOut) { 124 } else { [int]$process.ExitCode }
    $analysis = Invoke-RuntimeResourceEvidenceAnalysis `
        -Samples @($samples) `
        -FinalStatus $finalStatus `
        -ProfileEvidence $profileEvidence `
        -ProcessExitCode $exitCode `
        -ExpectedSourceCount $resolvedSources.Count `
        -ExpectedSourcePaths @($resolvedSources) `
        -ExpectedWarmupCycles $WarmupCycles `
        -ExpectedMeasuredCycles $MeasuredCycles `
        -Thresholds $thresholds `
        -RequireGraphics ([bool]$RequireGraphicsDiagnostics)

    $sourceEvidence = @(
        foreach ($path in $resolvedSources) {
            $item = Get-Item -LiteralPath $path
            [pscustomobject][ordered]@{
                path = $item.FullName
                kind = if ($item.PSIsContainer) { 'directory' } else { 'file' }
                length = if ($item.PSIsContainer) { $null } else { [long]$item.Length }
                last_write_utc = $item.LastWriteTimeUtc.ToString('o')
            }
        }
    )
    $result = [pscustomobject][ordered]@{
        format_kind = 'specforge_runtime_resource_stability_result'
        schema_version = 1
        result = $analysis.result
        tier = $tier.name
        report_only = [bool]$ReportOnly
        started_utc = (Get-Date).ToUniversalTime().Subtract($stopwatch.Elapsed).ToString('o')
        finished_utc = (Get-Date).ToUniversalTime().ToString('o')
        duration_ms = $stopwatch.ElapsedMilliseconds
        process_id = if ($null -ne $process) { $process.Id } else { $null }
        process_exit_code = $exitCode
        executable = $resolvedExecutable.Path
        sources = $sourceEvidence
        configuration = $configuration
        thresholds = $thresholds
        checks = $analysis.checks
        sampling = [ordered]@{
            accepted_sample_count = $samples.Count
            discarded_transition_sample_count =
                $discardedTransitionSamples
        }
        evidence = [ordered]@{
            result_json = $resultPath
            samples_csv = $samplesPath
            workload_configuration_json = $configurationPath
            workload_status_json = $statusPath
            startup_error_txt = $startupErrorPath
            profile_jsonl = $profileFiles[0].FullName
        }
    }
    Write-JsonFile `
        -Path $resultPath `
        -Value $result `
        -Depth 12

    $failedChecks = @($analysis.checks | Where-Object status -eq 'FAIL')
    Write-Host "$($analysis.result): SpecForge runtime resource stability"
    foreach ($check in $analysis.checks) {
        Write-Host "[$($check.status)] $($check.name): $($check.detail)"
    }
    Write-Host "Evidence: $resultPath"

    if ($failedChecks.Count -gt 0 -and -not $ReportOnly) {
        return 1
    }
    return 0
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        $code = Invoke-RuntimeResourceStability
        exit $code
    }
    catch {
        if ($null -ne $script:RuntimeResourceFailureResultPath) {
            $failureResult = [pscustomobject][ordered]@{
                format_kind = 'specforge_runtime_resource_stability_result'
                schema_version = 1
                result = 'FAIL'
                finished_utc = (Get-Date).ToUniversalTime().ToString('o')
                error = $_.Exception.Message
                evidence = $script:RuntimeResourceFailureEvidence
            }
            Write-JsonFile `
                -Path $script:RuntimeResourceFailureResultPath `
                -Value $failureResult `
                -Depth 8
            Write-Host 'FAIL: SpecForge runtime resource stability'
            Write-Host "Evidence: $script:RuntimeResourceFailureResultPath"
        }
        Write-Error $_
        exit 1
    }
}
