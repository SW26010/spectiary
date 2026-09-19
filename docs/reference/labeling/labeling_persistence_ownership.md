# Labeling persistence ownership

Issues #108 and #109 define one lifecycle and its labeling implementation.
The running task is authoritative until explicit Save As publishes a canonical
document. A checkpoint is best-effort session recovery, never a saved document.

[Sample Labeling](sample_labeling.md) defines the workflow and source identity;
the [canonical ASDF contract](canonical_asdf_schema.md) defines document content.
This page owns persisted field placement, coordination, recovery, and failures.

## Physical owners and field audit

| Owner | Fields |
| --- | --- |
| `state/sample-labeling-state.json` | Source identity/count/fingerprints; task identity, owner format and canonical locator; active selection; auto-advance, skip-labeled and remembered position. |
| `unsaved/sample-labeling-drafts.json` | Output-free task identity and source compatibility information; task name, provenance/timestamps, description/authors, label definitions and complete values. |
| Runtime only | Formal pending values and metadata; save failure/message/retry state; derived statistics; leases and open canonical generation. |
| Removed | Persistent `initial_publication_pending`, publication compensation and canonical recovery WAL. |

Canonical task content is loaded from the user-owned document. Ordinary state
must not contain canonical metadata or label definitions as a recovery payload.
Draft workflow preferences belong to ordinary state; recovery can use defaults
when that state is missing. Losing ordinary state never changes a canonical file.

The ordinary-state and checkpoint owners each use an independent schema-1
envelope. The old schema-4 wire-field audit is exhaustive below. A repeated identity is a
reference/compatibility constraint, not a second content owner.

| Schema-4 field | New contract |
| --- | --- |
| `format_kind`, `schema_version` | Independent ordinary-state and checkpoint schema envelopes. |
| `sources[].identity` | State registration key; checkpoint source-slot identity. |
| `sample_count` | State registration count; checkpoint compatibility constraint and values length. |
| `source_name` | State presentation hint; checkpoint source description, never a relink identity. |
| `source_fingerprint`, `context_fingerprint` | State matching references; checkpoint compatibility constraints. |
| `active_task_id` | State only. No new last-selected field is needed unless the workflow already requires one. |
| `tasks[].task_id` | State registration/session reference; draft identity in checkpoint; formal identity verified against ASDF. |
| `task_name` | Checkpoint for drafts; ASDF for canonical tasks. No authoritative name copy in state. |
| `canonical_metadata.created_at`, `modified_at` | Checkpoint for drafts; ASDF for formal content. |
| `canonical_metadata.origin.kind` | Checkpoint for drafts; ASDF for formal content. |
| `canonical_metadata.origin.annotation.name`, `format`, `fingerprint` | Checkpoint for promoted drafts; ASDF for formal content. |
| `canonical_metadata.description`, `authors[].name`, `identifier`, `email` | Checkpoint for drafts; ASDF for formal content. |
| `auto_advance`, `skip_labeled_on_advance`, `remembered_position` | State only, including preferences for draft task IDs. |
| `output.path`, `output.format` | State registration only; checkpoint entries cannot own an output path. |
| `labels[].code`, `name`, `shortcut` | Checkpoint for drafts; ASDF for formal content. |
| `values` | Complete checkpoint values for drafts; canonical values only in ASDF. |
| `pending_values[].index`, `value` | Removed from persistence; runtime sparse edits may remain. |
| `metadata_pending` | Removed from persistence; runtime metadata dirty flag may remain. |
| `save_state`, `save_message`, `save_message_kind` | Runtime only; recompute presentation after load. |
| `initial_publication_pending` | Delete the protocol and field, rather than relocating it. |

`pending_count`, label usage counts, labeled count, retry deadlines, open snapshots,
and leases are already derived/runtime state and remain so. The explicit
`display_name_hint` supports unopened lists, but cannot supply canonical
metadata, reconstruct missing documents, or override document contents.

## Coordination and partial writes

Both JSON owners use atomic replacement and the existing shared labeling commit
lock. A commit reloads both owners and applies the existing task patch under that
lock. Task/output leases and the source temporary-slot lease retain their roles.
There is no cross-file transaction, generation journal or second coordination
system. A reader reconciles a stale draft against a registered canonical task
with the same source and task identity. Canonical registration wins.

A new same-source draft first reconciles the existing compatible checkpoint.
Malformed or ambiguous entries fail closed; incompatible drafts are not silently
reused. Explicit deletion and successful canonical registration remove obsolete
checkpoints on the next successful checkpoint replacement. The single checkpoint
file is rewritten, rather than accumulating historical checkpoint generations.

## Publication and failures

Save As constructs the full canonical document from current memory, acquires its
output leases, checks conflicts, publishes atomically, and reopens/validates it.
Output conflict checks read ordinary registrations, so a damaged best-effort
checkpoint cannot prevent publishing an already-held in-memory draft.
Only then does the controller adopt the canonical owner and update registration.
Checkpoint cleanup is best-effort. A crash in that interval may leave a complete
canonical document and a stale checkpoint. No publication WAL is needed.

