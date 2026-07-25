[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Profile,

    [double]$BudgetMs = 7.6923,

    [double]$MinDragMs = 10000.0,

    [int]$MinInputSamples = 100,

    [switch]$ReportOnly,

    [switch]$AllowLegacyIncompleteRecording,

    [ValidateSet('Auto', 'Default', 'Uncapped')]
    [string]$ExpectedPanPacing = 'Auto'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'profile-analysis-common.ps1')
Add-Type -AssemblyName System.Web.Extensions
$profileJsonSerializer = [System.Web.Script.Serialization.JavaScriptSerializer]::new()
$profileJsonSerializer.MaxJsonLength = [int]::MaxValue

function Get-EventValue {
    param(
        [Parameter(Mandatory = $true)] $Event,
        [Parameter(Mandatory = $true)] [string]$Name,
        $Default = $null
    )

    if ($Event -is [System.Collections.IDictionary]) {
        if ($Event.ContainsKey($Name)) {
            return $Event[$Name]
        }
        return $Default
    }

    $property = $Event.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $Default
    }
    return $property.Value
}

function Read-PanProfileEvents {
    param(
        [string]$Path,
        [switch]$AllowPartial
    )

    $events = [System.Collections.Generic.List[object]]::new()
    $failures = [System.Collections.Generic.List[string]]::new()
    $summaryEvents = [System.Collections.Generic.List[object]]::new()
    $activePacingMarkers = [System.Collections.Generic.List[object]]::new()
    $dragWindows = [System.Collections.Generic.List[object]]::new()
    $lineNumber = 0
    $previousTimestamp = [int64]::MinValue
    $isChronological = $true
    $lastRuntimeMarker = $null
    $lastDisplayEvent = $null
    $lastPresentationFeedback = $null
    $dragStart = $null
    $lastTimestamp = 0L
    foreach ($rawLine in [System.IO.File]::ReadLines($Path)) {
        $lineNumber++
        $line = $rawLine.Trim()
        if (-not $line) {
            continue
        }

        try {
            $event = $profileJsonSerializer.DeserializeObject($line)
        }
        catch {
            $message = "${Path}:$lineNumber`: invalid or truncated JSON: $($_.Exception.Message)"
            if (-not $AllowPartial) {
                throw $message
            }
            [void]$failures.Add($message)
            break
        }

        if (-not ($event -is [System.Collections.IDictionary])) {
            continue
        }

        $timestampValue = $event['steady_ns']
        $eventName = $event['event']
        if ($null -eq $timestampValue -or $null -eq $eventName) {
            continue
        }

        $timestamp = [int64]$timestampValue
        if ($timestamp -lt $previousTimestamp) {
            $isChronological = $false
        }
        $previousTimestamp = $timestamp
        $lastTimestamp = $timestamp
        [void]$events.Add($event)

        switch ([string]$eventName) {
            'profile_recorder_summary' {
                [void]$summaryEvents.Add($event)
            }
            'runtime_config' {
                if ($null -ne $event['pan_pacing_effective']) {
                    $lastRuntimeMarker = $event
                }
            }
            'pan_pacing' {
                if ($event['pan_active']) {
                    [void]$activePacingMarkers.Add($event)
                }
            }
            'display_environment' {
                $lastDisplayEvent = $event
            }
            'presentation_feedback' {
                if ($event['target'] -eq 'main' -and
                    [double]$event['last_actual_duration'] -gt 0) {
                    $lastPresentationFeedback = $event
                }
            }
            'implot.pan_drag.state' {
                $active = Convert-ToBool $event['active']
                if ($active -and $null -eq $dragStart) {
                    $dragStart = $timestamp
                }
                elseif (-not $active -and $null -ne $dragStart) {
                    [void]$dragWindows.Add(
                        [pscustomobject]@{ Start = [int64]$dragStart; End = $timestamp }
                    )
                    $dragStart = $null
                }
            }
        }
    }

    if ($null -ne $dragStart) {
        [void]$dragWindows.Add(
            [pscustomobject]@{ Start = [int64]$dragStart; End = $lastTimestamp }
        )
    }

    return [pscustomobject]@{
        Events = [object[]]$events.ToArray()
        Failures = [string[]]$failures.ToArray()
        IsChronological = $isChronological
        SummaryEvents = [object[]]$summaryEvents.ToArray()
        LastSummary = if ($summaryEvents.Count -gt 0) { $summaryEvents[-1] } else { $null }
        LastRuntimeMarker = $lastRuntimeMarker
        ActivePacingMarkers = [object[]]$activePacingMarkers.ToArray()
        DragWindows = [object[]]$dragWindows.ToArray()
        LastDisplayEvent = $lastDisplayEvent
        LastPresentationFeedback = $lastPresentationFeedback
    }
}

