# Immersive Y-axis immediate rollback

Scope: the immediate rollback requested in
[issue #125](https://github.com/SW26010/spectiary/issues/125).

The immersive native-axis path no longer installs
`FormatNativeCompactYTick`. ImPlot owns tick generation and formatting again.
The removed callback blanked values that were not multiples of 0.1 and forced
the remaining values to one decimal place. Transparent styling, axis interaction
flags, source coordinates, curve data and viewport ownership are unchanged.

Label width may still change the external Y-axis footprint and move the plot
horizontally. This is the accepted immediate rollback limitation. Evaluation
and adoption of upstream ImPlot #668, including its ADR decision, remain future
work under #125. Source/normalized presentation remains separate work under
#122; this change does not implement normalization or complete all of #125.

## Validation (Windows / MSVC Debug, 2026-09-20)

- Full Debug application and test build passed using
  `scripts/build-ninja-msvc-debug.ps1 -TimeoutSec 600` with escalation.
- The spectrum-view render regression uses the production render path and
  checks actual ImPlot Y tick labels for scales 0.001, 1 and 1e12, including
  negative and positive coordinates. Ordinary/immersive transitions retain
  finite numeric labels and unchanged X/Y limits. Test-only ImPlot internals
  inspect the generated labels; production adds no internal dependency.
- `ctest --preset fast --parallel 4`: 80/91 passed in the restricted shell.
  Eleven storage/process/window tests failed or timed out in that environment.
- `ctest --preset fast --rerun-failed --parallel 1` with escalation: all 11/11
  passed. Combined coverage is 91/91, including existing pan, touchpad,
  fit/stored-limit, smoothing, spectral-line and widget tests.
- Final diff review and `git diff --check` passed. No P0/P1 findings in this
  change. No manual GUI/DPI acceptance or #668 evaluation is claimed.

An optional run against temporarily restored old source was rejected by
automatic approval review because it could overwrite uncommitted changes;
that operation was not performed. The successful checks above use the fixed
source.
