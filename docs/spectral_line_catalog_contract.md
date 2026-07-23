# Spectral Line Catalog Contract

## Purpose

SpecForge keeps public spectral reference markers in a versioned config file:

```text
config/spectral_lines.public.tsv
```

This file is the public default catalog and example catalog. It contains
physical line and band markers only. It must not contain subtype presets,
classification combinations, zoom windows, local research notes, line-strength
expectations, or private criteria.

This contract is intentionally strict for the built-in public catalog. Future
imported catalogs may omit catalog grouping and should then be treated as
ungrouped catalogs by the grouping-view layer.

## Coordinate Contract

- Unit: Angstrom.
- Wavelength medium: vacuum.
- Frame: rest-frame reference wavelengths.
- Atomic visible/NIR values in public references are often listed as air
  wavelengths. The catalog stores the vacuum equivalents because LAMOST/SDSS
  spectrum axes are normally vacuum wavelengths in this project.
- Molecular features and band heads may be approximate markers. They must be
  labeled as approximate markers rather than promoted to exact classification
  truth.

The plot may show the catalog when
`SpectrumCapabilities::can_show_spectral_lines` is true. If
`requires_rest_frame_warning` is true, the UI must warn that the rest-frame
catalog is reference-only for the current axis. Plot and UI code must not infer
or apply radial-velocity correction on their own.

## TSV Schema

The first non-comment row is the header. Lines beginning with `#` are comments.
Columns are tab-delimited. Required columns must be present in the public
catalog header; optional columns may be omitted. Empty trailing optional columns
are valid.

| Column | Required | Meaning |
| --- | --- | --- |
| `id` | yes | Unique stable ASCII id for UI state, tests, and local overlays. |
| `label` | yes | Domain label shown in the UI and plot annotations. |
| `kind` | yes | `line` or `band`. |
| `group` | yes | Public catalog group such as `Balmer`, `Ca II`, `CN`, or `Isotope`. Required for the built-in public catalog. |
| `vacuum_angstrom` | for `line` | Positive finite rest vacuum wavelength for a single line. |
| `start_vacuum_angstrom` | for `band` | Positive finite rest vacuum start for a band marker. |
| `end_vacuum_angstrom` | for `band` | Positive finite rest vacuum end for a band marker. |
| `display_label` | yes | Short plot label. May preserve familiar rounded labels. |
| `source_ref` | yes | Non-empty short public source handle or derivation handle. |
| `notes` | no | Public source/precision note only. No classification criteria. |

The built-in catalog's third-party attribution and transformation record lives
in the repository at `legal/DATA_SOURCES.txt` and is shipped as
`Legal/DATA_SOURCES.txt` under the executable directory. An approximate marker
may use a SpecForge-owned derivation handle, but its note must state that it is
approximate and must not imply laboratory or calibration precision. Removing
an external attribution requires replacing both the value and its provenance;
changing only `source_ref` is not sufficient.

Rows are sorted by marker position after loading. `line` rows draw vertical
reference lines. `band` rows draw shaded ranges. Bands are display markers, not
range-navigation windows.

## UI Contract

The Spectral Lines panel owns runtime catalog inspection state. The first
implementation has one active catalog at a time. The panel should expose:

- active catalog selection, even when only the built-in public catalog exists;
- read-only catalog grouping when the catalog supplies `group`;
- editable user grouping-view tabs that store marker references by catalog
  identity and marker id;
- per-marker visibility shared by every grouping view for the same catalog
  identity;
- text search over marker identity and display fields;
- a plot-label toggle.

There is no separate public `group` selection contract. Catalog groups and user
groups are tree organization surfaces with bulk visibility controls; they do not
create independent plot state. Hiding every marker in the active catalog is
represented by marker visibility state, not by a separate "hide public catalog"
switch.

Subtype-specific combinations are a separate, local/private overlay layer and
are not part of the public default catalog. A future local overlay may select
from public `id` values, but it must not override the public physical
wavelengths in this file.

## Plot Contract

The main plot receives visible markers from the active catalog only. Switching
between grouping-view tabs does not change plot visibility. Plot code does not
read config files, does not know subtype presets, and does not inspect
loader-specific metadata. It only respects the snapshot capability flags and
draws the supplied markers against the current X axis.

Labels should be staggered for nearby markers. Dense catalogs must remain
readable enough for inspection; hiding labels must keep the reference lines and
bands visible.

### Label placement contract

- A marker name is placed near the top of the plot. Its wavelength text is placed
  above the X axis. The two label sets avoid collisions independently.
- Outside an active pan gesture, labels are assigned in appearance-priority order
  to their most comfortable available lane: nearest the top for marker names and
  nearest the bottom for wavelength text. A continuously visible label keeps
  priority over a later or returning label; unseen labels reserve no lane.
- A label participates only while its physical anchor is inside the viewport: a
  line uses its wavelength and a band uses its midpoint. An intersecting band is
  still drawn when its midpoint is outside, but its label occupies no collision
  space. Outside an active pan, leaving ends the label's visibility tenure, so
  re-entry is a new appearance; during a pan it retains its gesture snapshot.
- Comfortable-lane compaction is deferred while a direct pan gesture remains
  active, such as while the mouse button is held or a supported
  touch/direct-manipulation gesture is still active. Each pre-existing label
  snapshots an immutable preferred lane at gesture start; a label first seen
  during the gesture snapshots its first feasible lane. Current geometry may
  require a temporary fallback lane, but that fallback never replaces the
  gesture preference. Necessary collision avoidance is still applied, and
  returning to the gesture-start geometry restores the same feasible layout
  before release, independent of the path taken.
- Pan visibility is transactional. Labels visible both at gesture start and at
  release preserve their committed appearance priority even if they temporarily
  leave and return. Labels absent at release end their tenure; labels first seen
  during the gesture are appended after surviving pre-gesture labels. The final
  visible set is committed atomically before comfortable-lane selection resumes,
  preventing release-time lane swaps. A future touchscreen pan path must drive
  the same transaction at least until the final contact is released.
- Fit view, X-scale changes, catalog changes, font/DPI changes, and material plot
  width changes start a new epoch. The initially visible labels are then ordered
  left to right.
- Spacing and insets are font-relative. Fixed pixels are reserved for hairline
  strokes or platform hit-testing, not text placement.
- Labels are rendered under the plot clip rect and edge-clamped horizontally.
  Text wider than the available plot width occupies that full width for collision
  detection; the original text is submitted unchanged and clipped, not omitted,
  truncated, or abbreviated. Top lanes grow downward, bottom lanes grow upward,
  and labels crossing the plot midpoint are omitted rather than drawn over the
  opposite label region.
- `spectral_line_label_layout` owns placement policy. Rendering supplies stable
  IDs, anchors, text widths, and layout context, then consumes placements. The
  algorithm may be replaced if this contract and its regression tests remain true.
