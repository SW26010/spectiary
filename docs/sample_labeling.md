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
are not accidentally treated as classification labels. The first implementation
does not convert string annotations into editable sample labeling tasks, but a
future implementation may support that through an explicit user action.
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

## Manual Labeling

Manual labeling normally starts with the source collection's single temporary
sample labeling draft slot. It has a stable task id, the stable persisted/domain
name `Temporary labeling task` (localized by the UI according to the selected
language), one sample label set, and workflow choices such as auto-advance
behavior. Historical, conflicting, or damaged cache state may still expose
more than one recovery draft; those rows are handled through the identity-bearing
recovery list. Labeling does not ask for a task name when this draft is created
or resumed.

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

The first row of `Labeling` should use one compact task selector rather than
separate task-name, create/resume, and annotation-drop rows. Its permanent first
item is `New labeling task` when no temporary draft exists, `Resume labeling
draft` when exactly one draft is paused, and the selected temporary draft while
that draft is active. When multiple drafts exist, recovery rows carrying their
source/task identities are the only activation route. Remaining items are the
source collection's formal local labeling annotations. The selector itself
remains the drag target for compatible rows
from `Annotations`. Pause and delete controls belong immediately to the
selector's right on the same row. Switching through the selector or drag target
must honor the same pending/failed output-save guard as explicit close.
The temporary-draft recovery list is rendered below this selector row so the
primary task control remains the first row of `Labeling`, including when the
list contains multiple drafts or the dock is narrow.

Pausing or closing a sample labeling task should deactivate the active sample
labeling task rather than hide the `Labeling` window or delete the task record.
Deactivation leaves the task record, label set, shortcuts, selected output path, remembered
position, and sample label result intact. After deactivation, `Labeling` has no
active task. If its temporary task still exists, the primary action is to resume
that task rather than create another one. A formal categorical annotation may
also be activated from the `Annotations` window.
In the first implementation, closing a formal output-bound task should be
disabled while it has pending or failed output saves, including pending or
failed metadata sidecar saves. The user must wait for autosave to complete or
fix the output save problem before deactivating that formal task. A temporary
task whose first `Save to...` attempt fails remains output-free and recoverable:
it keeps its temporary identity and error message and may be paused, deleted, or
saved to the same or a different target. Tasks in the internal-autosave-draft
state may also be closed because their current recovery state is owned by the
local task record. A later implementation may allow non-blocking close with
background retry and explicit pending-task surfacing.

Deleting a sample labeling task is a separate explicit operation from closing
or deactivating it. Delete removes the local task record and its internal draft.
It must not delete the task's selected output `.npy` file or adjacent portable metadata sidecar. If the
same output file is still loaded as an annotation later, SpecForge should treat
it according to the normal plain/external/local matching rules rather than
silently resurrecting the deleted local task record. Delete should be disabled
while the task has pending or failed output saves, matching close/deactivate.

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
task record, matching adjacent sample label result metadata such as
`<stem>.sf-labels.json`, or a loaded annotation relationship that marks the
values as an external or local sample label result.
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
rows default to the output filename stem recorded as the formal task name. The
user may edit the display name in place from the `Annotations` table. Clearing
the edited display name removes the custom local override and restores the
default display name. A custom display
name affects the `Annotations` row, the drag preview text, and the corresponding
entries shown in `Sample Filters` and `Sample Sorting`; hover text should still
reveal the original annotation path or file name so the source remains
inspectable.

Formal sample labeling task names and annotation display names are separate. The
formal task name is derived from the filename chosen through `Save to...` and is
written to portable metadata. Editing the annotation display name must not
rename the task, output file, or portable metadata.

After editable sample labeling exists, categorical annotation rows should expose
a small drag affordance so the user can drag that annotation to the Labeling
window and make it the active sample labeling task.

Dragging a categorical annotation from the sample annotation view should convert
that annotation into a local sample labeling task before it becomes editable.
The converted task uses the annotation file as its output target, so SpecForge
must warn that edits will modify the original data in place. The warning should
say this is appropriate only when the user intentionally wants to edit that data
or when the file represents an unfinished labeling task being restored, and it
should recommend backing up the original data first.

Adjacent sample label result metadata does not by itself make an annotation file
the current user's own labeling task. If a categorical annotation has matching
`<stem>.sf-labels.json` metadata but no matching local sample labeling task
record for the active source collection and output path, SpecForge should treat
it as an external label result and still show the in-place edit warning before
editing it. If the matching local task record exists, SpecForge may restore or
activate that task without the in-place edit warning because the user has
already established that file as the task's output target.

The annotation view should distinguish loaded per-sample data by workflow
relationship:

- Plain annotation: no matching sample label result metadata is available.
- External label result: matching sample label result metadata is available, but
  no matching local sample labeling task record exists.
- Local labeling task: matching sample label result metadata is available and a
  matching local sample labeling task record exists.

Plain annotations should display their current raw annotation value. Writable
integer categorical plain annotations may be dragged into `Labeling`, but doing
so follows the normal conversion flow and requires the in-place edit warning.
String and floating-point plain annotations are read-only in the first editable
labeling implementation and should not expose a first-version labeling drag
action.

