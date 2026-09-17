# SpecForge

SpecForge is an interactive spectral inspection tool. Its language separates
physical reference data from user-owned display organization.

## Language

### Sample Filtering Language Standard

SpecForge reserves this word family for user-authored sample subset selection.
Always qualify new user-facing text and new documentation as `sample filter`,
`sample filtering`, or `Sample Filters`. Tests and domain-facing code should
move toward the same qualification as related areas are touched, but existing
names may be migrated in orderly follow-up work. Unqualified forms such as
`filter`, `filtering`, `filtered`, `Filters`, `active filter`,
`filter condition`, and `filtered set` are terminology defects in new product
language unless they are part of an explicit alias list or migration note.

Do not use this word family for display transforms, loader validity rules,
spectral-line search, or overlay visibility:

- A display-only curve transform is **Spectrum smoothing**.
- A domain rule that keeps only plottable spectrum points is **Valid-point
  selection**.
- A point excluded by finite-value, wavelength, mask, or ivar rules is a
  **Rejected pixel**.
- Text matching inside a spectral-line grouping view is **Grouping view search**.
- Showing or hiding spectral-line markers is **Marker visibility**.

The shorthand rule is: sample filters select samples; smoothing transforms
displayed values; validity rules reject bad pixels; search matches text;
visibility shows or hides overlays.
Searches for unqualified forms in product text or new documentation should
treat them as terminology defects except inside this standard's explicit invalid
examples, alias lists, and migration notes. Existing code and test hits should
be treated as terminology migration candidates rather than immediate definition
violations.

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

**Source collection**:
A loaded set of one or more spectrum samples that share one source identity and
one sample ordering.
_Avoid_: Single plotted spectrum, label file, spectral-line catalog

**Source collection session**:
The user's active working context for one source collection. It owns sample
navigation state and loaded sample annotation results for that collection, while
the plot snapshot remains the current spectrum display view.
_Avoid_: Plot snapshot, labeling task, file parser

