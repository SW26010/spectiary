# Sample Labeling

This document describes sample-label inspection, manual labeling, and local
recovery boundaries. It is separate from spectral-line marker labels and from
spectral-line catalog user state.

## Language

Use the terms from `CONTEXT.md`:

- `Sample label`: a classification assigned to one spectrum sample.
- `Sample annotation value`: a stored per-sample value.
- `Sample annotation result`: stored per-sample values for a source collection;
  the general per-sample result shape.
- `Sample label value`: the stored value for one sample label.
- `Sample label mapping`: optional interpretation from stored values to
  user-facing labels.
- `Sample label set`: the explicit labels available for manual labeling.
- `Sample labeling task`: a distinct classification objective for a source
  collection.
- `Sample label result`: the classification-specific form of a sample annotation
  result for one sample labeling task.
- `Sample label result metadata`: portable metadata that explains a compact
  sample label result.
- `Sample labeling draft`: an in-progress label result saved for recovery.
- `Sample filtering`: user-authored sample subset selection. In SpecForge, this
  word family should be qualified in new product and documentation language and
  must not refer to spectrum smoothing, valid-point selection, spectral-line
  search, or marker visibility.

## Staged Rollout

Sample annotation and labeling is the next core product direction for SpecForge,
but it should advance in this order:

1. Sample navigation.
2. Read-only sample annotation inspection.
3. Editable sample labeling.
4. Sample filtering.

The first vertical slice is sample navigation plus read-only annotation
inspection. It includes navigating samples by row index or sample name,
automatically loading same-prefix `*_y.npy` annotation results for NPY source
collections, and displaying the current sample's annotation values.

Editable manual labeling, external label-output autosave, draft recovery, source
relink behavior, and sample filtering belong to later sub-stages after the
navigation and read-only annotation contracts are proven in real workflow use.

## Existing Labels

Existing per-sample annotation values may be loaded from companion data for the
active source collection. Integer annotation values may be interpreted as sample
label values through a sample label mapping when the user associates them with a
sample labeling task. String and floating-point annotation values may be shown
directly as read-only annotation results.

When existing integer annotation values are present but no mapping is available,
SpecForge should show the stored values rather than guessing class names.

For automatically loaded read-only annotations, SpecForge should infer the
annotation kind from dtype. Integer dtypes are categorical annotations and may be
treated as sample-labeling results for display. String dtypes should default to
plain read-only annotation display so companion name arrays or other text fields
are not accidentally treated as classification labels. Source-aware CSV
attachments using the defined `sample,label` or `filename,label` interchange
schema are the explicit exception: their text label column may be promoted to an
outputless editable task. Arbitrary string NPY annotations remain read-only.
Floating-point dtypes are continuous annotations; they should display raw values
and should not use sample label sets, mappings, classification shortcuts, or
clear-label behavior.

A source collection may have multiple formal sample labeling tasks and read-only
annotation results, but at most one output-free temporary sample labeling task.
Existing annotation files or arrays should not be treated as
globally tied to the currently selected sample label set; they may represent a
different classification dimension or a non-classification value.

Companion annotation data discovered next to a source collection should be loaded
automatically as read-only sample annotation results. It should use the
same per-sample display and navigation surfaces as editable manual labeling
results, but they must not be mutated unless the user explicitly starts an
editable task or chooses an output target.
For NPY source collections, a same-prefix `*_y.npy` companion should be loaded
automatically as a read-only sample annotation result by default. Other
annotation or label result files should not be loaded automatically in the first
implementation; the user should add them explicitly. In particular,
`*_label.npy`, `*_known_mask.npy`, `*_index.npy`, and similar pipeline helper
arrays are not first-stage auto-discovery targets.
Automatically loaded `*_y.npy` annotation results should accept common integer
dtypes, floating-point dtypes, and string or unicode dtypes.

Every sample annotation result loaded for a source collection must have exactly
one annotation value per spectrum sample. If the annotation result length does
not match the source collection's spectrum count, SpecForge must not attach it
to that source collection.

Manual/read-only annotation attachment accepts both the existing NPY forms and
canonical `.asdf` labeling documents. ASDF attachment is source-aware: the
document's base identity, source name, source fingerprint, and sample count must
match the active base collection. An `explicit_names` roster must additionally
match the active canonical sample names exactly and in source order;
`source_index` relies on the matching base identity to define that order. A
successful attach retains the complete canonical document (task identity/name,
label definitions, annotation identity/values, source identity, and roster)
while projecting the values and label definitions into the existing annotation
views. Count-only attachment APIs do not accept ASDF because they cannot prove
roster compatibility. Existing `.npy` and `.npy` plus `.sf-labels.json` reads
remain unchanged. An attached standalone ASDF remains an external read-only
result until the user explicitly confirms that it should be edited in place.
Confirmation adopts that same document as a canonical local owner: SpecForge
uses the document's stable task id/name, labels, and values, never generates a
replacement task identity, and never hands the path to the legacy NPY writer.
Adoption first acquires the task-identity and canonical one-file output leases,
then reopens the current durable generation through the ASDF document store and
validates source, roster, annotation, and task identity. Only after validation
does it persist a structural local task record and hydrate the editable
projection. Adoption itself neither copies nor rewrites the ASDF bytes.

## Manual Labeling

Manual labeling normally starts with the source collection's single temporary
sample labeling draft slot. It has a stable task id, starts with the default
name `Temporary labeling task`, and owns one sample label set plus workflow
choices such as auto-advance behavior. The default name is ordinary persisted
task data, not a fixed temporary-state label: the UI displays it verbatim and
keeps it editable without localizing or replacing it to communicate temporary
ownership. Historical, conflicting, or damaged cache state may still expose
more than one recovery draft; those rows are handled through the
identity-bearing recovery list. Labeling does not ask for a task name when this
draft is created or resumed.

Starting a new temporary sample labeling task should start with every sample
unlabeled. The initial label result is therefore a one-dimensional array filled
with `-1`. Copying or editing existing label files is handled by explicit user
file choices rather than by separate first-run task modes.

The first editable labeling implementation should only support numeric
categorical tasks. Each label in a sample label set should have an explicit
stable numeric code. The UI should suggest the next available code when the user
creates a label, but the user may explicitly choose a different unused code to
match an existing training or data convention. The saved code is the durable
identity used in numeric label results. Reordering labels, changing display
names, or changing shortcuts must not change existing label code meanings.
Changing a code that is already used by saved label values should require an
explicit confirmation because it changes the interpretation of existing data.

The labels panel should present one saved label per table row. Adding a label
first saves a definition with suggested defaults and then enters a transient
row-editing state. Leaving that editing state discards only the unsaved field
changes; it must not remove the already-created label. The row acts as one edit
focus scope, so clicking another row or control both cancels the current edit
and activates the clicked target in the same interaction.

Deleting a label that has assigned samples is destructive and must require
explicit confirmation. Confirming changes every sample using that code to the
unlabeled sentinel `-1`, removes the deleted code from any selected sample-filter
values, and reconciles sample navigation. If the current sample leaves the
active sample navigation sequence, the displayed spectrum snapshot must move to
the reconciled current sample. The confirmation must describe these effects
before the deletion is submitted.

Each numeric label needs a stable numeric code, a display name, and an optional
shortcut. Colors and other styling are optional later extensions. String and
floating-point annotations may be displayed read-only, but they should not be
edited or saved as first-version sample labeling tasks.

Auto-advance after labeling is a global workflow setting for the active sample
labeling task, not a per-label setting. When auto-advance is enabled, the
default target is the next sample in the current sample navigation sequence.
Without active sample filtering or sorting, that sequence is source order. A
separate skip-labeled option may make auto-advance jump to the next unlabeled
sample in the current sample navigation sequence instead.
Because sample navigation is owned outside the Labeling window, assigning a
label should emit an advance request rather than directly changing the current
sample. The sample navigation surface decides and performs the actual switch,
including skip-labeled behavior. Labeling does not inspect sample-filter or
sample navigation sequence state before issuing the request. After a Labeling
navigation request, Navigation should return a navigation result with the actual
current sample index and whether movement occurred. Labeling uses the returned
actual index to update the task's remembered labeling position. If Navigation does not
move, the returned index is still the current sample, so the write applies there
and the remembered labeling position remains there. The movement flag is program
state for control flow and tests, not a source for Labeling-owned user feedback.
Any user-facing reason for not moving belongs to Navigation, not Labeling.

