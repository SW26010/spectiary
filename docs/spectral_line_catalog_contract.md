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

`label` is UTF-8 presentation text, not an ASCII transliteration or a markup
language. Scientific notation should use the corresponding Unicode glyphs:
Greek letters without an intervening space for named Balmer lines (`Hα`
through `Hδ`), subscripts for molecular atom counts and transition indices
(`C₂`, `Na I D₂`), and superscript mass numbers for isotopologues (`¹³C¹²C`,
`¹³CN`). The native UI font atlas must therefore merge coverage for Greek and
the Unicode superscript/subscript blocks in addition to its CJK fallback.

Plot rendering interprets only Unicode superscript and subscript digits as
semantic script runs. It draws those runs with scaled ordinary digits from one
dedicated scientific font, while keeping Greek and baseline text in that same
font. Measurement and drawing must share this run model so collision avoidance
uses the rendered width. This is deliberately not a general markup or LaTeX
contract.

The built-in catalog's third-party attribution and transformation record lives
in the repository at `legal/DATA_SOURCES.txt`, is embedded in the executable,
and is available from About as Data Sources. An approximate marker may use a
SpecForge-owned derivation handle, but its note must state that it is approximate
and must not imply laboratory or calibration precision. Removing an external
attribution requires replacing both the value and its provenance; changing only
`source_ref` is not sufficient.

When one catalog marker combines multiple source transitions, such as an
unresolved multiplet represented as a band, `legal/DATA_SOURCES.txt` must list
every constituent transition wavelength and describe how they were combined.
A shared `source_ref` alone is not a sufficient transformation record.

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
- per-marker Auto or explicit RGBA color shared by every grouping view for the
  same catalog identity, with Reset to Auto represented by removing the
  override;
- text search over marker identity and display fields;
- a plot-label toggle.

There is no separate public `group` selection contract. Catalog groups and user
groups are tree organization surfaces with bulk visibility controls; they do not
create independent plot state. Hiding every marker in the active catalog is
represented by marker visibility state, not by a separate "hide public catalog"
switch.

## Concurrent user-state write contract

This is the operational contract for the catalog-only reconciliation exception
recorded in
[ADR 0004: Lightweight Multi-Instance Runs Share User State](adr/0004-lightweight-multi-instance-user-state.md#catalog-user-state-reconciliation-is-a-bounded-exception).
It does not change the policy of unrelated settings or caches.

Catalog user state is a startup snapshot. Ordinary GUI instances do not live-
synchronize their panel state, but a task-level write must reconcile that
snapshot with the durable cache immediately before replacement. Startup
canonicalization is applied before the reconciliation base snapshot is taken;
trimming names, repairing references, and selecting a valid fallback are
normalization, not an explicit task delta. The controller holds the cache's
short-lived commit lease while it reloads the latest document, merges the task,
canonicalizes the result, and uses the existing atomic cache writer. The lease
is named `<cache-path>.commit.lock`; its ownership is the live OS file handle,
not a stale PID or a best-effort marker.

The merge ownership is intentionally narrow:

- catalog additions are keyed by stable opaque view/group IDs. New user views
  and ordinary groups, including copies, use the existing UUID v4 generator.
  Built-in identities remain explicit. Names and order are independent of IDs;
  deletion does not persist an allocator reservation. Concurrent additions keep
  their distinct UUIDs. Defensive remapping still preserves both live additions
  if malformed/imported snapshots genuinely request the same identity. A local
  deletion wins over a concurrent edit to that same entity.
- names, generated-name provenance, unassigned flags, per-marker visibility,
  and per-marker explicit color overrides are field-owned. A field unchanged
  by the stale task is taken from the latest durable state; a field changed by
  the task wins a same-field conflict. Removing a color override is the marker
  color field's Reset-to-Auto tombstone and does not erase a peer's change to a
  different marker.
- marker references are keyed by catalog identity plus marker id. Disjoint
  additions/removals survive, while a removal from a group is a local tombstone
  for that group reference.
- ordering is owned only when the task explicitly issues a reorder. That
  explicit order includes newly created entities, so a new group moved before
  an existing group remains there on the first flush; durable-only additions
  are retained before the first task addition. Without an explicit reorder,
  concurrent additions remain in durable-addition then task-addition order.
  Selection is a scalar task field:
  an explicit task selection wins a conflict, while an unchanged selection is
  refreshed from durable state. A fallback selected automatically after the
  active view is deleted is not an explicit task selection and must not
  override a peer's explicit selection of a surviving view.
- panel expansion keys use the same view/group identity remapping and are
  reconciled independently from the catalog data. Labeling leases and unrelated
  local caches are not part of this contract.

An existing latest document that cannot be parsed, has an unsupported schema,
fails its body-shape checks, or violates semantic identity invariants (such as
empty/duplicate view or group identities, group identity reuse, or invalid
reference identity) is untrusted before any replacement. Maintenance,
destructor, and explicit task writes fail closed with the parser/semantic
diagnostic and leave the durable file untouched. Schema-one/two/three/four
documents are supported migration inputs only when every persisted catalog
entry belongs to the catalog being migrated; a legacy multi-catalog document
without domain definitions for all entries fails closed rather than producing
a partially migrated schema-six file. A single-catalog legacy document is
checked for raw view/group identity uniqueness, current-catalog marker
references, and (for schema three) the unassigned identity/flag pairing before
canonicalization, then canonicalized under the commit lease and validated again
before it is atomically rewritten. Schema-five migration preserves all live
identities and references, validates semantic state, and discards only obsolete
allocator fields. Schema six contains no sequence counters or reservation sets.
Current-schema semantic corruption is never repaired by a write. A missing cache
is still treated as the normal first-write empty state, preserving
single-instance startup behavior.

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

Every line and band owns an independent `Auto / ExplicitColor` selection keyed
by stable catalog identity plus marker id. Auto colors use the shared
theme-aware plot-series palette and a stable slot that is unaffected by frame,
filter, grouping, or visibility order. Explicit RGBA is theme-independent.
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
