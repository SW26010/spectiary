# Sample Labeling Architecture

SpecForge models per-sample data as sample annotation results and treats storage
formats such as NPY and CSV as annotation I/O adapters, not as the domain model.
New formal labeling owners use the canonical SpecForge sample-labeling schema
`2.0.0` in one ASDF document; legacy NPY owners retain their adjacent portable
metadata sidecar. Sample labeling task records, workflow settings, autosave
state, output paths, and recovery remain local user state rather than canonical
document fields. Annotation I/O belongs behind a domain or service boundary; UI
code consumes loaded annotation results and save state rather than parsing
dtype, shape, ASDF blocks, or file-write capabilities.

`SampleLabelingController` is the single mutable owner of active tasks, drafts,
output-save state, retry scheduling, and the labeling state cache. Callers
receive a borrowed read-only view with a revision and submit atomic domain
operations. View pointers are valid only until the controller's next mutation
or destruction and must not be retained across command submission or
maintenance. Operation results report the resulting revision and output/state
persistence status, including rejected operations. `SampleWorkflowCoordinator`
still owns navigation order, filters, sorting, and undo coordination. A
labeling write may request advance, but only the coordinator resolves and
applies that navigation request.

Workflow mutations cross the coordinator/session boundary as one complete
transition outcome. The outcome carries domain change flags, navigation
results, any snapshot index that must be loaded next, command-specific status,
and whether the session view must be invalidated. `SourceCollectionSession`
consumes that outcome while retaining ownership of the source roster,
activation, background follow-up, and presentation lifecycles.

We keep sample navigation, sample filtering, sample annotation inspection, and
active manual labeling as separate UI surfaces. This avoids letting a labeling
window own unrelated concerns like current index, sample-filter composition, loaded
read-only annotations, or format-specific file matching.

Rejected alternatives were storing local task records, drafts, pending values,
active task state, sample filters, or other workflow recovery state inside or
beside every label array, making `*_y.npy` the default output semantics, and using one
labeling window to both inspect every annotation and edit the active task. Those
choices would couple workflow recovery to one file format, create conflicting
sources of truth, and make shortcut ownership and write safety harder to reason
about.

## Canonical schema 2.0 identity and provenance decision

The canonical document keeps these concepts independent:

| Concept | Decision |
| --- | --- |
| Task ID | Immutable `labeling_task.id`, using only lowercase hyphenated UUID v4 canonical text. |
| Task name | Persistently editable UTF-8 canonical metadata. It must contain non-whitespace text, is neither trimmed nor normalized, and is not an identity. |
| Output filename | User-selected shell/filesystem state. A sanitized task-name suggestion is presentation policy only; selecting or later renaming a file does not change task identity or task name. |
| SpecForge schema version | Exactly `schema_version: 2.0.0` for current documents. |
| Annotation alignment | Required `annotation.alignment` declaration with exactly `mode: by_index` and `target: sample_roster`; values follow roster/source index order. |
| ASDF versions | File format `1.0.0`, Standard `1.5.0`, and ASDF core tag versions are independent container/vocabulary versions, not SpecForge schema versions. |

Canonical time is a millisecond-resolution strong time point in the runtime and
the exact `YYYY-MM-DDTHH:MM:SS.sssZ` form on the wire. Dates and ranges are
validated, `created_at` is immutable, and `modified_at` is monotonic. Assigning
or clearing a value, renaming a task, and adding/editing/removing a label
definition are semantic mutations when their canonical result actually changes;
future description/author editing must use the same rule. Output selection,
export, activation, navigation/workflow settings, and save or retry state are
not. Publication failure retains both the old durable timestamp and the
mutation's pending timestamp; retry uses the original mutation timestamp rather
than retry time.

Fresh writers emit `manual` or `annotation_promotion` origins. A promotion names
only a portable CSV/NPY basename and may carry `sha256:<64 lowercase hex>` over
the complete raw bytes read from the same opened artifact generation as the
parser. The digest is not computed from decoded labels, a sidecar, a path, or a
source-collection fingerprint. Readers may preserve a syntactically valid
future origin token unchanged, but current writers cannot introduce one and
preserving rewrites cannot alter origin or `created_at`.

`description` and `authors` are optional canonical fields without current UI.
Unset values stay absent; the supported author model contains required `name`
plus optional `identifier` and optional `email`. Every present author field is
non-whitespace UTF-8 text. Email is preserved exactly as user-supplied contact
metadata; it is not parsed as an RFC address, normalized, or inferred from Git,
the operating system, or other machine-local state.

Schema 2.0 explicitly declares its only annotation alignment contract. For an
`explicit_names` roster, `values[i]` labels `names[i]`; for a `source_index`
roster, `values[i]` labels source index `i`. Readers reject a missing or
ill-typed alignment map and any mode/target other than `by_index` and
`sample_roster`. This is a fixed self-description field, not an extensible
`by_id`/`by_key` alignment subsystem.

Full and values-only rewrites must start from the validated durable generation
of the same source, roster, annotation, and task. The preservation contract is
limited to unknown mapping entries in the supported YAML tree; label-local
entries follow stable label code. It does not cover arbitrary extra blocks or
sequence schemas, removed labels, metadata byte layout/padding, or an old block
index/trailer. A values-only rewrite changes only values and `modified_at`,
reuses only the encoded roster block, and recomputes the metadata and container
layout.

SpecForge labeling schema `1.0.0` and labeling cache schemas 1 through 3 have no
migration path; current local state uses cache schema 4. Unsupported state is
ignored/fails closed according to its owner rather than being guessed into
canonical identity or provenance.

Task copy/parent lineage, persisted edit history, canonical document revision,
incremental patches, and workflow status are non-goals for schema 2.0. The
controller's borrowed-view revision and local draft/formal, active, pending,
retry, output-path, navigation, and UI state remain runtime/local concerns.
The complete wire-tree example and field-level validation rules live in
[`sample_labeling.md`](../sample_labeling.md#canonical-schema-20-identity-and-provenance).

## Local persistence failure semantics

Source-session, navigation, labeling, and workflow state remain four
independently validated, independently written versioned JSON caches. Their
codecs continue to own schema support and field validation; the session only
aggregates owner-reported health.

| Condition | User signal | Continue? | Clear condition |
| --- | --- | --- | --- |
| Missing cache | None; use defaults | Yes | Not applicable |
| Corrupt cache | Non-blocking owner warning; a later write attempt reports retrying | Yes, with read-only salvage or defaults; labeling cache commits fail closed and preserve the original bytes | The cache is repaired or removed, then that owner successfully commits against the trusted latest state |
| Unsupported schema | Non-blocking owner warning; a later write attempt reports retrying | Yes, without reading unsupported state; labeling cache commits fail closed and preserve the original bytes | Supported state replaces the cache or the user removes it, then that owner successfully commits; labeling cache schemas 1–3 are not migrated |
| Partial save | Overall `retrying` health naming each failed owner while retries remain scheduled | Yes; attempt every other dirty cache | Each failed owner succeeds independently |
| Retrying | Overall `retrying` health; existing retry deadline remains active | Yes | First successful retry |
| Recovered | Overall recovery health | Yes | Next user mutation owned by the recovered cache |

There is no cross-file transaction: a failure in one cache must not suppress
attempts for the other caches. Normal shutdown consumes a per-owner flush
result so an incomplete flush is not reduced to an ignored aggregate boolean.