Existing sample label values are not enough to define the manual labeling
workflow because they do not necessarily contain shortcut, ordering, styling, or
navigation behavior.

## Sample Windows

SpecForge should separate sample navigation, sample filtering, sample annotation
inspection, and active manual labeling into distinct windows or surfaces.

The visible sample navigation window should be named `Navigation`. In this
document, `Sample navigation` remains the domain term for that surface and its
state ownership; `Navigation` does not refer to wavelength range navigation or
plot pan/zoom controls.
In the `Navigation` UI, `source sample` is a user-facing label for the current
source row index shown as a 1-based sample number. It is distinct from
`sequence`, which shows the current position within an active sample navigation
sequence.

The visible active manual labeling window should be named `Labeling`. In this
document, `Sample labeling task` and `active manual labeling` remain the domain
terms for the task and workflow behind that window.

The first row of `Labeling` should use one compact task selector for task
activation, create/resume, and annotation drop. Its permanent first item is
`New labeling task` when no temporary draft exists, `Resume labeling draft`
when exactly one draft is paused, and the selected temporary draft while that
draft is active. When multiple drafts exist, recovery rows carrying their
source/task identities are the only activation route. Remaining items are the
source collection's formal local labeling annotations. The selector itself
remains the drag target for compatible rows from `Annotations`. Pause and
delete controls belong immediately to the selector's right on the same row.
Switching through the selector or drag target must honor the same pending/failed
output-save guard as explicit close.

Whenever a task is active, a persistent `Task name` UTF-8 edit field appears
directly below the selector for both temporary drafts and formal tasks. Enter or
focus loss submits an actual change; task switching discards an edit aimed at
the previous task. The task header also shows the complete task id read-only
and provides a `Copy ID` button that remains accessible in a narrow dock. These
controls remain visible even when the active sample filter has no matching
sample. The temporary-draft recovery list follows this task header so the
primary task controls remain at the top of `Labeling`, including when the list
contains multiple drafts or the dock is narrow.

Pausing or closing a sample labeling task should deactivate the active sample
labeling task rather than hide the `Labeling` window or delete the task record.
Deactivation leaves the task record, label set, shortcuts, selected output path, remembered
position, and sample label result intact. After deactivation, `Labeling` has no
active task. If its temporary task still exists, the primary action is to resume
that task rather than create another one. A formal categorical annotation may
also be activated from the `Annotations` window.
In the first implementation, closing a formal output-bound task should be
disabled while it has pending or failed output saves, including pending or
failed canonical-document or legacy metadata-sidecar saves. The user must wait
for autosave to complete or fix the output save problem before deactivating that
formal task. A temporary
task whose first `Save to...` attempt fails remains output-free and recoverable:
it keeps its temporary identity and error message and may be paused, deleted, or
saved to the same or a different target. Tasks in the internal-autosave-draft
state may also be closed because their current recovery state is owned by the
local task record. A later implementation may allow non-blocking close with
background retry and explicit pending-task surfacing.

Deleting a sample labeling task is a separate explicit operation from closing
or deactivating it. Delete removes the local task record and its internal draft.
It must not delete any artifact owned by the task's persisted format: neither a
canonical ASDF document nor a legacy `.npy` result and its adjacent portable
metadata sidecar. If the same output is still loaded as an annotation later,
SpecForge should treat it according to the normal plain/external/local matching
rules rather than silently resurrecting the deleted local task record. Delete
should be disabled while the task has pending or failed output saves, matching
close/deactivate.

The visible sample filtering window should be named `Sample Filters`. In this
document, `Sample filtering` remains the domain term for sample-filter ownership
and behavior. The unqualified window label is invalid because it can be
confused with spectrum smoothing or loader validity rules.

The visible sample sorting panel should be named `Sample Sorting`. In this
document, `Sample sorting` remains the domain term for choosing the order used
by the sample navigation sequence. It is a separate dockable ImGui panel because
sorting is a first-class sample workflow surface in the dock layout, not a hidden
subsection of `Navigation` or `Sample Filters`. It is separate from `Sample
Filters` because sample filtering owns which samples are included, while sample
sorting owns only their navigation order.

Sample navigation owns the current sample index, previous/next movement,
ordering, and locating samples by index or sample name. In the first
implementation, that state should belong to a source-collection session or
controller rather than to the Shell UI, plot window, or Labeling window. Other
surfaces issue navigation requests and consume the resulting current sample;
they do not directly mutate the current sample index. Sample navigation does
not own sample filtering or manual labeling controls. It should remember the last
shown sample index for a source collection whether or not any sample labeling
task is active, and that remembered index should be persisted in local user state
by source collection identity. Row index and sample name are independent locating
mechanisms. For NPY source collections, sample names come from a
same-prefix `*_name.npy` companion when present and length matched; without that
companion, samples do not have sample names. For directory collections, each
contained FITS or CSV file name is the sample name. The UI may still show a
fallback sample display name for an unnamed NPY row, but sample-name location
searches source-provided sample names only. Sample-name location should use the
same realtime matching style as spectral-line search. As the user edits the
query, the navigation surface updates the matching sample list immediately.
Fallback sample display names are not searched; row-index location remains a
separate direct numeric location mechanism. Navigation is the only
surface that should expose previous/next sample movement and its shortcuts;
Labeling should not duplicate separate previous/next controls. Navigation
actions should still carry intent: direct location by row index or sample name is
a direct locate action, while previous/next movement is a sequential move action.
The default sequential movement shortcuts are unmodified Left Arrow for previous
and unmodified Right Arrow for next. They are active only in the Navigation,
ordinary plot, or immersive plot contexts. Text editing, modifier combinations,
and open popups retain ownership of those keys and suppress sample movement.
Keyboard focus takes precedence over pointer hover: a merely hovered Navigation
or plot context must not claim the arrow keys while another panel owns keyboard
focus.
Within those contexts, Tab and Shift+Tab remain the standard way to move control
focus; Left Arrow and Right Arrow are owned exclusively by sequential sample movement.

The first read-only vertical slice should keep navigation in source order and
support only previous/next movement, direct row-index jumps, and sample-name
realtime matching. Sorting by sample name or annotation value belongs to a later
navigation sub-stage after the current-sample and annotation-display contract is
stable.

Future sample navigation sorting modes may include sample name and annotation
value from a user-selected annotation result or sample labeling task. Sorting by
annotation value must not implicitly follow the active editable task, because
changing the active task should not unexpectedly reorder sample navigation.
Sample sorting belongs to the separate `Sample Sorting` panel. That panel
submits the active sample-sorting choice; it does not own the materialized sample
navigation sequence and should not duplicate Navigation-owned position or source
row counts. The first explicit-source implementation should show `Source order`
and `Sample name` by default. `Source order` is the default ascending ordering
and can be switched to descending source-row order from the same row-level
control. Additional supported annotation sort sources must be added by the user
from an add-source control in `Sample Sorting` or by dragging the annotation row
into the sort-source area in `Sample Sorting`. The top-level reset checkbox is a
shortcut for source-order ascending sorting and should uncheck automatically
when another sort source becomes active. Removing an added
sample-sorting entry should remove that entry from the visible list; if it was
the active sort source, navigation returns to source order. This first
implementation supports only one active sort source at a time. Multi-priority
sorting, entry reordering, and insertion-position semantics are deferred. The
active sort direction should be controlled from the sort-source row, using a
compact up/down arrow rather than a separate direction control group. The active
sort source is indicated by native selected button styling on the arrow;
inactive sort sources show a muted clickable arrow instead of a radio-style
selection control. Each visible sort source keeps its own cached direction, so
changing the active source's direction must not change inactive arrows.
Annotation-value sorting should support ascending and descending order. Read-only
string annotations may be sorted by lexical order, and floating-point
annotations may be sorted by numeric value. Integer annotations without
sample-label-result evidence may be sorted by numeric value. Integer annotations
with sample-label-result evidence must not be available for sample sorting in
the first implementation, because their stored codes identify labels rather than
rank. Sample-label-result evidence includes a matching local sample labeling
task record, a matching canonical ASDF document, matching adjacent legacy
sample label result metadata such as `<stem>.sf-labels.json`, or a loaded
annotation relationship that marks the values as an external or local sample
label result.
In the first implementation, an annotation should be available as a
sample-sorting source only when every sample has a comparable value for that
annotation. Annotation results with missing values, NaN values, or values that
cannot be compared consistently should not be exposed for sample sorting.
Applying or changing sorting creates an active sample navigation sequence even
when sample filtering is not active. It should keep the current sample selected
when that sample remains in the sequence, recompute its sequence position, and
make previous/next navigation, navigation lists, sample-name location, and
labeling auto-advance follow the sorted sequence. Sample sorting never changes
which samples belong to the sample navigation sequence.
The Navigation window exposes `source sample` as an editable 1-based source-row
number whenever direct source-row location is available. While a filtered or
sorted sequence is active, it also exposes `sequence` as an editable 1-based
position within the current sequence. A `source sample` submission locates that
source row by converting the entered number to a 0-based `LocateRow` target; a
`sequence` submission locates the source row at that position after active
filtering and sorting through `LocateSequencePosition`.
Both numeric fields use the same panel-private editing implementation while
keeping independent edit state. `Settings > Input > Live numeric navigation` is
persisted and enabled by default. While it is enabled, every actual text change
that parses as a valid in-range 1-based target submits a navigation intent in the
same UI frame, without debounce. For example, entering `45` submits the valid
prefix `4` and then `45` as 0-based targets `3` and `44`. Empty, invalid,
out-of-range, and unavailable source-row targets do not submit. These requests
express the latest intent immediately; asynchronous source loading may cancel an
older task or reject its completion, and does not guarantee that every prefix is
presented.

