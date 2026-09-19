[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Analyzer
)

$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-Analyzer {
    param(
        [string]$ProfilePath,
        [switch]$ReportOnly
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Analyzer, $ProfilePath)
        if ($ReportOnly) {
            $arguments += '-ReportOnly'
        }
        $output = @(& (Join-Path $PSHOME 'powershell.exe') @arguments 2>&1 |
                ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = ($output -join "`n") }
}

function New-ValidNavigationEvents {
    param(
        [long]$NavigationId,
        [string]$InputKind,
        [long]$BaseNs
    )

    $ms = 1000000L
    $attempt = [ordered]@{
        steady_ns = $BaseNs + 14 * $ms
        event = 'navigation_latency_attempt'
        navigation_id = $NavigationId
        attempt_index = 0
        target_index = 1
        source_task_id = 99
        source_kind = 'file'
        workflow_reused = $false
        context_reused = $false
        load_enqueued_steady_ns = $BaseNs + 4 * $ms
        worker_started_steady_ns = $BaseNs + 5 * $ms
        snapshot_load_started_steady_ns = $BaseNs + 6 * $ms
        snapshot_load_finished_steady_ns = $BaseNs + 8 * $ms
        context_prepared_steady_ns = $BaseNs + 9 * $ms
        source_revalidated_steady_ns = $BaseNs + 10 * $ms
        worker_prepared_steady_ns = $BaseNs + 11 * $ms
        completion_ready_steady_ns = $BaseNs + 12 * $ms
        completion_published_steady_ns = $BaseNs + 13 * $ms
        completion_drained_steady_ns = $BaseNs + 14 * $ms
        queue_wait_ms = 1.0
        source_inspection_ms = 1.0
        decode_ms = 2.0
        context_prepare_ms = 1.0
        source_revalidation_ms = 1.0
        workflow_prepare_ms = 1.0
        completion_ready_ms = 1.0
        ordered_publish_wait_ms = 1.0
        completion_service_wait_ms = 1.0
        attempt_total_ms = 10.0
    }
    $navigation = [ordered]@{
        steady_ns = $BaseNs + 17 * $ms
        event = 'navigation_latency'
        navigation_id = $NavigationId
        activation_frame = 42
        presentation_viewport_id = 7
        outcome = 'presented'
        input_kind = $InputKind
        from_index = 0
        target_index = 1
        cache_hit = $false
        cache_kind = 'none'
        row_count = 3
        filter_active = $false
        sort_active = $false
        query_active = $false
        pending_present = $false
        sequence_cache_hit = $false
        sequence_build_count = 2
        attempt_count = 1
        input_steady_ns = $BaseNs + 1 * $ms
        requested_steady_ns = $BaseNs + 2 * $ms
        target_resolved_steady_ns = $BaseNs + 3 * $ms
        snapshot_activated_steady_ns = $BaseNs + 15 * $ms
        ui_updated_steady_ns = $BaseNs + 16 * $ms
        first_present_steady_ns = $BaseNs + 17 * $ms
        input_to_request_ms = 1.0
        target_resolution_ms = 1.0
        effective_index_ms = 0.05
        pending_activation_supersede_ms = 0.05
        base_sequence_ms = 0.2
        target_lookup_ms = 0.1
        target_sequence_ms = 0.2
        navigation_state_result_ms = 0.4
        enqueue_ms = 1.0
        queue_wait_ms = 1.0
        source_inspection_ms = 1.0
        decode_ms = 2.0
        context_prepare_ms = 1.0
        source_revalidation_ms = 1.0
        workflow_prepare_ms = 1.0
        completion_ready_ms = 1.0
        ordered_publish_wait_ms = 1.0
        completion_service_wait_ms = 1.0
        retarget_gap_ms = 0.0
        activation_ms = 1.0
        ui_update_ms = 1.0
        ui_to_present_ms = 1.0
        total_ms = 16.0
    }
    return @([pscustomobject]$attempt, [pscustomobject]$navigation)
}

function Write-ProfileFixture {
    param([string]$Path, [object[]]$Events)
    [System.IO.File]::WriteAllLines(
        $Path,
        [string[]]@($Events | ForEach-Object { $_ | ConvertTo-Json -Compress }))
}

