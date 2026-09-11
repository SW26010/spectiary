[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [switch]$RequireLiveResize,
    [switch]$RequireDetachedBreakdown,
    [switch]$RequireNativeSizeBreakdown,
    [switch]$RequireClockSync,
    [ValidateSet('None', 'A', 'B')][string]$RedirectionArm = 'None',
    [switch]$RequireFeedbackBreakdown,
    [switch]$RequireFeedbackAcquireOnly
)
$ErrorActionPreference = 'Stop'
# Bound input and retain only open operations plus small event counters.
if ((Get-Item -LiteralPath $Path).Length -gt 101MB) { throw 'Profile exceeds the recorder file bound.' }
$names = @('viewport_lifecycle', 'viewport_capture_boundary', 'viewport_size_move', 'viewport_initialize',
    'viewport_resize', 'viewport_acquire', 'viewport_present', 'presentation_buffer_rebuild',
    'presentation_buffer_reset', 'presentation_buffer_allocation', 'presentation_source_rect',
    'presentation_render_target', 'presentation_available_wait', 'message_pump_batch',
    'render_invalidation_latency', 'render_frame', 'viewport_draw_submission',
    'platform_windows_update', 'platform_windows_render', 'platform_window_position', 'platform_window_size',
    'native_size_callback', 'native_size_message', 'native_size_thread_cpu', 'native_size_observation',
    'viewport_shutdown', 'presentation_feedback_collect', 'presentation_statistics_drain',
    'presentation_statistics_poll', 'presentation_statistics_item', 'presentation_statistics_get_next',
    'presentation_buffer_release', 'presentation_resource_release')
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
$native = @{}
$nativeResults = [Collections.Generic.List[object]]::new()
$lineNumber = 0
$clockSync = $null
$redirectionBuild = $null
$redirectionSamples = 0
$feedbackCoverage = @{}
$feedbackAcquireOnly = $false
$drainFrames = @{}
$reader = [IO.File]::OpenText((Resolve-Path -LiteralPath $Path).Path)
try {
while ($null -ne ($line = $reader.ReadLine())) {
    $lineNumber++
    $row = $line | ConvertFrom-Json
    if ($row.event -eq 'feedback_experiment') { $feedbackAcquireOnly = ($row.acquire_only -eq $true) }
    if ($row.event -eq 'runtime_config') { $redirectionBuild = $row.redirection_build }
    if ($RedirectionArm -ne 'None' -and $row.event -eq 'platform_window_size' -and $row.viewport_role -eq 'detached') {
        $style = $row.hwnd_ex_style
        if ($null -eq $style -or $style -is [string] -or $style -is [bool] -or $style -isnot [ValueType] -or
            $style -lt 0 -or [math]::Truncate([double]$style) -ne $style) { throw 'Missing or invalid sampled HWND style' }
        if ((($style -band 0x00200000) -ne 0) -ne ($RedirectionArm -eq 'B')) { throw 'HWND style does not match requested A/B arm' }
        $redirectionSamples++
    }
    if ($row.event -eq 'presentation_clock_sync') {
        foreach ($field in @('process_id', 'thread_id', 'steady_sample_ns', 'qpc_before', 'qpc_after', 'qpc_frequency')) {
            $v = $row.$field
            if ($null -eq $v -or $v -is [string] -or $v -is [bool] -or $v -isnot [ValueType] -or
                $v -le 0 -or [math]::Truncate([double]$v) -ne $v) { throw "Invalid clock field $field" }
        }
        if ($row.valid -isnot [bool] -or !$row.valid -or $row.qpc_after -lt $row.qpc_before) { throw 'Invalid clock correlation' }
        $clockSync = $row
        continue
    }
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
    if ($RequireFeedbackAcquireOnly -and $row.event -eq 'presentation_statistics_drain' -and
        $row.viewport_role -eq 'detached' -and $row.phase -eq 'begin') {
        $parent = $pending[[string]$row.parent_operation_id]
        if ($null -eq $parent -or $parent.event -ne 'viewport_acquire') { throw 'Experimental drain must be inside acquisition' }
        $key = [string]$row.viewport_lifetime
        if ($drainFrames.ContainsKey($key) -and $drainFrames[$key] -eq $row.frame) { throw 'Repeated experimental drain in one viewport frame' }
        $drainFrames[$key] = $row.frame
    }
    if ($row.event -eq 'viewport_lifecycle' -and $row.phase -eq 'end') { $drainFrames.Remove([string]$row.viewport_lifetime) }
    if ($row.event -in @('presentation_buffer_release', 'presentation_resource_release')) {
        if ($row.buffer_slot -isnot [ValueType] -or $row.buffer_slot -is [bool] -or
            $row.buffer_slot -lt 0 -or $row.buffer_slot -gt 2 -or [math]::Truncate([double]$row.buffer_slot) -ne $row.buffer_slot) {
            throw 'Invalid release buffer slot'
        }
        if ($row.event -eq 'presentation_resource_release' -and
            $row.resource_kind -notin @('available_event','presentation_buffer','render_target_view','texture')) { throw 'Invalid release resource kind' }
    }
    if ($row.event -in @('presentation_statistics_poll','presentation_statistics_get_next','presentation_statistics_item','presentation_resource_release')) {
        $parent = $pending[[string]$row.parent_operation_id]
        $expectedParent = switch ($row.event) {
            'presentation_statistics_poll' { 'presentation_statistics_drain' }
            'presentation_statistics_get_next' { 'presentation_statistics_item' }
            'presentation_statistics_item' { 'presentation_statistics_drain' }
            'presentation_resource_release' { 'presentation_buffer_release' }
        }
        if ($null -eq $parent -or $parent.event -ne $expectedParent) { throw 'Invalid feedback/release parent' }
        if ($row.event -eq 'presentation_statistics_poll' -and $row.timeout_ms -ne 0) { throw 'Statistics polling must remain nonblocking' }
    }
    if ($row.phase -eq 'end' -and $row.viewport_role -eq 'detached' -and
        $row.event -in @('presentation_feedback_collect','presentation_statistics_drain','presentation_statistics_poll',
            'presentation_statistics_get_next','presentation_resource_release')) { $feedbackCoverage[$row.event] = $true }
    if ($row.event -eq 'native_size_message') {
        foreach ($field in @('message_id', 'message_hwnd')) {
            if ($null -eq $row.$field -or $row.$field -is [string] -or $row.$field -is [bool] -or
                $row.$field -isnot [ValueType] -or $row.$field -lt 0 -or
                [math]::Truncate([double]$row.$field) -ne $row.$field) { throw "Invalid $field" }
        }
    }
    if ($row.event -eq 'native_size_callback' -and $row.phase -eq 'begin') {
        $native[[string]$row.operation_id] = @{ message_ms = 0.0; message_count = 0; cpu_ms = $null; observed = $false; summaries = @{} }
    }
    $nativeParent = [string]$row.parent_operation_id
    if ($row.event -eq 'native_size_message' -and $row.phase -eq 'end' -and $native.ContainsKey($nativeParent)) {
        $native[$nativeParent].message_ms += $row.duration_ms
        $native[$nativeParent].message_count++
    }
    if ($row.event -in @('native_size_thread_cpu', 'native_size_observation')) {
        if (!$native.ContainsKey($nativeParent) -or $row.phase -ne 'end') { throw 'Native summary without active callback' }
        if ($row.operation_id -ne 0 -or $native[$nativeParent].summaries.ContainsKey($row.event)) { throw 'Invalid or duplicate native summary' }
        $native[$nativeParent].summaries[$row.event] = $true
        if ($row.event -eq 'native_size_thread_cpu' -and $row.result_valid) {
            $native[$nativeParent].cpu_ms = $row.duration_ms
        }
        if ($row.event -eq 'native_size_observation') {
            $native[$nativeParent].observed = $row.result_valid -and $row.result -eq 0 -and $row.count -eq 0
            if ($RequireNativeSizeBreakdown -and !$native[$nativeParent].observed) { throw 'Native message hook unavailable or observation overflow/mismatch' }
        }
    }
    if ($row.event -eq 'native_size_callback' -and $row.phase -eq 'end') {
        $key = [string]$row.operation_id
        if (!$native.ContainsKey($key)) { throw 'Native callback end without begin' }
        $value = $native[$key]
        if ($value.summaries.Count -ne 2) { throw 'Missing native callback summary' }
        if ($RequireNativeSizeBreakdown -and !$value.observed) { throw 'Incomplete native callback observation' }
        $nativeResults.Add([pscustomobject]@{ frame = $row.frame; operation_id = $row.operation_id;
            duration_ms = $row.duration_ms; top_level_message_ms = $value.message_ms;
            unassigned_ms = [math]::Max([double]0, [double]($row.duration_ms - $value.message_ms));
            thread_cpu_ms = $value.cpu_ms; top_level_messages = $value.message_count; complete = $value.observed })
        if ($nativeResults.Count -gt 10000) { throw 'Native callback analysis bound exceeded' }
        $native.Remove($key)
    }
    $lifetime = [string]$row.viewport_lifetime
    if ($row.hwnd -ne 0) {
        if ($row.viewport_lifetime -le 0) { throw "Line ${lineNumber}: missing window lifetime" }
        if ($lifetimes.ContainsKey($lifetime) -and $lifetimes[$lifetime] -ne $row.hwnd) {
            throw "Line ${lineNumber}: lifetime changed HWND"
        }
        $lifetimes[$lifetime] = $row.hwnd
        if ($row.event -in @('viewport_lifecycle','viewport_capture_boundary') -and $row.phase -eq 'begin') {
            if ($live.ContainsKey($lifetime)) { throw 'Duplicate viewport lifetime begin' }
            $live[$lifetime] = $true
            $lifetimeRoles[$lifetime] = $row.viewport_role
        } elseif (!$live.ContainsKey($lifetime)) {
            throw 'Viewport event without lifecycle begin (partial capture)'
        }
        if ($lifetimeRoles[$lifetime] -ne $row.viewport_role) { throw 'Viewport role changed during its lifetime' }
        if ($row.event -in @('viewport_lifecycle','viewport_capture_boundary') -and $row.phase -eq 'end') { $live.Remove($lifetime) }
        if ($row.event -eq 'viewport_capture_boundary') {
            if ($row.phase -notin @('begin','end')) { throw 'Invalid capture boundary phase' }
            if ($row.operation_id -ne 0 -or $row.parent_operation_id -ne 0) { throw 'Capture boundary must be outside operations' }
            if ($row.in_size_move) {
                if ($row.size_move_id -le 0) { throw 'Capture boundary missing active size-move identity' }
                $moveKey = "$lifetime/$($row.size_move_id)"
                if ($row.phase -eq 'begin') { $moves[$moveKey] = $true }
                elseif (!$moves.ContainsKey($moveKey)) { throw 'Capture boundary missing active size-move begin' }
                else { $moves.Remove($moveKey) }
            }
        }
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
                'old_width', 'old_height', 'new_width', 'new_height', 'backend', 'present_mode', 'timeout_ms',
                'message_id', 'message_hwnd', 'buffer_slot', 'resource_kind')) {
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
if ($null -eq $summary -or $summary.dropped_events -ne 0 -or $summary.stop_reason -in @('write_failure', 'file_size_limit')) {
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
if ($RequireNativeSizeBreakdown -and (!$roles.ContainsKey('detached') -or $nativeResults.Count -eq 0 -or
    !$counts.ContainsKey('native_size_message'))) { throw 'Missing native size/message coverage' }
if ($RequireClockSync -and $null -eq $clockSync) { throw 'Missing process/thread clock correlation' }
if ($RequireFeedbackBreakdown -and $feedbackCoverage.Count -ne 5) { throw 'Missing detached feedback/release breakdown coverage' }
if ($RequireFeedbackAcquireOnly -and (!$feedbackAcquireOnly -or !$feedbackCoverage.ContainsKey('presentation_statistics_drain'))) {
    throw 'Missing acquisition-only experiment activation or drain coverage'
}
if ($RedirectionArm -ne 'None' -and ($redirectionBuild -ne 'creation_time_ab' -or $redirectionSamples -eq 0)) {
    throw 'A/B evidence requires the isolated build and sampled detached sizes'
}
[pscustomobject]@{ schema_valid = $true; dropped_events = 0; event_counts = $counts;
    slowest_native_size_callbacks = @($nativeResults | Sort-Object duration_ms -Descending | Select-Object -First 10);
    resize_roles = @($roles.Keys); root_cause = 'not_determined' } | ConvertTo-Json -Depth 5
