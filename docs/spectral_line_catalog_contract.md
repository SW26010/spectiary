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
Columns are tab-delimited. Required columns must be present in the header;
optional columns may be omitted. Empty trailing optional columns are valid.

| Column | Required | Meaning |
| --- | --- | --- |
| `id` | yes | Unique stable ASCII id for UI state, tests, and local overlays. |
| `label` | yes | Domain label shown in the UI and plot annotations. |
| `kind` | yes | `line` or `band`. |
| `group` | yes | Public filter group such as `Balmer`, `Ca II`, `CN`, or `Isotope`. |
| `vacuum_angstrom` | for `line` | Positive finite rest vacuum wavelength for a single line. |
| `start_vacuum_angstrom` | for `band` | Positive finite rest vacuum start for a band marker. |
| `end_vacuum_angstrom` | for `band` | Positive finite rest vacuum end for a band marker. |
| `display_label` | yes | Short plot label. May preserve familiar rounded labels. |
| `source_ref` | yes | Non-empty short public source handle or derivation handle. |
| `notes` | no | Public source/precision note only. No classification criteria. |

Rows are sorted by marker position after loading. `line` rows draw vertical
reference lines. `band` rows draw shaded ranges. Bands are display markers, not
range-navigation windows.

## UI Contract

The Spectral Lines panel owns runtime selection state:

- show or hide the public catalog;
- filter by public `group`;
- text-filter by `id`, `label`, `group`, or `display_label`;
- show or hide plot labels.

Subtype-specific combinations are a separate, local/private overlay layer and
are not part of the public default catalog. A future local overlay may select
from public `id` values, but it must not override the public physical
wavelengths in this file.

## Plot Contract

The main plot receives already-filtered catalog markers from UI state. Plot code
does not read config files, does not know subtype presets, and does not inspect
loader-specific metadata. It only respects the snapshot capability flags and
draws the supplied markers against the current X axis.

Labels should be staggered for nearby markers. Dense catalogs must remain
readable enough for inspection; hiding labels must keep the reference lines and
bands visible.