function Get-PanProfileMetadata {
    param([object[]]$Events)

    $activePacingMarkers = [System.Collections.Generic.List[object]]::new()
    $dragWindows = [System.Collections.Generic.List[object]]::new()
    $lastRuntimeMarker = $null
    $lastSummary = $null
    $lastDisplayEvent = $null
    $lastPresentationFeedback = $null
    $dragStart = $null
    $lastTimestamp = 0L

    foreach ($event in $Events) {
        $eventName = [string]$event['event']
        $timestamp = [int64]$event['steady_ns']
        $lastTimestamp = $timestamp
        switch ($eventName) {
            'profile_recorder_summary' {
                $lastSummary = $event
            }
            'runtime_config' {
                if ($null -ne $event['pan_pacing_effective']) {
                    $lastRuntimeMarker = $event
                }
            }
            'pan_pacing' {
                if ($event['pan_active']) {
                    [void]$activePacingMarkers.Add($event)
                }
            }
            'display_environment' {
                $lastDisplayEvent = $event
            }
            'presentation_feedback' {
                if ($event['target'] -eq 'main' -and
                    [double]$event['last_actual_duration'] -gt 0) {
                    $lastPresentationFeedback = $event
                }
            }
            'implot.pan_drag.state' {
                $active = Convert-ToBool $event['active']
                if ($active -and $null -eq $dragStart) {
                    $dragStart = $timestamp
                }
                elseif (-not $active -and $null -ne $dragStart) {
                    [void]$dragWindows.Add(
                        [pscustomobject]@{ Start = [int64]$dragStart; End = $timestamp }
                    )
                    $dragStart = $null
                }
            }
        }
    }

    if ($null -ne $dragStart) {
        [void]$dragWindows.Add(
            [pscustomobject]@{ Start = [int64]$dragStart; End = $lastTimestamp }
        )
    }

    return [pscustomobject]@{
        LastSummary = $lastSummary
        LastRuntimeMarker = $lastRuntimeMarker
        ActivePacingMarkers = [object[]]$activePacingMarkers.ToArray()
        DragWindows = [object[]]$dragWindows.ToArray()
        LastDisplayEvent = $lastDisplayEvent
        LastPresentationFeedback = $lastPresentationFeedback
    }
}

function Convert-ToDouble {
    param($Value)
    if ($null -eq $Value) {
        return [double]::NaN
    }
    return [double]$Value
}

function Convert-ToBool {
    param($Value)
    if ($Value -is [bool]) {
        return $Value
    }
    if ($null -eq $Value) {
        return $false
    }
    return [System.Convert]::ToBoolean($Value)
}


function Format-Milliseconds {
    param([double]$Value)
    if ([double]::IsNaN($Value)) {
        return '-'
    }
    return ('{0,8:F3}' -f $Value)
}

function Format-Number {
    param($Value)
    if ($null -eq $Value) {
        return '-'
    }
    return ('{0:F3}' -f [double]$Value)
}

function Convert-NanosecondsToMilliseconds {
    param([int64]$Nanoseconds)
    return [double]$Nanoseconds / 1000000.0
}

