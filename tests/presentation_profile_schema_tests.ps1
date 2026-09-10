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
function Check($rows, $expectSuccess, $requireBreakdown = $false) {
    $rows = @((Event 'viewport_lifecycle' 'begin' 0)) + @($rows) + @((Event 'viewport_lifecycle' 'end' 0))
    $rows | ForEach-Object { $_ | ConvertTo-Json -Compress } | Set-Content -LiteralPath $path
    $passed = $true
    try { & $Analyzer -Path $path -RequireDetachedBreakdown:$requireBreakdown | Out-Null } catch { $passed = $false }
    if ($passed -ne $expectSuccess) { throw "Unexpected schema result: expected $expectSuccess" }
}
try {
    Check (Fixture) $true
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
    $rows = Fixture; Check @($rows[0], $rows[2], $rows[3], $rows[4]) $false
    $rows = Fixture; Check @($rows[0], $rows[1], $rows[2], $rows[3]) $false
    Write-Output 'presentation profile schema tests passed'
} finally {
    [IO.File]::Delete($path)
    [IO.Directory]::Delete($directory)
}
