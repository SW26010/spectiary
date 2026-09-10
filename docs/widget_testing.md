# ImGui widget regression tests

The widget harness drives real rendered Dear ImGui controls with mouse, key,
and UTF-8 events through `ImGuiIO`, using a headless context and fixed 1/60-second
frames. Assertions read production `ApplicationSettings` after consuming the
panel's ordinary intent API. No renderer, desktop input, or screenshot oracle is
required.

## Run

The `ninja-msvc-debug` preset enables `SPECFORGE_BUILD_WIDGET_TESTS`. From the
repository checkout, run configure and build as separate commands (in Codex,
use sandbox escalation for both wrapper invocations):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure -TimeoutSec 240
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Target specforge_widget_tests -TimeoutSec 120
ctest --test-dir build/ninja-msvc-debug -L widget --output-on-failure --no-tests=error
```

The five CTest cases belong to the `fast` tier and the `widget` label. For a repeatability
check, add `--repeat until-fail:20`. Each process has a 30-second CTest timeout.
The executable can run all cases without arguments, or one of `failures`,
`input`, `theme`, `scale`, and `recording`.

## Dependency and production boundary

The CMake option defaults to OFF, including normal Release/Portable presets.
Only the Debug developer preset enables it. No production target depends on the
harness or its instrumented ImGui library; neither is installed or packaged.

`tests/widget/CMakeLists.txt` rebuilds the **existing MIT Dear ImGui version**
with `IMGUI_ENABLE_TEST_ENGINE` to expose its native item hooks. This macro's
name does not imply adoption of Dear ImGui Test Engine: the hook callbacks are
small SpecForge implementations, and no Test Engine source or dependency is
used. Do not enable vcpkg's `test-engine` feature.

The upstream docking archive has the same version and SHA-512 as the vcpkg port.
The initial configure needs network access to that archive; subsequent builds
reuse CMake's `_deps` cache. An installed version mismatch fails configuration
so dependency upgrades require reviewing the source pin and internal hook API
together. The instrumented static library precedes the ordinary UI dependency
in this test executable's link order, supplying the ImGui implementation for
the unmodified production Settings code. Production executables continue using
the vcpkg library.

## Locator and input limits

The harness records only current-frame item IDs, labels, clipped navigation
bounds, owning windows, and disabled state from native ImGui hooks. Tests do not
store a second rectangle map. Selectors match exact labels or stable `###`
identity suffixes. An exact owning-window name can disambiguate `Find`.

In this ImGui version `BeginCombo` reports bounds but no item label. For a
root-scope stable identity, the locator can derive the ID from the actual
owning-window ID and `###` suffix. This fallback does not resolve arbitrary
`PushID` stacks; labeled items in those stacks still work through `ItemInfo`.
The harness does not discover offscreen controls, automatically scroll, or
implement general path traversal. Navigate through real controls first.

Missing selectors fail after eight frames. Ambiguous and disabled click targets
fail explicitly. `Until` defaults to twelve frames and has a maximum budget of
120; ordinary frame advancement has the same per-call cap. The negative tests
assert exact diagnostics and frame counts. A successful input injection alone
is never a business assertion: always check the production owner's result.

## Initial coverage and responsibilities

- Input section navigation and a live numeric navigation checkbox round trip.
- Appearance navigation, theme popup selection, and stable theme state.
- Ctrl-click scale editing, UTF-8 numeric entry, deferred Enter commit, and Reset.
- Directory reset blocked during recording and restored after recording stops.

The theme, scale, and directory reset tests replace three direct private-handler
tests and remove their `SettingsPanelUiTestAccess` methods. Other private seams
remain for existing focused layout/lifecycle tests; this is an incremental
migration.

Unit/component tests remain responsible for settings persistence, sessions,
controllers, and domain edge cases. The existing real-process automation control
plane remains responsible for supported business workflows, capture, cleanup,
and release smoke testing. Win32/DXGI/compositor diagnostics remain responsible
for presentation cadence and platform behavior. This harness introduces no
business-command protocol and cannot establish rendering or timing correctness.