function Get-IntervalsMs {
    param([int64[]]$TimesNs)

    $values = [System.Collections.Generic.List[double]]::new()
    for ($index = 1; $index -lt $TimesNs.Count; $index++) {
        $delta = $TimesNs[$index] - $TimesNs[$index - 1]
        if ($delta -ge 0) {
            $values.Add((Convert-NanosecondsToMilliseconds $delta))
        }
    }
    return [double[]]$values.ToArray()
}

function Get-IntervalsMsInsideWindows {
    param(
        [int64[]]$TimesNs,
        [object[]]$Windows
    )

    if ($Windows.Count -eq 0) {
        return Get-IntervalsMs $TimesNs
    }

    $values = [System.Collections.Generic.List[double]]::new()
    foreach ($window in $Windows) {
        $windowTimes = [int64[]]@($TimesNs | Where-Object { $_ -ge $window.Start -and $_ -le $window.End })
        foreach ($value in (Get-IntervalsMs $windowTimes)) {
            $values.Add($value)
        }
    }
    return [double[]]$values.ToArray()
}

function Get-NextLatenciesMs {
    param(
        [int64[]]$InputsNs,
        [int64[]]$TargetsNs
    )

    $values = [System.Collections.Generic.List[double]]::new()
    if ($InputsNs.Count -eq 0 -or $TargetsNs.Count -eq 0) {
        return [double[]]$values.ToArray()
    }

    $targetIndex = 0
    foreach ($inputNs in $InputsNs) {
        while ($targetIndex -lt $TargetsNs.Count -and $TargetsNs[$targetIndex] -lt $inputNs) {
            $targetIndex++
        }
        if ($targetIndex -lt $TargetsNs.Count) {
            $values.Add((Convert-NanosecondsToMilliseconds ($TargetsNs[$targetIndex] - $inputNs)))
        }
    }
    return [double[]]$values.ToArray()
}

function Get-NextLatenciesMsInsideWindows {
    param(
        [int64[]]$InputsNs,
        [int64[]]$TargetsNs,
        [object[]]$Windows
    )

    if ($Windows.Count -eq 0) {
        return Get-NextLatenciesMs $InputsNs $TargetsNs
    }

    $values = [System.Collections.Generic.List[double]]::new()
    foreach ($window in $Windows) {
        $windowInputs = [int64[]]@($InputsNs | Where-Object { $_ -ge $window.Start -and $_ -le $window.End })
        $windowTargets = [int64[]]@($TargetsNs | Where-Object { $_ -ge $window.Start -and $_ -le $window.End })
        foreach ($value in (Get-NextLatenciesMs $windowInputs $windowTargets)) {
            $values.Add($value)
        }
    }
    return [double[]]$values.ToArray()
}

