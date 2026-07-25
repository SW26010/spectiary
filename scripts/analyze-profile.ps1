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

function Test-InWindow {
    param(
        [int64]$Timestamp,
        [object[]]$Windows
    )

    foreach ($window in $Windows) {
        if ($Timestamp -ge $window.Start -and $Timestamp -le $window.End) {
            return $true
        }
    }
    return $false
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

function Get-DurationValues {
    param(
        [object[]]$Events,
        [string]$EventName
    )

    $values = [System.Collections.Generic.List[double]]::new()
    foreach ($event in $Events) {
        if ((Get-EventValue $event 'event') -ne $EventName) {
            continue
        }
        $duration = Get-EventValue $event 'duration_ms'
        if ($null -ne $duration) {
            $values.Add((Convert-ToDouble $duration))
        }
    }
    return [double[]]$values.ToArray()
}

function Get-DurationValuesInsideWindows {
    param(
        [object[]]$Events,
        [string]$EventName,
        [object[]]$Windows
    )

    $values = [System.Collections.Generic.List[double]]::new()
    foreach ($event in $Events) {
        if ((Get-EventValue $event 'event') -ne $EventName) {
            continue
        }
        $timestamp = [int64](Get-EventValue $event 'steady_ns')
        if ($Windows.Count -gt 0 -and -not (Test-InWindow $timestamp $Windows)) {
            continue
        }
        $duration = Get-EventValue $event 'duration_ms'
        if ($null -ne $duration) {
            $values.Add((Convert-ToDouble $duration))
        }
    }
    return [double[]]$values.ToArray()
}

function Get-InstrumentedFrameWorkValues {
    param(
        [object[]]$Events,
        [object[]]$Windows
    )

    $stageNames = @{
        view_update = $true
        draw_submission = $true
        render_pass = $true
        present = $true
    }
    $totals = @{}
    foreach ($event in $Events) {
        $eventName = [string](Get-EventValue $event 'event' '')
        if (-not $stageNames.ContainsKey($eventName)) {
            continue
        }
        $timestamp = [int64](Get-EventValue $event 'steady_ns')
        if ($Windows.Count -gt 0 -and -not (Test-InWindow $timestamp $Windows)) {
            continue
        }
        $frame = Get-EventValue $event 'frame'
        $duration = Get-EventValue $event 'duration_ms'
        if ($null -eq $frame -or $null -eq $duration) {
            continue
        }
        $key = [string]$frame
        if (-not $totals.ContainsKey($key)) {
            $totals[$key] = 0.0
        }
        $totals[$key] += Convert-ToDouble $duration
    }
    return [double[]]@($totals.Values)
}

function Get-EventsInsideWindows {
    param(
        [object[]]$Events,
        [string]$EventName,
        [object[]]$Windows
    )

    return [object[]]@(
        foreach ($event in $Events) {
            if ((Get-EventValue $event 'event') -ne $EventName) {
                continue
            }
            $timestamp = [int64](Get-EventValue $event 'steady_ns')
            if ($Windows.Count -eq 0 -or (Test-InWindow $timestamp $Windows)) {
                $event
            }
        }
    )
}

function Get-EventTimes {
    param(
        [object[]]$Events,
        [string]$EventName
    )

    $times = [System.Collections.Generic.List[int64]]::new()
    foreach ($event in $Events) {
        if ((Get-EventValue $event 'event') -eq $EventName) {
            $times.Add([int64](Get-EventValue $event 'steady_ns'))
        }
    }
    return [int64[]]$times.ToArray()
}

function Get-DragWindows {
    param([object[]]$Events)

    $windows = [System.Collections.Generic.List[object]]::new()
    $start = $null
    $lastTime = 0L
    if ($Events.Count -gt 0) {
        $lastTime = [int64](Get-EventValue $Events[-1] 'steady_ns' 0)
    }

    foreach ($event in $Events) {
        if ((Get-EventValue $event 'event') -ne 'implot.pan_drag.state') {
            continue
        }

        $timestamp = [int64](Get-EventValue $event 'steady_ns')
        $active = Convert-ToBool (Get-EventValue $event 'active')
        if ($active -and $null -eq $start) {
            $start = $timestamp
        }
        elseif (-not $active -and $null -ne $start) {
            $windows.Add([pscustomobject]@{ Start = [int64]$start; End = [int64]$timestamp })
            $start = $null
        }
    }

    if ($null -ne $start) {
        $windows.Add([pscustomobject]@{ Start = [int64]$start; End = [int64]$lastTime })
    }
    return [object[]]$windows.ToArray()
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
    param([object[]]$Events)

    $displayEvents = @($Events | Where-Object { (Get-EventValue $_ 'event') -eq 'display_environment' })
    if ($displayEvents.Count -eq 0) {
        Write-Host 'Display environment: not logged'
        return
    }

    $event = $displayEvents[-1]
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
        $feedbackEvents = @($Events | Where-Object {
                (Get-EventValue $_ 'event') -eq 'presentation_feedback' -and
                (Get-EventValue $_ 'target') -eq 'main' -and
                [double](Get-EventValue $_ 'last_actual_duration' 0) -gt 0
            })
        $actualText = if ($backend -eq 'dxgi') { 'unavailable' } else { '-' }
        if ($backend -ne 'dxgi' -and $feedbackEvents.Count -gt 0) {
            $actualDuration = [double](Get-EventValue $feedbackEvents[-1] 'last_actual_duration' 0)
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
        [object[]]$Events,
        [string]$Expected,
        [System.Collections.Generic.List[string]]$Failures
    )

    $runtimeMarkers = @($Events | Where-Object {
            (Get-EventValue $_ 'event') -eq 'runtime_config' -and
            $null -ne (Get-EventValue $_ 'pan_pacing_effective')
        })
    if ($runtimeMarkers.Count -eq 0) {
        Write-Host "Pan pacing: expected=$Expected, marker=missing"
        if ($Expected -ne 'Auto') {
            Add-GateFailure $Failures "Expected $Expected pan pacing, but no runtime_config pacing marker was found."
        }
        return
    }

    $runtime = $runtimeMarkers[-1]
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

    $activeMarkers = @($Events | Where-Object {
            (Get-EventValue $_ 'event') -eq 'pan_pacing' -and
            (Convert-ToBool (Get-EventValue $_ 'pan_active'))
        })
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
        [object[]]$PresentEvents,
        [object[]]$AxisEvents,
        [double]$WindowMs
    )

    $windowSeconds = $WindowMs / 1000.0
    $submissionFps = if ($windowSeconds -gt 0) { $PresentEvents.Count / $windowSeconds } else { 0.0 }
    $axisFps = if ($windowSeconds -gt 0) { $AxisEvents.Count / $windowSeconds } else { 0.0 }

    $axisFrames = @{}
    foreach ($event in $AxisEvents) {
        $frame = Get-EventValue $event 'frame'
        if ($null -ne $frame) {
            $axisFrames[[string]$frame] = $true
        }
    }
    $framedSubmissions = 0
    $repeatedSubmissions = 0
    foreach ($event in $PresentEvents) {
        $frame = Get-EventValue $event 'frame'
        if ($null -eq $frame) {
            continue
        }
        $framedSubmissions++
        if (-not $axisFrames.ContainsKey([string]$frame)) {
            $repeatedSubmissions++
        }
    }
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
    param([object[]]$Events)

    $summaries = @($Events | Where-Object {
            (Get-EventValue $_ 'event') -eq 'profile_recorder_summary'
        })
    if ($summaries.Count -eq 0) {
        Write-Host 'Recorder: event_rate=unavailable, accepted_bytes=unavailable, dropped_events=unavailable'
        return
    }

    $summary = $summaries[-1]
    $durationSeconds = 0.0
    if ($Events.Count -gt 1) {
        $durationSeconds =
            ([int64](Get-EventValue $summary 'steady_ns') -
             [int64](Get-EventValue $Events[0] 'steady_ns')) / 1000000000.0
    }
    $eventRate = if ($durationSeconds -gt 0) {
        ($Events.Count - 1) / $durationSeconds
    }
    else {
        0.0
    }
    Write-Host (
        'Recorder: event_rate={0:F3} events/s, accepted_bytes={1}, dropped_events={2}' -f
            $eventRate,
            (Get-EventValue $summary 'accepted_bytes' 'unavailable'),
            (Get-EventValue $summary 'dropped_events' 'unavailable')
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
$readResult = Read-ProfileEvents $resolvedProfile.Path -AllowPartial:$ReportOnly
$rawEvents = @($readResult.Events)
foreach ($failure in $readResult.Failures) {
    Add-GateFailure $gateFailures $failure
}
if ($rawEvents.Count -eq 0) {
    Add-GateFailure $gateFailures "$($resolvedProfile.Path): no profile events found"
}

$summaryCheck = Test-ProfileRecorderSummary $rawEvents -AllowMissing:$AllowLegacyIncompleteRecording
$recordingCompleteness = $summaryCheck.Status
foreach ($failure in $summaryCheck.Failures) {
    Add-GateFailure $gateFailures $failure
}
if ($recordingCompleteness -like 'LEGACY*') {
    Write-Warning 'Legacy profile has no profile_recorder_summary; recording completeness cannot be verified.'
}

$events = @($rawEvents | Sort-Object { [int64](Get-EventValue $_ 'steady_ns') })
Test-PanPacingMarkers $events $ExpectedPanPacing $gateFailures

$windows = @(Get-DragWindows $events)
$totalWindowMs = 0.0
foreach ($window in $windows) {
    $totalWindowMs += Convert-NanosecondsToMilliseconds ($window.End - $window.Start)
}

$inputEvents = @(
    foreach ($event in $events) {
        if ((Get-EventValue $event 'event') -ne 'input') {
            continue
        }
        if ((Get-EventValue $event 'kind') -ne 'pointer_move') {
            continue
        }
        if (-not (Convert-ToBool (Get-EventValue $event 'left_down'))) {
            continue
        }
        $timestamp = [int64](Get-EventValue $event 'steady_ns')
        if ($windows.Count -eq 0 -or (Test-InWindow $timestamp $windows)) {
            $event
        }
    }
)

$inputNs = [int64[]]@($inputEvents | ForEach-Object { [int64](Get-EventValue $_ 'steady_ns') })
$panSampleNs = [int64[]]@(Get-EventTimes $events 'implot.pan_drag.sample')
$axisChangeNs = [int64[]]@(Get-EventTimes $events 'implot.axis_limits_changed')
$presentNs = [int64[]]@(Get-EventTimes $events 'present')
$axisEventsInsideWindows = @(Get-EventsInsideWindows $events 'implot.axis_limits_changed' $windows)
$presentEventsInsideWindows = @(Get-EventsInsideWindows $events 'present' $windows)

Write-Host "Profile: $($resolvedProfile.Path)"
Write-Host "Events: $($events.Count)"
Write-Host "Recording completeness: $recordingCompleteness"
Write-Host "ImPlot pan-drag windows: $($windows.Count)"
Write-Host ('ImPlot pan-drag window time: {0:F1} ms' -f $totalWindowMs)
Write-DisplayEnvironment $events
Write-PanRates $presentEventsInsideWindows $axisEventsInsideWindows $totalWindowMs
Write-RecorderStatistics $events
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
    Write-Metric 'view_update duration' (Get-DurationValuesInsideWindows $events 'view_update' $windows) $BudgetMs
    Write-Metric 'draw_submission duration' (Get-DurationValuesInsideWindows $events 'draw_submission' $windows) $BudgetMs
    Write-Metric 'render_pass duration' (Get-DurationValuesInsideWindows $events 'render_pass' $windows) $BudgetMs
    Write-Metric 'present duration' (Get-DurationValuesInsideWindows $events 'present' $windows) $BudgetMs
    Write-Metric 'instrumented frame work' (Get-InstrumentedFrameWorkValues $events $windows) $BudgetMs
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
if ($panSampleNs.Count -eq 0) {
    Add-GateFailure $gateFailures 'No implot.pan_drag.sample events were found.'
}
if ($axisChangeNs.Count -eq 0) {
    Add-GateFailure $gateFailures 'No implot.axis_limits_changed events were found.'
}
if ($presentNs.Count -eq 0) {
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
if ($panSampleNs.Count -eq 0) {
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
