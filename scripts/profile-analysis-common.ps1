Set-StrictMode -Version 2

function Get-EventValue {
    param(
        [Parameter(Mandatory = $true)] $Event,
        [Parameter(Mandatory = $true)] [string]$Name,
        $Default = $null
    )

    $property = $Event.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $Default
    }
    return $property.Value
}

function Get-Percentile {
    param(
        [Parameter(Mandatory = $true)] [double[]]$Values,
        [Parameter(Mandatory = $true)] [double]$Percent
    )

    if ($null -eq $Values -or @($Values).Count -eq 0) {
        return [double]::NaN
    }
    $Values = [double[]]@($Values)

    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 1) {
        return [double]$ordered[0]
    }

    $rank = ($ordered.Count - 1) * $Percent / 100.0
    $lower = [Math]::Floor($rank)
    $upper = [Math]::Ceiling($rank)
    if ($lower -eq $upper) {
        return [double]$ordered[[int]$rank]
    }

    $weight = $rank - $lower
    return ([double]$ordered[$lower] * (1.0 - $weight)) + ([double]$ordered[$upper] * $weight)
}

function Get-Stats {
    param([double[]]$Values)

    if ($null -eq $Values -or @($Values).Count -eq 0) {
        return [pscustomobject]@{
            Count = 0
            P50 = [double]::NaN
            P95 = [double]::NaN
            P99 = [double]::NaN
            Max = [double]::NaN
        }
    }
    $Values = [double[]]@($Values)

    return [pscustomobject]@{
        Count = $Values.Count
        P50 = Get-Percentile $Values 50.0
        P95 = Get-Percentile $Values 95.0
        P99 = Get-Percentile $Values 99.0
        Max = ($Values | Measure-Object -Maximum).Maximum
    }
}

function Read-ProfileEvents {
    param(
        [string]$Path,
        [switch]$AllowPartial
    )

    $events = [System.Collections.Generic.List[object]]::new()
    $failures = [System.Collections.Generic.List[string]]::new()
    $lineNumber = 0
    foreach ($rawLine in [System.IO.File]::ReadLines($Path)) {
        $lineNumber++
        $line = $rawLine.Trim()
        if (-not $line) {
            continue
        }

        try {
            $event = $line | ConvertFrom-Json
        }
        catch {
            $message = "${Path}:$lineNumber`: invalid or truncated JSON: $($_.Exception.Message)"
            if (-not $AllowPartial) {
                throw $message
            }
            [void]$failures.Add($message)
            break
        }

        if ($null -ne (Get-EventValue $event 'steady_ns') -and
            $null -ne (Get-EventValue $event 'event')) {
            [void]$events.Add($event)
        }
    }

    return [pscustomobject]@{
        Events = [object[]]$events.ToArray()
        Failures = [string[]]$failures.ToArray()
    }
}

function Test-JsonNonNegativeInteger {
    param($Value)

    $isInteger =
        $Value -is [sbyte] -or $Value -is [byte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
    if ($isInteger) {
        return [decimal]$Value -ge 0
    }
    if ($Value -is [decimal]) {
        return $Value -ge 0 -and [decimal]::Truncate($Value) -eq $Value
    }
    if ($Value -is [System.Numerics.BigInteger]) {
        return $Value -ge [System.Numerics.BigInteger]::Zero
    }
    return $false
}

function Test-ProfileRecorderSummary {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Events,
        [switch]$AllowMissing
    )

    $failures = [System.Collections.Generic.List[string]]::new()
    $summaries = @($Events | Where-Object { (Get-EventValue $_ 'event') -eq 'profile_recorder_summary' })
    $status = 'PASS'
    if ($summaries.Count -eq 0) {
        if ($AllowMissing) {
            $status = 'LEGACY (summary unavailable)'
        } else {
            $status = 'FAIL (summary missing)'
            [void]$failures.Add('profile_recorder_summary is missing; recording completeness cannot be verified.')
        }
    } elseif ($summaries.Count -ne 1) {
        $status = 'FAIL (multiple summaries)'
        [void]$failures.Add("Expected exactly one profile_recorder_summary, found $($summaries.Count).")
    } else {
        $summary = $summaries[0]
        if ((Get-EventValue $Events[-1] 'event') -ne 'profile_recorder_summary') {
            $status = 'FAIL (summary is not final)'
            [void]$failures.Add('profile_recorder_summary is not the final event.')
        }

        $droppedEventsValue = Get-EventValue $summary 'dropped_events'
        if ($null -eq $droppedEventsValue) {
            $status = 'FAIL (dropped_events missing)'
            [void]$failures.Add('profile_recorder_summary.dropped_events is missing.')
        } elseif (-not (Test-JsonNonNegativeInteger $droppedEventsValue)) {
            $status = 'FAIL (dropped_events invalid)'
            [void]$failures.Add('profile_recorder_summary.dropped_events must be a non-negative JSON integer.')
        } elseif ($droppedEventsValue -ne 0) {
            $status = "FAIL ($droppedEventsValue dropped events)"
            [void]$failures.Add("profile_recorder_summary.dropped_events is $droppedEventsValue; recording is incomplete.")
        }

        $stopReason = [string](Get-EventValue $summary 'stop_reason' '')
        if ($stopReason -notin @('explicit', 'duration_limit', 'file_size_limit')) {
            $status = "FAIL (invalid stop reason '$stopReason')"
            [void]$failures.Add("profile_recorder_summary.stop_reason '$stopReason' does not describe a completed recording.")
        }
    }

    return [pscustomobject]@{
        Status = $status
        Failures = [string[]]$failures.ToArray()
    }
}
