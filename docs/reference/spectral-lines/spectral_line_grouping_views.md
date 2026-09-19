# Spectral Line Grouping Views

## Purpose

Spectral line grouping views let users organize catalog markers for inspection
without changing the spectral-line catalog itself. The public catalog remains
the source of marker definitions, wavelengths, and any catalog-provided grouping.

This document describes user-owned grouping state and its local persistence. The
cache format is structured and versioned, but it is not a public import/export
format in the first implementation.

## Scope

The first implementation has one active spectral-line catalog at a time. The
Spectral Lines panel may switch the active catalog, but the plot receives visible
markers from that active catalog only. Simultaneous overlays from multiple
catalogs are out of scope.

## Concepts

The glossary source for these terms is [CONTEXT.md](../../../CONTEXT.md).

- `Spectral-line catalog`: marker definitions and optional catalog grouping.
- `Catalog identity`: the key that owns user state for one catalog.
- `Catalog grouping view`: read-only grouping derived from catalog grouping.
- `User grouping view`: editable user-owned marker organization.
- `Catalog user state`: saved user grouping views and marker visibility for one
  catalog identity.

## Catalog Identity

Grouping state is scoped to a catalog identity, not to the currently selected
tab or to a file path alone.

- The built-in catalog uses a fixed identity, such as `public-spectral-lines.v1`.
- Imported catalogs should prefer a declared stable catalog id when available.
- Imported catalogs without a declared id should receive a generated local
  identity. The original path and a content fingerprint may be recorded as
  matching hints, but the generated identity remains the user-state key.

This avoids applying grouping state from one catalog to an unrelated catalog that
happens to share a path.

## Grouping Views

Catalog and user grouping views share the same tree shape:

```text
grouping view
  group
    marker reference
```

A catalog grouping view exists only when the catalog supplies grouping. Its
marker membership is read-only. Users may still change marker visibility from
that view because visibility belongs to catalog user state, not to the grouping
membership.

The built-in public catalog must provide grouping. Imported catalogs may omit
grouping. If a catalog has no grouping and no user grouping views, the panel
should show an empty state that guides the user to create a user grouping view
with the `+` action.

Creating a user grouping view puts all current catalog marker references in the
non-removable, non-renamable `Unassigned` user group. The action does not reset
marker visibility. Duplicating an existing user grouping view is a separate tab
context-menu action.

## Ordering

Ordinary user groups can be reordered inside a user grouping view by dragging a
group and dropping it on an insertion line between groups. While a group is
being dragged, groups are temporarily displayed collapsed. The visible drop
indicator is always an insertion line; each insertion point has one continuous
hit area spanning the line itself, the upper half of the group below it, and the
lower half of the group above it.
After drop or cancel, the previous expanded state is restored. `Unassigned` is
fixed after ordinary groups and cannot be reordered. Marker references within
every group are displayed by catalog marker position, normally wavelength
position. Moving or copying a marker reference changes group membership only; it
does not store a per-group marker order.

## Marker References

User grouping views store marker references, not copied marker definitions. A
marker reference is identified by catalog identity and marker id.

If a referenced marker id is missing from the current catalog contents, keep the
reference as an unresolved marker reference. Show it as a disabled placeholder,
do not draw it on the plot, and do not guess a replacement by label or
wavelength. Optional last-known display fields may be stored only to make the
placeholder readable.

Markers present in the catalog but not organized into an ordinary user group in
the active user grouping view belong to `Unassigned`.

## Shared References

A marker reference should usually appear in one group within a user grouping
view, but the user may intentionally place it in multiple groups.

- A normal drag moves a marker reference between groups.
- An explicit copy action creates a shared marker reference.
- Shared references should show a status icon. The hover text should explain
  that the same marker also appears in other groups.

Marker visibility is shared across all occurrences of a marker in all grouping
views for the same catalog identity.

## Visibility

The plot only cares whether each marker is visible. Selecting a grouping view or
switching tabs does not change plot visibility.

When a catalog identity has no saved marker visibility, all current markers in
that catalog default to visible. After user changes exist, visibility is read
from catalog user state.

Each group has a bulk visibility control:

- all resolved markers visible;
- all resolved markers hidden;
- mixed visibility;
- non-actionable search state while search is active.

The bulk control changes marker visibility for resolved marker references. It is
not separate group visibility state.

## Search And Dragging

Search narrows the active grouping view tree while preserving group structure.
Groups with matching marker references may be expanded; groups without matching
children should not be expanded merely because search is active.

Dragging a marker while search is active moves the real marker reference in the
active editable user grouping view. Search is not a temporary result set.

The first implementation does not support cross-tab dragging.

## Local Persistence

