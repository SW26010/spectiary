[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Analyzer
)

$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function New-ProfileEvent {
    param(
        [long]$SteadyNs,
        [string]$Event,
        [hashtable]$Fields = @{}
    )

    $record = [ordered]@{
        steady_ns = $SteadyNs
        event = $Event
    }
    foreach ($entry in $Fields.GetEnumerator()) {
        $record[$entry.Key] = $entry.Value
    }
    return ($record | ConvertTo-Json -Compress)
}

function New-PassingProfileLines {
    param(
        [switch]$IncludeSummary,
        [int]$DroppedEvents = 0,
        [string]$StopReason = 'explicit',
        [switch]$OmitDroppedEvents
    )

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add((New-ProfileEvent 0 'implot.pan_drag.state' @{ active = $true }))
    $lines.Add((New-ProfileEvent 1000000 'input' @{ kind = 'pointer_move'; left_down = $true }))
    $lines.Add((New-ProfileEvent 1100000 'implot.pan_drag.sample'))
    $lines.Add((New-ProfileEvent 1200000 'implot.axis_limits_changed'))
    $lines.Add((New-ProfileEvent 1300000 'present' @{ duration_ms = 0.1 }))
    $lines.Add((New-ProfileEvent 2000000 'input' @{ kind = 'pointer_move'; left_down = $true }))
    $lines.Add((New-ProfileEvent 2100000 'implot.pan_drag.sample'))
    $lines.Add((New-ProfileEvent 2200000 'implot.axis_limits_changed'))
    $lines.Add((New-ProfileEvent 2300000 'present' @{ duration_ms = 0.1 }))
    $lines.Add((New-ProfileEvent 3000000 'implot.pan_drag.state' @{ active = $false }))
    if ($IncludeSummary) {
        $summaryFields = @{
            stop_reason = $StopReason
            accepted_bytes = 1024
        }
        if (-not $OmitDroppedEvents) {
            $summaryFields.dropped_events = $DroppedEvents
        }
        $lines.Add((New-ProfileEvent 4000000 'profile_recorder_summary' $summaryFields))
    }
    return [string[]]$lines.ToArray()
}

function Invoke-Analyzer {
    param(
        [string]$ProfilePath,
        [switch]$AllowLegacyIncompleteRecording
    )

    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $Analyzer,
        $ProfilePath,
        '-BudgetMs', '1000',
        '-MinDragMs', '0',
        '-MinInputSamples', '1'
    )
    if ($AllowLegacyIncompleteRecording) {
        $arguments += '-AllowLegacyIncompleteRecording'
    }

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& (Join-Path $PSHOME 'powershell.exe') @arguments 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($output -join "`n")
    }
}

$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("specforge_analyze_profile_tests_" + [guid]::NewGuid())
[System.IO.Directory]::CreateDirectory($temporaryDirectory) | Out-Null

