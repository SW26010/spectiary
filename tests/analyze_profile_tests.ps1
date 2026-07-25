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

function New-LargeProfileLines {
    $frameCount = 25000
    $lines = [System.Collections.Generic.List[string]]::new(100010)
    $lines.Add((New-ProfileEvent 1000000 'implot.pan_drag.state' @{ active = $true }))
    $lines.Add((New-ProfileEvent 1100000 'input' @{ kind = 'pointer_move'; left_down = $true }))
    $lines.Add((New-ProfileEvent 1200000 'implot.pan_drag.sample'))
    $lines.Add((New-ProfileEvent 1300000 'implot.axis_limits_changed'))
    $lines.Add((New-ProfileEvent 1400000 'input' @{ kind = 'pointer_move'; left_down = $true }))
    $lines.Add((New-ProfileEvent 1500000 'implot.pan_drag.sample'))
    $lines.Add((New-ProfileEvent 1600000 'implot.axis_limits_changed'))
    foreach ($frame in 1..$frameCount) {
        $base = 1000000L + ([long]$frame * 1000000L)
        $lines.Add(('{{"steady_ns":{0},"event":"view_update","frame":{1},"duration_ms":0.25}}' -f
                    $base, $frame))
        $lines.Add(('{{"steady_ns":{0},"event":"draw_submission","frame":{1},"duration_ms":0.25}}' -f
                    ($base + 1000L), $frame))
        $lines.Add(('{{"steady_ns":{0},"event":"render_pass","frame":{1},"duration_ms":0.25}}' -f
                    ($base + 2000L), $frame))
        $lines.Add(('{{"steady_ns":{0},"event":"present","frame":{1},"duration_ms":0.25}}' -f
                    ($base + 3000L), $frame))
    }
    $lines.Add((New-ProfileEvent 30000000000 'implot.pan_drag.state' @{ active = $false }))
    $lines.Add((New-ProfileEvent 31000000000 'view_update' @{
                frame = 99999
                duration_ms = 9999.0
            }))
    $lines.Add((New-ProfileEvent 32000000000 'profile_recorder_summary' @{
                stop_reason = 'explicit'
                accepted_bytes = 10000000
                dropped_events = 0
            }))
    return [string[]]$lines.ToArray()
}

