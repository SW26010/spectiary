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
  change. GUI follow-up is recorded below; no #668 evaluation is claimed.

An optional run against temporarily restored old source was rejected by
automatic approval review because it could overwrite uncommitted changes;
that operation was not performed. The successful checks above use the fixed
source.

## Computer Use GUI acceptance (2026-09-20)

Used the computer-use plugin (`@oai/sky`) to launch the local Debug
`build/ninja-msvc-debug/Spectiary.exe` and operate its actual file dialog,
F11 shortcut and plot mouse input. No automation endpoint supplied UI actions.

Local CSV fixtures in `logs/axis-125-gui/` contain 201 points with
`x = 5000 + i` and `y = scale * (2*sin(i/13) + 0.5*cos(i/3))`,
for integer `i` from 0 through 200. Scales are 0.001 and 1e12; the files have
the header `wavelength,flux` and are loaded through Files > Add files.

Observed results:

- Small-range ordinary mode shows numeric ticks at 0.0005 intervals. F11
  immersive mode shows useful positive/negative ticks at 0.0002 intervals,
  including zero. The old sub-0.1 blank-label failure does not recur.
- In immersive mode, wheel zoom changes the visible range, dragging translates
  the curve and axes, and double-click fit restores the complete X extent.
  The fitted view retains readable small-value ticks at 0.0001 intervals.
- Large-range ordinary and immersive modes use scientific notation, including
  `2.5e+12`, `2e+12`, and negative values. Wheel zoom retains useful labels
  and a scientific-notation coordinate readout.
- Returning with F11 preserves the displayed spectrum and adjusted view;
  changing from small to large source data refits to the new magnitude.
- No crash, lost input, or P0/P1 regression was observed in these interactions.

Acceptance covers the current desktop/font configuration, not a multi-DPI or
font-scaling matrix, and does not validate #122 normalization. The original
active FITS source and ordinary mode were restored. The two test sources remain
available in Files for reproduction; their local CSV files were retained.