Enter remains an explicit latest-intent submission even when the text names the
currently displayed row or position, so it can cancel a different deferred
target. In live mode, Escape ends editing while retaining the last navigation
intent already submitted; focus loss, a collapsed or hidden Navigation window,
a covered dock tab, and immersive plot mode also end the edit without replaying
the last value. When live numeric navigation is disabled, character-by-character
edits do not navigate: Enter commits, Escape cancels, and leaving a dirty field
queues one focus-loss commit after all same-frame panels have applied their
actions. A focus-loss commit is discarded when its activation-time
sequence-topology revision is no longer current. Both modes continue to use the
Shell's single frame-end navigation-input finalization entry.
`source sample` remains read-only whenever direct source-row location is
unavailable. Source, context, count, filtering, or sorting topology changes
invalidate edits from the previous topology. Ordinary committed or pending
cursor updates do not overwrite an active buffer. No-op sample-filter or sorting
reconciliation that retains the effective active state, membership, and row
order also retains the revision and the edit. The current source row remains
selected when eligible, and only its displayed sequence position is
resynchronized.

Sample filtering belongs to the separate `Sample Filters` window. It should
start with no selected sample-filter entries and support stacking multiple
sample-filter conditions after the user explicitly adds supported annotation
results from `Annotations`, including local labeling task rows displayed there.
The user may add a supported annotation result from an add-source control in
`Sample Filters` or by dragging the annotation row into `Sample Filters`.
Removing a sample-filter entry should clear any condition for that entry.
Resetting sample filters should remove all selected sample-filter values while
leaving the explicit source entries available for continued filtering. Sample
filtering must not implicitly follow the active editable task.
Categorical sample filtering should support selecting multiple values at once,
including an unlabeled sentinel when it is present in an annotation result. For
numeric categorical values with a label set or mapping, the sample-filter UI
should show the label name with the numeric code, such as `bad (1)`.
Without a mapping, it should show the raw code. Read-only string annotations may
participate in sample filtering by selecting multiple string values.
Floating-point annotations should not be available for sample filtering in the
first implementation. Multiple active sample-filter conditions use AND
semantics: a sample is included only when it satisfies every active condition.

When sample filtering is active, previous/next navigation, navigation lists,
sample-name location, and auto-advance should operate within the sample
navigation sequence. Applying or changing sample-filter conditions is a strong
workflow action. If the current sample remains in the new sample navigation
sequence, Navigation should keep it current and update its sequence position. If
the current sample is excluded from the new sample navigation sequence,
Navigation should remember it as the last sample before entering active sample
filtering and automatically move to the first sample in the new sequence. While
sample filtering remains active, changing sample-filter conditions or navigating
within the sequence must not replace that remembered sample. Clearing sample
filters should restore the last sample before entering active sample filtering
when it is still valid. An empty sample navigation sequence is a valid active
state; in that state no current sample should be displayed for the sample
workflow or main plot until the user clears or changes the sample-filter
conditions. The main plot must not keep rendering the last spectrum as fallback,
because Navigation has not provided a current source row for that state.
When sample filtering and sample sorting are both active, the sample navigation
sequence should first apply all active sample-filter conditions and then sort the
included samples. Sorting must not change which samples are included. Samples
with equal sort values should keep source order as a stable tie-break.
The construction of the sample navigation sequence should live behind one
navigation-owned logic boundary rather than being reimplemented by the `Sample
Filters`, `Sample Sorting`, `Navigation`, or `Labeling` panels. That boundary
should take the source collection rows, active sample-filter conditions, and
active sample-sorting choice, then produce the ordered row indexes, active or
empty sequence state, current sequence position, previous/next targets, and
sample-name matches scoped to the sequence.
As a first-implementation UX constraint, row-index location should be disabled
whenever sample filtering or sorting makes the sample navigation sequence differ
from the source collection's full source ordering. This is not a long-term
sample navigation definition. A later implementation may allow row-index
location within the active sample navigation sequence, but it must not jump to a
sample outside that sequence.
Requests from Labeling are still resolved by Navigation under these same
navigation rules; Labeling does not bypass or reinterpret the active sample
filter or sample navigation sequence.

The sample annotation view should be an independent `Annotations` window, not a
section inside the Files or Info windows. In this document, `Sample annotation
view` remains the domain term for that surface. The window should display loaded
sample annotation results, including automatically loaded `*_y.npy` data. It consumes
the active source collection and current sample from sample navigation, but it
does not own source selection, file management, or the current sample index. It
should show one annotation result per row with a display name and the current
sample's annotation value or mapped label name. In the first read-only vertical
slice, it should not show drag or convert-to-labeling affordances because
editable sample labeling is not available yet.

An annotation result's display name is user-facing local workflow state. It must
not rename the underlying annotation file, change the annotation path used as a
stable source identity, or rewrite portable sample label result metadata. The
default display name is the loaded result name: automatically loaded plain
annotations default to the annotation file name, metadata-backed label results
default to the metadata task name when available, and local sample labeling task
rows default to the formal task name stored in the local task record. The
user may edit the display name in place from the `Annotations` table. Clearing
the edited display name removes the custom local override and restores the
default display name. A custom display
name affects the `Annotations` row, the drag preview text, and the corresponding
entries shown in `Sample Filters` and `Sample Sorting`; hover text should still
reveal the original annotation path or file name so the source remains
inspectable.

Formal sample labeling task names and annotation display names are separate. A
manual draft starts with its stable default formal name, while an annotation
promotion derives its formal name from the promoted annotation metadata or
display name. Selecting `Save to...` does not derive or replace that name from
the output filename. Adopting an existing canonical ASDF preserves the document
task name; migrating a legacy owner preserves the sidecar task name. Editing the
annotation display name must not rename the task, output file, or portable
metadata.

After editable sample labeling exists, eligible annotation rows should expose a
small drag affordance so the user can drag that annotation to the Labeling window
and make it the active sample labeling task.

Dragging a writable integer categorical legacy annotation from the sample
annotation view converts it into a local task that keeps the annotation file as
its output target. SpecForge must warn that edits will modify the original data
in place. The warning should say this is appropriate only when the user
intentionally wants to edit that data or when the file represents an unfinished
labeling task being restored, and it should recommend backing up the original
data first. Promoting an eligible source-aware CSV text annotation instead
creates an outputless draft and never adopts or overwrites the CSV input.

Portable labeling evidence is format-specific. A canonical ASDF document owns
its task id, task metadata, source identity, roster, and values in one file. A
legacy NPY result obtains equivalent labeling evidence from its adjacent
`<stem>.sf-labels.json` sidecar. Neither a self-describing ASDF document nor a
matching legacy sidecar by itself makes the output the current user's own task.
Without a matching local task record for the active source and output path, the
annotation is an external label result and still requires the in-place edit
warning. With a matching local task record, SpecForge may restore or activate it
without that warning because the user has already established the output owner.

The annotation view should distinguish loaded per-sample data by workflow
relationship:

