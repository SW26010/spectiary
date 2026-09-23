# Spectral Line Grouping Views and Built-in Overlay

The public document contract is [Spectral Line List v1](spectral_line_list_v1.md).
The packaged base is a complete, read-only v1 document. `BuiltInSpectralLineAdapter`
resolves it with local customization into one validated `SpectralLineList`.
The controller and plot consume this effective content; they do not decode or
compose overlay JSON. User-owned canonical documents use the same model, with
open/edit/save workflows owned by issues #65–#67.

## Ownership and interaction

Every base marker and base grouping view is immutable. Ownership is identified
by stable IDs, including when the base has several views. Overlay views append
after base views and are editable. New views/groups and copies receive UUID v4
identities. Group references contain only marker IDs from this list.

Group order drives group presentation. Marker-array order is preserved by the
codec as representation data, without presentation/priority/scientific meaning
in v1. The UI projects markers by wavelength without rewriting stored arrays.
Moving/copying adds a reference; no marker-order operation or ordering merge is
introduced.
The same marker can occur in several groups, with a shared-reference indicator.
Moving/removing one reference does not remove another occurrence. Deleting a
group/view does not delete markers or reset visibility. Group dragging retains
the existing insertion-line interaction and temporary collapse behavior.

Unassigned is computed as all markers minus membership in ordinary groups of
the current view. It is shown after ordinary groups, cannot be renamed, deleted
or reordered, and is never persisted. Authored names/IDs, including `Unassigned`
and the legacy system spelling, remain ordinary canonical identities. The UI
allocates a noncolliding temporary key for its derived area.

Selecting a view changes no marker visibility or color. Visibility defaults to
visible and is shared across occurrences/views. Group bulk visibility is disabled
while grouping view search is active. Search preserves tree structure, matches
marker IDs/names/notes/coordinates, and does not change the stored membership.
Dragging during search edits real membership. Cross-tab dragging is outside scope.

Color schemes are independent from grouping. Their selection is session state.
The current UI edits the selected scheme, creating an ordinary `builtin.user-colors`
scheme named `Custom colors` when no scheme exists. This stable built-in
customization identity also allows two first edits from empty defaults to merge.
It does not encode inheritance. Explicit colors use v1 RGBA8; Auto has no mapping.

## Local persistence

The existing path stays `state/spectral-line-grouping-views.json`, beneath the
resolved Portable or LocalAppData application root. The internal envelope retains
`format_kind: "spectiary.catalog_user_state.cache"` and uses schema version **8**.
It is not an importable public v1 file. All record fields are closed and null is
invalid. One `catalogs` mapping key scopes the record to the packaged list ID:

```json
{
  "format_kind": "spectiary.catalog_user_state.cache",
  "schema_version": 8,
  "catalogs": {
    "public-spectral-lines.v1": {
      "overlay": {"grouping_views": []},
      "session": {
        "active_view_id": "__catalog_grouping_view__",
        "active_color_scheme_id": "",
        "marker_visibility": {},
        "expanded_groups": [],
        "view_names": {},
        "group_names": {}
      }
    }
  }
}
```

Overlay requires `grouping_views` (possibly empty), decoded by the canonical
subrecord codec. Optional `color_schemes` uses the same canonical scheme codec:

| Overlay value | Effective schemes |
| --- | --- |
| omitted | Complete current base collection |
| `[]` | Empty collection, all Auto |
| nonempty array | Complete replacement collection |

First actual color mutation copies the effective collection before editing the
selected mapping. Merely viewing/selecting, canceling, or an unchanged color
creates no override. Reset to Auto erases one mapping, never resumes inheritance.
Restore defaults removes the entire collection override; explicit empty is a
different operation. Explicit ownership is retained even if values match base.

Session owns active view/scheme, visibility, expansion, and generated-name
localization metadata. Expansion uses structured `(view_id, group_id)` pairs,
including derived Unassigned areas. `expanded_groups` is an array of objects
with exactly two nonempty string fields, `view_id` and `group_id`; duplicate
pairs are invalid. IDs remain opaque and may contain `/`. Pairs are written in
lexicographic order by view ID, then group ID.