function Collect-PanAnalysisData {
    param(
        [object[]]$Events,
        [object[]]$Windows
    )

    $inputNs = [System.Collections.Generic.List[int64]]::new()
    $panSampleNs = [System.Collections.Generic.List[int64]]::new()
    $axisChangeNs = [System.Collections.Generic.List[int64]]::new()
    $presentNs = [System.Collections.Generic.List[int64]]::new()
    $presentFrames = [System.Collections.Generic.List[string]]::new()
    $axisFrames = @{}
    $durationValues = @{
        view_update = [System.Collections.Generic.List[double]]::new()
        draw_submission = [System.Collections.Generic.List[double]]::new()
        render_pass = [System.Collections.Generic.List[double]]::new()
        present = [System.Collections.Generic.List[double]]::new()
    }
    $frameTotals = @{}
    $panSampleEventCount = 0
    $axisChangeEventCount = 0
    $presentEventCount = 0
    $hasWindows = $Windows.Count -gt 0
    $windowIndex = 0

    foreach ($event in $Events) {
        $eventName = [string]$event['event']
        $timestamp = [int64]$event['steady_ns']
        $insideWindow = -not $hasWindows
        if ($hasWindows) {
            while ($windowIndex -lt $Windows.Count -and
                $timestamp -gt $Windows[$windowIndex].End) {
                $windowIndex++
            }
            $insideWindow =
                $windowIndex -lt $Windows.Count -and
                $timestamp -ge $Windows[$windowIndex].Start -and
                $timestamp -le $Windows[$windowIndex].End
        }

        switch ($eventName) {
            'input' {
                if ($insideWindow -and
                    $event['kind'] -eq 'pointer_move' -and
                    (Convert-ToBool $event['left_down'])) {
                    [void]$inputNs.Add($timestamp)
                }
            }
            'implot.pan_drag.sample' {
                $panSampleEventCount++
                if ($insideWindow) {
                    [void]$panSampleNs.Add($timestamp)
                }
            }
            'implot.axis_limits_changed' {
                $axisChangeEventCount++
                if ($insideWindow) {
                    [void]$axisChangeNs.Add($timestamp)
                    $axisFrame = $event['frame']
                    if ($null -ne $axisFrame) {
                        $axisFrames[[string]$axisFrame] = $true
                    }
                }
            }
            'present' {
                $presentEventCount++
                if ($insideWindow) {
                    [void]$presentNs.Add($timestamp)
                    $presentFrame = $event['frame']
                    if ($null -ne $presentFrame) {
                        [void]$presentFrames.Add([string]$presentFrame)
                    }
                }
            }
        }

        if (-not $insideWindow -or -not $durationValues.ContainsKey($eventName)) {
            continue
        }

        $duration = $event['duration_ms']
        if ($null -eq $duration) {
            continue
        }
        $durationValue = Convert-ToDouble $duration
        [void]$durationValues[$eventName].Add($durationValue)

        $frame = $event['frame']
        if ($null -eq $frame) {
            continue
        }
        $key = [string]$frame
        if (-not $frameTotals.ContainsKey($key)) {
            $frameTotals[$key] = 0.0
        }
        $frameTotals[$key] += $durationValue
    }

    $repeatedSubmissions = 0
    foreach ($frame in $presentFrames) {
        if (-not $axisFrames.ContainsKey($frame)) {
            $repeatedSubmissions++
        }
    }

    return [pscustomobject]@{
        InputNs = [int64[]]$inputNs.ToArray()
        PanSampleNs = [int64[]]$panSampleNs.ToArray()
        AxisChangeNs = [int64[]]$axisChangeNs.ToArray()
        PresentNs = [int64[]]$presentNs.ToArray()
        ViewUpdateDurations = [double[]]$durationValues.view_update.ToArray()
        DrawSubmissionDurations = [double[]]$durationValues.draw_submission.ToArray()
        RenderPassDurations = [double[]]$durationValues.render_pass.ToArray()
        PresentDurations = [double[]]$durationValues.present.ToArray()
        InstrumentedFrameWork = [double[]]@($frameTotals.Values)
        PanSampleEventCount = $panSampleEventCount
        AxisChangeEventCount = $axisChangeEventCount
        PresentEventCount = $presentEventCount
        FramedSubmissionCount = $presentFrames.Count
        RepeatedSubmissionCount = $repeatedSubmissions
    }
}

