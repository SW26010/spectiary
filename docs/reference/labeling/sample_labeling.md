# Sample Labeling

This is the workflow entry point for sample-label inspection, manual labeling,
annotation interchange, and source identity. It is separate from spectral-line
marker labels and spectral-line catalog user state.

| Contract | Authoritative page |
| --- | --- |
| Panel behavior, sample navigation, sample filtering, sample sorting, shortcuts, and output relinking | [Sample workflow windows](sample_windows.md) |
| Canonical identities, provenance, schema, YAML example, and preserving rewrites | [Canonical ASDF schema](canonical_asdf_schema.md) |
| Local owners, draft recovery, publication, retries, and migration | [Labeling persistence ownership](labeling_persistence_ownership.md) |
| Retired NPY/sidecar publisher and bounded compatibility | [Legacy recovery audit](legacy-labeling-recovery-audit.md) |

## Language

Use the terms from [CONTEXT.md](../../../CONTEXT.md):

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
- `Sample labeling draft`: the in-progress result of a temporary task. Its
  running in-memory object is authoritative; a best-effort checkpoint may
  support resuming it before explicit canonical Save As.
- `Sample filtering`: user-authored sample subset selection. In Spectiary, this
  word family should be qualified in new product and documentation language and
  must not refer to spectrum smoothing, valid-point selection, spectral-line
  search, or marker visibility.

## Staged Rollout