try {
    $completePath = Join-Path $temporaryDirectory 'complete.jsonl'
    [System.IO.File]::WriteAllLines($completePath, (New-PassingProfileLines -IncludeSummary))
    $complete = Invoke-Analyzer $completePath
    Assert-True ($complete.ExitCode -eq 0) "A complete zero-drop recording should pass:`n$($complete.Output)"

    $droppedPath = Join-Path $temporaryDirectory 'dropped.jsonl'
    [System.IO.File]::WriteAllLines($droppedPath, (New-PassingProfileLines -IncludeSummary -DroppedEvents 3))
    $dropped = Invoke-Analyzer $droppedPath
    Assert-True ($dropped.ExitCode -ne 0) "A recording with dropped events must fail:`n$($dropped.Output)"
    Assert-True ($dropped.Output -match 'dropped_events') "The dropped-event failure should name dropped_events:`n$($dropped.Output)"

    $legacyPath = Join-Path $temporaryDirectory 'legacy.jsonl'
    [System.IO.File]::WriteAllLines($legacyPath, (New-PassingProfileLines))
    $legacyDefault = Invoke-Analyzer $legacyPath
    Assert-True ($legacyDefault.ExitCode -ne 0) "A missing recorder summary must fail by default:`n$($legacyDefault.Output)"

    $legacyAllowed = Invoke-Analyzer $legacyPath -AllowLegacyIncompleteRecording
    Assert-True ($legacyAllowed.ExitCode -eq 0) "Explicit legacy mode should allow a summary-less recording:`n$($legacyAllowed.Output)"
    Assert-True ($legacyAllowed.Output -match 'legacy') "Legacy mode should print an explicit warning:`n$($legacyAllowed.Output)"

    $missingDroppedPath = Join-Path $temporaryDirectory 'missing-dropped-events.jsonl'
    [System.IO.File]::WriteAllLines($missingDroppedPath, (New-PassingProfileLines -IncludeSummary -OmitDroppedEvents))
    $missingDropped = Invoke-Analyzer $missingDroppedPath
    Assert-True ($missingDropped.ExitCode -ne 0) "A summary without dropped_events must fail:`n$($missingDropped.Output)"

    $invalidStopPath = Join-Path $temporaryDirectory 'invalid-stop-reason.jsonl'
    [System.IO.File]::WriteAllLines($invalidStopPath, (New-PassingProfileLines -IncludeSummary -StopReason 'write_failure'))
    $invalidStop = Invoke-Analyzer $invalidStopPath
    Assert-True ($invalidStop.ExitCode -ne 0) "A write-failure summary must fail completeness:`n$($invalidStop.Output)"

    $nonFinalSummaryPath = Join-Path $temporaryDirectory 'non-final-summary.jsonl'
    $nonFinalSummaryLines = [System.Collections.Generic.List[string]]::new()
    $nonFinalSummaryLines.AddRange([string[]](New-PassingProfileLines -IncludeSummary))
    $nonFinalSummaryLines.Add((New-ProfileEvent 5000000 'event_after_summary'))
    [System.IO.File]::WriteAllLines($nonFinalSummaryPath, $nonFinalSummaryLines)
    $nonFinalSummary = Invoke-Analyzer $nonFinalSummaryPath
    Assert-True ($nonFinalSummary.ExitCode -ne 0) "A non-final recorder summary must fail:`n$($nonFinalSummary.Output)"

    $multipleSummaryPath = Join-Path $temporaryDirectory 'multiple-summaries.jsonl'
    $multipleSummaryLines = [System.Collections.Generic.List[string]]::new()
    $multipleSummaryLines.AddRange([string[]](New-PassingProfileLines -IncludeSummary))
    $multipleSummaryLines.Add((New-ProfileEvent 5000000 'profile_recorder_summary' @{
                stop_reason = 'explicit'
                accepted_bytes = 2048
                dropped_events = 0
            }))
    [System.IO.File]::WriteAllLines($multipleSummaryPath, $multipleSummaryLines)
    $multipleSummary = Invoke-Analyzer $multipleSummaryPath
    Assert-True ($multipleSummary.ExitCode -ne 0) "Multiple recorder summaries must fail:`n$($multipleSummary.Output)"

    $truncatedPath = Join-Path $temporaryDirectory 'truncated.jsonl'
    $truncatedLines = [System.Collections.Generic.List[string]]::new()
    $truncatedLines.AddRange([string[]](New-PassingProfileLines -IncludeSummary))
    $truncatedLines.Add('{"steady_ns":5000000,"event":"partial"')
    [System.IO.File]::WriteAllLines($truncatedPath, $truncatedLines)
    $truncated = Invoke-Analyzer $truncatedPath
    Assert-True ($truncated.ExitCode -ne 0) "A truncated final JSONL record must fail:`n$($truncated.Output)"
}
finally {
    [System.IO.Directory]::Delete($temporaryDirectory, $true)
}