function Write-Metric {
    param(
        [string]$Name,
        [double[]]$Values,
        [double]$BudgetMs
    )

    $stats = Get-Stats $Values
    $budgetText = 'EMPTY'
    if ($stats.Count -gt 0) {
        if ($stats.P95 -le $BudgetMs) {
            $budgetText = 'PASS'
        }
        else {
            $budgetText = 'MISS'
        }
    }

    Write-Host ('{0,-34} {1,7} {2} {3} {4} {5} {6,6}' -f `
            $Name,
            $stats.Count,
            (Format-Milliseconds $stats.P50),
            (Format-Milliseconds $stats.P95),
            (Format-Milliseconds $stats.P99),
            (Format-Milliseconds $stats.Max),
            $budgetText)

    return [pscustomobject]@{
        Name = $Name
        Count = $stats.Count
        P95 = $stats.P95
        Status = $budgetText
    }
}

function Write-DisplayEnvironment {
    param(
        $DisplayEvent,
        $PresentationFeedback
    )

    if ($null -eq $DisplayEvent) {
        Write-Host 'Display environment: not logged'
        return
    }

    $event = $DisplayEvent
    $dwmTimingOk = Convert-ToBool (Get-EventValue $event 'dwm_timing_ok')
    if ($dwmTimingOk) {
        $dwmText = 'DWM={0} Hz ({1} ms, target={2})' -f `
            (Format-Number (Get-EventValue $event 'dwm_refresh_hz')),
            (Format-Number (Get-EventValue $event 'dwm_refresh_period_ms')),
            (Get-EventValue $event 'dwm_query_target' '-')
    }
    else {
        $dwmText = 'DWM unavailable (result={0}, target={1})' -f `
            (Get-EventValue $event 'dwm_result' '-'),
            (Get-EventValue $event 'dwm_query_target' '-')
    }

    Write-Host (
        'Display environment: reason={0}, monitor={1}, Windows mode={2} Hz, {3}' -f `
            (Get-EventValue $event 'reason' '-'),
            (Get-EventValue $event 'monitor_device' '-'),
            (Format-Number (Get-EventValue $event 'display_mode_frequency_hz')),
            $dwmText
    )

    $backend = Get-EventValue $event 'presentation_backend' ''
    if ($backend) {
        $actualText = if ($backend -eq 'dxgi') { 'unavailable' } else { '-' }
        if ($backend -ne 'dxgi' -and $null -ne $PresentationFeedback) {
            $actualDuration = [double](Get-EventValue $PresentationFeedback 'last_actual_duration' 0)
            $actualText = '{0} Hz' -f (Format-Number (10000000.0 / $actualDuration))
        }
        Write-Host (
            'Presentation: backend={0}, degradation={1}, DRR={2}, virtual/physical/requested={3}/{4}/{5} Hz, actual={6}, system constraint={7}' -f `
                $backend,
                (Get-EventValue $event 'presentation_degradation' '-'),
                (Get-EventValue $event 'display_config_drr_configured' '-'),
                (Format-Number (Get-EventValue $event 'virtual_refresh_hz')),
                (Format-Number (Get-EventValue $event 'physical_refresh_hz')),
                (Format-Number (Get-EventValue $event 'presentation_requested_refresh_hz')),
                $actualText,
                (Get-EventValue $event 'system_refresh_constrained' '-')
        )
    }
    else {
        Write-Host (
            'Presentation: legacy DXGI, Present sync={0}, swapchain desc={1} Hz' -f `
                (Get-EventValue $event 'swapchain_present_sync_interval' '-'),
                (Format-Number (Get-EventValue $event 'swapchain_desc_refresh_hz'))
        )
    }
}