Catalog user state should be saved automatically to local user storage, scoped by
catalog identity. Panel state such as expanded tree groups may be saved in the
same internal cache, but it remains separate from catalog user state because it
is UI layout state, not marker organization. On Windows, the default location
should be under the user's local application data directory, for example:

```text
%LOCALAPPDATA%\Spectiary\state\spectral-line-grouping-views.json
```

The cache should be written after edits with a short debounce and flushed on
normal shutdown. Save and load failures should be non-blocking warnings in the
Spectral Lines panel. Failed saves must use a retry backoff instead of writing
again every frame. A corrupt or unsupported cache must not prevent the catalog
from loading.

This local cache remains a startup snapshot and is not live-synchronized across
ordinary GUI instances. Its task-level commit behavior follows the
[concurrent user-state write contract](#concurrent-user-state-write-contract)
and the bounded exception in
[ADR 0004](../../adr/0004-lightweight-multi-instance-user-state.md#catalog-user-state-reconciliation-is-a-bounded-exception);
the JSON format itself does not provide generic merge semantics.

The cache should be normalized and versioned:

```json
{
  "format_kind": "spectiary.catalog_user_state.cache",
  "schema_version": 6,
  "catalogs": {
    "public-spectral-lines.v1": {
      "active_view_id": "view-1",
      "marker_visibility": {
        "h_alpha": true
      },
      "marker_colors": {
        "h_alpha": {
          "mode": "explicit-color",
          "red": "0.95",
          "green": "0.42",
          "blue": "0.35",
          "alpha": "1"
        }
      },
      "grouping_views": [
        {
          "id": "view-1",
          "name": "Grouping 1",
          "name_source": "default_grouping_view",
          "name_ordinal": 1,
          "groups": [{
            "id": "__unassigned__",
            "name": "Unassigned",
            "is_unassigned": true,
            "marker_references": []
          }]
        }
      ]
    }
  },
  "catalog_panel_state": {
    "public-spectral-lines.v1": {
      "expanded_group_ids": [
        "view-1/group-1"
      ]
    }
  }
}
```

Schema 3 introduced explicit, writer-owned generated-name provenance. Schema 4
added the required monotonic allocator high-water marks and durable identity
reservation sets. Schema 5 added `marker_colors`, which stores only per-marker
`explicit-color` RGBA overrides keyed by stable marker id. An absent marker
entry is canonical Auto state; a theme-resolved Auto color is never persisted.
Schema 6 removes the sequence counters and reservation sets. New user views
and ordinary groups use UUID v4 identities; existing live IDs such as `view-1`
remain valid. Copies receive new identities, while built-in IDs remain unchanged.
Names and display order do not expose or depend on the UUIDs.
Generated names participate in UI localization only when a supported schema stores that
provenance explicitly.

Schema 1 and 2 do not contain immutable name provenance. Every editable grouping
view and group name loaded from those schemas remains user-owned and is displayed
verbatim in every language, including names shaped like `Grouping 1`, `Group 1`,
or `Catalog grouping view copy`. Migration must not infer ownership from editable
text, ids, or array order.

Schemas 1 through 4 are supported migration inputs. The cache body is validated
before migration is scheduled. A legacy cache with
only the catalog currently being migrated must first pass raw view/group
identity checks, current-catalog marker-reference checks, and schema-three
unassigned identity/flag checks; only then is it canonicalized, validated, and
rewritten once using the current schema, preserving those names without adding
generated-name provenance. Empty, duplicate, cross-catalog, or mismatched
legacy identities are therefore preserved as failure evidence rather than
repaired. A legacy cache containing unrelated
catalog or panel state entries is not safely migratable without their domain
definitions and is therefore rejected without a partial schema-six rewrite.
Schema 5 is also a migration input: its live identities, references, names,
colors, and panel state are preserved after semantic validation; obsolete
allocator history is discarded. A current-schema cache missing `marker_colors`,
with duplicate live identities, or with a malformed marker color mode/RGBA
payload is invalid and is never rewritten or merged. An
invalid body is reported and is never rewritten merely by opening and closing
the application.

The first implementation should treat this as an internal writer-owned cache,
not as a public exchange format. Its reader exists to load Spectiary's own
versioned cache plus supported legacy cache versions. Future import/export can
reuse the same core model, but should use a mature JSON library with stricter
validation, conflict handling, and explicit user confirmation.

## Concurrent user-state write contract

This is the operational contract for the catalog-only reconciliation exception
recorded in
[ADR 0004: Lightweight Multi-Instance Runs Share User State](../../adr/0004-lightweight-multi-instance-user-state.md#catalog-user-state-reconciliation-is-a-bounded-exception).
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
