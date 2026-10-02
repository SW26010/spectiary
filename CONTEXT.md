# Spectiary

Spectiary is an interactive spectral inspection tool. Its language separates
physical reference data from user-owned display organization.

## Language

### Sample Filtering Language Standard

Spectiary reserves this word family for user-authored sample subset selection.
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

**Spectral Line List**:
A complete portable document with stable identity, laboratory wavelength
semantics, markers, independent grouping views and color schemes. The normative
[v1 JSON contract](docs/reference/spectral-lines/spectral_line_list_v1.md) owns
validation and canonical field definitions.
_Avoid_: Generic catalog schema, ViewProfile, session state

**Public spectral-line list**:
The program-owned, read-only complete v1 document packaged with Spectiary.
Existing UI text may still call it the public spectral-line catalog.
_Avoid_: User line groups, private presets

**Line-list identity**:
The declared stable identity independent of path, filename, display name and
schema version. It scopes built-in state and identity-specific user-file sessions.
_Avoid_: File path alone, active tab

**Built-in spectral-line overlay**:
Application-managed durable customization containing additional grouping views
and an optional complete color-scheme override. Composition produces a valid
complete SpectralLineList; it owns no marker definitions or session state.
_Avoid_: A second public file format, copied catalog, deep patch

**Spectral-line session state**:
Active grouping/color selections, marker visibility, group expansion and UI name
localization provenance. It stays outside canonical content. The old catalog
user-state monolith is retired.
_Avoid_: Canonical grouping or color definitions
**Active Spectral Line List**:
The sole complete effective list consumed by the panel and plot. The built-in
owner composes its packaged base and durable overlay; an opened user-owned file
is a read-only canonical generation at its original path. Opening checks runtime
user-file admission and validates the production v1 codec before adoption.
Failed candidates preserve the previous owner/model/session. User-file sessions
are memory-only and isolated from built-in persistence, even if a user file
claims the built-in identity. No recent locator is restored at startup.

Selecting the already opened user list reuses its adopted generation; Open reads
current bytes again. Explicitly opening the same identity at a moved path can
reuse compatible session state, without searching for missing files. All valid
v1 lists remain inspectable, including lists with no grouping views or markers.
The controller hands the complete immutable `SpectralLineList` and session
presentation to the downstream `ProjectSpectralLineList` boundary, without
checking plot compatibility or changing coordinate semantics. Main and immersive
plots consume only that boundary's renderer-ready output; panel diagnostics use
its controlled status. Its temporary conservative guard passes through only the
existing laboratory/rest vacuum Angstrom path. Unsupported coordinates suppress
the overlay while the complete list remains active and inspectable. Snapshot
capability and rest-frame warning behavior still applies. Broader projection,
unit conversion and compatibility policy belong to #105/#129; editing/saving
user definitions remains #66/#67 scope.

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

A subsequent ordinary source-free GUI launch with a compatible registered peer
in the same runtime/config namespace restores the Files roster but starts with
no active source or sample. Background restore never presents a source before
user selection. This transient activation override never replaces the durable
source session or locked viewport; explicit source/viewport mutations persist normally.
Application settings/layout keep their normal lifecycle. Explicit-source and
automation startup retain their existing contracts.

An explicit source startup restores the persisted Files roster and overrides
the deferred activation target through the normal source-opening path, without
presenting another restored source first. The Windows taskbar Jump List projects
Files into shell
destinations using the current process identity, each explicitly opening a new
GUI instance. It is not a source registry. See
[ADR 0017](docs/adr/0017-source-jump-list-and-startup-policy.md).

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
state. Ordinary sequence navigation provides no current sample to downstream
sample workflow or plot surfaces in this state. Explicitly opening an existing
filesystem member may still provide a displayed sample, even when the sequence
is empty, without assigning a sequence position or Previous/Next cursor.
An empty sequence retains the active source and its task header and save state.
Source reuse identity, live workflow revision, and context/listing proofs do not
depend on a current sample; only resident snapshot lookup needs a target row.
_Avoid_: Source collection, narrowed-sequence index, labeling queue

**Sample filtering**:
The user-facing selection of a subset of spectrum samples from a source
collection.
Sample filtering owns navigation-sequence membership, not eligibility for
explicit display. Opening an existing filesystem member can display its source
row outside the sequence, with no sequence position or Previous/Next cursor.
Ordinary sample location remains constrained by the sequence. Disabling sample
filtering retains the explicitly displayed row, including under active sorting.
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
The in-progress sample label result of a temporary sample labeling task. Its
running in-memory object is authoritative; an internal best-effort checkpoint
may support resuming it before explicit canonical Save As.
_Avoid_: Final label output, label set, source data

**Spectral-line marker**:
A single laboratory reference line or band from a Spectral Line List that can be displayed on the plot.
_Avoid_: User group item, classification rule

**Marker visibility**:
The user-controlled display state of a spectral-line marker on the plot for a
line-list identity. The state belongs to the marker, even when the marker appears
in multiple groups or grouping views.
_Avoid_: Group row visibility, per-reference visibility

**Marker reference**:
A reference to a spectral-line marker by stable marker ID within the same list.
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
An invalid canonical or overlay reference whose marker ID is absent from the
same line list. Reads fail without partial application; there are no durable
placeholder references in v1.
_Avoid_: A disabled canonical marker, guessed name/wavelength match
**Grouping view**:
A document-owned named ordered list of groups, each referencing markers only by
stable ID. Visibility and current selection are separate session state.
_Avoid_: Plot-filter language, paired color profile

**User grouping view**:
An editable ordinary grouping view owned by a user document or built-in overlay.
Creating/selecting it does not itself change marker visibility.
_Avoid_: Intrinsic marker category

**Base grouping view**:
An ordinary grouping view owned by the packaged base. Its membership is read-only
because of its owner. Existing UI may call it a catalog grouping view.
_Avoid_: Array index zero, canonical read_only flag

**Ungrouped Spectral Line List**:
A valid list with no grouping views. Users can create an empty grouping view.
_Avoid_: Empty marker collection

**Group**:
A named ordered list of marker IDs within one grouping view. Groups do not nest,
IDs are unique across the entire list, and one marker may occur in multiple groups.
_Avoid_: Intrinsic marker group field

**Unassigned**:
A derived UI area containing markers absent from all ordinary groups of the
current view. It is fixed after ordinary groups and has no canonical special
identity/flag. An authored group named Unassigned is an ordinary group.
_Avoid_: Durable system group, unresolved reference storage
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
line-list identity. It may be empty.
_Avoid_: Catalog set, line-table variants

## Architecture Constraints

Enhancements, optimizations and compatibility work must keep implementation and
maintenance cost proportionate to demonstrated user benefit. Prefer documented
platform APIs, supported dependency interfaces and existing ownership boundaries.
Local improvements do not implicitly authorize replacement infrastructure or
open-ended patches. See [ADR 0010: Proportionate Complexity and Maintainable
Integrations](docs/adr/0010-proportionate-complexity-and-maintainable-integrations.md)
for scope, experimentation, escalation and bounded-outcome rules.