function Test-PanPacingMarkers {
    param(
        $RuntimeMarker,
        [object[]]$ActiveMarkers,
        [string]$Expected,
        [System.Collections.Generic.List[string]]$Failures
    )

    if ($null -eq $RuntimeMarker) {
        Write-Host "Pan pacing: expected=$Expected, marker=missing"
        if ($Expected -ne 'Auto') {
            Add-GateFailure $Failures "Expected $Expected pan pacing, but no runtime_config pacing marker was found."
        }
        return
    }

    $runtime = $RuntimeMarker
    $requested = [string](Get-EventValue $runtime 'pan_pacing_requested' '')
    $effective = [string](Get-EventValue $runtime 'pan_pacing_effective' '')
    $recognized = Convert-ToBool (Get-EventValue $runtime 'pan_pacing_recognized')
    Write-Host "Pan pacing: expected=$Expected, requested=$requested, effective=$effective, recognized=$recognized"

    if ($Expected -eq 'Auto') {
        return
    }

    $expectedEffective = if ($Expected -eq 'Uncapped') { 'uncapped' } else { 'display' }
    if ($effective -ne $expectedEffective -or -not $recognized) {
        Add-GateFailure $Failures (
            "Pan pacing marker mismatch: expected effective '$expectedEffective', found requested='$requested', effective='$effective', recognized='$recognized'."
        )
        return
    }

    if ($Expected -ne 'Uncapped') {
        return
    }

    if ($activeMarkers.Count -eq 0) {
        Add-GateFailure $Failures 'Uncapped analysis requires an active pan_pacing marker.'
        return
    }

    foreach ($marker in $activeMarkers) {
        $backend = [string](Get-EventValue $marker 'backend' '')
        $presentMode = [string](Get-EventValue $marker 'present_mode' '')
        $syncInterval = [int](Get-EventValue $marker 'sync_interval' -1)
        $presentFlags = [int](Get-EventValue $marker 'present_flags' -1)
        $tearingSupported = Convert-ToBool (Get-EventValue $marker 'tearing_supported')
        $expectedFlags = if ($tearingSupported) { 512 } else { 0 }
        $continuous = Convert-ToBool (Get-EventValue $marker 'continuous_rendering')
        if ((Get-EventValue $marker 'effective' '') -ne 'uncapped' -or
            $backend -ne 'dxgi' -or
            $presentMode -ne 'immediate' -or
            $syncInterval -ne 0 -or
            $presentFlags -ne $expectedFlags -or
            -not $continuous) {
            Add-GateFailure $Failures (
                "Uncapped pacing marker is inconsistent: backend='$backend', mode='$presentMode', sync=$syncInterval, flags=$presentFlags, tearing_supported=$tearingSupported, continuous=$continuous."
            )
            return
        }
    }
}

function Write-PanRates {
    param(
        [int]$PresentCount,
        [int]$AxisCount,
        [int]$FramedSubmissions,
        [int]$RepeatedSubmissions,
        [double]$WindowMs
    )

    $windowSeconds = $WindowMs / 1000.0
    $submissionFps = if ($windowSeconds -gt 0) { $PresentCount / $windowSeconds } else { 0.0 }
    $axisFps = if ($windowSeconds -gt 0) { $AxisCount / $windowSeconds } else { 0.0 }
    $repeatRatio = if ($framedSubmissions -gt 0) {
        100.0 * $repeatedSubmissions / $framedSubmissions
    }
    else {
        0.0
    }
    Write-Host (
        'Pan rates: submission_fps={0:F3}, axis_update_fps={1:F3}, repeated_submissions={2:F3}% ({3}/{4})' -f
            $submissionFps,
            $axisFps,
            $repeatRatio,
            $repeatedSubmissions,
            $framedSubmissions
    )
}

function Write-RecorderStatistics {
    param(
        $Summary,
        $FirstEvent,
        [int]$EventCount
    )

    if ($null -eq $Summary) {
        Write-Host 'Recorder: event_rate=unavailable, accepted_bytes=unavailable, dropped_events=unavailable'
        return
    }

    $durationSeconds = 0.0
    if ($EventCount -gt 1) {
        $durationSeconds =
            ([int64](Get-EventValue $Summary 'steady_ns') -
             [int64](Get-EventValue $FirstEvent 'steady_ns')) / 1000000000.0
    }
    $eventRate = if ($durationSeconds -gt 0) {
        ($EventCount - 1) / $durationSeconds
    }
    else {
        0.0
    }
    Write-Host (
        'Recorder: event_rate={0:F3} events/s, accepted_bytes={1}, dropped_events={2}' -f
            $eventRate,
            (Get-EventValue $Summary 'accepted_bytes' 'unavailable'),
            (Get-EventValue $Summary 'dropped_events' 'unavailable')
    )
}

function Add-GateFailure {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$Message
    )

    $Failures.Add($Message)
}

$resolvedProfile = Resolve-Path -Path $Profile
$gateFailures = [System.Collections.Generic.List[string]]::new()
$readResult = Read-PanProfileEvents $resolvedProfile.Path -AllowPartial:$ReportOnly
$rawEvents = @($readResult.Events)
foreach ($failure in $readResult.Failures) {
    Add-GateFailure $gateFailures $failure
}
if ($rawEvents.Count -eq 0) {
    Add-GateFailure $gateFailures "$($resolvedProfile.Path): no profile events found"
}