`view_names` and `group_names` map stable IDs to records
with optional `name_source`, `name_ordinal`, `generated_copy_count`, and
`generated_copy_base_name`. Existing name-source enum spellings and copy-count
limit (1024) are retained. No ownership is inferred from display text. Search
and label visibility remain in-memory UI state. Session restoration does not
modify canonical definitions.

Startup accepts only a wholly valid overlay/effective model. Wrong base IDs,
colliding group/view IDs, dangling references, unsupported versions and malformed
input produce a nonblocking diagnostic and leave the file untouched. A valid base
can still be displayed. Fallback does not authorize overwriting rejected state.
No guessed reference repair, placeholder content, or automatic reset is performed.

## Bounded schema 6/7 conversion

Schemas 6 and 7 are supported as migration inputs. Earlier pre-release schemas
are rejected unchanged. Validate old shapes, names, identities, references,
Unassigned identity/flag pairing, and explicit colors before producing a complete
valid effective model. Reject unrelated list identities, duplicate/dangling
references and composed collisions without publishing a partial conversion.

Both legacy schemas convert `expanded_group_ids` strings only at the migration
boundary: exactly one `/` separates two nonempty IDs. Malformed or ambiguous
keys and repeated pairs are rejected. Schema 7 otherwise preserves overlay and
session fields. A successful legacy load requests a save as schema 8.

For schema 6, preserve ordinary group membership, stored array order and stable identities.
Migration does not normalize marker arrays to UI wavelength order. Omit only the validated legacy system Unassigned
group; an ordinary group named Unassigned survives. Move localization provenance
to session maps. Quantize finite normalized old color channels to RGBA8 using the
v1 conversion, putting explicit colors in one ordinary scheme. No old explicit
colors means no override. Preserve compatible selections/visibility/expansion.
Conversion is reloaded and validated under the existing commit lease before
atomic publication. Schema 8 becomes the sole writable owner; no old writer or
legacy runtime domain model remains.

## Concurrent user-state write contract

This is the bounded exception in [ADR 0004](../../adr/0004-lightweight-multi-instance-user-state.md#catalog-user-state-reconciliation-is-a-bounded-exception).
It provides commit-time reconciliation, with no live synchronization, IPC owner,
WAL, persistent tombstones, historical generations, or generic merge framework.

Edits debounce for 500 ms; failed saves retry after 10 seconds and flush on normal
shutdown. Acquire `<state-path>.commit.lock` through the existing exclusive OS
file handle, with bounded retry. Hold it across reload, trust checks, task merge,
complete-model validation and atomic replacement. A failed save retains pending
in-memory work. An untrusted latest file is never repaired by a stale snapshot.

Grouping reconciliation retains field/entity membership, names, addition/deletion
and explicit group-order rules. Disjoint changes survive. Concurrent additions
with an ID collision are deterministically assigned fresh IDs, including their
session expansion/name keys. An edit to an entity deleted in latest is a conflict;
it must not recreate the entity. An explicit local deletion owns that deletion.
Task group ordering includes newly added groups; concurrent-only additions remain.
Marker membership reconciliation retains the latest surviving array order and
appends task additions, without inferring any marker-order intent.

Ordinary color ownership is `(scheme_id, marker_id)`, including concurrent first
overrides. Copying inherited collections is initialization, not ownership of all
copied fields. Same-field task edits win; Auto reset is a field deletion for this
commit, with no persisted tombstone. Whole clear/replace/restore operations own
the whole collection. A later ordinary edit uses the latest effective collection
and cannot resurrect untouched values from an older override. A deleted scheme
causes a controlled conflict rather than implicit resurrection.

Session restoration is reconciled separately. Explicit selections, including a
round-trip to the base value, own the selection; automatic fallback does not.
Visibility, expansion and localization metadata retain field-level updates.
Session normalization never repairs or rewrites durable canonical content.