The historical delivery order was sample navigation, read-only annotation
inspection, editable labeling, then sample filtering. The initial slice used
row/name navigation, same-prefix `*_y.npy` discovery, and annotation display.
That order explains the boundaries below; it is not a current feature-status
list. See the [initial product roadmap](../../evidence/product/initial-roadmap.md);
current work is tracked in [GitHub Issues](https://github.com/SW26010/spectiary/issues).

## Existing Labels

Existing per-sample annotation values may be loaded from companion data for the
active source collection. Integer annotation values may be interpreted as sample
label values through a sample label mapping when the user associates them with a
sample labeling task. String and floating-point annotation values may be shown
directly as read-only annotation results.

When existing integer annotation values are present but no mapping is available,
Spectiary should show the stored values rather than guessing class names.

For automatically loaded read-only annotations, Spectiary should infer the
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
not match the source collection's spectrum count, Spectiary must not attach it
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
Confirmation adopts that same document as a canonical local owner: Spectiary
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

The complete [sample workflow windows contract](sample_windows.md) defines
Navigation, Sample Filters, Sample Sorting, Annotations, and Labeling. Navigation
owns the current sample; other surfaces submit requests. Sample filtering chooses
membership, sample sorting chooses order, and labeling owns the active classification
task without taking over source selection or navigation.

## Persistence

The runtime task combines content with local workflow preferences, but these
have different persisted owners. [Labeling persistence ownership](labeling_persistence_ownership.md)
defines ordinary registration/session state, complete pre-canonical draft
checkpoints, canonical ASDF content, runtime-only dirty edits, failure handling,
and migration. Task name and label definitions belong to draft content or ASDF;
a local registration is not another content copy.

Task records are scoped to a source collection. The activated source determines
which tasks and annotations appear in the sample windows. A source may retain
multiple output-backed registrations and at most one normal output-free draft
slot. The remembered labeling position is a sample index only; the containing
task supplies source identity. It does not own Navigation's current index.

Sample navigation state is separate local user state scoped to source collection
identity. It persists the last shown sample index for the source collection
regardless of which sample labeling task is active. Activating or switching a
sample labeling task must not overwrite that navigation state or use the task's
remembered labeling position as the source collection's current sample index.
Loaded annotation paths are source collection session state, not sample
navigation cache state. They should be restored with the user's current source
collection session and kept separate from the last-index navigation cache.

### Source identity and fingerprints

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

### Source relinking

Relink requires the source spectrum count to match the task record's spectrum
count. If only the source name changed while the source fingerprint and spectrum
count still match, Spectiary may relink automatically and notify the user. If the
source fingerprint changed while the source name and spectrum count still match,
Spectiary should treat the loaded source as different by default and only relink
after explicit user confirmation. If both source name and source fingerprint
changed while spectrum count still matches, Spectiary should also treat the
loaded source as different by default and only relink after explicit user
confirmation. After relink, Spectiary should update the task record to the new
source collection identity so the user is not prompted for the same source change
every time. Relink does not reinterpret remembered labeling positions by sample
name, source path, or content matching. A remembered labeling position remains
the same sample index inside the relinked source collection.


## Annotation I/O

The sample-labeling model should treat annotation storage formats as adapters.
Core labeling, navigation, sample filtering, and annotation display should work with a
loaded sample annotation result: one value per spectrum sample, with a known
value kind such as categorical integer, categorical string, or continuous
floating point.

### Canonical ownership and NPY interchange

Canonical sample-labeling documents follow the
[ASDF schema and wire contract](canonical_asdf_schema.md).
The ASDF adapter writes newly formalized tasks and hydrates source-aware
annotations and registered tasks. New canonical
owners require a user-selected `.asdf` path before direct canonical publication.
NPY remains a read adapter for legacy-format owners and annotation promotion.
New promotions create output-free drafts. That legacy owner's lease identity
always protects both the selected result path and the adjacent
`<stem>.sf-labels.json`. NPY is also available through a separate one-shot value
export operation; that operation is not an output owner and does not acquire a
task-identity lease, write a metadata sidecar, change the autosave target, or
clear the task's runtime pending edits. It does acquire the legacy artifact set's
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
### CSV interchange and promotion

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

Annotation I/O belongs in a domain or service boundary, not in UI code. UI
surfaces should consume loaded sample annotation results, task records, and save
state without parsing storage formats or reimplementing dtype, shape, or write
capability rules.

### Output selection and export

Sample label results are labeling output. They should be written to an explicit
output location chosen by the user. Spectiary should recommend choosing an
external output file, but should not require one before labeling starts. Output
formats should be compact and practical for C++ streaming reads and writes;
verbose JSON is appropriate for drafts and recovery metadata, not as the
preferred export format for large label arrays.
New formal labeling tasks use canonical ASDF so source identity, task metadata,
roster, and the one-dimensional label values share one atomic generation.
Numeric tasks write stable label codes, and the sample label set owns the
interpretation from numeric code to user-facing label. Unlabeled samples use
`-1`; the Spectiary sample-labeling schema `2.0.0` value array is signed `int32`.
Save and autosave publish canonical ASDF; legacy registered owners must be
explicitly migrated before new formal content is published.
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

### Legacy sidecar read contract

Historical NPY labeling outputs have portable sample label result metadata beside
the compact `.npy` file. The publisher is retired; these fields describe
read-only annotation and migration inputs. This metadata is a data-contract sidecar, not a copy of the local sample
labeling task record. It includes a format kind, schema
version, referenced result file, stable task id, expected value count, expected
dtype, unlabeled sentinel, task name, label code/name/shortcut entries, and the
source collection identity summary when available. The referenced result file
is stored as a path relative to the metadata file's directory, normally
just the result file name, so the `.npy` plus metadata pair remains portable
when moved together. The source collection identity summary should reuse the
same source collection identity fields used by Spectiary's source/file
management flow, such as source name, source fingerprint, context fingerprint,
and spectrum count. It must not store the label value array, pending values,
local autosave draft, active task state, window visibility, sample-filter state, save
retry state, or a local absolute source path.
For an output file named `<stem>.npy`, the adjacent metadata file is
named `<stem>.sf-labels.json`.

When loading an existing numeric label result, Spectiary should use adjacent
sample label result metadata when it matches the label result file, value count,
and dtype. If matching metadata is absent, Spectiary may still load and display
the raw numeric label values, but it should not invent label names or shortcuts.
If adjacent metadata exists but does not match the label result, Spectiary
should warn and fall back to raw numeric label values instead of applying stale
or unrelated mappings. The first implementation should not allow the user to
force-bind mismatched metadata to a label result. The user may choose the correct
label result and matching metadata pair, but overriding metadata binding is
future work because an incorrect binding can silently change the meaning of
stored label codes.

### Classification value model

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

## Save State Indicator

The labeling UI distinguishes unsaved draft recovery from canonical publication,
following [the lifecycle contract](labeling_persistence_ownership.md#user-visible-lifecycle):

- unsaved draft with a successful recovery checkpoint (not document-save success);
- autosaved to the selected output location;
- pending changes waiting for the next autosave, including a count such as
  `pending: 5`;
- save failed and will be retried, with the pending count preserved.

The pending count is the number of distinct samples whose latest label value has
not yet been successfully persisted. Multiple edits to the same sample count as
one pending sample. This indicator should describe the current write state
without treating every restored draft as proof of a crash.
