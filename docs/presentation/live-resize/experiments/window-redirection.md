# Redirection surface experiment feasibility — 2026-09-11

> Historical investigation record. See the [current resize investigation](../README.md) for current decisions and next steps.

This follows the [07:30 ETW evidence](../evidence/20260911-073022-etw-waits.md).
The scope of this check is whether an isolated no-redirection HWND can support
the existing presentation paths. No production window style or resize policy
change remains in the application.

## Runtime attempt rejected

An attempted `SetWindowLongPtrW(GWL_EXSTYLE)` addition of
`WS_EX_NOREDIRECTIONBITMAP` after Composition initialization failed locally:
HRESULT `0x80070057` (`E_INVALIDARG`), with the read-back extended style remaining
256 (`WS_EX_WINDOWEDGE`). The intended bit was not applied. The attempted
runtime toggle, fallback restoration and runner switch were removed; there is
no `-NoRedirectionBitmap` capture option in the current runner.

This is an observed result for this call sequence and machine, not proof that
every possible pre-initialization style transition behaves identically. It rules
out using that post-initialization toggle as the proposed A/B implementation.

## Creation-time compatibility probe

`TestCreationTimeNoRedirectionCompatibility` in
`tests/d3d11_sdr_swap_chain_tests.cpp` creates a separate hidden HWND with
`WS_EX_NOREDIRECTIONBITMAP` supplied directly to `CreateWindowExW`.
It runs the existing presentation adapter twice, in separate HWND lifetimes:

| Requested policy | Backend selected locally | Result |
| --- | --- | --- |
| Prefer Composition | Composition | Initialization, three native resizes, buffer resizes, acquires and Presents pass |
| Composition disabled | DXGI | The same operations pass |

The test reads back the style after each native resize/Present and verifies
that the creation-time bit remains set. It changes only test HWND construction;
ordinary tests continue to default to their existing extended style.

Build wrapper log:
`logs/build/spectiary-ninja-msvc-debug-build-20260911-075003.out.log`.
The six focused CTest targets pass. `Testing/Temporary/LastTest.log` records
`backend=composition policy=prefer` and `backend=dxgi policy=disabled` for the
local probe. Prefer mode may select DXGI on machines without Composition;
that test must not be misreported as Composition coverage there.

These hidden-window API checks do not verify visible content, decorations,
occlusion, minimizing/restoring, docking, DPI/monitor transitions, performance,
or an actual mid-session Composition failure. A successful forced-DXGI run is
not a complete test of a runtime backend transition.

## Constraint on a real A/B build

The detached HWND is created by the compiled ImGui Win32 platform backend
before `Renderer_CreateWindow` initializes the presentation adapter. A valid
creation-time experiment therefore needs to participate in that platform
creation path. Its `ImGui_ImplWin32_GetWin32StyleFromViewportFlags` result must
also retain the experimental bit on later platform style updates; applying
a flag only after renderer initialization is insufficient.

## Available isolated A/B executable

`spectiary_redirection_experiment` is now an explicitly built, non-default target
under the existing widget-enabled developer configuration. It produces
`build/ninja-msvc-debug/redirection-experiment/Spectiary.exe`, with its own PDB,
metadata, runtime DLLs and public catalog. The normal executable stays separate.

The target compiles an experiment-local copy of the pinned MIT ImGui 1.92.8
Win32 backend already fetched for widget tests. CMake checks the version and
patch anchor; the sole behavioral patch adds WS_EX_NOREDIRECTIONBITMAP to the
detached style helper when the experiment environment variable is exactly `1`.
The same helper supplies both creation and later style updates. The original
backend source and installed dependency are not edited. Both A and B use this
same executable and backend, including the same environment lookup; only B
requests the flag. The application's main HWND creation and all buffer resize,
Present, wait and pacing policies are unchanged.

In B, the creation-time bit remains on that HWND if presentation falls back to
DXGI. This is an explicit experimental HWND contract, supported only by the
initial API compatibility probe above; it is not a production fallback fix.
Window styles must not be dynamically cleared on fallback based on the rejected
post-initialization approach.

Build with the repository wrapper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 -Configure -TimeoutSec 180
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 -Target spectiary_redirection_experiment spectiary_redirection_backend_tests -TimeoutSec 180
```

Run A, close normally and finish saving, then run B without rebuilding or changing
the source, display setup or interaction:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm A -SystemTrace
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm B -SystemTrace
```

Each run requests UAC only for WPR start/stop. Detach Spectrum, idle three seconds,
resize the outer border for 10–15 seconds, idle three seconds, redock and close.
Record stutter and any blank/transparent content, stale drawing, border or docking
issues separately for A and B. Preserve both directories. These are the first
same-build A/B runs; older baseline logs are context, not a substitute for A.

The runner sets the experiment explicitly (including clearing inherited values
for A/normal runs), records arm and executable SHA256 in
`redirection-experiment.json`, and selects the isolated executable for A/B.
JSONL `runtime_config.redirection_build` must be `creation_time_ab`.
`platform_window_size` begin/end record the actual numeric `hwnd_ex_style`;
the analyzer rejects missing samples, wrong executable identity or a style
inconsistent with the requested arm. Other presentation events use zero for
unsampled styles, except lifecycle begin. Style fields are not inferred from
the requested environment variable.

The real Win32 backend tests exercise A and B in separate processes, checking
main HWND exclusion, detached creation, TOOLWINDOW style updates without losing
the experiment bit, native client size and destruction. These and the six
existing focused targets pass locally. Schema fixtures reject incorrect arm or
build identity. Visible behavior and reduction in fence waits remain pending
actual A/B capture; the experiment is not yet a validated fix.