function Invoke-Analyzer {
    param(
        [string]$ProfilePath,
        [switch]$AllowLegacyIncompleteRecording,
        [ValidateSet('', 'Default', 'Uncapped')]
        [string]$ExpectedPanPacing = ''
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
    if ($ExpectedPanPacing) {
        $arguments += @('-ExpectedPanPacing', $ExpectedPanPacing)
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

    $unorderedPath = Join-Path $temporaryDirectory 'unordered.jsonl'
    $orderedLines = New-PassingProfileLines -IncludeSummary
    $unorderedLines = [string[]]@(
        $orderedLines[0],
        $orderedLines[5],
        $orderedLines[6],
        $orderedLines[7],
        $orderedLines[8],
        $orderedLines[1],
        $orderedLines[2],
        $orderedLines[3],
        $orderedLines[4],
        $orderedLines[9],
        $orderedLines[10]
    )
    [System.IO.File]::WriteAllLines($unorderedPath, $unorderedLines)
    $unordered = Invoke-Analyzer $unorderedPath
    Assert-True (
        $unordered.ExitCode -eq 0
    ) "An unordered profile should retain the analyzer's chronological sort fallback:`n$($unordered.Output)"

    $metricsPath = Join-Path $temporaryDirectory 'pan-metrics.jsonl'
    $metricsLines = [System.Collections.Generic.List[string]]::new()
    $metricsLines.Add((New-ProfileEvent 0 'runtime_config' @{
                pan_pacing_requested = 'uncapped'
                pan_pacing_effective = 'uncapped'
                pan_pacing_recognized = $true
            }))
    $metricsLines.Add((New-ProfileEvent 1000000 'implot.pan_drag.state' @{ active = $true; frame = 1 }))
    $metricsLines.Add((New-ProfileEvent 2000000 'pan_pacing' @{
                requested = 'uncapped'
                effective = 'uncapped'
                pan_active = $true
                continuous_rendering = $true
                backend = 'dxgi'
                present_mode = 'immediate'
                sync_interval = 0
                present_flags = 512
                tearing_supported = $true
                display_feedback = 'unavailable'
            }))
    $metricsLines.Add((New-ProfileEvent 100000000 'display_environment' @{
                presentation_backend = 'dxgi'
                presentation_degradation = 'tearing_allowed'
                dwm_timing_ok = $false
            }))
    $metricsLines.Add((New-ProfileEvent 110000000 'input' @{
                kind = 'pointer_move'
                left_down = $true
            }))
    $metricsLines.Add((New-ProfileEvent 120000000 'implot.pan_drag.sample' @{ frame = 1 }))
    $metricsLines.Add((New-ProfileEvent 310000000 'input' @{
                kind = 'pointer_move'
                left_down = $true
            }))
    $metricsLines.Add((New-ProfileEvent 700000000 'implot.pan_drag.sample' @{ frame = 4 }))
    foreach ($frame in 1..4) {
        $base = [long]$frame * 200000000
        $duration = [double]$frame
        $metricsLines.Add((New-ProfileEvent ($base - 4000000) 'view_update' @{
                    frame = $frame
                    duration_ms = $duration
                }))
        $metricsLines.Add((New-ProfileEvent ($base - 3000000) 'draw_submission' @{
                    frame = $frame
                    duration_ms = $duration
                }))
        $metricsLines.Add((New-ProfileEvent ($base - 2000000) 'render_pass' @{
                    frame = $frame
                    duration_ms = $duration
                }))
        if ($frame -eq 1 -or $frame -eq 3) {
            $metricsLines.Add((New-ProfileEvent ($base - 1000000) 'implot.axis_limits_changed' @{
                        frame = $frame
                    }))
        }
        $metricsLines.Add((New-ProfileEvent $base 'present' @{
                    frame = $frame
                    duration_ms = $duration
                }))
    }
    $metricsLines.Add((New-ProfileEvent 1000000000 'implot.pan_drag.state' @{ active = $false; frame = 5 }))
    $metricsLines.Add((New-ProfileEvent 1500000000 'view_update' @{
                frame = 99
                duration_ms = 9999.0
            }))
    $metricsLines.Add((New-ProfileEvent 2000000000 'profile_recorder_summary' @{
                stop_reason = 'explicit'
                accepted_bytes = 4096
                dropped_events = 0
            }))
    [System.IO.File]::WriteAllLines($metricsPath, $metricsLines)
    $metrics = Invoke-Analyzer $metricsPath -ExpectedPanPacing Uncapped
    Assert-True ($metrics.ExitCode -eq 0) "A consistent uncapped profile should pass:`n$($metrics.Output)"
    Assert-True (
        $metrics.Output -match 'submission_fps=4\.004, axis_update_fps=2\.002, repeated_submissions=50\.000% \(2/4\)'
    ) "Pan rates and repeated submissions should be computed inside the pan window:`n$($metrics.Output)"
    Assert-True (
        $metrics.Output -match 'instrumented frame work\s+4\s+10\.000\s+15\.400\s+15\.880'
    ) "Instrumented stages should be summed by frame before percentiles are computed:`n$($metrics.Output)"
    Assert-True (
        $metrics.Output -notmatch '9999\.000'
    ) "An extreme duration outside the pan window must not affect pan percentiles:`n$($metrics.Output)"
    Assert-True (
        $metrics.Output -match 'backend=dxgi.*actual=unavailable'
    ) "DXGI profiles must report Composition display feedback as unavailable:`n$($metrics.Output)"
    Assert-True (
        $metrics.Output -match 'accepted_bytes=4096, dropped_events=0'
    ) "Recorder byte and drop counts should be printed:`n$($metrics.Output)"

    $largePath = Join-Path $temporaryDirectory 'large-uncapped-style.jsonl'
    [System.IO.File]::WriteAllLines($largePath, (New-LargeProfileLines))
    $large = Invoke-Analyzer $largePath
    Assert-True ($large.ExitCode -eq 0) "A large uncapped-style profile should pass:`n$($large.Output)"
    Assert-True (
        $large.Output -match 'Events:\s+100010'
    ) "The large fixture must exercise at least 100,000 events:`n$($large.Output)"
    Assert-True (
        $large.Output -match 'instrumented frame work\s+25000\s+1\.000\s+1\.000\s+1\.000\s+1\.000'
    ) "Large-profile instrumented stages should be summed by frame before percentiles are computed:`n$($large.Output)"
    Assert-True (
        $large.Output -notmatch '9999\.000'
    ) "A large-profile extreme duration outside the pan window must not affect pan percentiles:`n$($large.Output)"

    $missingPacing = Invoke-Analyzer $completePath -ExpectedPanPacing Uncapped
    Assert-True (
        $missingPacing.ExitCode -ne 0 -and
        $missingPacing.Output -match 'pacing marker'
    ) "Uncapped analysis must fail when its runtime marker is missing:`n$($missingPacing.Output)"

    $mismatchPath = Join-Path $temporaryDirectory 'pacing-mismatch.jsonl'
    $mismatchLines = [System.Collections.Generic.List[string]]::new()
    $mismatchLines.Add((New-ProfileEvent 0 'runtime_config' @{
                pan_pacing_requested = 'display'
                pan_pacing_effective = 'display'
                pan_pacing_recognized = $true
            }))
    $mismatchLines.AddRange([string[]](New-PassingProfileLines -IncludeSummary))
    [System.IO.File]::WriteAllLines($mismatchPath, $mismatchLines)
    $mismatch = Invoke-Analyzer $mismatchPath -ExpectedPanPacing Uncapped
    Assert-True (
        $mismatch.ExitCode -ne 0 -and
        $mismatch.Output -match 'marker mismatch'
    ) "Uncapped analysis must fail when the effective marker is display paced:`n$($mismatch.Output)"

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
