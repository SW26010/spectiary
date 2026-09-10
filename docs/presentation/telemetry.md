# Live-resize presentation telemetry (schema 1)

For investigation scope and evidence, see [issue #56](live-resize/README.md).
This instrumentation does not select a resize strategy or diagnose #56. It leaves
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
shutdown for full lifecycle acceptance; UI-started/stopped partial captures
remain useful for inspection but intentionally fail the complete-evidence gate.

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