- Plain annotation: neither a canonical labeling document nor matching legacy
  sample-label metadata is available.
- External label result: canonical document metadata or matching legacy
  sidecar metadata is available, but no matching local task record exists.
- Local labeling task: the format's labeling metadata and a matching local task
  record both exist.

Plain annotations should display their current raw annotation value. Writable
integer categorical plain annotations may be dragged into `Labeling`, but doing
so follows the normal conversion flow and requires the in-place edit warning.
Floating-point plain annotations and arbitrary string NPY annotations are
read-only in the first editable labeling implementation. A source-aware CSV text
annotation with the defined label-column schema may expose the explicit
outputless-promotion action described below.

External label results should display the mapped label value from their
canonical document or adjacent legacy metadata and be visibly marked as
external. They may be dragged into `Labeling`, but doing so requires the
in-place edit warning. If the user confirms a standalone canonical ASDF,
SpecForge adopts its embedded stable task identity and exact document path as a
`canonical_asdf` owner after a leased store reopen; a same-id/different-path or
same-path/different-id local owner is a conflict and must not be resolved by
inventing `-2` style identities. Legacy annotation promotion remains the
separate `legacy_npy_with_sidecar` path.

Local labeling tasks should display the mapped label value and be visibly marked
as local. A canonical owner is missing when its ASDF document is unavailable; a
legacy owner is incomplete when its NPY result, metadata sidecar, or both are
unavailable. The row should show the corresponding missing-output state and
expose relink. A local labeling task may be clicked or dragged into `Labeling`
to activate it without the in-place edit warning. If another sample labeling
task is already active, the user must close or deactivate the current task
before activating the local task from `Annotations`.

A local task match always requires the active source collection identity, the
local record's normalized output path and persisted owner format, stable task
id, and sample/value count to match. For `canonical_asdf`, task id and source,
roster, and value contracts come from the ASDF document itself. For
`legacy_npy_with_sidecar`, the sidecar `task_id`, source summary, result
reference, dtype, and count must match the NPY result and local record. Moving
or renaming any required artifact, clearing local user state, copying another
user's output, or relinking a source collection may prevent recognition until
the user explicitly reconnects it.

If a local record still exists but its format's required artifact set is
missing, SpecForge should keep the task visible with a missing-output state
rather than downgrading it to a plain annotation or silently deleting it. The
missing state may appear in `Annotations`, and in `Labeling` when the missing
task is restored as active local state. Manual relink selects a replacement
output of the same owner format. Canonical relink validates the ASDF document's
task id, source identity, roster, annotation kind/count, label definitions, and
values. Legacy relink validates the NPY result plus adjacent
`<stem>.sf-labels.json` task id, source summary or explicit source-relink
confirmation, dtype, count, and label-code compatibility. Only after that
validation does the local record adopt the new path.

When the user imports or adds an annotation file, SpecForge may also use the
same validation rules to automatically reconnect a missing local labeling task
if the canonical document, or the legacy result and adjacent metadata, matches
that task record. Automatic relink is limited to the imported path and its
format-declared artifacts; SpecForge should not scan the broader filesystem for
moved label results.

In-place conversion is limited to writable integer categorical annotation
formats. Floating-point annotations and arbitrary string annotations must not be
converted into first-version sample labeling tasks. Source-aware CSV text
annotations are a narrow exception: an explicit promotion deterministically
maps the label text to categorical codes and creates an outputless task rather
than an in-place owner.

After an in-place legacy conversion, the task follows normal local sample
labeling task rules and the existing annotation file is the selected output
location. The converted task may expand its category set like any local task,
but writing back to the original output target is limited by that file's
writable format and integer dtype. If a new numeric code cannot be represented
safely, SpecForge should require the user to choose a different output target
before saving.
Activating an annotation as an existing local task must use the same identity
validation as annotation relationship display. A canonical annotation must
match the local task's owner format and output path plus its embedded task id,
source/roster identity, annotation kind, and sample count. A legacy annotation
must match the output path, sidecar `task_id`, result contract, and sample count.
A path-only match must not activate a local task or overwrite its output. If the
same path is owned by a different local task, activation remains blocked and the
annotation remains external.

When a temporary task is started locally inside SpecForge, it does not need the
in-place edit warning. It starts without an external output target and remains a
local recovery draft until the user selects `Save to...`. Selecting the output
location requires an `.asdf` path and promotes it to a formal local labeling
annotation only after one canonical document containing source identity, task
metadata, roster, and values is atomically written and reopened successfully.
The task retains the formal name established when the draft was created or
promoted; the chosen filename does not rename it. Successful publication then
permits a fresh temporary task for the same source collection. The selected
owner and pending overlay are checkpointed before publication; a failed first
publication restores the output-free draft immediately or through
maintenance/restart recovery if the compensating checkpoint is temporarily
unavailable. Locally created tasks may define and expand their own category
sets.

An active `legacy_npy_with_sidecar` owner exposes a separate explicit
`Migrate to ASDF...` action. This is not standalone-ASDF adoption and does not
rewrite the legacy result in place. The selected destination must be an `.asdf`
path, and migration proceeds as one owner-transfer transaction:

1. retain the active task-identity lease and both legacy artifact leases;
2. acquire the destination ASDF one-file lease;
3. synchronously checkpoint the legacy owner plus any sparse pending overlay;
4. build the canonical document from the current task identity, task name,
   label definitions, newest values, and the active source's canonical
   descriptor/roster;
5. atomically write and reopen the destination ASDF generation;
6. synchronously checkpoint the task with `canonical_asdf` ownership;
7. install the opened canonical snapshot and destination lease, then release
   the old NPY and sidecar leases.

The original `.npy` and `.sf-labels.json` bytes are never modified by this
operation. If destination lease acquisition, write-ahead checkpoint, ASDF
publication/reopen, or the durable owner switch fails, the old legacy owner
remains the trusted durable base and its sparse recovery overlay remains in the
local cache. A newly written ASDF whose owner-switch checkpoint fails is left as
an unowned Save-As artifact; it does not silently replace the legacy owner. The
user may retry the same explicit migration after the checkpoint failure is
resolved; the retry revalidates and republishes the destination before adopting
it. Path conflicts, write-ahead checkpoint failures, publication/reopen
failures, and owner-switch checkpoint failures are reported as stable semantic
error kinds mapped by the desktop UI to the selected language. Low-level I/O
details remain diagnostic data and are not displayed as untranslated UI text.

The first implementation should not allow two local sample labeling task records
for the same source collection to point at the same output path. If the user
selects an output path already used by another local task record, SpecForge
should report the conflict and offer low-risk choices such as activating the
existing task or choosing a different output path. Output ownership transfer,
task takeover, or automatic unbinding of the previous task is future work and
should not be part of the first implementation.

The Labeling window accepts only a sample label result through an active sample
labeling task. It should not display generic annotation results or inactive
annotation results, and it should not own or display the current sample index,
sample ordering, previous/next controls, or sample filters. It may show task-level
labeling progress such as labeled count, unlabeled count, pending count, and save
state. Keyboard shortcuts, clear-label behavior, auto-advance, autosave status,
and save-state indication belong only to the active editable task.

Activating a sample labeling task should show the label state for the current
sample from sample navigation. It must not silently restore or change the current
sample index. During an active labeling workflow, Labeling may request Navigation
to move to a specific sample or to an auto-advance target, but ordinary
previous/next movement belongs to Navigation. Navigation remains the owner of the
actual current sample index.

If the task record has a remembered labeling position that differs from
Navigation's current sample index, Labeling may show a one-time resume action
immediately after task activation. It may also show that action while the task is
active if the user uses Navigation, such as row-index or sample-name location, to
inspect another sample while the task's remembered labeling position still
points elsewhere. The user must explicitly confirm the resume action before
Labeling requests Navigation to jump. The action means resume labeling at the
remembered sample; it is not a restore of Navigation state and should not be
named Back or Restore. Direct locate actions in Navigation do not by themselves
update the task's remembered labeling position or dismiss the resume action.
If the remembered sample is outside the active sample navigation sequence, the
resume action must not bypass the sequence. In the first active-sequence
implementation, the resume action should be unavailable until the user clears or
changes the sample filter so the remembered sample is part of the active
sequence again.
Sequential move actions in Navigation do update the active task's remembered
labeling position, because moving one sample at a time is treated as part of the
active labeling workflow. Once the user labels or clears the current sample
through Labeling, the resume action should disappear and the task's remembered
labeling position should move to the current sample or to the auto-advance target
if auto-advance moves after the write. Moving past a sample without writing a
label is a skip; it advances the remembered labeling position but does not write
the unlabeled sentinel.

