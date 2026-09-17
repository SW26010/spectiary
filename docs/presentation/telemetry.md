# Live-resize presentation telemetry (schema 1)

For investigation scope, evidence and current decisions, see [issue #56](live-resize/README.md).
The ordinary instrumentation path does not select a resize strategy. It leaves
buffer count, synchronous rebuilding, fallback, Present flags, frame scheduling,
main acquire timeout (1000 ms) and detached acquire timeout (0 ms) unchanged.
Enable the existing bounded JSONL recorder with `SPECFORGE_PROFILE=1` (or the UI).
No per-event clock reads or JSON serialization occur while recording is inactive.
Window identity bookkeeping remains bounded by live presentation objects.

## Events and correlation

Every event below contains `schema_version=1`, `phase`, `viewport_role`
(`main`, `detached`, `application`), `viewport_id`, numeric `hwnd`,
`viewport_lifetime`, `size_move_id`, `in_size_move`, `operation_id`,
`parent_operation_id`, `frame`, `old_width`, `old_height`, `new_width`,
`new_height`, `backend`, `result`, `result_valid`, `present_completed`,
`count`, `timeout_ms`, `present_mode`, and `duration_ms`, plus the recorder's
normal timestamp/thread envelope. IDs and dimensions are unsigned values;
`result` is signed for HRESULT operations and the raw DWORD for available waits.
Durations use steady-clock milliseconds. They measure CPU wall time, including
blocking calls, and exclude the span's own begin/end event serialization.
Nested event overhead is included in its parent's duration.

| Event | Boundary and interpretation |
|---|---|
| `viewport_lifecycle` | `begin` when an initialized presentation object receives its HWND; `end` at shutdown. A reused HWND gets a new lifetime, including failed initialization followed by retry. |
| `viewport_initialize` | Initialize's backend work, including initial buffer creation. |
| `viewport_size_move` | Actual `WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE` observed by the existing UI-thread `WH_CALLWNDPROC` hook, including detached HWNDs. Entry allocates a size-move ID; exit retains it. |
| `viewport_resize` | Whole `D3D11WindowPresentation::Resize`, including fallback and failed/invalid attempts. Old size is the adapter's prior requested size, new size is this request; it is not proof of successful allocation. |
| `presentation_buffer_rebuild` | Composition reset/source-rect/allocation, or DXGI resize/color-space/render-target work. Parent links to resize or initialization. |
| `presentation_buffer_reset` | Release of old buffers/targets. |
| `presentation_source_rect` | Composition `SetSourceRect`. |
| `presentation_buffer_allocation` | Composition creation/registration of all three buffers, or DXGI `ResizeBuffers`. |
| `presentation_render_target` | DXGI render-target rebuilding after `ResizeBuffers`. |
| `viewport_acquire` | Per-viewport `BeginFrame`, including acquire retries and fallback. |
| `presentation_available_wait` | Actual `WaitForMultipleObjects`; `timeout_ms` is the unchanged requested timeout, `count=3`, `result` is wait status. WAIT_FAILED's error is captured before emitting telemetry. Absent on DXGI. |
| `viewport_present` | Whole per-viewport Present call. `present_mode` records the requested mode; backend is the backend at entry. `present_completed=true` only on an end with a valid `S_OK` result, matching existing application completion policy. This is API completion, **not scanout/display completion**. |
| `message_pump_batch` | One outer-loop `PeekMessage`/translate/dispatch batch, including time inside a nested modal move/size loop; end `count` counts removed outer messages. |
| `render_invalidation_latency` | First explicit scheduler `RequestFrame` emits begin; further requests coalesce. Next `RenderFrame` emits end with request count and first-request latency. No-request follow-up frames have operation/count zero; shutdown cancels any pending request. |
| `render_frame` | Whole RenderFrame, including pending resize and early acquire-retry return. |
| `viewport_draw_submission` | Each actual `ImGui_ImplDX11_RenderDrawData` call, with main/detached identity. CPU submission wall time, not GPU execution time. |
| `platform_windows_update` | Whole `ImGui::UpdatePlatformWindows`, nested directly under RenderFrame. Includes platform callbacks and renderer resize callbacks. |
| `platform_windows_render` | Whole `ImGui::RenderPlatformWindowsDefault`, including detached acquire, draw and Present. |
| `platform_window_position` | Existing ImGui `Platform_SetWindowPos` callback, forwarded once with the exact original floating-point arguments. Parent is the surrounding platform update when invoked there. |
| `platform_window_size` | Existing ImGui `Platform_SetWindowSize` callback, forwarded once without changing arguments. New dimensions describe the requested size rounded down to pixels; old dimensions remain the presentation adapter snapshot, not a new native-window query. |

Timed calls emit `begin` and `end` with one operation ID. Nested operations retain
the parent ID and window snapshot. HRESULT/wait results are valid only on returned
calls (`result_valid=true`); void spans and unwinding do not invent success.
Frame zero means outside RenderFrame. Application events use HWND/lifetime zero.
Non-resize events use old dimensions as the current snapshot and new dimensions
zero unless inherited from a resize. Main viewport ID may be zero before ImGui
initialization; HWND + lifetime remains the join key. Size-move state continues
to be tracked when recording is off. Starting a recording mid-operation can
therefore yield a partial interval and is not complete evidence.

ImGui's own detached border resizing need not enter the Windows modal size-move
loop. Its actual buffer resize still emits `viewport_resize`; no synthetic
WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE is invented for that interaction. Neither a
missing modal interval nor a Present return establishes the source of a stall.
Use existing composition statistics separately for display feedback.

## Machine acceptance and actual profile

Run `scripts/analyze-presentation-profile.ps1 -Path <recording.jsonl>` to check
field types, operation pairing/parent links, HWND lifetime consistency, completion
semantics, resize coverage and recorder completeness. `-RequireLiveResize` also
requires main/detached resize, native size-move, message-batch and invalidation
events. Dropped events, missing summaries or partial intervals fail acceptance;
they are not interpreted as zero latency. The script reports `root_cause` as
`not_determined` even when schema acceptance passes.

For a new evidence run, launch with the normal source and profiling enabled,
resize the main window repeatedly for 10–15 seconds, then detach the Spectrum
viewport and resize it for 10–15 seconds. Let each interval settle, redock, and
close normally before the recorder's duration limit. Preserve the JSONL,
build metadata, display/refresh configuration, source identity, interaction order,
and visual observations. Compare parent resize time with reset/allocation/wait,
per-viewport Present, outer message-batch and invalidation-to-frame latency.
Do not substitute hidden-window integration tests for that interactive evidence.

The bounded interactive runner is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -InitialSource <local-source-path>
```

It creates a separate recording directory, preserves presentation settings,
requests normal close after 180 seconds, and runs strict schema acceptance.
It never force-terminates the application. Record from launch through normal
shutdown for full lifecycle acceptance. Alternatively, use the bounded manual
capture mode described below; unmatched spans or boundaries still fail acceptance.

The follow-up to the 2026-09-11 capture needs only detached resize:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario Detached
```

Detach Spectrum, remain still for three seconds, resize for 10–15 seconds,
remain still for three seconds, redock and close normally. Preserve the previous
source/display settings for comparison. This scenario uses
`analyze-presentation-profile.ps1 -RequireDetachedBreakdown`: it requires actual
detached resize, both viewport draw roles, platform update/render and a platform
size callback. It does not require repeating the main-window modal resize.

The original callbacks are installed once, forwarded with unchanged arguments,
and restored at renderer shutdown if still owned by this instrumentation. Null
callbacks stay null. These void calls leave `result_valid=false`; their spans
measure duration without claiming a native API success result. Tests cover exact
argument forwarding, disabled recording, nesting and handler restoration.

Main/detached role is now captured when the presentation lifetime is registered,
so native HWND teardown does not change it. The analyzer now rejects a role
change within a lifetime. The original 00:33 recording retains its known final
main-role mislabel and consequently fails this tightened check; its bytes and
the previously recorded analysis remain unchanged.

Bounded tests cover disabled recording, nested failure results, HWND reuse,
coalesced invalidations, production JSONL fields, malformed/incomplete captures,
actual Win32 modal boundary messages and real D3D resize/Present calls. They
establish observability and unchanged API outcomes, not a performance root cause.

## Native size callback follow-up

The 00:50 capture places the largest detached resize stalls inside the original
ImGui platform size callback. The next capture adds these events without replacing
the compiled ImGui backend or changing its native calls:

| Event | Correlation and meaning |
| --- | --- |
| `native_size_callback` | Begin/end operation inside `platform_window_size`; wall duration of the forwarded callback with instrumentation overhead. |
| `native_size_message` | Begin/end from the existing thread's `WH_CALLWNDPROC` and an additional `WH_CALLWNDPROCRET` hook, only during a size callback. Parent is the callback or enclosing message. `message_id` is the numeric Win32 message; `message_hwnd` is its receiver, which may differ from the owning viewport's `hwnd`. |
| `native_size_thread_cpu` | One end-only summary, operation 0, parent callback operation. `duration_ms` is the thread's kernel plus user CPU delta from `GetThreadTimes`; `result_valid=false`, result -1 explicitly marks an unavailable sample. |
| `native_size_observation` | One end-only summary, operation 0, parent callback operation. Result 0 means the return hook is available; -1 means unavailable. `count` reports overflow, pairing or unwind errors; complete message observation requires result 0 and count 0. |

All events retain viewport lifetime, role, frame, old/new size and parent operation
fields. The two message fields are additive numeric fields in schema 1; historical
events without them remain readable. Message begin/end receiver and ID must match.
Each callback must have exactly one CPU summary and one observation summary.

Message spans include nested work and hook-chain overhead. Only direct child
message durations are summed, avoiding double counting nested messages. Callback
wall time minus that sum is reported as `unassigned_ms`; it is not a measurement
of the entire `SetWindowPos` call or proof of an OS, driver or DWM cause. The CPU
counter has coarse resolution and includes instrumentation; zero does not prove
no CPU work, and wall/CPU differences cannot distinguish preemption from blocking.

Storage is bounded to 32 nested messages per UI thread. Overflow or mismatch is
explicitly reported and disqualifies the strict native evidence gate. Failure to
install the optional return hook does not change application startup success.
The observer never dispatches messages, and the callback wrapper preserves the
original arguments and last-error value. Disabled recording creates no spans.

For this follow-up, repeat only the detached Spectrum outer-border resize:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize
```

Keep the same source and display settings. After detaching, wait three seconds,
resize for 10–15 seconds, wait three seconds, redock and close normally. The runner
requires both detached breakdown coverage and `-RequireNativeSizeBreakdown`.
The analyzer lists the ten slowest callbacks with wall, top-level message, CPU
and unassigned times, retaining at most 10,000 callback summaries. Passing this
gate validates evidence completeness; `root_cause` remains `not_determined`.

Tests cover real nested synchronous dispatch and unchanged return values, bounded
overflow, mismatch cleanup, disabled tracing, callback forwarding/restoration,
missing summaries, receiver/ID mismatch and incomplete hook observations.

## System scheduling capture

The 07:20 recording places a 141.55 ms gap between WM_NCCALCSIZE return and
WM_NCPAINT entry. Collect scheduling and stacks with the same interaction:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -SystemTrace
```

Run this from the usual, non-administrator repository terminal. Windows asks for
administrator approval to start WPR and again to save it. Only the WPR helper is
elevated; SpecForge retains the caller's privileges. Resize the detached Spectrum
outer border for 10–15 seconds, with three seconds idle before and after; redock
and close normally. Allow the second UAC prompt to save the trace.

The helper uses the installed WPR `CPU.Verbose` profile in file mode. Its local
profile details include CSwitch, ReadyThread and SampledProfile stacks and are
saved with the capture. Microsoft documents [CPU wait analysis with WPR/WPA](https://learn.microsoft.com/en-us/troubleshoot/windows-server/support-tools/support-tools-xperf-wpa-wpr)
and [named WPR instances and command options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options).
Each capture has a unique instance; the script never cancels other sessions.
The existing application timeout also bounds the interaction, and a finally
block saves this instance on normal script errors. If the shell is forcibly
closed or saving is denied, use the administrator `recovery_command` recorded
in `system-trace-session.json` to stop and save that specific instance.

The recording directory contains `system-trace.etl`, the normal JSONL,
`process-id.txt`, WPR logs/profile details, session metadata with the EXE SHA256,
and a `symbols` directory containing the matching EXE and PDB. Preserve them
together before rebuilding. WPR is system-wide; its ETL can contain other
process activity and paths. Capture remains local. File-mode ETL size depends
on system activity and is not subject to the JSONL byte limit.

On each recorded viewport lifecycle begin, `presentation_clock_sync` records
`process_id`, UI `thread_id`, `steady_sample_ns`, `qpc_before`, `qpc_after`,
`qpc_frequency` and boolean `valid`. QPC reads bracket the steady clock sample.
For a JSONL timestamp S, the estimated QPC tick is
`(qpc_before + qpc_after)/2 + (S - steady_sample_ns)*qpc_frequency/1e9`;
the bracket width supplies the sampling uncertainty. Convert that tick using
the ETL's own QPC origin/frequency, not wall-clock launch time. The system-trace
runner requires a valid clock record in addition to native resize coverage.

In WPA, select that process/UI thread and the slow callback interval. Inspect
CPU Usage (Precise) switch-out, ready and switch-in times/stacks to distinguish
time waiting from time ready but not scheduled; use sampled stacks as supporting
evidence. Resolve application stacks with the saved PDB. Check ETL event loss,
stack availability and clock alignment before accepting an attribution.
Successful WPR saving and JSONL schema checks alone do not prove usable stacks
or establish a root cause. No resize or presentation policy changes accompany
this capture path.

### Feedback and resource-release breakdown

Capture-window follow-up: the live-resize runner now starts recording **off** when
`-FeedbackBreakdown` or `-WindowedCapture` is selected. Prepare the detached viewport,
then use Settings > Diagnostics > Start Recording. `SPECFORGE_PROFILE_WINDOWED=1`
sets the existing recorder duration bound to five seconds, retaining its ordinary
queue/file bounds and frame-finalization tail. It does not affect runtime-resource
workload duration policy. Close normally after recording finishes; only one recording
is accepted by this runner. This replaces the former launch-to-exit capture instruction
for detailed resize trials.

On manual start, presentation tracing waits for the next complete RenderFrame.
`viewport_capture_boundary` begin snapshots each existing window's real identity,
dimensions and active size-move ID; it is not a fabricated HWND creation event.
The corresponding end snapshot is emitted after spans unwind and before the recorder
seals an active frame. Clock synchronization and sampled window style accompany begin
snapshots. Actual create/destroy events inside the interval remain `viewport_lifecycle`.
Size-move sessions intersecting a boundary are explicitly partial native sessions.
The analyzer accepts bounded window snapshots but still rejects missing span pairs,
missing closing window boundaries, dropped events and file-size-limit termination.
If recording stops while no frame tail is available, absent boundaries still fail
validation; five seconds is a recording limit, not a guarantee of complete evidence
under every minimized/lost-device condition.

The September 11 follow-up adds instrumentation only; polling remains zero-timeout,
statistics are drained with the original loop, and resource release order is unchanged.
All spans use the existing operation/parent-operation, frame and viewport-lifetime IDs.

| Event | Meaning and correlation |
| --- | --- |
| `presentation_feedback_collect` | Per-window `TakeCompositionFeedback`; supplies viewport identity even outside resize. |
| `presentation_statistics_drain` | One statistics drain; end `count` is the number of attempted items, including a failed GetNext call. |
| `presentation_statistics_poll` | Child of drain; `timeout_ms=0`, end `result` is raw WaitForSingleObject return. |
| `presentation_statistics_item` | Child of drain; covers GetNext, processing and the statistics object's Release; end result is GetNext HRESULT. |
| `presentation_statistics_get_next` | Child of item; isolates GetNextPresentStatistics and records its HRESULT. |
| `presentation_buffer_release` | One buffer slot reset; `buffer_slot` is 0..2, scoped to its parent reset and viewport lifetime. |
| `presentation_resource_release` | Child of buffer release; same slot, `resource_kind` is available_event, presentation_buffer, render_target_view or texture. Only non-null resources emit this span. |
| `viewport_shutdown` | Brackets backend shutdown before viewport lifecycle end, preserving release identity. |

Release duration measures synchronous CloseHandle/ComPtr::Reset return, not GPU retirement
or ultimate object destruction. It has no HRESULT (`result_valid=false`). Slot IDs are
reused across rebuilds; they are not allocation IDs. Other events serialize slot -1
unless inherited from a buffer scope, and resource kind `none` unless in a resource scope.

`analyze-presentation-profile.ps1 -RequireFeedbackBreakdown` requires completed detached
feedback, drain, poll, GetNext and resource-release events, in addition to ordinary lifecycle
and pairing checks. It rejects invalid release slots/kinds, incorrect child parent types,
and nonzero statistics polling timeout. Empty statistics queues need not produce GetNext;
a capture without it fails the coverage gate rather than proving that path fast.

The legacy `buffer_replacement_experiment.enabled` event name is retained for analyzer
and historical capture compatibility. It now reports the detached incremental-buffer
policy in production too (true by default); it does not assert that every viewport
selected Composition. Read per-viewport backend transitions for capability/failure fallback.
Only the isolated diagnostic executable reads `SPECFORGE_EXPERIMENT_INCREMENTAL_BUFFERS`;
there, an absent switch still selects baseline buffers for paired comparisons.

Use `profile-live-resize.ps1 -Scenario NativeSize -FeedbackBreakdown` for a manually
armed short capture with the isolated baseline buffer policy. Add `-IncrementalBuffers`
to exercise the production detached buffer strategy. Prepare the detached Spectrum,
start recording, and drag its outer border during the five-second recording. Close
normally afterwards. Extra events increase overhead and file volume; keep the recorder
limits and reject dropped or truncated captures. Absolute timings from different
instrumentation versions are not directly comparable. Experimental configurations and
results belong in the [issue #56 investigation](live-resize/README.md).
