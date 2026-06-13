# SpecForge

SpecForge is an interactive spectral inspection tool. Its language separates
physical reference data from user-owned display organization.

## Language

**Spectral-line catalog**:
A set of physical spectral reference markers with stable marker identifiers,
optional grouping supplied by the catalog, and declared wavelength semantics.
_Avoid_: User line groups, private presets

**Public spectral-line catalog**:
The built-in spectral-line catalog shipped with SpecForge.
_Avoid_: User line groups, private presets

**Catalog identity**:
The identity used to decide which user grouping views belong to which
spectral-line catalog.
_Avoid_: File path alone, active tab

**Catalog user state**:
The user's saved organization and marker visibility for one catalog identity. It
does not include the catalog's marker definitions.
_Avoid_: Catalog content, app layout

**Spectral-line marker**:
A single reference line or band from a catalog that can be displayed on the plot.
_Avoid_: User group item, classification rule

**Marker visibility**:
The user-controlled display state of a spectral-line marker on the plot for a
catalog identity. The state belongs to the marker, even when the marker appears
in multiple groups or grouping views.
_Avoid_: Group row visibility, per-reference visibility

**Marker reference**:
A user-owned reference to a spectral-line marker by catalog identity and marker
identifier.
_Avoid_: Copied marker data, label match

**Shared marker reference**:
A marker reference that appears in more than one group within the same user
grouping view.
_Avoid_: Duplicate marker, conflicting marker

**Move marker reference**:
A user action that transfers a marker reference from one user group to another
within the same editable user grouping view, including while that view is
filtered by grouping view search.
_Avoid_: Edit catalog group, rewrite marker

**Copy marker reference**:
A user action that adds a marker reference to another user group while keeping
the original reference in place.
_Avoid_: Duplicate marker data, clone line

**Duplicate grouping view**:
A user action that creates a new user grouping view from an existing user
grouping view.
_Avoid_: New blank view, catalog grouping view

**Unresolved marker reference**:
A marker reference whose catalog identity is present but whose marker identifier
is not present in the current catalog contents.
_Avoid_: Deleted line, invalid catalog row

**Grouping view**:
A tree-shaped organization of spectral-line marker references for one catalog
identity. Its marker visibility controls are shared catalog user state.
_Avoid_: Plot filter, catalog content

**User grouping view**:
A named, editable grouping view whose marker membership belongs to the user. It
does not change the public spectral-line catalog or its public groups, and
creating or selecting it does not change plot visibility by itself.
_Avoid_: Catalog group, line-table group

**Catalog grouping view**:
A grouping view whose marker membership is derived from the spectral-line
catalog's own grouping and is read-only. It exists only when the catalog
provides grouping.
_Avoid_: User grouping view, editable catalog groups

**Ungrouped spectral-line catalog**:
A spectral-line catalog that does not provide its own grouping. It has no
catalog grouping view.
_Avoid_: All markers view, empty catalog

**User group**:
A top-level group inside a user grouping view. It contains marker references and
does not nest other user groups. Ordinary user groups can be reordered within a
user grouping view.
_Avoid_: Catalog group, subgroup

**Unassigned user group**:
The non-removable, non-renamable user group that contains current catalog marker
references not organized into ordinary user groups in the same user grouping
view. It is fixed after ordinary user groups.
_Avoid_: Catalog group, missing markers

**User group visibility control**:
A bulk control on a user group that changes marker visibility for the group's
resolved marker references. It is not separate visibility state for the group,
and it has a non-actionable search state when the visible group contents are
filtered by search.
_Avoid_: Group visibility state, per-group overlay state

**Grouping view search**:
A text filter applied to the active grouping view tree. It preserves user group
structure and only expands groups whose marker references match.
_Avoid_: Global catalog search, flat result list

**Grouping view set**:
The user's optional collection of alternative user grouping views for one
catalog identity. It may be empty.
_Avoid_: Catalog set, line-table variants