The active editable task should display all labels in its sample label set as
table rows. Each row should show the label name, numeric code, optional shortcut,
and edit/delete actions. Clicking the non-action area of a row assigns that label
to the current sample, and the row matching the current sample's label value
should be highlighted. A clear-label button should be available for writing the
unlabeled value. When the current sample is unlabeled, the active task should
display `Unlabeled`, no label row should be highlighted, and the clear-label
button should be disabled.

Shortcut uniqueness is scoped to one sample labeling task. Different tasks may
reuse the same shortcut because only one task is active at a time. Within the
active task, assigning a shortcut already used by another label requires the user
to press the same key a second time in the shortcut capture control. The editor
should show a small inline notice after the first press rather than opening a
modal. Saving the edit then binds the shortcut to the newly edited label and
leaves the previous label unbound.

Shortcut editing is available only after the user explicitly enters a label
row's edit mode. The normal label row remains a full-row assignment target and
must not expose an independently clickable shortcut editor. In edit mode the
shortcut control is a key capture control, not a text input. It displays letter
bindings as uppercase keyboard legends, accepts the next unmodified letter or
digit key, treats top-row and keypad digits as the same portable digit binding,
uses Backspace or Delete to clear the pending binding, and uses Escape to cancel
capture. The stored shortcut remains the canonical lowercase ASCII form.

Labeling shortcuts should work when the Labeling window or plot context is
active, but not while the user is editing text. A shortcut binding must not use
reserved UI operation keys or modifier-driven interactions used elsewhere in the
application, such as plot controls or spectral-line drag operations.
The first implementation should only allow unmodified letter and digit keys for
label shortcuts. Modifier combinations, navigation keys, whitespace keys, Enter,
Delete, and similar UI operation keys are out of scope for label bindings.
Shortcuts are optional for all labels and labeling actions. If a label or clear
operation has no shortcut, it remains available through its visible UI control.

`Ctrl+Z` undoes the most recent label assignment or clear operation for the
active task. A label write and its automatic sample advance form one undoable
workflow transaction: undo restores the previous label value and returns to the
affected sample even when that row is outside the current sample navigation
sequence under active sample filtering.
Repeated `Ctrl+Z` operations walk a bounded in-memory history for the current
source and task. That history is not persisted and is discarded when its source
identity or active task changes, when the task is deleted or deactivated, or when
label definitions change. Label-definition edits themselves are not undoable in
the first implementation.

Navigation, label assignment, and label undo should share one application-level
sample-workflow shortcut router. Focused Labeling and plot contexts may route
label assignment and undo; focused Navigation and plot contexts may route sample
movement. A hover-only plot context may route commands only when no other ImGui
window owns keyboard focus. Text editing, shortcut capture, open popups or modals,
and other focused panels retain ownership and suppress these workflow commands.
The router must still submit every applicable ImGui shortcut route on every
frame, including frames where command execution is suppressed, because
`ImGui::Shortcut()` also registers ownership for the next frame. It collects all
matches before selecting one command in `undo -> navigation -> label` priority;
top-row and keypad aliases are submitted separately rather than with
short-circuit evaluation. Blocked frames may register routes but never execute
workflow commands; active items and popups keep input ownership.

## Persistence

Sample labeling task records are user workflow configuration and should default
to local user storage under the SpecForge application data directory. A task
record owns the stable task id, task name, label set, mapping choices, workflow
settings, selected output path when one exists, internal autosave state, and an
optional remembered labeling position for the one-time resume action. It does
not own the source collection's current sample index. Like catalog user state,
this is local user state by default rather than an exported data product.
The remembered labeling position belongs to the sample labeling task and stores
only a sample index. It does not carry source identity, source path, sample name,
or relink information; the containing task record supplies the source collection
identity.
Task records are scoped to a source collection. SpecForge may keep multiple
source collections in its candidate/source list, but the activated source
collection determines which task records and sample annotations are shown in the
sample windows. Each source collection may retain multiple output-backed task
records but only one output-free temporary task record.

Sample navigation state is separate local user state scoped to source collection
identity. It persists the last shown sample index for the source collection
regardless of which sample labeling task is active. Activating or switching a
sample labeling task must not overwrite that navigation state or use the task's
remembered labeling position as the source collection's current sample index.
Loaded annotation paths are source collection session state, not sample
navigation cache state. They should be restored with the user's current source
collection session and kept separate from the last-index navigation cache.

A source collection identity for task-record lookup should include source name,
source fingerprint, and spectrum count. For NPY sources, the source name is the
file name; for folder collections, it is the folder name. The full directory path
may be useful for file access and display, but it is not required as the source
name.

For NPY source collections, the first source fingerprint should use file size,
file modification time, NPY dtype, and NPY shape. It should not hash the full
array content in the first implementation.

For folder source collections, the first source fingerprint should be derived
from the supported sample-file listing. The listing must be normalized and sorted
before fingerprinting so filesystem enumeration order does not change the
identity. Each entry should include relative file name, file size, and file
modification time. The first implementation should not read full file contents
for this fingerprint.

If multiple loaded source collections share the same source name, task-record
matching still uses the full source collection identity. The UI may show a parent
directory or short path to disambiguate same-name sources, but that display text
does not become the source name.

Relink requires the source spectrum count to match the task record's spectrum
count. If only the source name changed while the source fingerprint and spectrum
count still match, SpecForge may relink automatically and notify the user. If the
source fingerprint changed while the source name and spectrum count still match,
SpecForge should treat the loaded source as different by default and only relink
after explicit user confirmation. If both source name and source fingerprint
changed while spectrum count still matches, SpecForge should also treat the
loaded source as different by default and only relink after explicit user
confirmation. After relink, SpecForge should update the task record to the new
source collection identity so the user is not prompted for the same source change
every time. Relink does not reinterpret remembered labeling positions by sample
name, source path, or content matching. A remembered labeling position remains
the same sample index inside the relinked source collection.

Local task records should be persisted in a versioned JSON cache under the
user's local SpecForge application data directory, consistent with catalog user
state persistence. Writes should be debounced, flushed on normal shutdown, and
retried after non-blocking save failures.

Each `SampleLabelingTask` persists its formal output ownership as an explicit
path-and-format pair. State-cache schema 4 writes this as `output.path` plus
`output.format`; temporary drafts use `none`, `Save to...` tasks and existing
canonical document owners use `canonical_asdf`, and existing or explicitly
adopted legacy-format owners use `legacy_npy_with_sidecar`. Explicit promotion
of an existing NPY annotation is one such adoption. Schema 4 also stores the
canonical task metadata (`created_at`, `modified_at`, `origin`, optional
`description`, and `authors`). Schema 4 author records preserve required `name`
plus optional `identifier` and `email`; absent optional fields remain absent.
Cache schemas 1 through 3 are unsupported and
ignored rather than migrated because they cannot supply both a strict UUID v4
identity and trustworthy canonical creation metadata. Formal records do not
duplicate the full values array in this cache; they retain only sparse pending
values plus local session/recovery state. The write-ahead
`initial_publication_pending` phase is
durable only until canonical creation is adopted or reconciled back to a true
temporary draft. Legacy-to-ASDF migration does not use that temporary-only
phase: its pre-publication checkpoint continues to name the legacy owner, and a
second synchronous checkpoint changes the owner only after ASDF publication and
reopen succeed. Output leases, conflict detection, and recovery projection
checks use the task's stored format rather than guessing from its filename
extension.

The schema reader accepts syntactically valid future `origin.kind` tokens so an
existing document can be opened and preserved forward-compatibly. A current
SpecForge writer may create only `manual` or `annotation_promotion` origins.
Promotion provenance stores a portable annotation basename, never an absolute
path or a name containing directory components.

## Annotation I/O

The sample-labeling model should treat annotation storage formats as adapters.
Core labeling, navigation, sample filtering, and annotation display should work with a
loaded sample annotation result: one value per spectrum sample, with a known
value kind such as categorical integer, categorical string, or continuous
floating point.

### Canonical schema 2.0 identity and provenance

The following names identify different things and must not be used
interchangeably:

