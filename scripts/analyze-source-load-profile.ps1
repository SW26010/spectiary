[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Profile,
    [switch]$ReportOnly
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'profile-analysis-common.ps1')

function Add-Failure {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$Message
    )
    [void]$Failures.Add($Message)
}

function Get-RequiredJsonInt64 {
    param(
        [object]$Event,
        [string]$Field,
        [long]$Minimum,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    $value = Get-EventValue $Event $Field
    if (-not (Test-JsonNonNegativeInteger $value)) {
        Add-Failure $Failures "$Context.$Field must be a JSON integer >= $Minimum."
        return $null
    }
    try {
        $parsed = [long]$value
    } catch {
        Add-Failure $Failures "$Context.$Field is outside the Int64 range."
        return $null
    }
    if ($parsed -lt $Minimum) {
        Add-Failure $Failures "$Context.$Field must be a JSON integer >= $Minimum."
        return $null
    }
    return $parsed
}

function Get-RequiredDuration {
    param(
        [object]$Event,
        [string]$Field,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    $value = Get-EventValue $Event $Field
    if ($null -eq $value) {
        Add-Failure $Failures "$Context.$Field is required."
        return $null
    }
    try {
        $parsed = [double]$value
    } catch {
        Add-Failure $Failures "$Context.$Field must be numeric."
        return $null
    }
    if ([double]::IsNaN($parsed) -or [double]::IsInfinity($parsed) -or $parsed -lt 0.0) {
        Add-Failure $Failures "$Context.$Field must be finite and non-negative."
        return $null
    }
    return $parsed
}

function Test-RequiredBoolean {
    param(
        [object]$Event,
        [string]$Field,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    if ((Get-EventValue $Event $Field) -isnot [bool]) {
        Add-Failure $Failures "$Context.$Field must be a JSON boolean."
    }
}

function Test-TimeOrder {
    param(
        [long[]]$Values,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    for ($index = 0; $index -lt $Values.Count; $index++) {
        if ($Values[$index] -le 0) {
            Add-Failure $Failures "$Context contains a missing or non-positive timestamp."
            return
        }
        if ($index -gt 0 -and $Values[$index] -lt $Values[$index - 1]) {
            Add-Failure $Failures "$Context timestamps are not monotonic."
            return
        }
    }
}

function Test-DurationMatches {
    param(
        [object]$Event,
        [string]$Field,
        [long]$BeginNs,
        [long]$EndNs,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    $actual = Get-RequiredDuration $Event $Field $Context $Failures
    if ($null -eq $actual -or $BeginNs -le 0 -or $EndNs -lt $BeginNs) {
        return
    }
    $expected = [double]($EndNs - $BeginNs) / 1000000.0
    if ([Math]::Abs($actual - $expected) -gt 0.001) {
        Add-Failure $Failures (
            "$Context.$Field does not match its timestamps " +
            "({0:F4} vs {1:F4} ms)." -f $actual, $expected)
    }
}

function Test-AggregateDurationSum {
    param(
        [object]$Event,
        [object[]]$Children,
        [string]$Field,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    $actual = Get-RequiredDuration $Event $Field $Context $Failures
    if ($null -eq $actual) {
        return
    }
    $expected = 0.0
    foreach ($child in $Children) {
        $value = Get-EventValue $child $Field
        if ($null -eq $value) {
            return
        }
        $expected += [double]$value
    }
    if ([Math]::Abs($actual - $expected) -gt 0.001) {
        Add-Failure $Failures (
            "$Context.$Field does not match the child-event sum " +
            "({0:F4} vs {1:F4} ms)." -f $actual, $expected)
    }
}

function Format-Metric {
    param(
        [object[]]$Events,
        [string]$Field
    )

    $values = [double[]]@(
        $Events | ForEach-Object { [double](Get-EventValue $_ $Field) })
    $stats = Get-Stats $values
    return [pscustomobject]@{
        Metric = $Field
        Count = $stats.Count
        P50 = '{0:F3}' -f $stats.P50
        P95 = '{0:F3}' -f $stats.P95
        Max = '{0:F3}' -f $stats.Max
    }
}

$resolvedProfile = Resolve-Path -Path $Profile
$failures = [System.Collections.Generic.List[string]]::new()
$readResult = Read-ProfileEvents $resolvedProfile.Path -AllowPartial:$ReportOnly
$events = @($readResult.Events)
foreach ($failure in $readResult.Failures) {
    Add-Failure $failures $failure
}
if ($events.Count -eq 0) {
    Add-Failure $failures "$($resolvedProfile.Path): no profile events found"
}

$summaryCheck = Test-ProfileRecorderSummary $events
foreach ($failure in $summaryCheck.Failures) {
    Add-Failure $failures $failure
}

$sourceLoadEvents = @(
    $events | Where-Object { (Get-EventValue $_ 'event') -eq 'source_load_latency' })
$attemptEvents = @(
    $events | Where-Object { (Get-EventValue $_ 'event') -eq 'source_load_latency_attempt' })
$roundEvents = @(
    $events | Where-Object {
        (Get-EventValue $_ 'event') -eq 'source_load_latency_preparation_round'
    })

if ($sourceLoadEvents.Count -eq 0) {
    Add-Failure $failures 'No source_load_latency events were found.'
}

$allowedOutcomes = @('presented', 'failed', 'rejected', 'superseded')
$sourceLoadsById = @{}
foreach ($event in $sourceLoadEvents) {
    $context = 'source_load_latency'
    $sourceLoadId = Get-RequiredJsonInt64 $event 'source_load_id' 1 $context $failures
    $outcome = [string](Get-EventValue $event 'outcome' '')
    $requestKind = [string](Get-EventValue $event 'request_kind' '')
    $sourceKind = [string](Get-EventValue $event 'source_kind' '')
    if ($outcome -notin $allowedOutcomes) {
        Add-Failure $failures "$context.outcome '$outcome' is unsupported."
    }
    if ($requestKind -ne 'explicit_open') {
        Add-Failure $failures "$context.request_kind '$requestKind' is unsupported."
    }
    if ($sourceKind -notin @('file', 'folder', 'unknown')) {
        Add-Failure $failures "$context.source_kind '$sourceKind' is unsupported."
    }
    foreach ($field in @(
            'activation_frame', 'presentation_viewport_id', 'target_index',
            'attempt_count', 'preparation_round_count', 'listing_scan_count',
            'first_source_task_id', 'final_source_task_id')) {
        [void](Get-RequiredJsonInt64 $event $field 0 $context $failures)
    }
    Test-RequiredBoolean $event 'context_reused' $context $failures
    Test-RequiredBoolean $event 'workflow_reused' $context $failures
    [void](Get-RequiredDuration $event 'total_ms' $context $failures)
    if ($null -ne $sourceLoadId) {
        $key = [string]$sourceLoadId
        if ($sourceLoadsById.ContainsKey($key)) {
            Add-Failure $failures "source_load_id $sourceLoadId is duplicated."
        } else {
            $sourceLoadsById[$key] = $event
        }
    }
}

$attemptsBySourceLoadId = @{}
foreach ($attempt in $attemptEvents) {
    $sourceLoadId = Get-RequiredJsonInt64 `
        $attempt 'source_load_id' 1 'source_load_latency_attempt' $failures
    if ($null -eq $sourceLoadId) {
        continue
    }
    $key = [string]$sourceLoadId
    if (-not $sourceLoadsById.ContainsKey($key)) {
        Add-Failure $failures (
            "source_load_latency_attempt references unknown source_load_id $sourceLoadId.")
        continue
    }
    if (-not $attemptsBySourceLoadId.ContainsKey($key)) {
        $attemptsBySourceLoadId[$key] =
            [System.Collections.Generic.List[object]]::new()
    }
    [void]$attemptsBySourceLoadId[$key].Add($attempt)
}

$roundsByAttempt = @{}
foreach ($round in $roundEvents) {
    $sourceLoadId = Get-RequiredJsonInt64 `
        $round 'source_load_id' 1 'source_load_latency_preparation_round' $failures
    $attemptIndex = Get-RequiredJsonInt64 `
        $round 'attempt_index' 0 'source_load_latency_preparation_round' $failures
    if ($null -eq $sourceLoadId -or $null -eq $attemptIndex) {
        continue
    }
    if (-not $sourceLoadsById.ContainsKey([string]$sourceLoadId)) {
        Add-Failure $failures (
            "source_load_latency_preparation_round references unknown " +
            "source_load_id $sourceLoadId.")
        continue
    }
    $key = "$sourceLoadId`:$attemptIndex"
    if (-not $roundsByAttempt.ContainsKey($key)) {
        $roundsByAttempt[$key] = [System.Collections.Generic.List[object]]::new()
    }
    [void]$roundsByAttempt[$key].Add($round)
}

$presentedEvents = @(
    $sourceLoadEvents | Where-Object { (Get-EventValue $_ 'outcome') -eq 'presented' })
if ($presentedEvents.Count -eq 0) {
    Add-Failure $failures 'No presented source_load_latency events were found.'
}

$summaryDurationFields = @(
    'accept_to_enqueue_ms',
    'queue_wait_ms',
    'source_inspection_ms',
    'decode_ms',
    'context_prepare_ms',
    'source_revalidation_ms',
    'workflow_prepare_ms',
    'completion_ready_ms',
    'ordered_publish_wait_ms',
    'completion_service_wait_ms',
    'retarget_gap_ms',
    'activation_ms',
    'ui_update_ms',
    'ui_to_present_ms',
    'total_ms'
)
$attemptAggregateFields = @(
    'queue_wait_ms',
    'source_inspection_ms',
    'decode_ms',
    'context_prepare_ms',
    'source_revalidation_ms',
    'workflow_prepare_ms',
    'completion_ready_ms',
    'ordered_publish_wait_ms',
    'completion_service_wait_ms'
)
$attemptDurationFields = @($attemptAggregateFields) + @('attempt_total_ms')
$roundDurationFields = @(
    'source_inspection_ms',
    'decode_ms',
    'context_prepare_ms',
    'source_revalidation_ms',
    'round_total_ms'
)
$validPresentedEvents = [System.Collections.Generic.List[object]]::new()

foreach ($event in $presentedEvents) {
    $failureCountBefore = $failures.Count
    $sourceLoadId = Get-RequiredJsonInt64 `
        $event 'source_load_id' 1 'source_load_latency' $failures
    if ($null -eq $sourceLoadId) {
        continue
    }
    $context = "source_load_latency[$sourceLoadId]"
    $activationFrame = Get-RequiredJsonInt64 `
        $event 'activation_frame' 1 $context $failures
    $viewportId = Get-RequiredJsonInt64 `
        $event 'presentation_viewport_id' 1 $context $failures
    $targetIndex = Get-RequiredJsonInt64 `
        $event 'target_index' 0 $context $failures
    $attemptCount = Get-RequiredJsonInt64 `
        $event 'attempt_count' 1 $context $failures
    $roundCount = Get-RequiredJsonInt64 `
        $event 'preparation_round_count' 1 $context $failures
    $listingScanCount = Get-RequiredJsonInt64 `
        $event 'listing_scan_count' 0 $context $failures
    $firstTaskId = Get-RequiredJsonInt64 `
        $event 'first_source_task_id' 1 $context $failures
    $finalTaskId = Get-RequiredJsonInt64 `
        $event 'final_source_task_id' 1 $context $failures
    $acceptedNs = Get-RequiredJsonInt64 `
        $event 'accepted_steady_ns' 1 $context $failures
    $firstEnqueuedNs = Get-RequiredJsonInt64 `
        $event 'first_enqueued_steady_ns' 1 $context $failures
    $finalDrainedNs = Get-RequiredJsonInt64 `
        $event 'final_completion_drained_steady_ns' 1 $context $failures
    $activatedNs = Get-RequiredJsonInt64 `
        $event 'snapshot_activated_steady_ns' 1 $context $failures
    $uiUpdatedNs = Get-RequiredJsonInt64 `
        $event 'ui_updated_steady_ns' 1 $context $failures
    $presentNs = Get-RequiredJsonInt64 `
        $event 'first_present_steady_ns' 1 $context $failures
    $terminalNs = Get-RequiredJsonInt64 `
        $event 'terminal_steady_ns' 1 $context $failures
    [void]$activationFrame
    [void]$viewportId
    Test-TimeOrder (
        [long[]]@(
            $acceptedNs, $firstEnqueuedNs, $finalDrainedNs,
            $activatedNs, $uiUpdatedNs, $presentNs)) `
        "$context end-to-end" $failures
    if ($terminalNs -ne $presentNs) {
        Add-Failure $failures "$context.terminal_steady_ns must equal first Present."
    }
    if ((Get-EventValue $event 'source_kind') -notin @('file', 'folder')) {
        Add-Failure $failures "$context.source_kind must identify the presented source."
    }
    foreach ($field in $summaryDurationFields) {
        [void](Get-RequiredDuration $event $field $context $failures)
    }
    Test-DurationMatches `
        $event 'accept_to_enqueue_ms' $acceptedNs $firstEnqueuedNs $context $failures
    Test-DurationMatches `
        $event 'activation_ms' $finalDrainedNs $activatedNs $context $failures
    Test-DurationMatches `
        $event 'ui_update_ms' $activatedNs $uiUpdatedNs $context $failures
    Test-DurationMatches `
        $event 'ui_to_present_ms' $uiUpdatedNs $presentNs $context $failures
    Test-DurationMatches $event 'total_ms' $acceptedNs $presentNs $context $failures

    $attempts = @()
    $sourceLoadKey = [string]$sourceLoadId
    if ($attemptsBySourceLoadId.ContainsKey($sourceLoadKey)) {
        $attempts = @(
            $attemptsBySourceLoadId[$sourceLoadKey] |
                Sort-Object { [long](Get-EventValue $_ 'attempt_index') })
    }
    if ($null -ne $attemptCount -and $attempts.Count -ne $attemptCount) {
        Add-Failure $failures (
            "$context expected $attemptCount attempt events, found $($attempts.Count).")
    }

    $previousDrainedNs = 0L
    $observedRoundCount = 0L
    $observedListingScanCount = 0L
    for ($index = 0; $index -lt $attempts.Count; $index++) {
        $attempt = $attempts[$index]
        $attemptContext = "$context.attempt[$index]"
        $attemptIndex = Get-RequiredJsonInt64 `
            $attempt 'attempt_index' 0 $attemptContext $failures
        if ($null -ne $attemptIndex -and $attemptIndex -ne $index) {
            Add-Failure $failures (
                "$attemptContext.attempt_index must be contiguous and equal to $index.")
        }
        $attemptTarget = Get-RequiredJsonInt64 `
            $attempt 'target_index' 0 $attemptContext $failures
        $taskId = Get-RequiredJsonInt64 `
            $attempt 'source_task_id' 1 $attemptContext $failures
        $attemptRoundCount = Get-RequiredJsonInt64 `
            $attempt 'preparation_round_count' 1 $attemptContext $failures
        $sourceKind = [string](Get-EventValue $attempt 'source_kind' '')
        if ($sourceKind -notin @('file', 'folder')) {
            Add-Failure $failures "$attemptContext.source_kind '$sourceKind' is unsupported."
        }
        Test-RequiredBoolean $attempt 'workflow_reused' $attemptContext $failures
        Test-RequiredBoolean $attempt 'context_reused' $attemptContext $failures

        $enqueuedNs = Get-RequiredJsonInt64 `
            $attempt 'load_enqueued_steady_ns' 1 $attemptContext $failures
        $workerNs = Get-RequiredJsonInt64 `
            $attempt 'worker_started_steady_ns' 1 $attemptContext $failures
        $loadStartNs = Get-RequiredJsonInt64 `
            $attempt 'snapshot_load_started_steady_ns' 1 $attemptContext $failures
        $loadFinishNs = Get-RequiredJsonInt64 `
            $attempt 'snapshot_load_finished_steady_ns' 1 $attemptContext $failures
        $contextNs = Get-RequiredJsonInt64 `
            $attempt 'context_prepared_steady_ns' 1 $attemptContext $failures
        $revalidatedNs = Get-RequiredJsonInt64 `
            $attempt 'source_revalidated_steady_ns' 1 $attemptContext $failures
        $preparedNs = Get-RequiredJsonInt64 `
            $attempt 'worker_prepared_steady_ns' 1 $attemptContext $failures
        $readyNs = Get-RequiredJsonInt64 `
            $attempt 'completion_ready_steady_ns' 1 $attemptContext $failures
        $publishedNs = Get-RequiredJsonInt64 `
            $attempt 'completion_published_steady_ns' 1 $attemptContext $failures
        $drainedNs = Get-RequiredJsonInt64 `
            $attempt 'completion_drained_steady_ns' 1 $attemptContext $failures
        Test-TimeOrder (
            [long[]]@(
                $enqueuedNs, $workerNs, $loadStartNs, $loadFinishNs, $contextNs,
                $revalidatedNs, $preparedNs, $readyNs, $publishedNs, $drainedNs)) `
            "$attemptContext phase" $failures
        if ($index -eq 0 -and $enqueuedNs -ne $firstEnqueuedNs) {
            Add-Failure $failures (
                "$attemptContext enqueue does not match first_enqueued_steady_ns.")
        }
        if ($index -gt 0 -and $enqueuedNs -lt $previousDrainedNs) {
            Add-Failure $failures "$attemptContext overlaps the previous attempt."
        }
        foreach ($field in $attemptDurationFields) {
            [void](Get-RequiredDuration $attempt $field $attemptContext $failures)
        }
        Test-DurationMatches `
            $attempt 'queue_wait_ms' $enqueuedNs $workerNs $attemptContext $failures
        Test-DurationMatches `
            $attempt 'workflow_prepare_ms' $revalidatedNs $preparedNs $attemptContext $failures
        Test-DurationMatches `
            $attempt 'completion_ready_ms' $preparedNs $readyNs $attemptContext $failures
        Test-DurationMatches `
            $attempt 'ordered_publish_wait_ms' $readyNs $publishedNs $attemptContext $failures
        Test-DurationMatches `
            $attempt 'completion_service_wait_ms' $publishedNs $drainedNs $attemptContext $failures
        Test-DurationMatches `
            $attempt 'attempt_total_ms' $enqueuedNs $drainedNs $attemptContext $failures

        $rounds = @()
        $roundKey = "$sourceLoadId`:$attemptIndex"
        if ($roundsByAttempt.ContainsKey($roundKey)) {
            $rounds = @(
                $roundsByAttempt[$roundKey] |
                    Sort-Object {
                        [long](Get-EventValue $_ 'preparation_round_index')
                    })
        }
        if ($null -ne $attemptRoundCount -and $rounds.Count -ne $attemptRoundCount) {
            Add-Failure $failures (
                "$attemptContext expected $attemptRoundCount preparation rounds, " +
                "found $($rounds.Count).")
        }
        $observedRoundCount += $rounds.Count
        $previousRoundEndNs = 0L
        for ($roundIndex = 0; $roundIndex -lt $rounds.Count; $roundIndex++) {
            $round = $rounds[$roundIndex]
            $roundContext = "$attemptContext.preparation_round[$roundIndex]"
            $reportedRoundIndex = Get-RequiredJsonInt64 `
                $round 'preparation_round_index' 0 $roundContext $failures
            if ($null -ne $reportedRoundIndex -and $reportedRoundIndex -ne $roundIndex) {
                Add-Failure $failures (
                    "$roundContext.preparation_round_index must be contiguous " +
                    "and equal to $roundIndex.")
            }
            $roundTarget = Get-RequiredJsonInt64 `
                $round 'target_index' 0 $roundContext $failures
            $roundTaskId = Get-RequiredJsonInt64 `
                $round 'source_task_id' 1 $roundContext $failures
            if ($roundTarget -ne $attemptTarget) {
                Add-Failure $failures "$roundContext.target_index does not match its attempt."
            }
            if ($roundTaskId -ne $taskId) {
                Add-Failure $failures "$roundContext.source_task_id does not match its attempt."
            }
            $roundSourceKind = [string](Get-EventValue $round 'source_kind' '')
            if ($roundSourceKind -ne $sourceKind) {
                Add-Failure $failures "$roundContext.source_kind does not match its attempt."
            }
            foreach ($field in @(
                    'hint_present', 'generation_current_at_start',
                    'listing_scan_performed', 'context_reused',
                    'revalidation_succeeded')) {
                Test-RequiredBoolean $round $field $roundContext $failures
            }
            $hintPresent = Get-EventValue $round 'hint_present'
            $generationCurrent = Get-EventValue $round 'generation_current_at_start'
            $listingScanned = Get-EventValue $round 'listing_scan_performed'
            if ($listingScanned -eq $true) {
                $observedListingScanCount++
            }
            if ($roundSourceKind -eq 'file' -and
                ($hintPresent -eq $true -or
                 $generationCurrent -eq $true -or
                 $listingScanned -eq $true)) {
                Add-Failure $failures (
                    "$roundContext file rounds cannot report folder diagnostics.")
            }
            if ($generationCurrent -eq $true -and $hintPresent -ne $true) {
                Add-Failure $failures (
                    "$roundContext cannot report a current generation without a hint.")
            }
            $roundSucceeded = Get-EventValue $round 'revalidation_succeeded'
            if ($roundIndex -lt $rounds.Count - 1 -and $roundSucceeded -eq $true) {
                Add-Failure $failures (
                    "$roundContext cannot succeed before a later retry.")
            }
            if ($roundIndex -eq $rounds.Count - 1 -and $roundSucceeded -ne $true) {
                Add-Failure $failures "$roundContext final revalidation must succeed."
            }

            $roundStartedNs = Get-RequiredJsonInt64 `
                $round 'preparation_started_steady_ns' 1 $roundContext $failures
            $roundLoadStartNs = Get-RequiredJsonInt64 `
                $round 'snapshot_load_started_steady_ns' 1 $roundContext $failures
            $roundLoadFinishNs = Get-RequiredJsonInt64 `
                $round 'snapshot_load_finished_steady_ns' 1 $roundContext $failures
            $roundContextNs = Get-RequiredJsonInt64 `
                $round 'context_prepared_steady_ns' 1 $roundContext $failures
            $roundRevalidatedNs = Get-RequiredJsonInt64 `
                $round 'source_revalidated_steady_ns' 1 $roundContext $failures
            Test-TimeOrder (
                [long[]]@(
                    $roundStartedNs, $roundLoadStartNs, $roundLoadFinishNs,
                    $roundContextNs, $roundRevalidatedNs)) `
                "$roundContext phase" $failures
            if ($roundIndex -eq 0 -and $roundStartedNs -lt $workerNs) {
                Add-Failure $failures "$roundContext starts before its worker."
            }
            if ($roundIndex -gt 0 -and $roundStartedNs -lt $previousRoundEndNs) {
                Add-Failure $failures "$roundContext overlaps the previous round."
            }
            foreach ($field in $roundDurationFields) {
                [void](Get-RequiredDuration $round $field $roundContext $failures)
            }
            Test-DurationMatches `
                $round 'source_inspection_ms' `
                $roundStartedNs $roundLoadStartNs $roundContext $failures
            Test-DurationMatches `
                $round 'decode_ms' `
                $roundLoadStartNs $roundLoadFinishNs $roundContext $failures
            Test-DurationMatches `
                $round 'context_prepare_ms' `
                $roundLoadFinishNs $roundContextNs $roundContext $failures
            Test-DurationMatches `
                $round 'source_revalidation_ms' `
                $roundContextNs $roundRevalidatedNs $roundContext $failures
            Test-DurationMatches `
                $round 'round_total_ms' `
                $roundStartedNs $roundRevalidatedNs $roundContext $failures
            $previousRoundEndNs = $roundRevalidatedNs
        }

        if ($rounds.Count -gt 0) {
            if ([long](Get-EventValue $rounds[0] 'preparation_started_steady_ns') -ne $workerNs -or
                [long](Get-EventValue $rounds[0] 'snapshot_load_started_steady_ns') -ne $loadStartNs -or
                [long](Get-EventValue $rounds[-1] 'snapshot_load_finished_steady_ns') -ne $loadFinishNs -or
                [long](Get-EventValue $rounds[-1] 'context_prepared_steady_ns') -ne $contextNs -or
                [long](Get-EventValue $rounds[-1] 'source_revalidated_steady_ns') -ne $revalidatedNs) {
                Add-Failure $failures (
                    "$attemptContext boundary timestamps do not match its rounds.")
            }
            foreach ($field in @(
                    'source_inspection_ms', 'decode_ms',
                    'context_prepare_ms', 'source_revalidation_ms')) {
                Test-AggregateDurationSum `
                    $attempt $rounds $field $attemptContext $failures
            }
            if ((Get-EventValue $attempt 'context_reused') -ne
                (Get-EventValue $rounds[-1] 'context_reused')) {
                Add-Failure $failures (
                    "$attemptContext.context_reused does not match its final round.")
            }
        }
        $previousDrainedNs = $drainedNs
    }

    if ($attempts.Count -gt 0) {
        $firstAttempt = $attempts[0]
        $finalAttempt = $attempts[-1]
        if ([long](Get-EventValue $finalAttempt 'completion_drained_steady_ns') -ne
            $finalDrainedNs) {
            Add-Failure $failures (
                "$context final attempt drain does not match the summary.")
        }
        if ([long](Get-EventValue $firstAttempt 'source_task_id') -ne $firstTaskId -or
            [long](Get-EventValue $finalAttempt 'source_task_id') -ne $finalTaskId) {
            Add-Failure $failures "$context task-id boundaries do not match its attempts."
        }
        if ([long](Get-EventValue $finalAttempt 'target_index') -ne $targetIndex) {
            Add-Failure $failures "$context final target does not match target_index."
        }
        if ((Get-EventValue $finalAttempt 'source_kind') -ne
            (Get-EventValue $event 'source_kind')) {
            Add-Failure $failures "$context source_kind does not match its final attempt."
        }
        foreach ($field in @('context_reused', 'workflow_reused')) {
            if ((Get-EventValue $finalAttempt $field) -ne
                (Get-EventValue $event $field)) {
                Add-Failure $failures "$context.$field does not match its final attempt."
            }
        }
        foreach ($field in $attemptAggregateFields) {
            Test-AggregateDurationSum $event $attempts $field $context $failures
        }
        $expectedRetargetGapMs = 0.0
        for ($index = 1; $index -lt $attempts.Count; $index++) {
            $previousAttemptDrainedNs =
                [long](Get-EventValue $attempts[$index - 1] 'completion_drained_steady_ns')
            $nextAttemptEnqueuedNs =
                [long](Get-EventValue $attempts[$index] 'load_enqueued_steady_ns')
            if ($nextAttemptEnqueuedNs -ge $previousAttemptDrainedNs) {
                $expectedRetargetGapMs +=
                    [double]($nextAttemptEnqueuedNs - $previousAttemptDrainedNs) /
                    1000000.0
            }
        }
        $actualRetargetGapMs =
            Get-RequiredDuration $event 'retarget_gap_ms' $context $failures
        if ($null -ne $actualRetargetGapMs -and
            [Math]::Abs($actualRetargetGapMs - $expectedRetargetGapMs) -gt 0.001) {
            Add-Failure $failures (
                "$context.retarget_gap_ms does not match attempt gaps " +
                "({0:F4} vs {1:F4} ms)." -f
                    $actualRetargetGapMs, $expectedRetargetGapMs)
        }
    }
    if ($null -ne $roundCount -and $observedRoundCount -ne $roundCount) {
        Add-Failure $failures (
            "$context expected $roundCount total preparation rounds, " +
            "found $observedRoundCount.")
    }
    if ($null -ne $listingScanCount -and
        $observedListingScanCount -ne $listingScanCount) {
        Add-Failure $failures (
            "$context expected $listingScanCount listing scans, " +
            "found $observedListingScanCount.")
    }
    if ($failures.Count -eq $failureCountBefore) {
        [void]$validPresentedEvents.Add($event)
    }
}

Write-Host "Profile: $($resolvedProfile.Path)"
Write-Host "Recording completeness: $($summaryCheck.Status)"
Write-Host (
    "Source-load events: $($sourceLoadEvents.Count); " +
    "presented: $($presentedEvents.Count); " +
    "valid presented: $($validPresentedEvents.Count)")
if ($sourceLoadEvents.Count -gt 0) {
    Write-Host 'Outcomes:'
    $sourceLoadEvents |
        Group-Object { [string](Get-EventValue $_ 'outcome' 'unknown') } |
        Sort-Object Name |
        ForEach-Object { Write-Host ('  {0}: {1}' -f $_.Name, $_.Count) }
}

foreach ($group in @(
        $validPresentedEvents |
            Group-Object { [string](Get-EventValue $_ 'source_kind') } |
            Sort-Object Name)) {
    Write-Host ''
    Write-Host "Source kind: $($group.Name); count: $($group.Count)"
    @(
        $summaryDurationFields |
            ForEach-Object { Format-Metric ([object[]]$group.Group) $_ }) |
        Format-Table -AutoSize
}

if ($failures.Count -eq 0) {
    Write-Host 'Result: PASS'
} else {
    Write-Host 'Result: FAIL'
    foreach ($failure in $failures) {
        Write-Host " - $failure"
    }
    if (-not $ReportOnly) {
        exit 1
    }
}
