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

function Get-RequiredInt64 {
    param(
        [object]$Event,
        [string]$Field,
        [long]$Minimum,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    $value = Get-EventValue $Event $Field
    $parsed = 0L
    if ($null -eq $value -or
        -not [long]::TryParse([string]$value, [ref]$parsed) -or
        $parsed -lt $Minimum) {
        Add-Failure $Failures "$Context.$Field must be an integer >= $Minimum."
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
    }
    catch {
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

    $value = Get-EventValue $Event $Field
    if ($value -isnot [bool]) {
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
        Add-Failure $Failures ("$Context.$Field does not match its timestamps ({0:F4} vs {1:F4} ms)." -f $actual, $expected)
    }
}

function Test-AggregateDurationSum {
    param(
        [object]$Event,
        [object[]]$Attempts,
        [string]$Field,
        [string]$Context,
        [System.Collections.Generic.List[string]]$Failures
    )

    $actualValue = Get-EventValue $Event $Field
    if ($null -eq $actualValue) {
        return
    }
    $expected = 0.0
    foreach ($attempt in $Attempts) {
        $attemptValue = Get-EventValue $attempt $Field
        if ($null -eq $attemptValue) {
            return
        }
        $expected += [double]$attemptValue
    }
    $actual = [double]$actualValue
    if ([Math]::Abs($actual - $expected) -gt 0.001) {
        Add-Failure $Failures ("$Context.$Field does not match the attempt sum ({0:F4} vs {1:F4} ms)." -f $actual, $expected)
    }
}

function Get-MetricValues {
    param(
        [object[]]$Events,
        [string]$Field
    )
    return [double[]]@($Events | ForEach-Object { [double](Get-EventValue $_ $Field) })
}

function Format-Metric {
    param(
        [object[]]$Events,
        [string]$Field
    )
    $stats = Get-Stats (Get-MetricValues $Events $Field)
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

$navigationEvents = @($events | Where-Object { (Get-EventValue $_ 'event') -eq 'navigation_latency' })
$attemptEvents = @($events | Where-Object { (Get-EventValue $_ 'event') -eq 'navigation_latency_attempt' })
$preparationRoundEvents = @(
    $events | Where-Object { (Get-EventValue $_ 'event') -eq 'navigation_latency_preparation_round' })
$folderDiagnosticFields = @(
    'hint_present',
    'generation_current_at_start',
    'listing_scan_performed'
)
$hasFolderListingDiagnostics = @(
    $preparationRoundEvents |
        Where-Object {
            foreach ($field in $folderDiagnosticFields) {
                if ($null -ne $_.PSObject.Properties[$field]) {
                    return $true
                }
            }
            return $false
        }).Count -gt 0
$hasContextReuseDiagnostics = @(
    @($attemptEvents) + @($preparationRoundEvents) |
        Where-Object {
            $null -ne $_.PSObject.Properties['context_reused']
        }).Count -gt 0
if ($navigationEvents.Count -eq 0) {
    Add-Failure $failures 'No navigation_latency events were found.'
}

$allowedOutcomes = @('presented', 'failed', 'rejected', 'superseded', 'coalesced')
$allowedInputKinds = @('keyboard_previous', 'keyboard_next', 'ui_previous', 'ui_next', 'auto_advance')
$navigationById = @{}
foreach ($event in $navigationEvents) {
    $context = 'navigation_latency'
    $navigationId = Get-RequiredInt64 $event 'navigation_id' 1 $context $failures
    $outcome = [string](Get-EventValue $event 'outcome' '')
    $inputKind = [string](Get-EventValue $event 'input_kind' '')
    if ($outcome -notin $allowedOutcomes) {
        Add-Failure $failures "$context.outcome '$outcome' is unsupported."
    }
    if ($inputKind -notin $allowedInputKinds) {
        Add-Failure $failures "$context.input_kind '$inputKind' is unsupported."
    }
    [void](Get-RequiredInt64 $event 'from_index' 0 $context $failures)
    [void](Get-RequiredInt64 $event 'target_index' 0 $context $failures)
    [void](Get-RequiredInt64 $event 'attempt_count' 0 $context $failures)
    [void](Get-RequiredDuration $event 'total_ms' $context $failures)
    Test-RequiredBoolean $event 'cache_hit' $context $failures
    if ($null -ne $navigationId) {
        $key = [string]$navigationId
        if ($navigationById.ContainsKey($key)) {
            Add-Failure $failures "navigation_id $navigationId is duplicated."
        } else {
            $navigationById[$key] = $event
        }
    }
}

$attemptsByNavigationId = @{}
foreach ($attempt in $attemptEvents) {
    $navigationId = Get-RequiredInt64 $attempt 'navigation_id' 1 'navigation_latency_attempt' $failures
    if ($null -eq $navigationId) {
        continue
    }
    $key = [string]$navigationId
    if (-not $navigationById.ContainsKey($key)) {
        Add-Failure $failures "navigation_latency_attempt references unknown navigation_id $navigationId."
        continue
    }
    if (-not $attemptsByNavigationId.ContainsKey($key)) {
        $attemptsByNavigationId[$key] = [System.Collections.Generic.List[object]]::new()
    }
    [void]$attemptsByNavigationId[$key].Add($attempt)
}

$preparationRoundsByAttempt = @{}
foreach ($round in $preparationRoundEvents) {
    $navigationId = Get-RequiredInt64 $round 'navigation_id' 1 'navigation_latency_preparation_round' $failures
    $attemptIndex = Get-RequiredInt64 $round 'attempt_index' 0 'navigation_latency_preparation_round' $failures
    if ($null -eq $navigationId -or $null -eq $attemptIndex) {
        continue
    }
    $key = "$navigationId`:$attemptIndex"
    if (-not $preparationRoundsByAttempt.ContainsKey($key)) {
        $preparationRoundsByAttempt[$key] = [System.Collections.Generic.List[object]]::new()
    }
    [void]$preparationRoundsByAttempt[$key].Add($round)
}

$presentedEvents = @($navigationEvents | Where-Object { (Get-EventValue $_ 'outcome') -eq 'presented' })
if ($presentedEvents.Count -eq 0) {
    Add-Failure $failures 'No presented navigation_latency events were found.'
}
$validPresentedEvents = [System.Collections.Generic.List[object]]::new()
$aggregateDurationFields = @(
    'input_to_request_ms',
    'target_resolution_ms',
    'enqueue_ms',
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
$attemptDurationFields = @(
    'queue_wait_ms',
    'source_inspection_ms',
    'decode_ms',
    'context_prepare_ms',
    'source_revalidation_ms',
    'workflow_prepare_ms',
    'completion_ready_ms',
    'ordered_publish_wait_ms',
    'completion_service_wait_ms',
    'attempt_total_ms'
)

foreach ($event in $presentedEvents) {
    $failureCountBefore = $failures.Count
    $navigationId = Get-RequiredInt64 $event 'navigation_id' 1 'navigation_latency' $failures
    if ($null -eq $navigationId) {
        continue
    }
    $context = "navigation_latency[$navigationId]"
    $activationFrame = Get-RequiredInt64 $event 'activation_frame' 1 $context $failures
    $viewportId = Get-RequiredInt64 $event 'presentation_viewport_id' 1 $context $failures
    $targetIndex = Get-RequiredInt64 $event 'target_index' 0 $context $failures
    $attemptCount = Get-RequiredInt64 $event 'attempt_count' 1 $context $failures
    $inputNs = Get-RequiredInt64 $event 'input_steady_ns' 1 $context $failures
    $requestedNs = Get-RequiredInt64 $event 'requested_steady_ns' 1 $context $failures
    $targetResolvedNs = Get-RequiredInt64 $event 'target_resolved_steady_ns' 1 $context $failures
    $activatedNs = Get-RequiredInt64 $event 'snapshot_activated_steady_ns' 1 $context $failures
    $uiUpdatedNs = Get-RequiredInt64 $event 'ui_updated_steady_ns' 1 $context $failures
    $presentNs = Get-RequiredInt64 $event 'first_present_steady_ns' 1 $context $failures
    [void]$activationFrame
    [void]$viewportId
    Test-TimeOrder ([long[]]@($inputNs, $requestedNs, $targetResolvedNs)) "$context input/request" $failures
    Test-TimeOrder ([long[]]@($activatedNs, $uiUpdatedNs, $presentNs)) "$context activation/present" $failures

    foreach ($field in $aggregateDurationFields) {
        [void](Get-RequiredDuration $event $field $context $failures)
    }
    Test-DurationMatches $event 'input_to_request_ms' $inputNs $requestedNs $context $failures
    Test-DurationMatches $event 'target_resolution_ms' $requestedNs $targetResolvedNs $context $failures
    Test-DurationMatches $event 'ui_update_ms' $activatedNs $uiUpdatedNs $context $failures
    Test-DurationMatches $event 'ui_to_present_ms' $uiUpdatedNs $presentNs $context $failures
    Test-DurationMatches $event 'total_ms' $inputNs $presentNs $context $failures

    $key = [string]$navigationId
    $attempts = @()
    if ($attemptsByNavigationId.ContainsKey($key)) {
        $attempts = @(
            $attemptsByNavigationId[$key] |
                Sort-Object { [long](Get-EventValue $_ 'attempt_index') })
    }
    if ($null -ne $attemptCount -and $attempts.Count -ne $attemptCount) {
        Add-Failure $failures "$context expected $attemptCount attempt events, found $($attempts.Count)."
    }

    $previousDrainedNs = 0L
    for ($index = 0; $index -lt $attempts.Count; $index++) {
        $attempt = $attempts[$index]
        $attemptContext = "$context.attempt[$index]"
        $attemptIndex = Get-RequiredInt64 $attempt 'attempt_index' 0 $attemptContext $failures
        if ($null -ne $attemptIndex -and $attemptIndex -ne $index) {
            Add-Failure $failures "$attemptContext.attempt_index must be contiguous and equal to $index."
        }
        $attemptTargetIndex = Get-RequiredInt64 $attempt 'target_index' 0 $attemptContext $failures
        $sourceTaskId = Get-RequiredInt64 $attempt 'source_task_id' 1 $attemptContext $failures
        $sourceKind = [string](Get-EventValue $attempt 'source_kind' '')
        if ($sourceKind -notin @('file', 'folder')) {
            Add-Failure $failures "$attemptContext.source_kind '$sourceKind' is unsupported."
        }
        Test-RequiredBoolean $attempt 'workflow_reused' $attemptContext $failures
        if ($hasContextReuseDiagnostics) {
            Test-RequiredBoolean $attempt 'context_reused' $attemptContext $failures
        }

        $enqueuedNs = Get-RequiredInt64 $attempt 'load_enqueued_steady_ns' 1 $attemptContext $failures
        $workerNs = Get-RequiredInt64 $attempt 'worker_started_steady_ns' 1 $attemptContext $failures
        $loadStartNs = Get-RequiredInt64 $attempt 'snapshot_load_started_steady_ns' 1 $attemptContext $failures
        $loadFinishNs = Get-RequiredInt64 $attempt 'snapshot_load_finished_steady_ns' 1 $attemptContext $failures
        $contextNs = Get-RequiredInt64 $attempt 'context_prepared_steady_ns' 1 $attemptContext $failures
        $revalidatedNs = Get-RequiredInt64 $attempt 'source_revalidated_steady_ns' 1 $attemptContext $failures
        $preparedNs = Get-RequiredInt64 $attempt 'worker_prepared_steady_ns' 1 $attemptContext $failures
        $readyNs = Get-RequiredInt64 $attempt 'completion_ready_steady_ns' 1 $attemptContext $failures
        $publishedNs = Get-RequiredInt64 $attempt 'completion_published_steady_ns' 1 $attemptContext $failures
        $drainedNs = Get-RequiredInt64 $attempt 'completion_drained_steady_ns' 1 $attemptContext $failures
        $attemptTimes = [long[]]@(
            $enqueuedNs, $workerNs, $loadStartNs, $loadFinishNs, $contextNs,
            $revalidatedNs, $preparedNs, $readyNs, $publishedNs, $drainedNs)
        Test-TimeOrder $attemptTimes "$attemptContext phase" $failures
        if ($index -eq 0 -and $enqueuedNs -lt $targetResolvedNs) {
            Add-Failure $failures "$attemptContext was enqueued before target resolution."
        }
        if ($index -gt 0 -and $enqueuedNs -lt $previousDrainedNs) {
            Add-Failure $failures "$attemptContext overlaps the previous completed attempt."
        }

        foreach ($field in $attemptDurationFields) {
            [void](Get-RequiredDuration $attempt $field $attemptContext $failures)
        }
        Test-DurationMatches $attempt 'queue_wait_ms' $enqueuedNs $workerNs $attemptContext $failures
        $roundCountProperty = $attempt.PSObject.Properties['preparation_round_count']
        if ($null -eq $roundCountProperty) {
            Test-DurationMatches $attempt 'source_inspection_ms' $workerNs $loadStartNs $attemptContext $failures
            Test-DurationMatches $attempt 'decode_ms' $loadStartNs $loadFinishNs $attemptContext $failures
            Test-DurationMatches $attempt 'context_prepare_ms' $loadFinishNs $contextNs $attemptContext $failures
            Test-DurationMatches $attempt 'source_revalidation_ms' $contextNs $revalidatedNs $attemptContext $failures
        } else {
            $roundCount = Get-RequiredInt64 $attempt 'preparation_round_count' 1 $attemptContext $failures
            $roundKey = "$navigationId`:$attemptIndex"
            $rounds = @()
            if ($preparationRoundsByAttempt.ContainsKey($roundKey)) {
                $rounds = @(
                    $preparationRoundsByAttempt[$roundKey] |
                        Sort-Object { [long](Get-EventValue $_ 'preparation_round_index') })
            }
            if ($null -ne $roundCount -and $rounds.Count -ne $roundCount) {
                Add-Failure $failures "$attemptContext expected $roundCount preparation rounds, found $($rounds.Count)."
            }

            $previousRoundEndNs = 0L
            for ($roundIndex = 0; $roundIndex -lt $rounds.Count; $roundIndex++) {
                $round = $rounds[$roundIndex]
                $roundContext = "$attemptContext.preparation_round[$roundIndex]"
                $reportedRoundIndex = Get-RequiredInt64 $round 'preparation_round_index' 0 $roundContext $failures
                if ($null -ne $reportedRoundIndex -and $reportedRoundIndex -ne $roundIndex) {
                    Add-Failure $failures "$roundContext.preparation_round_index must be contiguous and equal to $roundIndex."
                }
                $roundTargetIndex = Get-RequiredInt64 $round 'target_index' 0 $roundContext $failures
                $roundSourceTaskId = Get-RequiredInt64 $round 'source_task_id' 1 $roundContext $failures
                if ($null -ne $attemptTargetIndex -and $roundTargetIndex -ne $attemptTargetIndex) {
                    Add-Failure $failures "$roundContext.target_index does not match its attempt."
                }
                if ($null -ne $sourceTaskId -and $roundSourceTaskId -ne $sourceTaskId) {
                    Add-Failure $failures "$roundContext.source_task_id does not match its attempt."
                }
                $roundSourceKind = [string](Get-EventValue $round 'source_kind' '')
                if ($roundSourceKind -ne $sourceKind) {
                    Add-Failure $failures "$roundContext.source_kind '$roundSourceKind' does not match '$sourceKind'."
                }
                if ($hasFolderListingDiagnostics) {
                    foreach ($field in $folderDiagnosticFields) {
                        Test-RequiredBoolean $round $field $roundContext $failures
                    }
                    $hintPresent = Get-EventValue $round 'hint_present'
                    $generationCurrent = Get-EventValue $round 'generation_current_at_start'
                    $listingScanned = Get-EventValue $round 'listing_scan_performed'
                    if ($roundSourceKind -eq 'file' -and
                        ($hintPresent -eq $true -or
                         $generationCurrent -eq $true -or
                         $listingScanned -eq $true)) {
                        Add-Failure $failures "$roundContext file rounds cannot report folder-listing diagnostics."
                    }
                    if ($generationCurrent -eq $true -and $hintPresent -ne $true) {
                        Add-Failure $failures "$roundContext cannot report a current generation without a hint."
                    }
                }
                if ($hasContextReuseDiagnostics) {
                    Test-RequiredBoolean $round 'context_reused' $roundContext $failures
                }
                Test-RequiredBoolean $round 'revalidation_succeeded' $roundContext $failures
                $roundSucceeded = Get-EventValue $round 'revalidation_succeeded'
                if ($roundIndex -lt $rounds.Count - 1 -and $roundSucceeded -eq $true) {
                    Add-Failure $failures "$roundContext cannot succeed before a later retry."
                }
                if ($roundIndex -eq $rounds.Count - 1 -and $roundSucceeded -ne $true) {
                    Add-Failure $failures "$roundContext final revalidation must succeed."
                }

                $roundStartedNs = Get-RequiredInt64 $round 'preparation_started_steady_ns' 1 $roundContext $failures
                $roundLoadStartNs = Get-RequiredInt64 $round 'snapshot_load_started_steady_ns' 1 $roundContext $failures
                $roundLoadFinishNs = Get-RequiredInt64 $round 'snapshot_load_finished_steady_ns' 1 $roundContext $failures
                $roundContextNs = Get-RequiredInt64 $round 'context_prepared_steady_ns' 1 $roundContext $failures
                $roundRevalidatedNs = Get-RequiredInt64 $round 'source_revalidated_steady_ns' 1 $roundContext $failures
                Test-TimeOrder (
                    [long[]]@($roundStartedNs, $roundLoadStartNs, $roundLoadFinishNs, $roundContextNs, $roundRevalidatedNs)) `
                    "$roundContext phase" $failures
                if ($roundIndex -eq 0 -and $roundStartedNs -lt $workerNs) {
                    Add-Failure $failures "$roundContext starts before its worker."
                }
                if ($roundIndex -gt 0 -and $roundStartedNs -lt $previousRoundEndNs) {
                    Add-Failure $failures "$roundContext overlaps the previous preparation round."
                }
                foreach ($field in @('source_inspection_ms', 'decode_ms', 'context_prepare_ms', 'source_revalidation_ms', 'round_total_ms')) {
                    [void](Get-RequiredDuration $round $field $roundContext $failures)
                }
                Test-DurationMatches $round 'source_inspection_ms' $roundStartedNs $roundLoadStartNs $roundContext $failures
                Test-DurationMatches $round 'decode_ms' $roundLoadStartNs $roundLoadFinishNs $roundContext $failures
                Test-DurationMatches $round 'context_prepare_ms' $roundLoadFinishNs $roundContextNs $roundContext $failures
                Test-DurationMatches $round 'source_revalidation_ms' $roundContextNs $roundRevalidatedNs $roundContext $failures
                Test-DurationMatches $round 'round_total_ms' $roundStartedNs $roundRevalidatedNs $roundContext $failures
                $previousRoundEndNs = $roundRevalidatedNs
            }

            if ($rounds.Count -gt 0) {
                if ([long](Get-EventValue $rounds[0] 'snapshot_load_started_steady_ns') -ne $loadStartNs -or
                    [long](Get-EventValue $rounds[-1] 'snapshot_load_finished_steady_ns') -ne $loadFinishNs -or
                    [long](Get-EventValue $rounds[-1] 'context_prepared_steady_ns') -ne $contextNs -or
                    [long](Get-EventValue $rounds[-1] 'source_revalidated_steady_ns') -ne $revalidatedNs) {
                    Add-Failure $failures "$attemptContext boundary timestamps do not match its preparation rounds."
                }
                foreach ($field in @('source_inspection_ms', 'decode_ms', 'context_prepare_ms', 'source_revalidation_ms')) {
                    Test-AggregateDurationSum $attempt $rounds $field $attemptContext $failures
                }
                if ($hasContextReuseDiagnostics -and
                    (Get-EventValue $attempt 'context_reused') -ne
                    (Get-EventValue $rounds[-1] 'context_reused')) {
                    Add-Failure $failures "$attemptContext.context_reused does not match its final preparation round."
                }
            }
        }
        Test-DurationMatches $attempt 'workflow_prepare_ms' $revalidatedNs $preparedNs $attemptContext $failures
        Test-DurationMatches $attempt 'completion_ready_ms' $preparedNs $readyNs $attemptContext $failures
        Test-DurationMatches $attempt 'ordered_publish_wait_ms' $readyNs $publishedNs $attemptContext $failures
        Test-DurationMatches $attempt 'completion_service_wait_ms' $publishedNs $drainedNs $attemptContext $failures
        Test-DurationMatches $attempt 'attempt_total_ms' $enqueuedNs $drainedNs $attemptContext $failures
        $previousDrainedNs = $drainedNs
    }

    if ($attempts.Count -gt 0) {
        $firstEnqueuedNs = [long](Get-EventValue $attempts[0] 'load_enqueued_steady_ns')
        Test-DurationMatches $event 'enqueue_ms' $targetResolvedNs $firstEnqueuedNs $context $failures
        if ($activatedNs -lt $previousDrainedNs) {
            Add-Failure $failures "$context activated before its final attempt was drained."
        }
        Test-DurationMatches $event 'activation_ms' $previousDrainedNs $activatedNs $context $failures
        $finalAttemptTarget = [long](Get-EventValue $attempts[-1] 'target_index' -1)
        if ($null -ne $targetIndex -and $finalAttemptTarget -ne $targetIndex) {
            Add-Failure $failures "$context final attempt target $finalAttemptTarget does not match target_index $targetIndex."
        }

        foreach ($field in $attemptDurationFields | Where-Object { $_ -ne 'attempt_total_ms' }) {
            Test-AggregateDurationSum $event $attempts $field $context $failures
        }
        $expectedRetargetGapMs = 0.0
        for ($index = 1; $index -lt $attempts.Count; $index++) {
            $previousAttemptDrainedNs = [long](Get-EventValue $attempts[$index - 1] 'completion_drained_steady_ns')
            $nextAttemptEnqueuedNs = [long](Get-EventValue $attempts[$index] 'load_enqueued_steady_ns')
            if ($previousAttemptDrainedNs -gt 0 -and $nextAttemptEnqueuedNs -ge $previousAttemptDrainedNs) {
                $expectedRetargetGapMs += [double]($nextAttemptEnqueuedNs - $previousAttemptDrainedNs) / 1000000.0
            }
        }
        $actualRetargetGapMs = Get-RequiredDuration $event 'retarget_gap_ms' $context $failures
        if ($null -ne $actualRetargetGapMs -and
            [Math]::Abs($actualRetargetGapMs - $expectedRetargetGapMs) -gt 0.001) {
            Add-Failure $failures ("$context.retarget_gap_ms does not match attempt gaps ({0:F4} vs {1:F4} ms)." -f $actualRetargetGapMs, $expectedRetargetGapMs)
        }
    }

    if ($failures.Count -eq $failureCountBefore) {
        [void]$validPresentedEvents.Add($event)
    }
}

Write-Host "Profile: $($resolvedProfile.Path)"
Write-Host "Recording completeness: $($summaryCheck.Status)"
Write-Host "Navigation events: $($navigationEvents.Count); presented: $($presentedEvents.Count); valid presented: $($validPresentedEvents.Count)"
if ($navigationEvents.Count -gt 0) {
    Write-Host 'Outcomes:'
    $navigationEvents |
        Group-Object { [string](Get-EventValue $_ 'outcome' 'unknown') } |
        Sort-Object Name |
        ForEach-Object { Write-Host ('  {0}: {1}' -f $_.Name, $_.Count) }
}

$metricFields = @(
    'total_ms',
    'input_to_request_ms',
    'target_resolution_ms',
    'enqueue_ms',
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
    'ui_to_present_ms'
)
foreach ($group in @($validPresentedEvents | Group-Object { [string](Get-EventValue $_ 'input_kind') } | Sort-Object Name)) {
    Write-Host ''
    Write-Host "Input kind: $($group.Name); count: $($group.Count)"
    @($metricFields | ForEach-Object { Format-Metric ([object[]]$group.Group) $_ }) |
        Format-Table -AutoSize
}

if ($hasContextReuseDiagnostics) {
    $validNavigationIds = @{}
    foreach ($event in $validPresentedEvents) {
        $validNavigationIds[[string](Get-EventValue $event 'navigation_id')] = $true
    }
    $validAttempts = @(
        $attemptEvents |
            Where-Object {
                $validNavigationIds.ContainsKey(
                    [string](Get-EventValue $_ 'navigation_id'))
            })
    if ($validAttempts.Count -gt 0) {
        Write-Host ''
        Write-Host 'Context materialization diagnostics:'
        @(
            $validAttempts |
                Group-Object {
                    '{0}|{1}' -f
                        [string](Get-EventValue $_ 'source_kind'),
                        [bool](Get-EventValue $_ 'context_reused')
                } |
                Sort-Object Name |
                ForEach-Object {
                    $parts = $_.Name -split '\|'
                    $stats = Get-Stats (
                        Get-MetricValues ([object[]]$_.Group) 'context_prepare_ms')
                    [pscustomobject]@{
                        SourceKind = $parts[0]
                        ContextReused = $parts[1]
                        Count = $stats.Count
                        ContextP50 = '{0:F3}' -f $stats.P50
                        ContextP95 = '{0:F3}' -f $stats.P95
                    }
                }) |
            Format-Table -AutoSize
    }
}

if ($hasFolderListingDiagnostics) {
    $validNavigationIds = @{}
    foreach ($event in $validPresentedEvents) {
        $validNavigationIds[[string](Get-EventValue $event 'navigation_id')] = $true
    }
    $validFolderRounds = @(
        $preparationRoundEvents |
            Where-Object {
                (Get-EventValue $_ 'source_kind') -eq 'folder' -and
                $validNavigationIds.ContainsKey(
                    [string](Get-EventValue $_ 'navigation_id'))
            })
    if ($validFolderRounds.Count -gt 0) {
        Write-Host ''
        Write-Host 'Folder listing generation diagnostics:'
        @(
            $validFolderRounds |
                Group-Object {
                    '{0}|{1}|{2}' -f
                        [bool](Get-EventValue $_ 'hint_present'),
                        [bool](Get-EventValue $_ 'generation_current_at_start'),
                        [bool](Get-EventValue $_ 'listing_scan_performed')
                } |
                Sort-Object Name |
                ForEach-Object {
                    $parts = $_.Name -split '\|'
                    $stats = Get-Stats (
                        Get-MetricValues ([object[]]$_.Group) 'source_inspection_ms')
                    [pscustomobject]@{
                        HintPresent = $parts[0]
                        GenerationCurrent = $parts[1]
                        ListingScan = $parts[2]
                        Count = $stats.Count
                        InspectionP50 = '{0:F3}' -f $stats.P50
                        InspectionP95 = '{0:F3}' -f $stats.P95
                    }
                }) |
            Format-Table -AutoSize
    }
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
