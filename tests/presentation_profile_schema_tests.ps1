param([Parameter(Mandatory = $true)][string]$Analyzer)
$ErrorActionPreference = 'Stop'
$directory = Join-Path ([IO.Path]::GetTempPath()) ('specforge-resize-schema-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $directory | Out-Null
$path = Join-Path $directory 'fixture.jsonl'
function Event($name, $phase, $operation) {
    return [ordered]@{ event = $name; schema_version = 1; phase = $phase; viewport_role = 'main';
        hwnd = 42; viewport_id = 7; viewport_lifetime = 1; size_move_id = 0; in_size_move = $false;
        operation_id = $operation; parent_operation_id = 0; frame = 1; old_width = 320; old_height = 240;
        new_width = 640; new_height = 480; backend = 'dxgi'; result = 0; result_valid = ($phase -eq 'end');
        present_completed = ($name -eq 'viewport_present' -and $phase -eq 'end'); count = 0;
        timeout_ms = 0; duration_ms = 0.1; present_mode = 'display_vsync' }
}
function Fixture {
    return @((Event 'viewport_resize' 'begin' 1), (Event 'viewport_resize' 'end' 1),
        (Event 'viewport_present' 'begin' 2), (Event 'viewport_present' 'end' 2),
        @{ event = 'profile_recorder_summary'; stop_reason = 'explicit'; dropped_events = 0 })
}
function Check($rows, $expectSuccess, $requireBreakdown = $false, $requireNative = $false, $requireFeedback = $false) {
    $rows = @((Event 'viewport_lifecycle' 'begin' 0)) + @($rows) + @((Event 'viewport_lifecycle' 'end' 0))
    $rows | ForEach-Object { $_ | ConvertTo-Json -Compress } | Set-Content -LiteralPath $path
    $passed = $true
    try { & $Analyzer -Path $path -RequireDetachedBreakdown:$requireBreakdown -RequireNativeSizeBreakdown:$requireNative -RequireFeedbackBreakdown:$requireFeedback | Out-Null } catch { $passed = $false }
    if ($passed -ne $expectSuccess) { throw "Unexpected schema result: expected $expectSuccess" }
}
try {
    Check (Fixture) $true
    Check (@(@{ event = 'presentation_feedback'; target = 'main'; present_submissions = 4 }) + @(Fixture)) $true
    $limited = Fixture; $limited[4].stop_reason = 'file_size_limit'; Check $limited $false
    Check (Fixture) $false $false $false $true
    function FeedbackFixture {
        $rows = @((Event 'viewport_lifecycle' 'begin' 0),
            (Event 'presentation_feedback_collect' 'begin' 20),
            (Event 'presentation_statistics_drain' 'begin' 21),
            (Event 'presentation_statistics_poll' 'begin' 22), (Event 'presentation_statistics_poll' 'end' 22),
            (Event 'presentation_statistics_item' 'begin' 23),
            (Event 'presentation_statistics_get_next' 'begin' 24), (Event 'presentation_statistics_get_next' 'end' 24),
            (Event 'presentation_statistics_item' 'end' 23),
            (Event 'presentation_statistics_drain' 'end' 21),
            (Event 'presentation_feedback_collect' 'end' 20),
            (Event 'presentation_buffer_release' 'begin' 25),
            (Event 'presentation_resource_release' 'begin' 26), (Event 'presentation_resource_release' 'end' 26),
            (Event 'presentation_buffer_release' 'end' 25), (Event 'viewport_lifecycle' 'end' 0))
        foreach ($row in $rows) {
            $row.hwnd = 43; $row.viewport_lifetime = 2; $row.viewport_role = 'detached'; $row.backend = 'composition'
            $row.parent_operation_id = switch ($row.operation_id) { 21 {20} 22 {21} 23 {21} 24 {23} 26 {25} default {0} }
            if ($row.operation_id -in @(25,26)) { $row.buffer_slot = 1; $row.resource_kind = 'texture' }
        }
        return @(Fixture) + $rows
    }
    Check (FeedbackFixture) $true $false $false $true
    $windowedRows = FeedbackFixture
    foreach ($row in $windowedRows) {
        if ($row.event -eq 'viewport_lifecycle') { $row.event = 'viewport_capture_boundary' }
    }
    Check $windowedRows $true $false $false $true
    Check @($windowedRows | Where-Object { !($_.event -eq 'viewport_capture_boundary' -and $_.phase -eq 'end') }) $false
    foreach ($row in $windowedRows) {
        if ($row.event -eq 'viewport_capture_boundary') { $row.in_size_move = $true; $row.size_move_id = 99 }
    }
    Check $windowedRows $true
    ($windowedRows | Where-Object { $_.event -eq 'viewport_capture_boundary' -and $_.phase -eq 'end' }).size_move_id = 98
    Check $windowedRows $false
    Check (@(@{ event = 'presentation_feedback'; target = 'detached'; present_submissions = 4 }) + @(FeedbackFixture)) $true $false $false $true
    $rows = FeedbackFixture; ($rows | Where-Object operation_id -eq 26)[0].buffer_slot = 3; Check $rows $false
    $rows = FeedbackFixture; ($rows | Where-Object operation_id -eq 26)[0].resource_kind = 'unknown'; Check $rows $false
    $rows = FeedbackFixture; ($rows | Where-Object operation_id -eq 24)[0].parent_operation_id = 21; Check $rows $false
    $rows = FeedbackFixture; ($rows | Where-Object operation_id -eq 22)[0].timeout_ms = 1; Check $rows $false
    Check (Fixture) $true
    try { & $Analyzer -Path $path -RequireClockSync | Out-Null; throw 'Expected missing clock failure' }
    catch { if ($_.Exception.Message -ne 'Missing process/thread clock correlation') { throw } }
    $clock = @{ event = 'presentation_clock_sync'; process_id = 1; thread_id = 2;
        steady_sample_ns = 100; qpc_before = 10; qpc_after = 12; qpc_frequency = 1000; valid = $true }
    Check (@($clock) + @(Fixture)) $true
    & $Analyzer -Path $path -RequireClockSync | Out-Null
    $clock.qpc_after = 9; Check (@($clock) + @(Fixture)) $false
    $rows = Fixture; $rows[1].duration_ms = '0.1'; Check $rows $false
    $rows = Fixture; $rows[1].viewport_lifetime = 2; Check $rows $false
    $rows = Fixture; $rows[4].dropped_events = 1; Check $rows $false
    $rows = Fixture; $rows[3].present_completed = $false; Check $rows $false
    $rows = Fixture; $rows[0].parent_operation_id = 99; Check $rows $false
    $rows = Fixture; $rows[1].viewport_role = 'detached'; Check $rows $false
    Check (Fixture) $false $true
    $breakdown = @((Event 'viewport_draw_submission' 'begin' 3), (Event 'viewport_draw_submission' 'end' 3))
    $detached = @((Event 'viewport_lifecycle' 'begin' 0),
        (Event 'platform_window_size' 'begin' 5), (Event 'platform_window_size' 'end' 5),
        (Event 'viewport_resize' 'begin' 6), (Event 'viewport_resize' 'end' 6),
        (Event 'viewport_draw_submission' 'begin' 8), (Event 'viewport_draw_submission' 'end' 8),
        (Event 'viewport_lifecycle' 'end' 0))
    foreach ($row in $detached) { $row.hwnd = 43; $row.viewport_lifetime = 2; $row.viewport_role = 'detached' }
    foreach ($row in $detached[1..4]) { $row.parent_operation_id = 4 }
    foreach ($row in $detached[5..6]) { $row.parent_operation_id = 7 }
    $update = @((Event 'platform_windows_update' 'begin' 4), (Event 'platform_windows_update' 'end' 4))
    $render = @((Event 'platform_windows_render' 'begin' 7), (Event 'platform_windows_render' 'end' 7))
    foreach ($row in @($update) + @($render)) { $row.hwnd = 0; $row.viewport_lifetime = 0; $row.viewport_role = 'application' }
    $complete = @(Fixture) + $breakdown + @($detached[0], $update[0]) + @($detached[1..4]) +
        @($update[1], $render[0]) + @($detached[5..6]) + @($render[1], $detached[7])
    Check $complete $true $true
    Check $complete $false $true $true
    $native = @((Event 'native_size_callback' 'begin' 10),
        (Event 'native_size_message' 'begin' 11), (Event 'native_size_message' 'end' 11),
        (Event 'native_size_thread_cpu' 'end' 0), (Event 'native_size_observation' 'end' 0),
        (Event 'native_size_callback' 'end' 10))
    foreach ($row in $native) { $row.hwnd = 43; $row.viewport_lifetime = 2; $row.viewport_role = 'detached'; $row.parent_operation_id = 10 }
    $native[0].parent_operation_id = 5; $native[5].parent_operation_id = 5
    foreach ($row in $native[1..2]) { $row.message_id = 70; $row.message_hwnd = 43 }
    $nativeComplete = @(Fixture) + $breakdown + @($detached[0], $update[0], $detached[1]) + $native +
        @($detached[2..4]) + @($update[1], $render[0]) + @($detached[5..6]) + @($render[1], $detached[7])
    Check $nativeComplete $true $true $true
    $native[5].duration_ms = 2.3456
    Check $nativeComplete $true $true $true
    $analysis = & $Analyzer -Path $path -RequireNativeSizeBreakdown | ConvertFrom-Json
    if ([math]::Abs($analysis.slowest_native_size_callbacks[0].unassigned_ms - 2.2456) -gt 0.000001) {
        throw 'Native residual must retain fractional milliseconds'
    }
    $native[4].result = -1; Check $nativeComplete $false $true $true
    $native[4].result = 0; $native[4].count = 1; Check $nativeComplete $false $true $true
    $native[4].count = 0; $native[2].message_id = 71; Check $nativeComplete $false
    $native[2].message_id = 70
    Check @($nativeComplete | Where-Object { $_.event -ne 'native_size_observation' }) $false
    $native[3].result_valid = $false; $native[3].result = -1
    Check $nativeComplete $true $true $true # CPU unavailability is explicit, not missing evidence.
    $rows = Fixture; Check @($rows[0], $rows[2], $rows[3], $rows[4]) $false
    $rows = Fixture; Check @($rows[0], $rows[1], $rows[2], $rows[3]) $false
    Write-Output 'presentation profile schema tests passed'
} finally {
    [IO.File]::Delete($path)
    [IO.Directory]::Delete($directory)
}