$summaryEvents = @($readResult.SummaryEvents)
$summaryCheckEvents = [System.Collections.Generic.List[object]]::new()
foreach ($summaryEvent in $summaryEvents) {
    [void]$summaryCheckEvents.Add($summaryEvent)
}
if ($summaryEvents.Count -eq 0) {
    $completenessSentinel = if ($rawEvents.Count -gt 0) {
        $rawEvents[-1]
    }
    else {
        [pscustomobject]@{ event = '' }
    }
    [void]$summaryCheckEvents.Add($completenessSentinel)
}
elseif ($summaryEvents.Count -eq 1 -and
    $rawEvents.Count -gt 0 -and
    $rawEvents[-1]['event'] -ne 'profile_recorder_summary') {
    [void]$summaryCheckEvents.Add($rawEvents[-1])
}
$summaryCheck = Test-ProfileRecorderSummary `
    ([object[]]$summaryCheckEvents.ToArray()) `
    -AllowMissing:$AllowLegacyIncompleteRecording
$recordingCompleteness = $summaryCheck.Status
foreach ($failure in $summaryCheck.Failures) {
    Add-GateFailure $gateFailures $failure
}
if ($recordingCompleteness -like 'LEGACY*') {
    Write-Warning 'Legacy profile has no profile_recorder_summary; recording completeness cannot be verified.'
}

$events = if ($readResult.IsChronological) {
    $rawEvents
}
else {
    @($rawEvents | Sort-Object { [int64](Get-EventValue $_ 'steady_ns') })
}
$profileMetadata = if ($readResult.IsChronological) {
    $readResult
}
else {
    Get-PanProfileMetadata $events
}
Test-PanPacingMarkers `
    $profileMetadata.LastRuntimeMarker `
    $profileMetadata.ActivePacingMarkers `
    $ExpectedPanPacing `
    $gateFailures

$windows = @($profileMetadata.DragWindows)
$totalWindowMs = 0.0
foreach ($window in $windows) {
    $totalWindowMs += Convert-NanosecondsToMilliseconds ($window.End - $window.Start)
}

$analysisData = Collect-PanAnalysisData $events $windows
$inputNs = $analysisData.InputNs
$panSampleNs = $analysisData.PanSampleNs
$axisChangeNs = $analysisData.AxisChangeNs
$presentNs = $analysisData.PresentNs

