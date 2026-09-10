[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [switch]$RequireLiveResize,
    [switch]$RequireDetachedBreakdown
)
$ErrorActionPreference = 'Stop'
# Bound input and retain only open operations plus small event counters.
if ((Get-Item -LiteralPath $Path).Length -gt 101MB) { throw 'Profile exceeds the recorder file bound.' }
$names = @('viewport_lifecycle', 'viewport_size_move', 'viewport_initialize',
    'viewport_resize', 'viewport_acquire', 'viewport_present', 'presentation_buffer_rebuild',
    'presentation_buffer_reset', 'presentation_buffer_allocation', 'presentation_source_rect',
    'presentation_render_target', 'presentation_available_wait', 'message_pump_batch',
    'render_invalidation_latency', 'render_frame', 'viewport_draw_submission',
    'platform_windows_update', 'platform_windows_render', 'platform_window_position', 'platform_window_size')
$numeric = @('schema_version', 'hwnd', 'viewport_id', 'viewport_lifetime', 'size_move_id',
    'operation_id', 'parent_operation_id', 'frame', 'old_width', 'old_height', 'new_width',
    'new_height', 'result', 'count', 'timeout_ms', 'duration_ms')
$pending = @{}
$counts = @{}
$lifetimes = @{}
$live = @{}
$lifetimeRoles = @{}
$breakdownRoles = @{}
$moves = @{}
$roles = @{}
$summary = $null
$lineNumber = 0
$reader = [IO.File]::OpenText((Resolve-Path -LiteralPath $Path).Path)
try {
while ($null -ne ($line = $reader.ReadLine())) {
    $lineNumber++
    $row = $line | ConvertFrom-Json
    if ($row.event -eq 'profile_recorder_summary') { $summary = $row; continue }
    if ($row.event -notin $names) { continue }
    foreach ($field in $numeric) {
        $value = $row.$field
        if ($null -eq $value -or $value -is [string] -or $value -is [bool] -or
            $value -isnot [ValueType]) { throw "Line ${lineNumber}: numeric $field missing or invalid" }
        if ($field -ne 'duration_ms' -and [math]::Truncate([double]$value) -ne $value) {
            throw "Line ${lineNumber}: integer $field required"
        }
        if ($field -ne 'result' -and $value -lt 0) { throw "Line ${lineNumber}: negative $field" }
    }
    foreach ($field in @('in_size_move', 'result_valid', 'present_completed')) {
        if ($row.$field -isnot [bool]) { throw "Line ${lineNumber}: boolean $field missing" }
    }
    if ($row.schema_version -ne 1 -or $row.duration_ms -lt 0 -or
        $row.viewport_role -notin @('main', 'detached', 'application') -or
        $row.phase -notin @('begin', 'end', 'cancel') -or
        $row.backend -notin @('none', 'composition', 'dxgi') -or
        $row.present_mode -notin @('none', 'display_vsync', 'compositor_clock', 'immediate')) {
        throw "Line ${lineNumber}: invalid presentation schema"
    }
    if ($row.present_completed -ne ($row.event -eq 'viewport_present' -and
        $row.phase -eq 'end' -and $row.result_valid -and $row.result -eq 0)) {
        throw "Line ${lineNumber}: completion must mean S_OK API return"
    }
    $counts[$row.event]++
    $lifetime = [string]$row.viewport_lifetime
    if ($row.hwnd -ne 0) {
        if ($row.viewport_lifetime -le 0) { throw "Line ${lineNumber}: missing window lifetime" }
        if ($lifetimes.ContainsKey($lifetime) -and $lifetimes[$lifetime] -ne $row.hwnd) {
            throw "Line ${lineNumber}: lifetime changed HWND"
        }
        $lifetimes[$lifetime] = $row.hwnd
        if ($row.event -eq 'viewport_lifecycle' -and $row.phase -eq 'begin') {
            if ($live.ContainsKey($lifetime)) { throw 'Duplicate viewport lifetime begin' }
            $live[$lifetime] = $true
            $lifetimeRoles[$lifetime] = $row.viewport_role
        } elseif (!$live.ContainsKey($lifetime)) {
            throw 'Viewport event without lifecycle begin (partial capture)'
        }
        if ($lifetimeRoles[$lifetime] -ne $row.viewport_role) { throw 'Viewport role changed during its lifetime' }
        if ($row.event -eq 'viewport_lifecycle' -and $row.phase -eq 'end') { $live.Remove($lifetime) }
    }
    if ($row.event -eq 'viewport_size_move') {
        $key = "$lifetime/$($row.size_move_id)"
        if ($row.size_move_id -le 0) { throw 'Missing size-move identity' }
        if ($row.phase -eq 'begin') {
            if ($moves.ContainsKey($key)) { throw 'Duplicate size-move begin' }
            $moves[$key] = $true
        } else {
            if (!$moves.ContainsKey($key)) { throw 'Size-move exit without entry (partial capture)' }
            $moves.Remove($key)
        }
    }
    if ($row.operation_id -gt 0) {
        $key = [string]$row.operation_id
        if ($row.phase -eq 'begin') {
            if ($pending.ContainsKey($key)) { throw 'Duplicate operation begin' }
            if ($row.parent_operation_id -gt 0 -and !$pending.ContainsKey([string]$row.parent_operation_id)) {
                throw 'Missing parent operation'
            }
            $pending[$key] = $row
            if ($pending.Count -gt 4096) { throw 'Open-operation bound exceeded' }
        } else {
            if (!$pending.ContainsKey($key)) { throw 'Operation end without begin (partial capture)' }
            $begin = $pending[$key]
            foreach ($field in @('event', 'parent_operation_id', 'viewport_lifetime', 'hwnd',
                'old_width', 'old_height', 'new_width', 'new_height', 'backend', 'present_mode', 'timeout_ms')) {
                if ($begin.$field -ne $row.$field) { throw "Operation changed $field" }
            }
            $pending.Remove($key)
        }
    }
    if ($row.event -eq 'viewport_draw_submission' -and $row.phase -eq 'end') {
        $breakdownRoles[$row.viewport_role] = $true
    }
    if ($row.event -eq 'viewport_resize' -and $row.phase -eq 'end') {
        if ($row.new_width -le 0 -or $row.new_height -le 0) {
            if (!$row.result_valid -or $row.result -ge 0) { throw 'Invalid resize dimensions without failure' }
        }
        if ($row.result_valid -and $row.result -eq 0) { $roles[$row.viewport_role] = $true }
    }
}
} finally { $reader.Dispose() }
if ($null -eq $summary -or $summary.dropped_events -ne 0 -or $summary.stop_reason -eq 'write_failure') {
    throw 'Missing recorder summary or dropped/failed recording: evidence is incomplete'
}
if ($pending.Count -gt 0 -or $moves.Count -gt 0 -or $live.Count -gt 0) {
    throw 'Unfinished operation, viewport lifetime or size-move session: evidence is incomplete'
}
if (!$counts.ContainsKey('viewport_resize') -or !$counts.ContainsKey('viewport_present')) {
    throw 'No resize/Present coverage'
}
if ($RequireLiveResize -and (!$roles.ContainsKey('main') -or !$roles.ContainsKey('detached') -or
    !$counts.ContainsKey('viewport_size_move') -or !$counts.ContainsKey('message_pump_batch') -or
    !$counts.ContainsKey('render_invalidation_latency'))) { throw 'Missing live main/detached resize coverage' }
if ($RequireDetachedBreakdown -and (!$roles.ContainsKey('detached') -or
    !$breakdownRoles.ContainsKey('main') -or !$breakdownRoles.ContainsKey('detached') -or
    !$counts.ContainsKey('platform_windows_update') -or !$counts.ContainsKey('platform_windows_render') -or
    !$counts.ContainsKey('platform_window_size'))) { throw 'Missing detached resize render/platform breakdown coverage' }
[pscustomobject]@{ schema_valid = $true; dropped_events = 0; event_counts = $counts;
    resize_roles = @($roles.Keys); root_cause = 'not_determined' } | ConvertTo-Json -Depth 5
