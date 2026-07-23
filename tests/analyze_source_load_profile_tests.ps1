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
        $arguments = @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass',
            '-File', $Analyzer, $ProfilePath)
        if ($ReportOnly) {
            $arguments += '-ReportOnly'
        }
        $output = @(
            & (Join-Path $PSHOME 'powershell.exe') @arguments 2>&1 |
                ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($output -join "`n")
    }
}

function New-SourceLoadAttemptEvents {
    param(
        [long]$SourceLoadId,
        [long]$AttemptIndex,
        [long]$TargetIndex,
        [long]$TaskId,
        [long]$EnqueuedNs
    )

    $ms = 1000000L
    $workerNs = $EnqueuedNs + $ms
    $loadStartNs = $workerNs + $ms
    $loadFinishNs = $loadStartNs + 2 * $ms
    $contextNs = $loadFinishNs + $ms
    $revalidatedNs = $contextNs + $ms
    $preparedNs = $revalidatedNs + $ms
    $readyNs = $preparedNs + $ms
    $publishedNs = $readyNs + $ms
    $drainedNs = $publishedNs + $ms

    $round = [pscustomobject][ordered]@{
        steady_ns = $drainedNs
        event = 'source_load_latency_preparation_round'
        source_load_id = $SourceLoadId
        attempt_index = $AttemptIndex
        preparation_round_index = 0
        target_index = $TargetIndex
        source_task_id = $TaskId
        source_kind = 'file'
        hint_present = $false
        generation_current_at_start = $false
        listing_scan_performed = $false
        context_reused = $false
        revalidation_succeeded = $true
        preparation_started_steady_ns = $workerNs
        snapshot_load_started_steady_ns = $loadStartNs
        snapshot_load_finished_steady_ns = $loadFinishNs
        context_prepared_steady_ns = $contextNs
        source_revalidated_steady_ns = $revalidatedNs
        source_inspection_ms = 1.0
        decode_ms = 2.0
        context_prepare_ms = 1.0
        source_revalidation_ms = 1.0
        round_total_ms = 5.0
    }
    $attempt = [pscustomobject][ordered]@{
        steady_ns = $drainedNs
        event = 'source_load_latency_attempt'
        source_load_id = $SourceLoadId
        attempt_index = $AttemptIndex
        target_index = $TargetIndex
        source_task_id = $TaskId
        source_kind = 'file'
        workflow_reused = $false
        context_reused = $false
        preparation_round_count = 1
        load_enqueued_steady_ns = $EnqueuedNs
        worker_started_steady_ns = $workerNs
        snapshot_load_started_steady_ns = $loadStartNs
        snapshot_load_finished_steady_ns = $loadFinishNs
        context_prepared_steady_ns = $contextNs
        source_revalidated_steady_ns = $revalidatedNs
        worker_prepared_steady_ns = $preparedNs
        completion_ready_steady_ns = $readyNs
        completion_published_steady_ns = $publishedNs
        completion_drained_steady_ns = $drainedNs
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
    return @($round, $attempt)
}

function New-ValidSourceLoadEvents {
    param(
        [long]$SourceLoadId,
        [switch]$Retarget
    )

    $ms = 1000000L
    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($event in New-SourceLoadAttemptEvents `
            $SourceLoadId 0 1 71 (2 * $ms)) {
        [void]$events.Add($event)
    }

    $attemptCount = 1
    $targetIndex = 1
    $finalTaskId = 71
    $finalDrainedNs = 12 * $ms
    $retargetGapMs = 0.0
    if ($Retarget) {
        foreach ($event in New-SourceLoadAttemptEvents `
                $SourceLoadId 1 2 72 (13 * $ms)) {
            [void]$events.Add($event)
        }
        $attemptCount = 2
        $targetIndex = 2
        $finalTaskId = 72
        $finalDrainedNs = 23 * $ms
        $retargetGapMs = 1.0
    }

    $activatedNs = $finalDrainedNs + $ms
    $uiUpdatedNs = $activatedNs + $ms
    $presentNs = $uiUpdatedNs + $ms
    $summary = [pscustomobject][ordered]@{
        steady_ns = $presentNs
        event = 'source_load_latency'
        source_load_id = $SourceLoadId
        activation_frame = 42
        presentation_viewport_id = 7
        outcome = 'presented'
        request_kind = 'explicit_open'
        source_kind = 'file'
        target_index = $targetIndex
        attempt_count = $attemptCount
        preparation_round_count = $attemptCount
        listing_scan_count = 0
        first_source_task_id = 71
        final_source_task_id = $finalTaskId
        context_reused = $false
        workflow_reused = $false
        accepted_steady_ns = 1 * $ms
        first_enqueued_steady_ns = 2 * $ms
        final_completion_drained_steady_ns = $finalDrainedNs
        snapshot_activated_steady_ns = $activatedNs
        ui_updated_steady_ns = $uiUpdatedNs
        first_present_steady_ns = $presentNs
        terminal_steady_ns = $presentNs
        accept_to_enqueue_ms = 1.0
        queue_wait_ms = 1.0 * $attemptCount
        source_inspection_ms = 1.0 * $attemptCount
        decode_ms = 2.0 * $attemptCount
        context_prepare_ms = 1.0 * $attemptCount
        source_revalidation_ms = 1.0 * $attemptCount
        workflow_prepare_ms = 1.0 * $attemptCount
        completion_ready_ms = 1.0 * $attemptCount
        ordered_publish_wait_ms = 1.0 * $attemptCount
        completion_service_wait_ms = 1.0 * $attemptCount
        retarget_gap_ms = $retargetGapMs
        activation_ms = 1.0
        ui_update_ms = 1.0
        ui_to_present_ms = 1.0
        total_ms = [double]($presentNs - $ms) / 1000000.0
    }
    [void]$events.Add($summary)
    return [object[]]$events.ToArray()
}