Write-Host "Profile: $($resolvedProfile.Path)"
Write-Host "Events: $($events.Count)"
Write-Host "Recording completeness: $recordingCompleteness"
Write-Host "ImPlot pan-drag windows: $($windows.Count)"
Write-Host ('ImPlot pan-drag window time: {0:F1} ms' -f $totalWindowMs)
Write-DisplayEnvironment $profileMetadata.LastDisplayEvent $profileMetadata.LastPresentationFeedback
Write-PanRates `
    $presentNs.Count `
    $axisChangeNs.Count `
    $analysisData.FramedSubmissionCount `
    $analysisData.RepeatedSubmissionCount `
    $totalWindowMs
$recorderSummary = $profileMetadata.LastSummary
$firstEvent = if ($events.Count -gt 0) { $events[0] } else { $null }
Write-RecorderStatistics $recorderSummary $firstEvent $events.Count
Write-Host 'Measurement scope: submission_fps is application submission rate; display/photon FPS and CPU/GPU/power are unmeasured.'
Write-Host ('Budget: p95 <= {0:F4} ms' -f $BudgetMs)
Write-Host ('Quality gate: drag >= {0:F1} ms, input samples >= {1}' -f $MinDragMs, $MinInputSamples)
Write-Host ''
Write-Host ('{0,-34} {1,7} {2,8} {3,8} {4,8} {5,8} {6,6}' -f 'metric', 'count', 'p50', 'p95', 'p99', 'max', 'budget')
Write-Host ('-' * 84)
$metrics = @(
    Write-Metric 'win32 drag move interval' (Get-IntervalsMsInsideWindows $inputNs $windows) $BudgetMs
    Write-Metric 'implot pan sample interval' (Get-IntervalsMsInsideWindows $panSampleNs $windows) $BudgetMs
    Write-Metric 'present interval' (Get-IntervalsMsInsideWindows $presentNs $windows) $BudgetMs
    Write-Metric 'input -> pan sample' (Get-NextLatenciesMsInsideWindows $inputNs $panSampleNs $windows) $BudgetMs
    Write-Metric 'input -> axis limits' (Get-NextLatenciesMsInsideWindows $inputNs $axisChangeNs $windows) $BudgetMs
    Write-Metric 'input -> present' (Get-NextLatenciesMsInsideWindows $inputNs $presentNs $windows) $BudgetMs
    Write-Metric 'view_update duration' $analysisData.ViewUpdateDurations $BudgetMs
    Write-Metric 'draw_submission duration' $analysisData.DrawSubmissionDurations $BudgetMs
    Write-Metric 'render_pass duration' $analysisData.RenderPassDurations $BudgetMs
    Write-Metric 'present duration' $analysisData.PresentDurations $BudgetMs
    Write-Metric 'instrumented frame work' $analysisData.InstrumentedFrameWork $BudgetMs
)

if ($windows.Count -eq 0) {
    Add-GateFailure $gateFailures 'No ImPlot pan-drag window was detected.'
}
if ($totalWindowMs -lt $MinDragMs) {
    Add-GateFailure $gateFailures ('Drag window time {0:F1} ms is below the {1:F1} ms minimum.' -f $totalWindowMs, $MinDragMs)
}
if ($inputNs.Count -lt $MinInputSamples) {
    Add-GateFailure $gateFailures "Left-button pointer_move samples $($inputNs.Count) are below the $MinInputSamples minimum."
}
if ($analysisData.PanSampleEventCount -eq 0) {
    Add-GateFailure $gateFailures 'No implot.pan_drag.sample events were found.'
}
if ($analysisData.AxisChangeEventCount -eq 0) {
    Add-GateFailure $gateFailures 'No implot.axis_limits_changed events were found.'
}
if ($analysisData.PresentEventCount -eq 0) {
    Add-GateFailure $gateFailures 'No present events were found.'
}

$primaryMetricNames = @(
    'win32 drag move interval',
    'implot pan sample interval',
    'present interval',
    'input -> axis limits',
    'input -> present'
)
foreach ($metricName in $primaryMetricNames) {
    $metric = $metrics | Where-Object { $_.Name -eq $metricName } | Select-Object -First 1
    if ($null -eq $metric) {
        Add-GateFailure $gateFailures "Primary metric '$metricName' was not computed."
        continue
    }
    if ($metric.Status -ne 'PASS') {
        if ($metric.Count -eq 0) {
            Add-GateFailure $gateFailures "Primary metric '$metricName' has no samples."
        }
        else {
            Add-GateFailure $gateFailures ("Primary metric '{0}' p95 {1:F3} ms exceeds budget {2:F4} ms." -f $metric.Name, $metric.P95, $BudgetMs)
        }
    }
}

if ($inputNs.Count -eq 0) {
    Write-Host ''
    Write-Warning 'No left-button pointer_move events were found inside an ImPlot pan-drag window.'
}
if ($analysisData.PanSampleEventCount -eq 0) {
    Write-Host ''
    Write-Warning 'No implot.pan_drag.sample events were found. Drag inside the main Spectrum plot.'
}

Write-Host ''
if ($gateFailures.Count -eq 0) {
    Write-Host 'Result: PASS'
}
else {
    Write-Host 'Result: FAIL'
    foreach ($failure in $gateFailures) {
        Write-Host " - $failure"
    }

    if (-not $ReportOnly) {
        exit 1
    }
}