| Name | Meaning | Mutation rule |
| --- | --- | --- |
| Task ID | The canonical identity of one labeling task. It is stored at `labeling_task.id`. | Immutable. A rename, Save As, export, or output relink does not create a new ID. |
| Task name | User-authored UTF-8 display data stored at `labeling_task.name`. | Persistently editable. Changing it advances `modified_at` but does not change the task ID or any path. |
| Output filename | The user-selected filesystem name of the canonical `.asdf` owner. | Independent of both task ID and task name. The dialog may suggest a sanitized filename from the task name, but the final name is not canonical metadata and later task renames do not rename it. |
| SpecForge schema version | The semantic contract selected by the YAML `schema_version`. | Current writers emit exactly `2.0.0`; this is not an ASDF implementation version. |
| SpecForge build source | Generation provenance stored in `specforge_build`. | A current writer or rewrite stamps its own compiled build identity. `head` requires one full 40-character lowercase hexadecimal revision; `working_tree` requires `source_revision` to be absent. |
| ASDF file-format version | The container framing version in `#ASDF 1.0.0`. | Independently versioned by ASDF. It does not mean SpecForge schema 1.0. |
| ASDF Standard version | The tag/schema vocabulary declared by `#ASDF_STANDARD 1.5.0`. | Independently versioned by ASDF. The `!core/asdf-1.1.0` root tag is likewise an ASDF core tag, not the SpecForge schema version. |

GitHub issue #82 is a bounded self-description patch incorporated into
`schema_version: 2.0.0`. It does not introduce schema `2.1.0`.

Task IDs use only the canonical UUID v4 text form: 36 lowercase ASCII
characters in `8-4-4-4-12` groups, version nibble `4`, and RFC variant nibble
`8`, `9`, `a`, or `b`. Uppercase hex, braces, `urn:uuid:` prefixes, missing
hyphens, other UUID versions, and other variants are rejected even if a more
permissive UUID parser could interpret them.

Creating a manual draft obtains a fresh UUID from the secure generator and sets
`created_at == modified_at` with `origin.kind: manual`. Promoting a plain CSV or
NPY annotation also creates a fresh UUID; it does not inherit a sidecar task ID
or derive an ID from a name. Canonical ASDF adoption instead preserves the
embedded ID and all canonical metadata exactly. A generated collision is
resolved by generating another UUID, never by appending `-2`, `-copy`, or a
similar non-UUID suffix.

A task name must be valid UTF-8 and contain at least one code point that is not
Unicode whitespace. SpecForge preserves the submitted text exactly: it does not
trim it and does not perform Unicode normalization. The task name is not a
filename, source identity, annotation display-name override, or temporary-task
status marker.

Canonical timestamps are millisecond `sys_time` values in memory and exactly
24 bytes on the wire:

```text
YYYY-MM-DDTHH:MM:SS.sssZ
```

The four-digit year range is `0000` through `9999`. The parser validates the
actual proleptic-Gregorian date, including leap years, and requires hour
`00..23`, minute and second `00..59`, exactly three millisecond digits, and a
literal UTC `Z`. It does not accept offsets, a missing fractional part, extra
fractional precision, or leap-second `60`. `created_at` is immutable after task
creation, `modified_at` may not precede it, and replacement generations may not
move `modified_at` backwards.

The following are canonical semantic mutations and advance `modified_at` only
when they actually change data:

- assigning or clearing a sample label value;
- changing the task name;
- adding, editing, or removing a label definition, including a confirmed code
  migration and its affected values;
- any future accepted edit to `description` or `authors`.

No-op assignments, clears, renames, and label-definition saves preserve the
timestamp. Task activation/deactivation, output-path selection, export,
auto-advance and skip-labeled settings, remembered navigation position,
pending/save-failure state, and retry scheduling are local workflow operations,
not canonical semantic mutations.

The mutation clock is sampled when the semantic mutation is accepted. If
atomic publication fails, the durable document keeps its old value and old
`modified_at`, while the pending replacement retains the mutation timestamp.
A retry republishes that retained timestamp rather than sampling retry time. A
later semantic mutation may advance the pending timestamp; a clock value that
is not later never makes it regress. A rewrite with no semantic mutation must
preserve `modified_at`.

Current writers create only these origin kinds:

- `manual`: `origin.annotation` is absent.
- `annotation_promotion`: `origin.annotation` is required. Its `name` is a
  portable basename, never an absolute path, drive-prefixed name, `.`/`..`, or
  a string containing `/` or `\\`; `format` is `csv` or `npy`; and
  `fingerprint`, when present, is `sha256:` followed by 64 lowercase hex
  digits.

For CSV and NPY promotion, the annotation fingerprint covers every raw byte of
the imported artifact, including the CSV encoding/line endings or the NPY
header and payload. It does not hash decoded values, a sidecar, the path string,
the source-collection fingerprint, or a projected label set. Parsing and
SHA-256 use the same opened file generation; path replacement cannot combine
values from one generation with a fingerprint from another. Fingerprints remain
optional when the already-open artifact cannot be rewound or hashed.

Readers accept a syntactically valid future lowercase `origin.kind` token so an
unchanged future origin can survive a preserving rewrite. A fresh current
writer cannot introduce such a token, and an existing origin is immutable.
Future origins do not relax the portable-basename, supported annotation-format,
or fingerprint rules when an annotation origin is present.

`description` and `authors` are optional canonical fields, but there is no UI
for editing them. An unset description and an empty author list are omitted,
not materialized as `null` or `authors: []`. Each author contains a required
non-whitespace UTF-8 `name`, an optional non-whitespace UTF-8 `identifier`, and
an optional non-whitespace UTF-8 `email`. The email is portable user-supplied
contact metadata: SpecForge does not apply RFC address parsing, DNS validation,
normalization, or case rewriting, and never infers it from Git, the operating
system, or other machine-local state. Role, organization, and additional author
ID fields are not part of schema 2.0.

This is a complete schema 2.0 YAML-tree example. The two ndarray descriptors
refer to the roster and values binary blocks that follow the YAML metadata in
the ASDF container:

```yaml
#ASDF 1.0.0
#ASDF_STANDARD 1.5.0
%YAML 1.1
%TAG ! tag:stsci.edu:asdf/
--- !core/asdf-1.1.0
asdf_library: !core/software-1.0.0
  name: SpecForge
  version: 0.8.0
specforge_build:
  source_mode: head
  source_revision: "0123456789abcdef0123456789abcdef01234567"
format_kind: "specforge.sample_labeling"
schema_version: "2.0.0"
source_collection:
  identity: "source:example"
  source_kind: "npy"
  name: "spectra.npy"
  fingerprint: "source-fingerprint-v1"
  sample_count: 2
sample_roster:
  identity_kind: "explicit_names"
  names: !core/ndarray-1.0.0
    source: 0
    datatype: [ucs4, 8]
    byteorder: little
    shape: [2]
annotation:
  kind: "categorical_integer"
  alignment:
    mode: "by_index"
    target: "sample_roster"
  values: !core/ndarray-1.0.0
    source: 1
    datatype: int32
    byteorder: little
    shape: [2]
  missing:
    semantic: "unlabeled"
    value: -1
labeling_task:
  id: "123e4567-e89b-42d3-a456-426614174000"
  name: "银河候选复核 😀"
  created_at: "2026-08-30T08:00:00.000Z"
  modified_at: "2026-08-30T08:15:42.125Z"
  origin:
    kind: "annotation_promotion"
    annotation:
      name: "initial-labels.csv"
      format: "csv"
      fingerprint: "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  description: "Review promoted candidate labels."
  authors:
    - name: "Alice"
      identifier: "https://orcid.org/0000-0001-2345-6789"
      email: "alice@example.org"
  labels:
    - code: 0
      name: "非目标"
      shortcut: "n"
    - code: 1
      name: "目标"
      shortcut: "t"
...
```

```text
annotation.values[i] annotates canonical sample i.

explicit_names:
    canonical sample i = sample_roster.names[i]

source_index:
    canonical sample i = source index i
```

