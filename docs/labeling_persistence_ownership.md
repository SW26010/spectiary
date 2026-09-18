# Labeling persistence ownership

Issues #108 and #109 define one lifecycle and its labeling implementation.
The running task is authoritative until explicit Save As publishes a canonical
document. A checkpoint is best-effort session recovery, never a saved document.

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

The schema-4 wire-field audit is exhaustive below. A repeated identity is a
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
Only then does the controller adopt the canonical owner and update registration.
Checkpoint cleanup is best-effort. A crash in that interval may leave a complete
canonical document and a stale checkpoint. No publication WAL is needed.

Formal edits attempt atomic canonical writes directly. A failed write retains
dirty values/metadata and retry state in memory. Local state/checkpoint failures
do not impose a write-ahead dependency on canonical output. Abnormal termination
may lose edits since the last successful canonical write.

Legacy migration leaves its input files unchanged, publishes and validates the
replacement ASDF, then switches registration. There is no migration WAL.
The pre-release monolithic cache is not an ongoing compatibility owner; unsupported
old local state must never cause a canonical file to be modified or removed.

## User-visible lifecycle

Checkpoint publication never reports document-save success. Drafts stay unsaved
until explicit canonical publication. Ordinary close retains paused drafts and
flushes their checkpoints; a known checkpoint failure blocks close so the user
can retry, explicitly save, delete/discard the draft, or cancel closing. Pending
formal writes also block normal close. Pausing a task or changing source retains
the live working object and dirty-task leases. Windows
session end must not veto shutdown solely because a pre-canonical draft exists;
already-maintained checkpoints are best-effort recovery, not a zero-loss promise.
