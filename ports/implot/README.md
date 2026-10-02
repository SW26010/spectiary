# ImPlot axis range input patch (#140)

This overlay copies the implot 1.0 port from the manifest's vcpkg baseline
`eb35a05cc21c69ec1399f7532b7dd61d271ac0d1`. The source hash, build recipe and
linkage remain unchanged; port-version 1 applies one menu-only patch.

ImPlot's numeric menu forwards NaN to `SetMin`/`SetMax`, which normalize it to
zero. Reject non-finite candidate values before calling those setters, retain
the viewport, and show a diagnostic until a valid edit. Keep the built-in drag,
lock, fit and menu behavior. This does not alter programmatic axis setters.
Valid text edits equal to the current value also clear the diagnostic. Since
ImGui's scalar return value reports only numeric changes, the patch checks its
input-text edit state and requires a complete finite parse for this recovery.

The overlay is selected through `vcpkg-configuration.json` for all manifest
builds, including release builds. No generated install-tree edits are needed.
`spectiary_plot_axis_range_widget_tests` drives the actual menu widgets for
both bounds of both axes and checks editing, submission and valid recovery.

On an ImPlot upgrade, re-evaluate this patch against upstream and remove the
overlay when upstream rejects non-finite menu input. Keep the regression test.