Formal edits attempt atomic canonical writes directly. A failed write retains
dirty values/metadata and retry state in memory. Local state/checkpoint failures
do not impose a write-ahead dependency on canonical output. Abnormal termination
may lose edits since the last successful canonical write.

## Bounded local-state migration

Legacy migration leaves its input files unchanged, publishes and validates the
replacement ASDF, then switches registration. There is no migration WAL.
Only when neither new JSON owner exists is the pre-release schema-4 monolith
read as a bounded migration input. Compatible output-free drafts may become
checkpoints; formal pending metadata/value payloads, errors, and retry state
are discarded. Old monolithic schemas 1–3 remain unsupported; they are distinct
from the independent schema-1 envelopes of the current split owners.
The monolithic cache is not an ongoing compatibility owner, and unsupported
old local state must never cause a canonical file to be modified or removed.
Output leases and conflict checks continue to use explicit owner format. See
the [legacy recovery audit](legacy-labeling-recovery-audit.md).

## User-visible lifecycle

Draft edits automatically update their best-effort checkpoint; after successful
canonical publication, later edits autosave to that document. There is no separate
live-save toggle or primary Save/Cancel editing mode. Draft-only status by itself
does not cause a blocking retention prompt; the known-failure rules below still
apply to normal close.

Checkpoint publication never reports document-save success. Drafts stay unsaved
until explicit canonical publication. Ordinary close retains paused drafts and
flushes their checkpoints; a known checkpoint failure blocks close so the user
can retry, explicitly save, delete/discard the draft, or cancel closing. Pending
formal writes also block normal close. Pausing a task or changing source retains
the live working object and dirty-task leases. Windows
session end must not veto shutdown solely because a pre-canonical draft exists;
already-maintained checkpoints are best-effort recovery, not a zero-loss promise.

## Canonical activation and runtime retries

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
remembered position, and this process's leased runtime dirty edits are applied afterward. The
controller keeps the resulting `SampleLabelingAsdfOpenSnapshot` as the active
durable generation only in memory; it is never serialized into the state cache.
Malformed documents, source/roster mismatches, and task-id mismatches fail the
activation without falling back to legacy NPY hydration. Assigning or clearing
values builds a complete replacement directly from the opened snapshot plus the authoritative task
values and canonical metadata. The resulting values-plus-`modified_at`
publication uses the values fast path; it preserves the supported unknown
mappings and the encoded roster block while replacing the values generation.
Only successful atomic replacement advances the in-memory snapshot and clears
the corresponding runtime sparse overlay. Failure leaves the prior ASDF value,
`modified_at`, and whole generation trusted on disk, retains the runtime overlay, and
schedules an owner-aware retry using the mutation's original timestamp rather
than a retry-time clock value.

A successful foreground publication or maintenance retry synchronizes the
attached annotation generation before the active snapshot can be released and
invalidates sample-filter/sample-sort projections that may have consumed the older generation.
Canonical retries run only while their source is active with its matching
descriptor; switching sources retains the runtime dirty edits and that source's next
activation re-arms publication. Task renames and label add/edit/remove
operations, including shortcuts and used-code rewrites, take the full-document
path, so known metadata, values, and forward-compatible unknown metadata remain
one canonical generation. The old snapshot is invalid as soon as a full
replacement reaches disk; the mutation is successful only after the replacement
reopens and its known generation matches the intended document. The requested
reopen source descriptor is checked against that intended document before
replacement, so an incompatible public-store call cannot alter the durable
owner. If reopen fails, the runtime overlay remains pending, the stale snapshot is
discarded, and retry first opens the current durable file before rewriting it.
Creating a new ASDF owner uses a full canonical document write followed by
source-aware reopen and generation validation. The controller adopts the formal
owner only after both steps and output-lease refresh succeed.

## Draft recovery and workflow registration

The normal output-free task slot receives best-effort checkpoint autosaves in
`unsaved/sample-labeling-drafts.json`, independently of registration/session
preferences. Historical, conflicting, or damaged state may expose multiple
identity-bearing recovery rows. Restoring a checkpoint should be visible as
recovery, without treating it as proof of a crash or requiring a clean-exit marker.

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

Starting or returning to a draft reconciles its compatible checkpoint. Formal
tasks read task content from their canonical document; failed formal edits from
a terminated process are not recovered from ordinary state. Registration keeps
the output path, workflow preferences, and remembered labeling position, with
only a non-authoritative name hint for unopened lists.

A selected output path is not enough to establish ownership: publication must
succeed before the task becomes canonical. Once registered, a matching internal
checkpoint is superseded and must not appear as a separate discovered annotation
or file-load option. A sample labeling context includes source identity, task ID,
and the selected output path when present. Identity and fingerprint/relink rules
are defined in [Sample Labeling](sample_labeling.md#source-identity-and-fingerprints).

Recovery must never silently overwrite an explicit result. A bare path or an
external sidecar's task ID does not restore local ownership; active source,
local registration, task identity, output path, and format validation must agree.
The [window contract](sample_windows.md#local-ownership-and-relinking) defines the
visible relationship and relink actions.