External label results should display the mapped label value from their adjacent
sample label result metadata and be visibly marked as external. They may be
dragged into `Labeling`, but doing so requires the in-place edit warning. If the
user confirms, SpecForge creates a local task record whose output path points at
that label result file.

Local labeling tasks should display the mapped label value and be visibly marked
as local. If the task's output file or metadata sidecar is missing, the row
should show the missing-output state and expose relink. A local labeling task may
be clicked or dragged into `Labeling` to activate it without the in-place edit
warning. If another sample labeling task is already active, the user must close
or deactivate the current task before activating the local task from
`Annotations`.

A local sample labeling task match requires the active source collection
identity to match, the local task record's normalized output path to point to the
annotation's label result file, the sidecar `task_id` to match the local task
record's stable task id, and the sample/value count to match. Moving or renaming
the output `.npy`, moving or renaming the adjacent metadata file, clearing local
user state, copying another user's `.npy` plus metadata, or relinking a source
collection may prevent SpecForge from recognizing a file as a local labeling
task until the user explicitly reconnects it.

If a local sample labeling task record still exists but its selected output
file, adjacent metadata file, or both are missing, SpecForge should keep the task
record visible as a local labeling task with a missing-output state rather than
downgrading it to a plain annotation or silently deleting it. The missing state
may appear in the `Annotations` window for that local labeling task row, and in
the `Labeling` window when the missing task is the active task restored from
local state. The user should be able to manually relink the task by selecting the
new label result file. Relink should then look for the adjacent
`<stem>.sf-labels.json` metadata, validate the task id, source identity summary
or explicit source relink confirmation, value count, dtype, and label-code
compatibility, then update the local task record's output path.

When the user imports or adds an annotation file, SpecForge may also use the
same validation rules to automatically reconnect a missing local labeling task
if the annotation file and adjacent metadata match that task record. This
automatic relink should be limited to the imported annotation path and its
adjacent metadata; SpecForge should not scan the broader filesystem looking for
moved label results.

Only writable first-implementation integer categorical annotation formats may be
converted this way. Floating-point and string annotations must not be converted
into first-version sample labeling tasks. String annotation conversion remains a
future extension point and must still require an explicit user action.

After conversion, the task follows normal local sample labeling task rules. The
existing annotation file is simply the selected output location for that task.
The converted task may expand its category set like any local task, but writing
back to the original output target is limited by that file's writable format and
integer dtype. If a new numeric code cannot be represented safely, SpecForge
should require the user to choose a different output target before saving.
Activating an annotation as an existing local task must use the same identity
validation as annotation relationship display. A metadata-backed annotation
must match the local task's output path, sidecar `task_id`, and sample count; a
path-only match must not activate a local task or overwrite its output. If the
same path is owned by a different local task, activation remains blocked and the
annotation remains external.

When a temporary task is started locally inside SpecForge, it does not need the
in-place edit warning. It starts without an external output target and remains a
local recovery draft until the user selects `Save to...`. Selecting the output
location promotes it to a formal local labeling annotation only after both the
label array and portable metadata sidecar are written successfully, derives its
name from the chosen filename stem, and permits a fresh temporary task for the
same source collection. A failed first write does not bind the path or rename
the task. Locally created tasks may define and expand their own category sets.

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

## Annotation I/O

The sample-labeling model should treat annotation storage formats as adapters.
Core labeling, navigation, sample filtering, and annotation display should work with a
loaded sample annotation result: one value per spectrum sample, with a known
value kind such as categorical integer, categorical string, or continuous
floating point.

Format-specific details belong behind annotation I/O. NPY is the only
first-implementation adapter, not the domain model. Future adapters such as CSV
can be added if they can produce or consume the same per-sample annotation result
shape and validate that the value count matches the source collection's spectrum
count.
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
The first export adapter should prioritize a one-dimensional `.npy` label array
at the user's chosen output path, with one label value per spectrum sample.
Numeric tasks write stable label codes, and the sample label set owns the
interpretation from numeric code to user-facing label. Unlabeled samples in
numeric tasks should use `-1`, so numeric label outputs should use a signed
integer dtype. The first implementation should write new numeric label outputs
as `int32`.
`*_y.npy` is not the default output meaning; it is only a special auto-loaded
companion convention for existing labels in the NPY adapter.

When a numeric sample label result is written to an explicit output location,
SpecForge should also write portable sample label result metadata beside the
compact `.npy` output. This metadata is a data-contract sidecar, not a copy of
the local sample labeling task record. It should include a format kind, schema
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

After an output location is selected, `autosaved to output` means the compact
label result and its portable metadata have both been saved successfully. If the
label result is saved but metadata save fails, the task remains pending or
failed rather than claiming a complete output save.
When a task has an output location, metadata-only changes such as label
code/name/shortcut changes, unlabeled sentinel changes, or source identity
summary changes should rewrite the adjacent sample label result metadata even
when the label value array is unchanged. Such changes should enter the same
pending or failed save-state path until the metadata sidecar is saved
successfully.

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
