# Incremental buffer replacement diagnostic

> Historical investigation record. See the [current resize investigation](../README.md) for current decisions and next steps.

Latest evidence: [the first complete paired capture](../evidence/20260911-1023-incremental-ab.md)
shows lower whole-frame tails with the intended replacement count. The user subsequently
confirmed that treatment feels smoother. Ordinary-style replication and production
suitability remain open.

Implemented after the [bounded replacement proposal](../design/buffer-replacement.md).
Default off. Only the isolated executable reads
`SPECFORGE_EXPERIMENT_INCREMENTAL_BUFFERS=1`; the ordinary application leaves main
and detached presentation on their existing policy. This is an explicitly authorized
resize-policy experiment, not an instrumentation-only patch or an accepted fix.

## Implemented behavior

Detached creation still installs three exact-size buffers. In the experiment, a
renderer resize updates the latest desired size and request serial without rebuilding.
BeginFrame retains ordinary statistics draining, queries each available-event with
timeout zero, and selects an available matching-size slot. Otherwise it creates one
replacement for an available slot that is not the last successfully submitted slot.
The latest request supersedes intermediate sizes; no resize-request queue is retained.

Creation is transactional: texture, RTV, presentation buffer and event must all
succeed before the replacement is swapped into the array. The temporary then owns
the old slot and releases it in the original order. Failed partial construction
releases the temporary and leaves installed generations intact before the window
adapter follows its existing DXGI fallback. The selected buffer is queried again
before binding; no available slot returns the existing still-drawing skip outcome.
Source rectangle and render-target size match the latest request before drawing.
Successful Present records the new bound generation.

The ImGui frame ID enforces at most one replacement attempt per viewport/frame.
The direct low-level API's default frame ID zero provides no cross-call frame guard;
the actual detached renderer always supplies a nonzero ImGui frame ID. State is per
swap chain and resets on shutdown/recreation. No new thread or blocking availability
wait is introduced. Driver/API calls and resource release may still block internally.

The limit is three installed slots plus one temporary, with a **256 MiB per-viewport
logical RGBA8 pixel budget** including the old slot until replacement commits. Checked
dimension arithmetic rejects zero, unsupported (>16,384) or over-budget dimensions
before allocation. Initial allocation checks all three slots. Budget failure uses an
explicit HRESULT/reason and the existing DXGI fallback. This is not a cap on driver,
compositor or total process GPU memory, and the DXGI fallback has its existing policy.

The API permits querying/waiting on available events and requires the caller to close
their handles; it does not grant applications control of the event state. The experiment
never resets or signals those events. See [GetAvailableEvent](https://learn.microsoft.com/en-us/windows/win32/api/presentation/nf-presentation-ipresentationbuffer-getavailableevent).

## Telemetry and acceptance

`buffer_replacement_experiment.enabled` reports actual activation at startup and
manual capture start. Manifest `incremental_buffers` records the requested switch.

| Event | Meaning |
| --- | --- |
| `presentation_resize_request` | Child of viewport_resize; latest request serial and target dimensions. |
| `presentation_buffer_selection` | Child of viewport_acquire; action select/replace/skip_unavailable/skip_frame_budget/budget_failure, HRESULT and selected identity. |
| `presentation_buffer_replace` | Child of selection; complete allocation/commit/old-release attempt. Failure means no installed-slot swap occurred. |
| `presentation_buffer_submission` | Child of viewport_present; successful submission identity, not display completion. |

Fields: `resize_request_serial`, `allocation_generation`, `bound_generation`,
`bound_buffer_slot`, `buffer_slot`, `logical_bytes`, `buffer_action`, and existing
new_width/new_height. Generation is unique within a viewport lifetime, not globally.
For selection/replacement, `count` is the three-bit observed availability mask;
`logical_bytes` is the planned peak including the temporary for replacement, or
installed bytes for selection. Failure before a plan is feasible can report zero.
Old-resource release spans carry the retired generation; an uncommitted partial
temporary can have generation zero. Generation assignment occurs after creation.

The analyzer checks typed fields, bounded slots/budget, event parents, one replacement
attempt per viewport/frame, availability mask and bound-slot exclusion, and that a
submitted identity/dimensions/request matches successful acquisition. With
`-RequireIncrementalBuffers`, missing activation/replacement coverage, simultaneous
acquisition-only feedback, or detached DXGI Present invalidates the performance trial.
Existing zero-drop and complete capture requirements remain. Diagnostics of fallback
can still be read without treating them as a valid performance comparison.

Tests cover budget/overflow, exact-size reuse, bound/unavailable exclusion, independent
planning, four creation-stage fault injections with old generations preserved, latest
request winning, one replacement per frame, recreation and real DXGI fallback. The
test-only allocation checkpoint is not exposed through runtime configuration. The
ordinary detached lifecycle and presentation tests remain in the same test suite.

## Interactive comparison when ready

After the positive B observation, the next comparison holds **ordinary window style A**
fixed. The historical B pair used executable SHA256
`F8AC3E627379D5F809BA263A54A09574D26E7E54860C94839049B7153C8EAF44`.
After source changes, build the isolated target with the repository MSVC wrapper
and use the same resulting executable for both arms; the historical hash does not
identify a new build. The runner clears inherited no-redirection and acquisition-only
feedback settings. Execute these separately, observing each for 30–60 seconds before
starting its five-second recording:

```powershell
# Ordinary window style, baseline buffers.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm A -FeedbackBreakdown
# Ordinary window style, incremental buffers.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm A -FeedbackBreakdown -IncrementalBuffers
```

The A gate verifies sampled detached HWNDs do not carry WS_EX_NOREDIRECTIONBITMAP;
the treatment gate additionally requires actual incremental activation/replacement
coverage and rejects detached DXGI fallback. A remains the isolated executable with
ordinary style; this is not an installation or promotion to the standard executable.
Keep the same source, detached panels and display for both runs. After this pair,
evaluate UX and whole-frame tails before deciding on resource-stability work.

The original B reproduction commands follow for reference:

Use the same newly built executable and keep window style B and ordinary feedback
frequency fixed. The first command is control; the second changes only buffer policy:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm B -FeedbackBreakdown
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm B -FeedbackBreakdown -IncrementalBuffers
```

Each launch starts recording off. Prepare Spectrum and the data, then choose
Settings > Diagnostics > Start Recording and drag the detached outer border during
the five-second recording. After recording finishes, close normally. Do not start
a second recording in the same run. Compare stutter, skipped frames and total-frame
tails, not just Resize duration, which is intentionally deferred. No ETL is needed
for the first UX trial. A positive B result still needs an ordinary-window-style trial
and longer bounded resource-stability checks before any production proposal.