$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("spectiary_navigation_profile_tests_" + [guid]::NewGuid())
[System.IO.Directory]::CreateDirectory($temporaryDirectory) | Out-Null

try {
    $completePath = Join-Path $temporaryDirectory 'complete.jsonl'
    $completeEvents = @()
    $completeEvents += New-ValidNavigationEvents 1 'keyboard_next' 0
    $completeEvents += New-ValidNavigationEvents 2 'ui_next' 20000000
    $interpolationEvents = @(New-ValidNavigationEvents 5 'keyboard_next' 40000000)
    $interpolationEvents[1].steady_ns = 71000000
    $interpolationEvents[1].first_present_steady_ns = 71000000
    $interpolationEvents[1].ui_to_present_ms = 15.0
    $interpolationEvents[1].total_ms = 30.0
    $completeEvents += $interpolationEvents
    $completeEvents += [pscustomobject]@{
        steady_ns = 72000000
        event = 'navigation_prefetch'
        prefetch_id = 1
        source_task_id = 101
        target_index = 2
        direction = 'next'
        outcome = 'completed'
        scheduled_steady_ns = 70000000
        cancel_requested_steady_ns = 0
        terminal_steady_ns = 72000000
        duration_ms = 2.0
    }
    $completeEvents += [pscustomobject]@{
        steady_ns = 80000000
        event = 'navigation_prefetch'
        prefetch_id = 1
        source_task_id = 101
        target_index = 2
        direction = 'next'
        outcome = 'consumed'
        scheduled_steady_ns = 70000000
        cancel_requested_steady_ns = 0
        terminal_steady_ns = 80000000
        duration_ms = 10.0
    }
    $completeEvents += [pscustomobject]@{
        steady_ns = 100000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $completePath $completeEvents
    $complete = Invoke-Analyzer $completePath
    Assert-True ($complete.ExitCode -eq 0) "A complete navigation recording should pass:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'total_ms') "The report should include total_ms:`n$($complete.Output)"
    Assert-True ($complete.Output -match '16.000') "The report should calculate navigation metrics:`n$($complete.Output)"
    Assert-True ($complete.Output -match '29.300') "Navigation p95 should use the shared linear interpolation:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'Input kind: keyboard_next; count: 2') "Keyboard timings must be reported separately:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'Input kind: ui_next; count: 1') "UI timings must be reported separately:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'base_sequence_ms') "The target-resolution breakdown should be reported:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'Target resolution diagnostics:') "Target-resolution diagnostics should be grouped:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'Snapshot cache kinds:') "Cache origins should be summarized:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'Snapshot hit rate:') "Snapshot hit rate should be reported:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'Prefetch outcomes:') "Prefetch lifecycle outcomes should be summarized:`n$($complete.Output)"
    Assert-True ($complete.Output -match 'consumed: 1') "Prefetch consumption should be visible:`n$($complete.Output)"

    $carryInPath = Join-Path $temporaryDirectory 'prefetch-carry-in.jsonl'
    $carryInEvents = @(New-ValidNavigationEvents 6 'keyboard_next' 0)
    $carryInEvents += [pscustomobject]@{
        steady_ns = 25000000
        event = 'navigation_prefetch'
        prefetch_id = 9
        source_task_id = 109
        target_index = 3
        direction = 'next'
        outcome = 'consumed'
        scheduled_steady_ns = 10000000
        cancel_requested_steady_ns = 0
        terminal_steady_ns = 25000000
        duration_ms = 15.0
    }
    $carryInEvents += [pscustomobject]@{
        steady_ns = 30000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $carryInPath $carryInEvents
    $carryIn = Invoke-Analyzer $carryInPath
    Assert-True (
        $carryIn.ExitCode -eq 0 -and
        $carryIn.Output -match 'consumed: 1') `
        "A prefetch completed before recording may appear as consumed-only carry-in:`n$($carryIn.Output)"

    $invalidPrefetchAssociations = @(
        [pscustomobject]@{
            Name = 'source-task'
            Mutate = {
                param([object[]]$Events)
                $Events[3].source_task_id = 202
            }
        },
        [pscustomobject]@{
            Name = 'target-index'
            Mutate = {
                param([object[]]$Events)
                $Events[3].target_index = 7
            }
        },
        [pscustomobject]@{
            Name = 'direction'
            Mutate = {
                param([object[]]$Events)
                $Events[3].direction = 'previous'
            }
        },
        [pscustomobject]@{
            Name = 'scheduled-time'
            Mutate = {
                param([object[]]$Events)
                $Events[3].scheduled_steady_ns = 9000000
                $Events[3].duration_ms = 21.0
            }
        },
        [pscustomobject]@{
            Name = 'terminal-order'
            Mutate = {
                param([object[]]$Events)
                $Events[3].steady_ns = 11000000
                $Events[3].terminal_steady_ns = 11000000
                $Events[3].duration_ms = 1.0
            }
        })
    foreach ($case in $invalidPrefetchAssociations) {
        $invalidPath = Join-Path $temporaryDirectory "prefetch-$($case.Name).jsonl"
        $invalidEvents = @(New-ValidNavigationEvents 20 'keyboard_next' 0)
        $invalidEvents += [pscustomobject]@{
            steady_ns = 12000000
            event = 'navigation_prefetch'
            prefetch_id = 20
            source_task_id = 201
            target_index = 2
            direction = 'next'
            outcome = 'completed'
            scheduled_steady_ns = 10000000
            terminal_steady_ns = 12000000
            duration_ms = 2.0
        }
        $invalidEvents += [pscustomobject]@{
            steady_ns = 30000000
            event = 'navigation_prefetch'
            prefetch_id = 20
            source_task_id = 201
            target_index = 2
            direction = 'next'
            outcome = 'consumed'
            scheduled_steady_ns = 10000000
            terminal_steady_ns = 30000000
            duration_ms = 20.0
        }
        & $case.Mutate $invalidEvents
        $invalidEvents += [pscustomobject]@{
            steady_ns = 40000000
            event = 'profile_recorder_summary'
            stop_reason = 'explicit'
            accepted_bytes = 1024
            dropped_events = 0
        }
        Write-ProfileFixture $invalidPath $invalidEvents
        $invalidAssociation = Invoke-Analyzer $invalidPath
        Assert-True (
            $invalidAssociation.ExitCode -ne 0) `
        "A mismatched prefetch $($case.Name) association must fail:`n$($invalidAssociation.Output)"
    }

    $canceledPrefetchPath =
        Join-Path $temporaryDirectory 'prefetch-canceled-terminal.jsonl'
    $canceledPrefetchEvents =
        @(New-ValidNavigationEvents 21 'keyboard_next' 0)
    $canceledPrefetchEvents += [pscustomobject]@{
        steady_ns = 30000000
        event = 'navigation_prefetch'
        prefetch_id = 21
        source_task_id = 221
        target_index = 2
        direction = 'next'
        outcome = 'canceled'
        scheduled_steady_ns = 10000000
        cancel_requested_steady_ns = 15000000
        terminal_steady_ns = 30000000
        duration_ms = 20.0
    }
    $canceledPrefetchEvents += [pscustomobject]@{
        steady_ns = 40000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture `
        $canceledPrefetchPath `
        $canceledPrefetchEvents
    $canceledPrefetch =
        Invoke-Analyzer $canceledPrefetchPath
    Assert-True (
        $canceledPrefetch.ExitCode -eq 0) `
        "A canceled prefetch should preserve request-to-terminal order:`n$($canceledPrefetch.Output)"

    $invalidCancelOrderPath =
        Join-Path $temporaryDirectory 'prefetch-invalid-cancel-order.jsonl'
    $invalidCancelOrderEvents =
        @(New-ValidNavigationEvents 22 'keyboard_next' 0)
    $invalidCancelOrderEvents += [pscustomobject]@{
        steady_ns = 14000000
        event = 'navigation_prefetch'
        prefetch_id = 22
        source_task_id = 222
        target_index = 2
        direction = 'next'
        outcome = 'canceled'
        scheduled_steady_ns = 10000000
        cancel_requested_steady_ns = 15000000
        terminal_steady_ns = 14000000
        duration_ms = 4.0
    }
    $invalidCancelOrderEvents += [pscustomobject]@{
        steady_ns = 40000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture `
        $invalidCancelOrderPath `
        $invalidCancelOrderEvents
    $invalidCancelOrder =
        Invoke-Analyzer $invalidCancelOrderPath
    Assert-True (
        $invalidCancelOrder.ExitCode -ne 0) `
        "A canceled prefetch terminal before its request must fail:`n$($invalidCancelOrder.Output)"

    $autoAdvancePath = Join-Path $temporaryDirectory 'auto-advance-target-resolution.jsonl'
    $autoAdvanceEvents = @(New-ValidNavigationEvents 12 'auto_advance' 0)
    $autoAdvanceEvents += [pscustomobject]@{
        steady_ns = 20000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $autoAdvancePath $autoAdvanceEvents
    $autoAdvance = Invoke-Analyzer $autoAdvancePath
    Assert-True (
        $autoAdvance.ExitCode -eq 0 -and
        $autoAdvance.Output -match 'Input kind: auto_advance; count: 1') `
        "A complete auto-advance target-resolution report should pass:`n$($autoAdvance.Output)"

    $legacyPath = Join-Path $temporaryDirectory 'legacy-target-resolution.jsonl'
    $legacyEvents = @(New-ValidNavigationEvents 9 'ui_next' 0)
    foreach ($field in @(
            'row_count',
            'filter_active',
            'sort_active',
            'query_active',
            'pending_present',
            'sequence_cache_hit',
            'sequence_build_count',
            'effective_index_ms',
            'pending_activation_supersede_ms',
            'base_sequence_ms',
            'target_lookup_ms',
            'target_sequence_ms',
            'navigation_state_result_ms')) {
        $legacyEvents[1].PSObject.Properties.Remove($field)
    }
    $legacyEvents += [pscustomobject]@{
        steady_ns = 20000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $legacyPath $legacyEvents
    $legacy = Invoke-Analyzer $legacyPath
    Assert-True ($legacy.ExitCode -eq 0) "A complete legacy navigation log should remain readable:`n$($legacy.Output)"
    Assert-True (
        $legacy.Output -notmatch 'Target resolution diagnostics:') `
        "Legacy logs should not fabricate target-resolution diagnostics:`n$($legacy.Output)"

    $partialTargetResolutionPath = Join-Path $temporaryDirectory 'partial-target-resolution.jsonl'
    $partialTargetResolutionEvents = @(New-ValidNavigationEvents 10 'ui_next' 0)
    $partialTargetResolutionEvents[1].PSObject.Properties.Remove('target_sequence_ms')
    $partialTargetResolutionEvents += [pscustomobject]@{
        steady_ns = 20000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $partialTargetResolutionPath $partialTargetResolutionEvents
    $partialTargetResolution = Invoke-Analyzer $partialTargetResolutionPath
    Assert-True (
        $partialTargetResolution.ExitCode -ne 0) `
        "A partial target-resolution schema should fail:`n$($partialTargetResolution.Output)"

    foreach ($integerField in @('row_count', 'sequence_build_count')) {
        $stringIntegerPath = Join-Path $temporaryDirectory "string-$integerField.jsonl"
        $stringIntegerEvents = @(New-ValidNavigationEvents 13 'ui_next' 0)
        $stringIntegerEvents[1].$integerField =
            [string]$stringIntegerEvents[1].$integerField
        $stringIntegerEvents += [pscustomobject]@{
            steady_ns = 20000000
            event = 'profile_recorder_summary'
            stop_reason = 'explicit'
            accepted_bytes = 1024
            dropped_events = 0
        }
        Write-ProfileFixture $stringIntegerPath $stringIntegerEvents
        $stringInteger = Invoke-Analyzer $stringIntegerPath
        Assert-True (
            $stringInteger.ExitCode -ne 0) `
            "$integerField as a JSON string must fail strict integer validation:`n$($stringInteger.Output)"
    }

    $invalidTargetResolutionSumPath = Join-Path $temporaryDirectory 'invalid-target-resolution-sum.jsonl'
    $invalidTargetResolutionSumEvents = @(New-ValidNavigationEvents 11 'keyboard_next' 0)
    $invalidTargetResolutionSumEvents[1].navigation_state_result_ms = 1.4
    $invalidTargetResolutionSumEvents += [pscustomobject]@{
        steady_ns = 20000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $invalidTargetResolutionSumPath $invalidTargetResolutionSumEvents
    $invalidTargetResolutionSum = Invoke-Analyzer $invalidTargetResolutionSumPath
    Assert-True (
        $invalidTargetResolutionSum.ExitCode -ne 0) `
        "A target-resolution breakdown that does not sum to the aggregate should fail:`n$($invalidTargetResolutionSum.Output)"

    $multiRoundPath = Join-Path $temporaryDirectory 'multi-round.jsonl'
    $multiRoundBase = @(New-ValidNavigationEvents 8 'ui_next' 0)
    $multiRoundAttempt = $multiRoundBase[0]
    $multiRoundNavigation = $multiRoundBase[1]
    $multiRoundAttempt.source_kind = 'folder'
    $multiRoundAttempt | Add-Member -NotePropertyName preparation_round_count -NotePropertyValue 2
    $multiRoundAttempt.snapshot_load_finished_steady_ns = 12000000
    $multiRoundAttempt.context_prepared_steady_ns = 13000000
    $multiRoundAttempt.source_revalidated_steady_ns = 14000000
    $multiRoundAttempt.worker_prepared_steady_ns = 15000000
    $multiRoundAttempt.completion_ready_steady_ns = 16000000
    $multiRoundAttempt.completion_published_steady_ns = 17000000
    $multiRoundAttempt.completion_drained_steady_ns = 18000000
    $multiRoundAttempt.source_inspection_ms = 2.0
    $multiRoundAttempt.decode_ms = 3.0
    $multiRoundAttempt.context_prepare_ms = 2.0
    $multiRoundAttempt.source_revalidation_ms = 2.0
    $multiRoundAttempt.workflow_prepare_ms = 1.0
    $multiRoundAttempt.attempt_total_ms = 14.0
    $multiRoundNavigation.steady_ns = 21000000
    $multiRoundNavigation.snapshot_activated_steady_ns = 19000000
    $multiRoundNavigation.ui_updated_steady_ns = 20000000
    $multiRoundNavigation.first_present_steady_ns = 21000000
    $multiRoundNavigation.source_inspection_ms = 2.0
    $multiRoundNavigation.decode_ms = 3.0
    $multiRoundNavigation.context_prepare_ms = 2.0
    $multiRoundNavigation.source_revalidation_ms = 2.0
    $multiRoundNavigation.workflow_prepare_ms = 1.0
    $multiRoundNavigation.activation_ms = 1.0
    $multiRoundNavigation.total_ms = 20.0
    $multiRoundEvents = @(
        [pscustomobject][ordered]@{
            steady_ns = 18000000
            event = 'navigation_latency_preparation_round'
            navigation_id = 8
            attempt_index = 0
            preparation_round_index = 0
            target_index = 1
            source_task_id = 99
            source_kind = 'folder'
            hint_present = $true
            generation_current_at_start = $true
            listing_scan_performed = $false
            context_reused = $true
            revalidation_succeeded = $false
            preparation_started_steady_ns = 5000000
            snapshot_load_started_steady_ns = 6000000
            snapshot_load_finished_steady_ns = 8000000
            context_prepared_steady_ns = 9000000
            source_revalidated_steady_ns = 10000000
            source_inspection_ms = 1.0
            decode_ms = 2.0
            context_prepare_ms = 1.0
            source_revalidation_ms = 1.0
            round_total_ms = 5.0
        },
        [pscustomobject][ordered]@{
            steady_ns = 18000000
            event = 'navigation_latency_preparation_round'
            navigation_id = 8
            attempt_index = 0
            preparation_round_index = 1
            target_index = 1
            source_task_id = 99
            source_kind = 'folder'
            hint_present = $false
            generation_current_at_start = $false
            listing_scan_performed = $true
            context_reused = $false
            revalidation_succeeded = $true
            preparation_started_steady_ns = 10000000
            snapshot_load_started_steady_ns = 11000000
            snapshot_load_finished_steady_ns = 12000000
            context_prepared_steady_ns = 13000000
            source_revalidated_steady_ns = 14000000
            source_inspection_ms = 1.0
            decode_ms = 1.0
            context_prepare_ms = 1.0
            source_revalidation_ms = 1.0
            round_total_ms = 4.0
        },
        $multiRoundAttempt,
        $multiRoundNavigation,
        [pscustomobject][ordered]@{
            steady_ns = 30000000
            event = 'profile_recorder_summary'
            stop_reason = 'explicit'
            accepted_bytes = 2048
            dropped_events = 0
        })
    Write-ProfileFixture $multiRoundPath $multiRoundEvents
    $multiRound = Invoke-Analyzer $multiRoundPath
    Assert-True ($multiRound.ExitCode -eq 0) "A correctly attributed TOCTOU retry should pass:`n$($multiRound.Output)"
    Assert-True ($multiRound.Output -match 'Folder listing generation diagnostics') `
        "Folder listing diagnostics should be summarized:`n$($multiRound.Output)"
    Assert-True ($multiRound.Output -match 'Context materialization diagnostics') `
        "Context reuse diagnostics should be summarized:`n$($multiRound.Output)"

    $multiRoundAttempt.decode_ms = 99.0
    Write-ProfileFixture $multiRoundPath $multiRoundEvents
    $invalidMultiRound = Invoke-Analyzer $multiRoundPath
    Assert-True ($invalidMultiRound.ExitCode -ne 0) "Attempt stages that disagree with preparation rounds must fail:`n$($invalidMultiRound.Output)"
    $multiRoundAttempt.decode_ms = 3.0

    $multiRoundEvents[1].generation_current_at_start = $true
    Write-ProfileFixture $multiRoundPath $multiRoundEvents
    $invalidFolderDiagnostics = Invoke-Analyzer $multiRoundPath
    Assert-True ($invalidFolderDiagnostics.ExitCode -ne 0) `
        "A current folder generation without a hint must fail:`n$($invalidFolderDiagnostics.Output)"
    $multiRoundEvents[1].generation_current_at_start = $false

    $multiRoundEvents[1].context_reused = 'false'
    Write-ProfileFixture $multiRoundPath $multiRoundEvents
    $invalidContextReuse = Invoke-Analyzer $multiRoundPath
    Assert-True ($invalidContextReuse.ExitCode -ne 0) `
        "A non-boolean context_reused field must fail:`n$($invalidContextReuse.Output)"
    $multiRoundEvents[1].context_reused = $false

    $nonMonotonicPath = Join-Path $temporaryDirectory 'non-monotonic.jsonl'
    $nonMonotonicEvents = @(New-ValidNavigationEvents 3 'keyboard_previous' 0)
    $nonMonotonicEvents[0].worker_started_steady_ns = 3000000
    $nonMonotonicEvents += [pscustomobject]@{
        steady_ns = 20000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $nonMonotonicPath $nonMonotonicEvents
    $nonMonotonic = Invoke-Analyzer $nonMonotonicPath
    Assert-True ($nonMonotonic.ExitCode -ne 0) "Non-monotonic phase timestamps should fail:`n$($nonMonotonic.Output)"

    $invalidAggregatePath = Join-Path $temporaryDirectory 'invalid-aggregate.jsonl'
    $invalidAggregateEvents = @(New-ValidNavigationEvents 4 'ui_previous' 0)
    $invalidAggregateEvents[1].decode_ms = 99.0
    $invalidAggregateEvents += [pscustomobject]@{
        steady_ns = 20000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 1024
        dropped_events = 0
    }
    Write-ProfileFixture $invalidAggregatePath $invalidAggregateEvents
    $invalidAggregate = Invoke-Analyzer $invalidAggregatePath
    Assert-True ($invalidAggregate.ExitCode -ne 0) "Aggregate stages that disagree with attempts should fail:`n$($invalidAggregate.Output)"

    $missingFieldsPath = Join-Path $temporaryDirectory 'missing-fields.jsonl'
    [System.IO.File]::WriteAllLines($missingFieldsPath, @(
            '{"steady_ns":1,"event":"navigation_latency","outcome":"presented"}',
            '{"steady_ns":2,"event":"profile_recorder_summary","stop_reason":"explicit","accepted_bytes":10,"dropped_events":0}'
        ))
    $missingFields = Invoke-Analyzer $missingFieldsPath
    Assert-True ($missingFields.ExitCode -ne 0) "A presented event without latency fields should fail:`n$($missingFields.Output)"

    $emptyPath = Join-Path $temporaryDirectory 'empty.jsonl'
    [System.IO.File]::WriteAllLines($emptyPath, @(
            '{"steady_ns":2,"event":"profile_recorder_summary","stop_reason":"explicit","accepted_bytes":10,"dropped_events":0}'
        ))
    $empty = Invoke-Analyzer $emptyPath
    Assert-True ($empty.ExitCode -ne 0) "A recording without navigation events should fail:`n$($empty.Output)"

    $supersededOnlyPath = Join-Path $temporaryDirectory 'superseded-only.jsonl'
    [System.IO.File]::WriteAllLines($supersededOnlyPath, @(
            '{"steady_ns":1,"event":"navigation_latency","outcome":"superseded","total_ms":3.0}',
            '{"steady_ns":2,"event":"profile_recorder_summary","stop_reason":"explicit","accepted_bytes":10,"dropped_events":0}'
        ))
    $supersededOnly = Invoke-Analyzer $supersededOnlyPath
    Assert-True ($supersededOnly.ExitCode -ne 0) "A recording without a presented navigation should fail:`n$($supersededOnly.Output)"

    $droppedPath = Join-Path $temporaryDirectory 'dropped.jsonl'
    [System.IO.File]::WriteAllLines($droppedPath, @(
            '{"steady_ns":1,"event":"navigation_latency","outcome":"presented","total_ms":10.0}',
            '{"steady_ns":2,"event":"profile_recorder_summary","stop_reason":"explicit","accepted_bytes":10,"dropped_events":2}'
        ))
    $dropped = Invoke-Analyzer $droppedPath
    Assert-True ($dropped.ExitCode -ne 0) "A recording with dropped events should fail:`n$($dropped.Output)"

    foreach ($invalidDroppedValue in @('false', '0.1')) {
        $invalidDroppedPath = Join-Path $temporaryDirectory ("invalid-dropped-" + $invalidDroppedValue.Replace('.', '-') + '.jsonl')
        $invalidDroppedEvents = @()
        $invalidDroppedEvents += New-ValidNavigationEvents 6 'ui_next' 0
        $invalidDroppedLines = [System.Collections.Generic.List[string]]::new()
        $invalidDroppedLines.AddRange([string[]]@($invalidDroppedEvents | ForEach-Object { $_ | ConvertTo-Json -Compress }))
        $invalidDroppedLines.Add(
            '{"steady_ns":20000000,"event":"profile_recorder_summary","stop_reason":"explicit","accepted_bytes":10,"dropped_events":' +
            $invalidDroppedValue + '}')
        [System.IO.File]::WriteAllLines($invalidDroppedPath, $invalidDroppedLines)
        $invalidDropped = Invoke-Analyzer $invalidDroppedPath
        Assert-True ($invalidDropped.ExitCode -ne 0) `
            "dropped_events=$invalidDroppedValue must fail strict JSON integer validation:`n$($invalidDropped.Output)"
    }

    $truncatedReportOnlyPath = Join-Path $temporaryDirectory 'truncated-report-only.jsonl'
    $truncatedReportOnlyEvents = @()
    $truncatedReportOnlyEvents += New-ValidNavigationEvents 7 'keyboard_next' 0
    $truncatedReportOnlyLines = [System.Collections.Generic.List[string]]::new()
    $truncatedReportOnlyLines.AddRange(
        [string[]]@($truncatedReportOnlyEvents | ForEach-Object { $_ | ConvertTo-Json -Compress }))
    $truncatedReportOnlyLines.Add('{"steady_ns":20000000,"event":"partial"')
    [System.IO.File]::WriteAllLines($truncatedReportOnlyPath, $truncatedReportOnlyLines)
    $truncatedReportOnly = Invoke-Analyzer $truncatedReportOnlyPath -ReportOnly
    Assert-True ($truncatedReportOnly.ExitCode -eq 0) `
        "ReportOnly should report the valid prefix of a truncated recording:`n$($truncatedReportOnly.Output)"
    Assert-True (
        $truncatedReportOnly.Output -match 'Input kind: keyboard_next; count: 1' -and
        $truncatedReportOnly.Output -match 'invalid or truncated JSON') `
        "ReportOnly should show both valid-prefix metrics and the parse failure:`n$($truncatedReportOnly.Output)"
}
finally {
    [System.IO.Directory]::Delete($temporaryDirectory, $true)
}