For `sample_roster.identity_kind: source_index`, `sample_roster.names` is
absent and the values ndarray uses block source `0`. In both roster forms the
required `annotation.alignment` map is exactly `mode: by_index` and
`target: sample_roster`: `values[i]` belongs to `sample_roster.names[i]` for an
explicit roster and to source index `i` for a source-index roster. Missing
alignment fields, non-string values, or any other mode/target are semantic
errors; schema 2.0 has no compatibility path for an alignment-less document.
The values contract is one-dimensional little-endian signed `int32`, its length
is exactly `source_collection.sample_count`, and `-1` is the reserved unlabeled
sentinel. Schema 2.0 has no `annotation.name`; the canonical task name lives
only at `labeling_task.name`, while `origin.annotation.name` is provenance for
the promoted input artifact and is not a second task name.

`asdf_library` identifies the producer as `SpecForge` version `0.8.0`.
`specforge_build` records only the source state compiled into that producer:
clean `head` builds include the complete revision, while `working_tree` builds
omit `source_revision` entirely. The field is generation provenance rather
than user-authored task content. Fresh writes, values-only rewrites, and full
metadata rewrites all stamp the current binary's identity, so a rewrite does
not retain the producer identity of the generation it replaces.

Preserving rewrites start from a validated durable generation and retain
unknown mapping entries in the existing YAML tree, including nested source,
roster, annotation/descriptor, task, origin/annotation-origin, build-source,
and label maps. Known build-source fields are replaced with the current
binary's identity while unknown entries inside that map are retained.
Label-local entries are matched by stable label code. This is deliberately not
a general forward-compatibility promise: SpecForge does not preserve arbitrary
extra binary blocks, unknown sequence-item schemas, replaced known fields,
metadata bytes/padding, or the old block index/trailer. Removed labels lose
their label-local unknown fields, and the canonical `authors` sequence is
rebuilt from the supported author model. Fresh writes have no unknown metadata
to preserve. Both full and values-only rewrites require the durable base to be
the same source/roster/annotation/task identity; values-only rewrites may change
only values and `modified_at` and reuse only the encoded roster block.

SpecForge labeling schema `1.0.0` documents are unsupported and are not
migrated to `2.0.0`. Likewise, labeling state-cache schemas 1 through 3 are
ignored rather than migrated; current local task records use cache schema 4.
These version boundaries are unrelated to the ASDF file-format `1.0.0` header.

Schema 2.0 intentionally has no task-copy/parent lineage, persisted edit
history, document revision counter, incremental patch log, or workflow-status
field. Draft/formal ownership, active task, auto-advance, navigation position,
pending/retry/save state, output path, and UI state remain local workflow data.
The controller's in-memory view revision is not a canonical document revision.

Format-specific details belong behind annotation I/O. Canonical sample-labeling
documents use SpecForge schema `2.0.0`, serialized with ASDF file format `1.0.0`
and ASDF Standard `1.5.0`; this ASDF adapter is the writer for newly formalized
tasks and the source-aware annotation/task-hydration adapter. New canonical
owners require an `.asdf` path before their write-ahead checkpoint is committed.
NPY remains a read adapter and the persistence writer
for existing or explicitly adopted legacy-format owners, including tasks created
by promoting an existing NPY annotation. That legacy owner's lease identity
always protects both the selected result path and the adjacent
`<stem>.sf-labels.json`. NPY is also available through a separate one-shot value
export operation; that operation is not an output owner and does not acquire a
task-identity lease, write a metadata sidecar, change the autosave target, or
clear the task's pending overlay. It does acquire the legacy artifact set's
stable and physical identity locks for the duration of the single write. While
that transient guard is held, export rejects paths owned in the local task
cache, aliases of those paths, paths protected by another instance, and NPY
files with an existing adjacent metadata sidecar. The guard is released before
the command returns and never becomes task state. Output artifact
ownership is declared by format rather than inferred from a filename extension:
canonical ASDF owns one document, while legacy NPY owns the `.npy` result and
its `.sf-labels.json` sidecar. Explicit migration creates a new canonical ASDF
owner from the active legacy task without routing the destination through the
legacy writer or changing either source artifact.
CSV is also available as a one-shot interchange export through the domain and
controller boundary. Folder sources write `filename,label`; every non-folder
source writes `sample,label`. The first column uses canonical sample names when
present and otherwise contains the zero-based canonical source-row index. Label
text uses the stable non-localized export serialization, including the fixed
`unlabeled` sentinel and reversible escaping for colliding label names. CSV
writes are atomic and transiently guard only the selected `.csv` target; they do
not adopt an output owner, create a sidecar, or change task/save/navigation
state. Source-aware CSV ingestion accepts the same `filename,label` and
`sample,label` schemas, maps identities back to the canonical source roster,
and loads the decoded label column as a plain text annotation. Duplicate,
unknown, or missing identities are rejected. A user may promote such a CSV
attachment into an outputless editable task. Missing cells map to the canonical
unlabeled code `-1`; distinct non-missing text values are sorted by their exact
UTF-8 bytes and assigned stable codes `0..N-1`, so a labeled literal
`unlabeled` remains distinct from the missing sentinel. The promotion records a
fresh UUID v4 plus portable `annotation_promotion` provenance containing the CSV
display name, `format: csv`, and, when available, a SHA-256 fingerprint of the
complete imported bytes. It never overwrites or adopts the source CSV as an
output owner. The task remains a draft until the user explicitly selects a
canonical ASDF output path.

The ASDF codec remains a caller-owned stream component. A separate domain
document store owns source-aware open, the immutable durable roster/base
snapshot, and atomic replacement. Every canonical publication supplies a
complete replacement document, including its intended `modified_at`; the store
does not synthesize metadata from a values span alone. For a pure label-value
mutation, the controller uses the timestamped values fast path. It rebuilds the
YAML metadata from the durable metadata tree with the unknown-field-preserving
builder, re-emitting the new `modified_at` and values descriptor. It reuses only
the already encoded roster block, re-encodes the values block, and recomputes
the resulting block layout and offsets. It emits a new block index for those
offsets instead of copying the input index or its stale layout padding. In
particular, the fast path does not byte-copy a combined metadata-and-roster
prefix. A successful values
publication refreshes the open snapshot's durable base, so later value saves
can repeat that path without reopening.

Metadata mutations use the full-document rewrite. That path merges edited known
SpecForge schema `2.0.0` fields into the validated generation and preserves
forward-compatible unknown mapping entries; label-local unknown entries are
associated by stable label code. A full rewrite of an existing path must obtain
such a durable base and fail safely when it cannot, rather than reconstructing
only the old reader's object model and silently dropping future fields. It must
also retain the opened source/roster identity, annotation kind, and stable task
id so opaque metadata is not transplanted into another logical document. A
successful full rewrite invalidates the old snapshot and requires reopening the
new generation. Both the codec durable-base check and the document-store seam
treat `created_at` and `origin` as immutable, and require the replacement
`modified_at` to be no earlier than the durable generation. A
forward-compatible unknown origin may therefore survive a rewrite unchanged,
but a rewrite cannot introduce or alter it.
Controller leases, recovery state, retry policy, and UI activation remain above
that store and are not codec responsibilities.

Adopting a standalone canonical document acquires its stable task-identity
lease and ASDF one-file output lease before reopening the writable store. The
read-only attachment generation supplies only the user-selected path and
expected embedded task id; it is not trusted as the durable base. A malformed,
changed, source/roster-incompatible, or task-id-mismatched generation fails
closed without creating a local owner or modifying the document. Successful
adoption stores only structural/session state locally and retains the reopened
snapshot in memory, so the first actual value or metadata edit enters the same
autosave pipeline as every other canonical owner.

Activating a cached canonical owner first acquires the ASDF document's one-file
output lease, then opens the document store against the controller's owned
canonical source descriptor: base identity, source kind, source name,
fingerprint, sample count, and canonical sample names. The opened document is
authoritative for task id/name, labels, and base values. Local auto-advance,
remembered position, and sparse pending overlays are applied afterward. The
controller keeps the resulting `SampleLabelingAsdfOpenSnapshot` as the active
durable generation only in memory; it is never serialized into the state cache.
Malformed documents, source/roster mismatches, and task-id mismatches fail the
activation without falling back to legacy NPY hydration. Assigning or clearing
values first commits the newest sparse overlay to the local cache, then builds a
complete replacement from the opened snapshot plus the authoritative task
values and canonical metadata. The resulting values-plus-`modified_at`
publication uses the values fast path; it preserves the supported unknown
mappings and the encoded roster block while replacing the values generation.
Only successful atomic replacement advances the in-memory snapshot and clears
the corresponding sparse overlay. Failure leaves the prior ASDF value,
`modified_at`, and whole generation trusted on disk, retains the overlay, and
schedules an owner-aware retry using the mutation's original timestamp rather
than a retry-time clock value.