Startup display and status follow the saved active source collection. Failed
restored sources remain removable, non-selectable Files rows. See the
[source list and startup restore requirements](docs/product_requirements.md#文件源列表与启动恢复)
for display, diagnostics, and persistence behavior.

**Sample label**:
A classification assigned to one spectrum sample within a source collection. It
is separate from spectral-line marker labels and does not define spectral
reference data.
_Avoid_: Spectral-line label, marker label, metadata tag

**Sample annotation value**:
A stored per-sample value associated with one spectrum sample. It may be a
classification code, string, or continuous value.
_Avoid_: Spectrum flux, sample name, row index

**Sample annotation result**:
The collection of sample annotation values associated with spectrum samples in a
source collection. It is the general per-sample result shape; a sample label
result is the classification-specific form.
_Avoid_: Label set, UI preferences, spectral-line catalog

**Sample label value**:
The stored value that represents a sample label for one spectrum sample. It may
already be human-readable or may need a mapping before display.
_Avoid_: Marker label, display tag, spectrum name

**Sample label mapping**:
An optional interpretation from stored sample label values to user-facing sample
labels.
_Avoid_: Required class list, spectral-line mapping, parser rule

**Sample label set**:
The explicit set of sample labels available within one sample labeling task.
_Avoid_: Existing label values, metadata tags, grouping view

**Sample labeling task**:
A distinct classification objective for a source collection, with its own sample
label set and sample label result.
_Avoid_: Only label file, global classification, spectral-line grouping

**Active sample labeling task**:
The sample labeling task currently accepting manual label writes for a source
collection. It may be deactivated without deleting its task record, label set,
sample label result, or output target.
_Avoid_: Labeling window visibility, implicit task deletion, sample annotation
view

**Temporary sample labeling task**:
The single output-free sample labeling task retained for one source collection.
An ordinary manual draft uses the default name `Temporary labeling task`, or
the smallest available positive integer suffix when that exact name is already
in the source's task list. User-entered duplicate names remain valid. An
annotation-promotion draft retains its promoted task name. It may be paused and
resumed, and becomes a local labeling annotation only after the user selects an
output location. A source collection may not own two temporary sample labeling
tasks.
_Avoid_: Unsaved copy of every formal task, unnamed annotation, global task

**Sample navigation**:
The user-facing control of the current spectrum sample within a source
collection, including movement, ordering, and locating a sample.
_Avoid_: Labeling window, label set, plot pan

**Sample navigation sequence**:
The ordered set of spectrum sample row indexes that sequential sample
navigation consumes. Without active sample filtering or sorting, it is the
source collection's full sample ordering. With active sample filtering or
sorting, previous, next, navigation-list movement, and labeling auto-advance
operate within this sequence while the current sample remains identified by its
source row index. An empty sample navigation sequence is a valid active sequence
state that provides no current sample to downstream sample workflow or plot
surfaces.
_Avoid_: Source collection, narrowed-sequence index, labeling queue

**Sample filtering**:
The user-facing selection of a subset of spectrum samples from a source
collection.
_Avoid_: Unqualified filter language, spectrum smoothing, valid-point
selection, grouping view search, marker visibility, plot zoom

**Sample sorting**:
The user-facing choice of the order used by the sample navigation sequence.
Sample sorting never changes which samples belong to the sequence.
_Avoid_: Source row index, sample filtering, source collection identity

**Spectrum smoothing**:
A display-only plot transform that draws a derived curve from current spectrum
y values without changing the current snapshot.
_Avoid_: Spectrum-filter language, pixel-filter language, domain-filter language

**Valid-point selection**:
A domain loading rule that keeps only spectrum points eligible for plotting,
including finite-value, positive-wavelength, mask, and ivar checks.
_Avoid_: Pixel-filter language, mask-filter language, ivar-filter language,
smoothing

**Rejected pixel**:
A spectrum point excluded by valid-point selection because its values, mask, or
ivar make it unplottable.
_Avoid_: Filtered-pixel language, smoothed point, hidden marker

**Sample annotation view**:
The user-facing display of loaded sample annotation results for the current
spectrum sample.
_Avoid_: Labeling task editor, sample navigation, source metadata

**Sample name**:
An optional source-provided name for one spectrum sample. It is separate from row
index and may be absent.
_Avoid_: Row index, fallback sample display name, file path

**Fallback sample display name**:
A UI-only display string generated when a spectrum sample has no source-provided
sample name. It may help identify the current row, but it is not a sample name
and does not participate in sample-name location.
_Avoid_: Sample name, source identity, relink key

**Sample label result**:
A classification-specific sample annotation result whose values are sample label
values for one sample labeling task.
_Avoid_: Label set, UI preferences, generic annotation result, continuous annotation

**Sample label result metadata**:
A portable description of a sample label result's label codes, display names,
shortcuts, unlabeled sentinel, and expected result shape. It explains a compact
label output file without owning the label values or local workflow recovery
state.
_Avoid_: Sample labeling task record, sample labeling draft, label array payload

**Sample labeling draft**:
The in-progress sample label result of a temporary sample labeling task, saved
for recovery before the user selects the intended labeling output.
_Avoid_: Final label output, label set, source data

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
narrowed by grouping view search.
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
_Avoid_: Plot-filter language, catalog content

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
and it has a non-actionable search state when search narrows the visible group
contents.
_Avoid_: Group visibility state, per-group overlay state

**Grouping view search**:
A text search applied to the active grouping view tree. It preserves user group
structure and only expands groups whose marker references match.
_Avoid_: Global-catalog-filter language, flat result list, sample filter

**Grouping view set**:
The user's optional collection of alternative user grouping views for one
catalog identity. It may be empty.
_Avoid_: Catalog set, line-table variants

## Architecture Constraints

Enhancements, optimizations and compatibility work must keep implementation and
maintenance cost proportionate to demonstrated user benefit. Prefer documented
platform APIs, supported dependency interfaces and existing ownership boundaries.
Local improvements do not implicitly authorize replacement infrastructure or
open-ended patches. See [ADR 0010: Proportionate Complexity and Maintainable
Integrations](docs/adr/0010-proportionate-complexity-and-maintainable-integrations.md)
for scope, experimentation, escalation and bounded-outcome rules.
