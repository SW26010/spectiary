# Sample workflow windows

This is the interaction contract for Navigation, Sample Filters, Sample Sorting,
Annotations, and Labeling. Start with [Sample Labeling](sample_labeling.md) for
the domain workflow and import/export rules. Persistence and failure semantics
are owned by [labeling persistence ownership](labeling_persistence_ownership.md).

## Panel responsibilities

Spectiary should separate sample navigation, sample filtering, sample annotation
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

## Task selection and lifecycle

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
controls remain visible even when the active sample filtering has no matching
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
failed canonical-document saves. Legacy NPY/sidecar owners are read-only
migration inputs and do not have an autosave publisher. The user must wait
for autosave to complete or fix the output save problem before deactivating that
formal task. A temporary
task whose first `Save to...` attempt fails remains output-free and recoverable:
it keeps its temporary identity and error message and may be paused, deleted, or
saved to the same or a different target. Pausing an output-free draft retains its live working object and dirty-task
lease. Application close flushes draft checkpoints and follows the
[known-failure guard](labeling_persistence_ownership.md#user-visible-lifecycle).

Deleting a sample labeling task is a separate explicit operation from closing
or deactivating it. Delete removes the local task record and its internal draft.
It must not delete any artifact owned by the task's persisted format: neither a
canonical ASDF document nor a legacy `.npy` result and its adjacent portable
metadata sidecar. If the same output is still loaded as an annotation later,
Spectiary should treat it according to the normal plain/external/local matching
rules rather than silently resurrecting the deleted local task record. Delete
should be disabled while the task has pending or failed output saves, matching
close/deactivate.

## Navigation and ordering

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

The original read-only vertical slice used source order, previous/next movement,
direct row-index jumps, and sample-name matching. The sorting contract below
extends that sequence without transferring ownership of the current sample.
Sorting by annotation value must not implicitly follow the active editable task,
because changing the active task must not unexpectedly reorder navigation.
## Sample Sorting

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
## Numeric navigation

The Navigation window exposes `source sample` as an editable 1-based source-row
number whenever direct source-row location is available. While a filtered or
sorted sequence is active, it also exposes `sequence` as an editable 1-based
position within the current sequence. A `source sample` submission locates that
source row by converting the entered number to a 0-based `LocateRow` target; a
`sequence` submission locates the source row at that position after active
sample filtering and sorting through `LocateSequencePosition`.
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
unavailable. Source, context, count, sample filtering, or sorting topology changes
invalidate edits from the previous topology. Ordinary committed or pending
cursor updates do not overwrite an active buffer. No-op sample-filter or sorting
reconciliation that retains the effective active state, membership, and row
order also retains the revision and the edit. The current source row remains
selected when eligible, and only its displayed sequence position is
resynchronized.

## Sample Filters

Sample filtering belongs to the separate `Sample Filters` window. It should
start with no selected sample-filter entries and support stacking multiple
sample-filter conditions after the user explicitly adds supported annotation
results from `Annotations`, including local labeling task rows displayed there.
The user may add a supported annotation result from an add-source control in
`Sample Filters` or by dragging the annotation row into `Sample Filters`.
Removing a sample-filter entry should clear any condition for that entry.
Resetting sample filters should remove all selected sample-filter values while
leaving the explicit source entries available for continued sample filtering. Sample
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

## Annotations

The sample annotation view should be an independent `Annotations` window, not a
section inside the Files or Info windows. In this document, `Sample annotation
view` remains the domain term for that surface. The window should display loaded
sample annotation results, including automatically loaded `*_y.npy` data. It consumes
the active source collection and current sample from sample navigation, but it
does not own source selection, file management, or the current sample index. It
should show one annotation result per row with a display name and the current
sample's annotation value or mapped label name. The original read-only vertical
slice omitted drag and promotion controls; eligible editable workflows use the
explicit actions described below.

An annotation result's display name is user-facing local workflow state. It must
not rename the underlying annotation file, change the annotation path used as a
stable source identity, or rewrite portable sample label result metadata. The
default display name is the loaded result name: automatically loaded plain
annotations default to the annotation file name, metadata-backed label results
default to the metadata task name when available, and local sample labeling task
rows default to the task's authoritative content name. An unopened local
registration may supply only a display-name hint until canonical content is read. The
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

Eligible annotation rows should expose a
small drag affordance so the user can drag that annotation to the Labeling window
and make it the active sample labeling task.

Promoting an integer categorical NPY annotation or an eligible source-aware CSV
text annotation creates an output-free draft. Promotion never adopts or
overwrites the input artifact. Editing a standalone canonical ASDF instead
requires an explicit in-place edit warning: explain that later edits modify the
selected document and recommend preserving a backup when appropriate. These
are separate operations; legacy NPY/sidecar registrations remain read-only
inputs for explicit ASDF migration.

Portable labeling evidence is format-specific. A canonical ASDF document owns
its task id, task metadata, source identity, roster, and values in one file. A
legacy NPY result obtains equivalent labeling evidence from its adjacent
`<stem>.sf-labels.json` sidecar. Neither a self-describing ASDF document nor a
matching legacy sidecar by itself makes the output the current user's own task.
Without a matching local task record for the active source and output path, the
annotation is an external label result. Adopting an external canonical ASDF
requires the in-place edit warning; promoting NPY/CSV creates a draft instead.
With a matching local task record, Spectiary may restore or activate the
registered owner without repeating the warning. Legacy registration does not
authorize writing the retired NPY/sidecar format.

The annotation view should distinguish loaded per-sample data by workflow
relationship:

- Plain annotation: neither a canonical labeling document nor matching legacy
  sample-label metadata is available.
- External label result: canonical document metadata or matching legacy
  sidecar metadata is available, but no matching local task record exists.
- Local labeling task: the format's labeling metadata and a matching local task
  record both exist.

Plain annotations should display their current raw annotation value. Integer
categorical NPY annotations may be dragged into `Labeling` to create an
output-free draft; the input stays unchanged.
Floating-point plain annotations and arbitrary string NPY annotations are
read-only in the first editable labeling implementation. A source-aware CSV text
annotation with the defined label-column schema may expose the explicit
outputless-promotion action described below.

External label results should display the mapped label value from their
canonical document or adjacent legacy metadata and be visibly marked as
external. Dragging a standalone canonical ASDF into `Labeling` requires the
in-place edit warning. If the user confirms that adoption,
Spectiary adopts its embedded stable task identity and exact document path as a
`canonical_asdf` owner after a leased store reopen; a same-id/different-path or
same-path/different-id local owner is a conflict and must not be resolved by
inventing `-2` style identities. NPY/CSV annotation promotion remains the
separate output-free draft path; `legacy_npy_with_sidecar` identifies an
existing read-only migration owner.

### Local ownership and relinking

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
missing, Spectiary should keep the task visible with a missing-output state
rather than downgrading it to a plain annotation or silently deleting it. The
missing state may appear in `Annotations`, and in `Labeling` when the missing
task is restored as active local state. Manual relink selects a replacement
output of the same owner format. Canonical relink validates the ASDF document's
task id, source identity, roster, annotation kind/count, label definitions, and
values. Legacy relink validates the NPY result plus adjacent
`<stem>.sf-labels.json` task id, source summary or explicit source-relink
confirmation, dtype, count, and label-code compatibility. Only after that
validation does the local record adopt the new path.

When the user imports or adds an annotation file, Spectiary may also use the
same validation rules to automatically reconnect a missing local labeling task
if the canonical document, or the legacy result and adjacent metadata, matches
that task record. Automatic relink is limited to the imported path and its
format-declared artifacts; Spectiary should not scan the broader filesystem for
moved label results.

Promotion accepts integer categorical NPY annotations and the source-aware CSV
text interchange schema. Floating-point annotations and arbitrary string NPY
annotations remain read-only. CSV promotion deterministically maps text labels
to numeric codes, as specified in [Annotation I/O](sample_labeling.md#annotation-io).
New formal output uses the canonical signed-int32 value contract; promotion
does not constrain future writes to the source NPY dtype because it never writes
that source artifact.

Activating an annotation as an existing local task must use the same identity
validation as annotation relationship display. A canonical annotation must
match the local task's owner format and output path plus its embedded task id,
source/roster identity, annotation kind, and sample count. A legacy annotation
must match the output path, sidecar `task_id`, result contract, and sample count.
A path-only match must not activate a local task or overwrite its output. If the
same path is owned by a different local task, activation remains blocked and the
annotation remains external.

### Canonical publication and legacy migration

When a temporary task is started locally inside Spectiary, it does not need the
in-place edit warning. It starts without an external output target and remains a
local recovery draft until the user selects `Save to...`. Selecting the output
location requires an `.asdf` path and promotes it to a formal local labeling
annotation only after one canonical document containing source identity, task
metadata, roster, and values is atomically written and reopened successfully.
The task retains the formal name established when the draft was created or
promoted; the chosen filename does not rename it. Successful publication then
permits a fresh temporary task for the same source collection. Publication, registration, checkpoint cleanup, and partial failures follow the
single [publication contract](labeling_persistence_ownership.md#publication-and-failures).

An active `legacy_npy_with_sidecar` owner exposes explicit `Migrate to ASDF...`.
The controller retains task and legacy artifact leases, acquires the destination
lease, publishes and reopens the ASDF, then changes ordinary registration and
adopts the destination lease. The legacy NPY and sidecar are never modified.
Failure before registration leaves the legacy owner available; an extra complete
ASDF may remain unregistered. No migration WAL or formal pending overlay is
persisted. Path conflicts, publication/reopen failures, and registration failures
remain distinct user-facing errors with low-level details confined to diagnostics.

The first implementation should not allow two local sample labeling task records
for the same source collection to point at the same output path. If the user
selects an output path already used by another local task record, Spectiary
should report the conflict and offer low-risk choices such as activating the
existing task or choosing a different output path. Output ownership transfer,
task takeover, or automatic unbinding of the previous task is future work and
should not be part of the first implementation.

## Labeling controls and resume

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

## Shortcuts and undo

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