A successful foreground publication or maintenance retry synchronizes the
attached annotation generation before the active snapshot can be released and
invalidates filter/sort projections that may have consumed the older generation.
Canonical retries run only while their source is active with its matching
descriptor; switching sources parks the durable overlay and that source's next
activation re-arms publication. Task renames and label add/edit/remove
operations, including shortcuts and used-code rewrites, take the full-document
path, so known metadata, values, and forward-compatible unknown metadata remain
one canonical generation. The old snapshot is invalid as soon as a full
replacement reaches disk; the mutation is successful only after the replacement
reopens and its known generation matches the intended document. The requested
reopen source descriptor is checked against that intended document before
replacement, so an incompatible public-store call cannot alter the durable
owner. If reopen fails, the local overlay remains pending, the stale snapshot is
discarded, and retry first opens the current durable file before rewriting it.
Creating a new ASDF owner uses a full canonical document write followed by
source-aware reopen and generation validation. The controller adopts the formal
owner only after both steps and output-lease refresh succeed.

Annotation I/O belongs in a domain or service boundary, not in UI code. UI
surfaces should consume loaded sample annotation results, task records, and save
state without parsing storage formats or reimplementing dtype, shape, or write
capability rules.

Sample label results are labeling output. They should be written to an explicit
output location chosen by the user. SpecForge should recommend choosing an
external output file, but should not require one before labeling starts. Output
formats should be compact and practical for C++ streaming reads and writes;
verbose JSON is appropriate for drafts and recovery metadata, not as the
preferred export format for large label arrays.
New formal labeling tasks use canonical ASDF so source identity, task metadata,
roster, and the one-dimensional label values share one atomic generation.
Numeric tasks write stable label codes, and the sample label set owns the
interpretation from numeric code to user-facing label. Unlabeled samples use
`-1`; the SpecForge sample-labeling schema `2.0.0` value array is signed `int32`.
Save and autosave continue to publish through the task's declared owner format.
`Export Labels` writes the current task values in canonical source-roster order
using the adjacent NPY/CSV UI selection without changing that owner. NPY sources
recommend NPY and folder sources recommend CSV when a source context becomes
active; a manual override remains selected until the source identity changes.
The save dialog filter, default extension, and missing-extension completion
follow the current selection. This lightweight preference does not enter task
canonical data. The exported file does not become an autosave target, is not
attached automatically, and NPY interchange export has no canonical
`.sf-labels.json` sidecar.
`*_y.npy` is not the default output meaning; it is only a special auto-loaded
companion convention for existing labels in the NPY adapter.

When a legacy NPY owner is written to its explicit output location, SpecForge
also writes portable sample label result metadata beside the compact `.npy`
output. This metadata is a data-contract sidecar, not a copy of the local sample
labeling task record. It includes a format kind, schema
version, referenced result file, stable task id, expected value count, expected
dtype, unlabeled sentinel, task name, label code/name/shortcut entries, and the
source collection identity summary when available. The referenced result file
should be stored as a path relative to the metadata file's directory, normally
just the result file name, so the `.npy` plus metadata pair remains portable
when moved together. The source collection identity summary should reuse the
same source collection identity fields used by SpecForge's source/file
management flow, such as source name, source fingerprint, context fingerprint,
and spectrum count. It must not store the label value array, pending values,
local autosave draft, active task state, window visibility, sample-filter state, save
retry state, or a local absolute source path.
For an output file named `<stem>.npy`, the adjacent metadata file should be
named `<stem>.sf-labels.json`.

For a legacy NPY owner, `autosaved to output` means the compact label result and
its portable metadata have both been saved successfully. If the label result is
saved but metadata save fails, the task remains pending or failed. Metadata-only
changes rewrite that sidecar even when the label value array is unchanged. For a
canonical ASDF owner, `autosaved to output` means the complete intended values
or metadata generation was atomically replaced and, for metadata-changing
writes, reopened successfully.

When loading an existing numeric label result, SpecForge should use adjacent
sample label result metadata when it matches the label result file, value count,
and dtype. If matching metadata is absent, SpecForge may still load and display
the raw numeric label values, but it should not invent label names or shortcuts.
If adjacent metadata exists but does not match the label result, SpecForge
should warn and fall back to raw numeric label values instead of applying stale
or unrelated mappings. The first implementation should not allow the user to
force-bind mismatched metadata to a label result. The user may choose the correct
label result and matching metadata pair, but overriding metadata binding is
future work because an incorrect binding can silently change the meaning of
stored label codes.

The first labeling workflow is single-label classification. A label array has
one element per spectrum sample; each element is either one label value or the
task's unlabeled sentinel. Multi-label, multi-task, confidence, and per-sample
notes are out of scope for the first export path.
Clearing a label is a first-class operation that writes the task's unlabeled
sentinel for the current sample. It is not represented as a real label in the
sample label set and must not consume a stable label code.

String and floating-point arrays may be loaded and displayed as existing
read-only annotations. Manual classification output in the first editable
implementation writes numeric label codes only. String task editing and
compressed label result formats can be added later as explicit export options.

The normal temporary sample labeling draft slot for a source collection protects
in-progress work before it has been written to a selected output location. It
should be saved automatically as part of the local task record and kept separate
from formal sample label results. Historical, conflicting, or damaged cache
state may expose multiple recovery drafts; those drafts remain individually
addressable in the recovery list.

By default, manual labeling should autosave to the local draft. Once the user
selects an explicit output location, label changes should autosave to that
output location. There is no separate live-save toggle in the first
implementation, and editable labeling should not use a document-style
Save/Cancel workflow as its primary persistence model. Closing the task,
switching sources, or exiting the application should not show blocking save
reminders or retention prompts solely because the task is draft-only. The save
state indicator is responsible for making the current persistence state visible.

After an output location is selected, the external output becomes the source of
truth for the task's label result. The local task record should continue to save
task metadata, workflow settings, output path, optional remembered labeling
position, and other UI recovery state, but should not maintain a competing
second label result for the same task.

The first implementation should not proactively delete old drafts. Drafts for
the same labeling context may be overwritten by newer autosaves, while unrelated
drafts remain available in the temporary-draft recovery list until the user
chooses `Recover`, `Keep`, or `Delete`. `Recover` and `Delete` use the lease-safe
controller operations. `Keep` is only a local acknowledgement for the current
UI session: it remains when the workflow presentation is reset or another
source is selected, but is cleared if that observed recovery row disappears or
changes identity/content, and it is not restored after the UI session ends. It
does not acquire or release a lease, modify the draft, or persist the
acknowledgement. Each recovery row exposes the source and task identity,
recovery status, progress, and save state; recovery operations must not silently
overwrite a formal labeling task.

When the user starts or returns to an editable task, SpecForge should use the
local task record to restore available internal autosave state. The restored
state should clearly show that it came from internal autosave. The draft does
not need a separate "clean-exit" or "crash-exit" marker.

If the user selects an external output path for a task, the local task record
stores that path. The associated internal draft should no longer be exposed as a
separate auto-discovered file, auto-loaded label result, or load option when the
user manually loads external label files. External label result metadata may
carry a stable task id, but that task id does not restore local workflow
ownership unless it also matches a local task record for the active source
collection and output path.

A sample labeling context should include the source collection identity, the
sample labeling task id, and the output path when one has been selected. The
initial source fingerprint may use file size and modification time for files, or
a directory listing summary for folder collections; a later implementation may
upgrade it to a content hash when the extra cost is justified.

The recovery path must not silently overwrite an explicit sample label result.
Because a bare external label result path does not prove task ownership,
SpecForge should not infer draft ownership from a label file path alone. Adjacent
sample label result metadata may carry a stable task id, but it only identifies a
local task when it also matches a local task record for the active source
collection and output path.

## Save State Indicator

The labeling UI should show where the current label changes have been written:

- internal autosave draft only;
- autosaved to the selected output location;
- pending changes waiting for the next autosave, including a count such as
  `pending: 5`;
- save failed and will be retried, with the pending count preserved.

The pending count is the number of distinct samples whose latest label value has
not yet been successfully persisted. Multiple edits to the same sample count as
one pending sample. This indicator should describe the current write state
without treating every restored draft as proof of a crash.
