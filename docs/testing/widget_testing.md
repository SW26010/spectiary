# ImGui widget regression tests

The widget harness drives real rendered Dear ImGui controls with mouse, key,
and UTF-8 events through `ImGuiIO`, using a headless context and fixed 1/60-second
frames. Assertions read production settings, session intents, workflow state,
and chooser callbacks after consuming ordinary panel APIs. No renderer, desktop input, or screenshot oracle is
required.

## Run

The `ninja-msvc-debug` preset enables `SPECTIARY_BUILD_WIDGET_TESTS`. From the
repository checkout, run configure and build as separate commands (in Codex,
use sandbox escalation for both wrapper invocations):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure -TimeoutSec 240
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -TimeoutSec 240
ctest --test-dir build/ninja-msvc-debug -L widget --output-on-failure --no-tests=error
```

The `widget` label covers Settings, SourceCollection annotations, Files and
numeric navigation, annotation-to-labeling confirmation, Spectral Lines,
SampleWorkflow, and the Shell file-menu interaction. Settings and panel cases
belong to `fast`; the existing Shell suite retains its `extended` tier. Build all
targets before running the label. The standalone widget executables have
30-second timeouts; existing mixed suites retain their CTest timeouts.
For repeatability, add `--repeat until-fail:20` to a focused widget selection.
The Settings executable also accepts `failures`, `input`, `theme`, `scale`, or
`recording`; `input` includes General, Language and legal-document controls,
and `recording` also checks repair of warned fallback settings.

Run `fast` for each migration commit, and `extended` at major panel and final
checkpoints. Do not build concurrently with `extended`: its metadata regression
case invokes the MSVC wrapper itself and requires exclusive build access.

## Dependency and production boundary

The CMake option defaults to OFF, including normal Release/Portable presets.
Only the Debug developer preset enables it. No production target depends on the
harness or its instrumented ImGui library; neither is installed or packaged.

`tests/widget/CMakeLists.txt` rebuilds the **existing MIT Dear ImGui version**
with `IMGUI_ENABLE_TEST_ENGINE` to expose its native item hooks. This macro's
name does not imply adoption of Dear ImGui Test Engine: the hook callbacks are
small Spectiary implementations, and no Test Engine source or dependency is
used. Do not enable vcpkg's `test-engine` feature.

The upstream docking archive has the same version and SHA-512 as the vcpkg port.
The initial configure needs network access to that archive; subsequent builds
reuse CMake's `_deps` cache. An installed version mismatch fails configuration
so dependency upgrades require reviewing the source pin and internal hook API
together. The instrumented static library precedes the ordinary UI dependency
in each opted-in test executable's link order. `spectiary_widget_harness` owns
the implementation once; `spectiary_add_widget_suite` creates additional panel
suites with the same link ordering. Existing mixed suites prepend the harness
only when the option is enabled. Production executables continue using
the vcpkg library.

## Locator and input limits

The harness records only current-frame item IDs, labels, clipped navigation
bounds, owning windows, ID stacks, and disabled state from native ImGui hooks. Tests do not
store a second rectangle map. Selectors match exact labels or stable `###`
identity suffixes. An exact owning-window name can disambiguate `Find`.

`Observe` inspects only the current frame and returns no value for a missing or
clipped item. Its optional semantic `PushID` scope disambiguates repeated recovery
rows by source/task identity, including duplicate task IDs with different row
tokens. `Find` adds bounded frame settlement and optional window disambiguation.
For unlabeled `BeginCombo` items, the locator hashes the requested exact ID or
`###` suffix against the observed native ID seed. It does not invent geometry.

`Click` supports left and right mouse buttons. `FrameMode::ExistingContext`
attaches hooks to a fixture-owned context and uses that fixture's complete frame
driver, preserving tests that control hidden, collapsed, or docked presentation.
The temporary-driver click overload restores its prior driver even on failure.
The harness does not discover offscreen controls, automatically scroll, or
implement general path traversal. Navigate through real controls first.

Missing selectors fail after eight frames. Ambiguous and disabled click targets
fail explicitly. `Until` defaults to twelve frames and has a maximum budget of
120; ordinary frame advancement has the same per-call cap. The negative tests
assert exact diagnostics and frame counts. A successful input injection alone
is never a business assertion: always check the production owner's result.

## Migration boundary and intentional exceptions

SourceCollection and SampleWorkflow retain no test-only widget rectangle members
or maps. Their tests locate ordinary controls through native item observations.
Settings ordinary checkbox, language, scale and reset scans are consolidated
into owner-asserting widget tests. Shell File > Open File uses stable menu IDs.
The localized sorting-source popup uses rendered clicks and checks the exact
submitted source. Numeric navigation widget cases cover source-row and sequence
position commits, blur, cancellation and live prefixes; the component suite
retains its precise topology and presentation transitions. Annotation activation
drags the real annotation row onto the labeling selector before confirming or
cancelling. Spectral-line grouping creation, rename, duplicate and deletion
check the controller result by stable identity.

Settings warning-repair tests load malformed files, reselect fallback controls,
and verify the repaired owner reloads cleanly. Legal-document widget tests embed
the production resources and cover opening, switching and full-content copying
with a captured clipboard. They scroll the actual content pane with a bounded
wheel loop; the harness itself does not implement offscreen discovery.

The mixed SampleWorkflow and Shell files keep component cases when widget tests
are OFF. Only rendered controls requiring instrumentation are compiled under
`IMGUI_ENABLE_TEST_ENGINE`; production targets never receive that definition.

Remaining seams have explicit responsibilities:

- SourceCollection numeric input tests use normal Ctrl+A, text and key events.
  Programmatic focus/deactivation and read-only internal buffer observations
  remain for component input-routing, topology, hidden and docking transitions.
  No test writes `InputTextState` or its edited flag.
- SampleWorkflow keeps state inspection and fixture setup for shortcut capture,
  edit lifecycle, validation, result notices and recovery identity tokens.
  Single-frame mouse press/release sequences retain callback and one-shot
  assertions using current harness bounds; they do not read panel geometry.
- Header layout and disabled/occluded hit-tests intentionally use pointer
  coordinates. Recovery identity text has no widget ID: its bounded hover scan
  checks the full tooltip and remains a geometry test.
- Settings retains focused layout, artifact identity, legal disclosure geometry, text
  hit-testing, viewport constraints and platform-focus seams. The General
  action-driving private render accessor was removed.
- Shell retains component session/controller injection and a narrow menu-render
  fixture seam; menu selection itself is rendered widget input.

Unit/component tests remain responsible for persistence, sessions, controllers,
and domain edge cases. Real-process automation remains responsible for business
workflows, capture, cleanup and release smoke testing. Win32/DXGI/compositor and
plot tests retain their geometry and presentation input APIs. This harness is
not a business-command protocol and does not establish rendering or timing
correctness.