function Add-RecorderSummary {
    param([object[]]$Events, [long]$DroppedEvents = 0)

    return @($Events) + [pscustomobject][ordered]@{
        steady_ns = 100000000
        event = 'profile_recorder_summary'
        stop_reason = 'explicit'
        accepted_bytes = 4096
        dropped_events = $DroppedEvents
    }
}

function Copy-Events {
    param([object[]]$Events)
    return [object[]]@(
        ($Events | ConvertTo-Json -Depth 8) | ConvertFrom-Json)
}

function Write-ProfileFixture {
    param([string]$Path, [object[]]$Events)
    [System.IO.File]::WriteAllLines(
        $Path,
        [string[]]@($Events | ForEach-Object { $_ | ConvertTo-Json -Compress }))
}

$temporaryDirectory = Join-Path (
    [System.IO.Path]::GetTempPath()) (
    'specforge-source-load-analyzer-' + [guid]::NewGuid().ToString('N'))
[void][System.IO.Directory]::CreateDirectory($temporaryDirectory)
try {
    $validPath = Join-Path $temporaryDirectory 'valid.jsonl'
    Write-ProfileFixture `
        $validPath `
        (Add-RecorderSummary (New-ValidSourceLoadEvents 1))
    $valid = Invoke-Analyzer $validPath
    Assert-True ($valid.ExitCode -eq 0) `
        "A complete source-load trace should pass:`n$($valid.Output)"
    Assert-True (
        $valid.Output -match 'Source kind: file; count: 1' -and
        $valid.Output -match 'Result: PASS') `
        "The analyzer should report quantitative source-load metrics:`n$($valid.Output)"

    $retargetPath = Join-Path $temporaryDirectory 'retarget.jsonl'
    Write-ProfileFixture `
        $retargetPath `
        (Add-RecorderSummary (New-ValidSourceLoadEvents 2 -Retarget))
    $retarget = Invoke-Analyzer $retargetPath
    Assert-True ($retarget.ExitCode -eq 0) `
        "A follow-up/retarget trace should aggregate both attempts:`n$($retarget.Output)"
    Assert-True ($retarget.Output -match 'valid presented: 1') `
        "The retarget trace should remain a valid presented sample:`n$($retarget.Output)"

    $missingAttemptPath = Join-Path $temporaryDirectory 'missing-attempt.jsonl'
    $missingAttemptEvents = @(
        Add-RecorderSummary (
            @(New-ValidSourceLoadEvents 3) |
                Where-Object { $_.event -ne 'source_load_latency_attempt' }))
    Write-ProfileFixture $missingAttemptPath $missingAttemptEvents
    $missingAttempt = Invoke-Analyzer $missingAttemptPath
    Assert-True ($missingAttempt.ExitCode -ne 0) `
        "A summary without its attempt event must fail:`n$($missingAttempt.Output)"

    $nonMonotonicPath = Join-Path $temporaryDirectory 'non-monotonic.jsonl'
    $nonMonotonicEvents = Copy-Events (
        Add-RecorderSummary (New-ValidSourceLoadEvents 4))
    $nonMonotonicEvents[1].worker_started_steady_ns = 1000000
    Write-ProfileFixture $nonMonotonicPath $nonMonotonicEvents
    $nonMonotonic = Invoke-Analyzer $nonMonotonicPath
    Assert-True ($nonMonotonic.ExitCode -ne 0) `
        "Non-monotonic attempt timestamps must fail:`n$($nonMonotonic.Output)"

    $invalidAggregatePath = Join-Path $temporaryDirectory 'aggregate.jsonl'
    $invalidAggregateEvents = Copy-Events (
        Add-RecorderSummary (New-ValidSourceLoadEvents 5 -Retarget))
    $invalidAggregateEvents[-2].decode_ms = 99.0
    Write-ProfileFixture $invalidAggregatePath $invalidAggregateEvents
    $invalidAggregate = Invoke-Analyzer $invalidAggregatePath
    Assert-True ($invalidAggregate.ExitCode -ne 0) `
        "A summary that disagrees with attempt sums must fail:`n$($invalidAggregate.Output)"

    $invalidRoundCountPath = Join-Path $temporaryDirectory 'round-count.jsonl'
    $invalidRoundCountEvents = Copy-Events (
        Add-RecorderSummary (New-ValidSourceLoadEvents 6))
    $invalidRoundCountEvents[1].preparation_round_count = 2
    Write-ProfileFixture $invalidRoundCountPath $invalidRoundCountEvents
    $invalidRoundCount = Invoke-Analyzer $invalidRoundCountPath
    Assert-True ($invalidRoundCount.ExitCode -ne 0) `
        "A missing preparation round must fail:`n$($invalidRoundCount.Output)"

    $invalidBooleanPath = Join-Path $temporaryDirectory 'boolean.jsonl'
    $invalidBooleanEvents = Copy-Events (
        Add-RecorderSummary (New-ValidSourceLoadEvents 7))
    $invalidBooleanEvents[1].context_reused = 'false'
    Write-ProfileFixture $invalidBooleanPath $invalidBooleanEvents
    $invalidBoolean = Invoke-Analyzer $invalidBooleanPath
    Assert-True ($invalidBoolean.ExitCode -ne 0) `
        "A string boolean must fail strict JSON validation:`n$($invalidBoolean.Output)"

    $invalidIntegerPath = Join-Path $temporaryDirectory 'integer.jsonl'
    $invalidIntegerEvents = Copy-Events (
        Add-RecorderSummary (New-ValidSourceLoadEvents 8))
    $invalidIntegerEvents[-2].attempt_count = 1.5
    Write-ProfileFixture $invalidIntegerPath $invalidIntegerEvents
    $invalidInteger = Invoke-Analyzer $invalidIntegerPath
    Assert-True ($invalidInteger.ExitCode -ne 0) `
        "A fractional count must fail strict JSON validation:`n$($invalidInteger.Output)"

    $supersededOnlyPath = Join-Path $temporaryDirectory 'superseded-only.jsonl'
    $supersededOnlyEvents = Copy-Events (
        Add-RecorderSummary (New-ValidSourceLoadEvents 9))
    $supersededOnlyEvents[-2].outcome = 'superseded'
    Write-ProfileFixture $supersededOnlyPath $supersededOnlyEvents
    $supersededOnly = Invoke-Analyzer $supersededOnlyPath
    Assert-True ($supersededOnly.ExitCode -ne 0) `
        "A recording without a presented source load must fail:`n$($supersededOnly.Output)"

    $droppedPath = Join-Path $temporaryDirectory 'dropped.jsonl'
    Write-ProfileFixture `
        $droppedPath `
        (Add-RecorderSummary (New-ValidSourceLoadEvents 10) 2)
    $dropped = Invoke-Analyzer $droppedPath
    Assert-True ($dropped.ExitCode -ne 0) `
        "A recording with dropped events must fail:`n$($dropped.Output)"

    $truncatedPath = Join-Path $temporaryDirectory 'truncated.jsonl'
    $truncatedLines = [System.Collections.Generic.List[string]]::new()
    $truncatedLines.AddRange(
        [string[]]@(
            New-ValidSourceLoadEvents 11 |
                ForEach-Object { $_ | ConvertTo-Json -Compress }))
    [void]$truncatedLines.Add('{"steady_ns":50000000,"event":"partial"')
    [System.IO.File]::WriteAllLines($truncatedPath, $truncatedLines)
    $truncated = Invoke-Analyzer $truncatedPath -ReportOnly
    Assert-True ($truncated.ExitCode -eq 0) `
        "ReportOnly should accept a truncated recording for diagnosis:`n$($truncated.Output)"
    Assert-True (
        $truncated.Output -match 'Source kind: file; count: 1' -and
        $truncated.Output -match 'invalid or truncated JSON' -and
        $truncated.Output -match 'Result: FAIL') `
        "ReportOnly should show prefix metrics and strict failures:`n$($truncated.Output)"
}
finally {
    [System.IO.Directory]::Delete($temporaryDirectory, $true)
}
