# Built-in Spectral Line List and Plot Contract

## Packaged document

`config/spectral_lines.public.json` is a complete [Spectral Line List v1](spectral_line_list_v1.md)
using the same production codec as user documents. There is one packaged path,
loaded from the package-root path in Debug or embedded JSON in static release.
The former TSV loader and intrinsic marker group/display-label/source fields
are retired. Base scientific content is program-owned and read-only.

The public list contains physical line and band references only, never subtype
presets, classification combinations, navigation windows or private criteria.
The default grouping is an ordinary v1 grouping view. Unchanged marker, view and
group identities are retained; filename/schema/name changes do not redefine them.

## Physical and presentation contract

The packaged reference coordinates are laboratory/rest, vacuum Angstroms.
Public atomic air values are converted to vacuum as recorded in
`legal/DATA_SOURCES.txt`, consistent with the existing LAMOST/SDSS display axis.
Approximate molecular features remain identified as approximate in marker notes.
A band is a display reference range, never a range-navigation window.

Plot capability flags still control availability and rest-frame warnings. UI and
plot code do not infer or apply radial-velocity, heliocentric or barycentric
corrections. Broader observed-coordinate semantics belong to #105.

Names are plain UTF-8 Unicode: Hα, C₂, Na I D₂, ¹³C¹²C and ¹³CN retain scientific
notation. The scientific text renderer interprets Unicode superscript/subscript
digits as script runs in its dedicated font, with shared measurement/drawing.
There is no general markup or LaTeX interpretation. Wavelength labels are derived
from the numeric coordinates; there is no separately stored display label.

Built-in provenance, licensing and transformation records remain in
`legal/DATA_SOURCES.txt` and About/Data Sources. Combined multiplets retain a
record of constituent transition values and transformation there. Copying content
to a user document does not inject hidden provenance or rights fields.

## UI and overlay contract

The panel consumes one effective SpectralLineList with independently selected
grouping and color dimensions. Base views are read-only by owner, while overlay
views are editable. Visibility is session state shared by every occurrence of a
marker; all markers initially default to visible. Grouping view search and label
visibility do not alter canonical content. The [grouping and overlay contract](spectral_line_grouping_views.md)
defines derived Unassigned, ordering, persistence, and bounded schema 6 conversion.

## Concurrent user-state write contract

See the authoritative [overlay commit contract](spectral_line_grouping_views.md#concurrent-user-state-write-contract).
Only application-managed built-in customization uses that reconciliation; it does
not extend to canonical user-owned file saves.
## Plot Contract

The main plot receives visible markers from the active catalog only. Switching
between grouping-view tabs does not change plot visibility. Plot code does not
read config files, does not know subtype presets, and does not inspect
loader-specific metadata. It only respects the snapshot capability flags and
draws the supplied markers against the current X axis.

Every line and band owns an independent `Auto / ExplicitColor` selection keyed
by stable catalog identity plus marker id. Auto colors use the shared
theme-aware plot-series palette and a stable slot that is unaffected by frame,
grouping view search, grouping, or visibility order. Explicit RGBA is theme-independent.
Line strokes and band outlines use the same resolved color as both labels; a
band fill may derive lower opacity while retaining that resolved RGB.

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
