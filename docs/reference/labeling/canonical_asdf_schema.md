# Canonical ASDF sample-labeling schema

This is the normative wire and preservation contract for canonical labeling
documents. [Sample Labeling](sample_labeling.md) owns the user workflow and
annotation interchange; [labeling persistence ownership](labeling_persistence_ownership.md)
owns local registration, checkpoints, publication lifecycle, and failures.
The decision rationale is recorded in [ADR 0001](../../adr/0001-sample-labeling-architecture.md).

## Canonical schema 2.0 identity and provenance

The following names identify different things and must not be used
interchangeably:

| Name | Meaning | Mutation rule |
| --- | --- | --- |
| Task ID | The canonical identity of one labeling task. It is stored at `labeling_task.id`. | Immutable. A rename, Save As, export, or output relink does not create a new ID. |
| Task name | User-authored UTF-8 display data stored at `labeling_task.name`. | Persistently editable. Changing it advances `modified_at` but does not change the task ID or any path. |
| Output filename | The user-selected filesystem name of the canonical `.asdf` owner. | Independent of both task ID and task name. The dialog may suggest a sanitized filename from the task name, but the final name is not canonical metadata and later task renames do not rename it. |
| Spectiary schema identity and version | The semantic contract selected by YAML `format_kind` and `schema_version`. | Current documents use exactly `spectiary.sample_labeling` and `2.0.0`; these are not ASDF implementation versions. |
| Spectiary build source | Generation provenance stored in `spectiary_build`. | A current writer or rewrite stamps its own compiled build identity. `head` requires one full 40-character lowercase hexadecimal revision; `working_tree` requires `source_revision` to be absent. |
| ASDF file-format version | The container framing version in `#ASDF 1.0.0`. | Independently versioned by ASDF. It does not mean Spectiary schema 1.0. |
| ASDF Standard version | The tag/schema vocabulary declared by `#ASDF_STANDARD 1.5.0`. | Independently versioned by ASDF. The `!core/asdf-1.1.0` root tag is likewise an ASDF core tag, not the Spectiary schema version. |

GitHub issue #82 is a bounded self-description patch incorporated into
`schema_version: 2.0.0`. It does not introduce schema `2.1.0`.

## Task identity and names

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
Unicode whitespace. Spectiary preserves the submitted text exactly: it does not
trim it and does not perform Unicode normalization. The task name is not a
filename, source identity, annotation display-name override, or temporary-task
status marker.

## Timestamps and semantic mutations

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

## Origin provenance

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
contact metadata: Spectiary does not apply RFC address parsing, DNS validation,
normalization, or case rewriting, and never infers it from Git, the operating
system, or other machine-local state. Role, organization, and additional author
ID fields are not part of schema 2.0.

## Complete YAML-tree example

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
  name: Spectiary
  version: 0.8.0
spectiary_build:
  source_mode: head
  source_revision: "0123456789abcdef0123456789abcdef01234567"
format_kind: "spectiary.sample_labeling"
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

## Sample alignment and values

For `sample_roster.identity_kind: source_index`, `sample_roster.names` is
absent and the values ndarray uses block source `0`. In both roster forms the
required `annotation.alignment` map is exactly `mode: by_index` and
`target: sample_roster`: `values[i]` belongs to `sample_roster.names[i]` for an
explicit roster and to source index `i` for a source-index roster. Missing
alignment fields, non-string values, or any other mode/target are semantic
errors; schema 2.0 has no compatibility path for an alignment-less document.
This fixed declaration does not introduce a `by_id` or `by_key` alignment subsystem.
The values contract is one-dimensional little-endian signed `int32`, its length
is exactly `source_collection.sample_count`, and `-1` is the reserved unlabeled
sentinel. Schema 2.0 has no `annotation.name`; the canonical task name lives
only at `labeling_task.name`, while `origin.annotation.name` is provenance for
the promoted input artifact and is not a second task name.

## Producer and build provenance

`asdf_library.name` is exactly `Spectiary`; `asdf_library.version` is the
current build's generated application version (`0.8.0` in the example above).
The required adjacent `spectiary_build` map comes only from the generated build-identity
header. Runtime Git queries, release sidecars, compiler/SDK/dependency inventory,
executable hashes, and completion timestamps do not participate.
It records only the source state compiled into that producer:
clean `head` builds include the complete revision, while `working_tree` builds
omit `source_revision` entirely. The field is generation provenance rather
than user-authored task content. Fresh writes, values-only rewrites, and full
metadata rewrites all stamp the current binary's identity, so a rewrite does
not retain the producer identity of the generation it replaces.

## Unknown-field preservation

Preserving rewrites start from a validated durable generation and retain
unknown mapping entries in the existing YAML tree, including nested source,
roster, annotation/descriptor, task, origin/annotation-origin, build-source,
and label maps. Known build-source fields are replaced with the current
binary's identity while unknown entries inside that map are retained.
Label-local entries are matched by stable label code. This is deliberately not
a general forward-compatibility promise: Spectiary does not preserve arbitrary
extra binary blocks, unknown sequence-item schemas, replaced known fields,
metadata bytes/padding, or the old block index/trailer. Removed labels lose
their label-local unknown fields, and the canonical `authors` sequence is
rebuilt from the supported author model. Fresh writes have no unknown metadata
to preserve. Both full and values-only rewrites require the durable base to be
the same source/roster/annotation/task identity; values-only rewrites may change
only task values and `modified_at` and reuse only the encoded roster block.
Generation provenance still refreshes `spectiary_build` to the current binary.

## Compatibility and non-goals

Spectiary labeling schema `1.0.0` documents are unsupported and are not
migrated to `2.0.0`. The former `specforge.sample_labeling` namespace is not a
read alias, including for schema `2.0.0`; unsupported originals remain unchanged.
See [ADR 0011](../../adr/0011-project-identity-contracts.md) for the format-name
cutover. Local JSON schema support is a separate contract in
[labeling persistence ownership](labeling_persistence_ownership.md#bounded-local-state-migration).
These version boundaries are unrelated to the ASDF file-format `1.0.0` header.

Schema 2.0 intentionally has no task-copy/parent lineage, persisted edit
history, document revision counter, incremental patch log, or workflow-status
field. Draft/formal ownership, active task, auto-advance, navigation position,
pending/retry/save state, output path, and UI state remain local workflow data.
The controller's in-memory view revision is not a canonical document revision.

## Codec and durable-generation rewrites

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
Spectiary schema `2.0.0` fields into the validated generation and preserves
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
