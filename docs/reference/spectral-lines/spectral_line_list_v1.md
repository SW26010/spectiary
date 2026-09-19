# Spectral Line List v1

The canonical, portable single-file JSON document is decoded by
`ParseSpectralLineListJson`. A successful read returns one complete
`SpectralLineList`; failure returns a diagnostic and no partial model. The
caller adopts only successful results. Atomic writes validate before replacing
the destination. No path or application state defines document identity.

## Normative field table

All records are closed: unknown fields are errors, including optional additions
from future formats. Null is never a spelling of absence. Strings are UTF-8
Unicode, with no markup interpretation. IDs and names must be nonempty. IDs
are opaque, case-sensitive strings, preserved on serialization and rename.

| Record | Required fields | Optional fields (omission) |
| --- | --- | --- |
| Document | `format_kind`: `"spectiary.spectral_line_list"`; `schema_version`: integer `1`; `id`, `name`: strings; `coordinate`: coordinate record; `markers`: marker array | `description`, `creator`, `created_at`, `modified_at`: strings (no value); `grouping_views`: grouping array (empty); `color_schemes`: scheme array (empty) |
| Coordinate | `unit`: `"angstrom"`, `"nm"`, or `"um"`; `medium`: `"air"` or `"vacuum"`; `laboratory_rest`: boolean `true` | none |
| Line marker | `id`, `name`: strings; `kind`: `"line"`; `coordinate`: finite positive number | `note`: string (no note) |
| Band marker | `id`, `name`: strings; `kind`: `"band"`; `start`, `end`: finite positive numbers, `start < end` | `note`: string (no note) |
| Grouping view | `id`, `name`: strings; `groups`: group array | none |
| Group | `id`, `name`: strings; `marker_ids`: string array | none |
| Color scheme | `id`, `name`: strings; `colors`: object mapping marker IDs to `#RRGGBBAA` strings | none |

All collections may be empty. Optional text may be empty and is preserved
distinctly from omission. Timestamps are opaque metadata strings in v1; writers
should use ISO 8601, but the codec does not invent dates or normalize them.
Hexadecimal color digits accept either case and round-trip unchanged. RGBA8
conversion from application floating-point colors clamps each finite component
to [0,1] and rounds `component * 255` to the nearest integer; decoding divides
each unsigned byte by 255. Auto has no persisted color entry.

Marker, grouping-view, and color-scheme IDs are unique within their own kind.
Group IDs are unique across every view in the document. Group references must
resolve within the same document and cannot repeat within one group. A marker
may occur in multiple groups/views or none. Color references must resolve.
Each mapping key occurs once. Array order is authored order; readers and writers
never sort groups or group membership. JSON object key order is not semantic.

`Unassigned` is only a UI projection of markers unreferenced by the current
view. No ID or name is reserved for it in v1. An authored group with this name
has ordinary semantics. Grouping and color schemes are independent dimensions.
No schemes, or no mapping for a marker, follows Auto. Session selection,
visibility, expansion, generated-name localization metadata, search, and
theme-resolved colors are outside this model.

The existing bounded JSON reader rejects duplicate keys, malformed UTF-8,
nonfinite numbers, excessive bytes/depth/nodes, and malformed JSON. Semantic
validation rejects invalid coordinates, enums, identities and references.
Version dispatch belongs exclusively to the codec. New fields require a schema
revision; unknown input is rejected rather than silently lost on save.

## Packaged data and ownership

`config/spectral_lines.public.json` is a complete v1 document. The conversion
retains marker IDs, the existing default view ID and generated base-group IDs.
The legacy group column becomes ordinary groups, initially ordered as in the
existing panel. `source_ref` and `display_label` are not canonical fields.
Project provenance remains in `legal/DATA_SOURCES.txt`.

Issue #114 owns the subsequent runtime cutover and internal overlay: base
grouping views append with overlay views, and an optional complete color-scheme
collection overrides the base. These are application-managed customizations,
not an alternative public format. Issues #65–#67 own open/edit/save UI workflows.
