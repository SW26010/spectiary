# Lightweight Multi-Instance Runs Share User State

SpecForge supports concurrently running ordinary GUI processes as independent
in-memory viewing contexts over the same resolved local user state root. It does
not introduce persisted per-instance sessions, a primary process, an IPC
configuration owner, or live state propagation. Each instance loads shared
settings, recovery state, and catalog user state as a startup snapshot; later
explicit writes update the shared defaults, and the last completed write wins.
This keeps the common temporary multi-spectrum viewing case lightweight while
retaining the existing Portable and LocalAppData storage contracts.

Labeling is the exception because its task cache and external result/metadata
pair represent user-authored work. Different labeling targets may be edited by
different instances concurrently, but one logical target may have only one
SpecForge editor. A target-scoped operating-system lease protects the editing
lifetime. Shared labeling-task-cache commits take a separate short-lived
cross-process lock, reload the latest JSON, apply only task-level upserts or
explicit deletion tombstones, validate the task and output identities, and
atomically replace the document. Ordinary task saves do not overwrite the
persisted active-task selection; only explicit activation or deactivation may
update that best-effort next-launch choice.

Cache coordination always acquires the adjacent normalized-path lock as its
mandatory baseline, even while the cache file or its parent directory is being
created. Resolved physical identities add separate alias locks only; they do
not replace the baseline or make labeling fail closed. When more than one lock
is needed, all contenders acquire the baseline and physical alias locks in the
same stable order, releasing a partial set before retrying or reporting a
failure. This prevents cache-creation races from switching a target into a new
lock namespace.

The stable source-and-task lease is held for the duration of active editing.
Pausing/deactivating a task releases its lease only after the corresponding
selection update is committed; deleting a task keeps the lease until its
tombstone is committed. The stable lease remains held across a temporary
draft becoming an output-backed task and never changes identity during that
transition. Temporary tasks additionally hold one source-scoped draft-slot
lease, while formal tasks also hold an output lease covering the complete
artifact set: both the selected result path and its derived `.sf-labels.json`
metadata sidecar.

Every output artifact contributes a normalized-path identity. This is the
mandatory coordination baseline and the exclusion guaranteed by the
implementation, including when a physical identity cannot be inspected.
When available, the lease also adds FILE_ID/directory-entry identities for an
existing target (or the parent-directory identity plus normalized leaf for a
new target). Those physical identities are a best-effort enhancement for
hardlink, junction, drive-mapping, and UNC/DNS/DFS alias protection; SMB final
path strings are not trusted to collapse aliases, and failure to resolve a
physical identity does not make single-instance labeling unusable.

Output replacement is atomic and may assign a new physical FILE_ID. The
output-save hot path therefore refreshes the normalized leases and retains
previously held physical leases without another synchronous probe. A newly
assigned physical identity is not guaranteed to be covered immediately after
replacement; a later non-hot activation, conflict check, or maintenance
refresh may converge the physical lease set. This is deliberate best-effort
alias protection rather than a guarantee of immediate post-replacement alias
exclusion.

Temporary-to-formal conversion first commits a write-ahead task checkpoint that
contains the selected output path and pending recovery state. No result or
metadata write begins until that checkpoint succeeds. If no artifact was
replaced and the write fails, or a compensating checkpoint can be committed
after a partial write, the checkpoint is restored to the existing temporary
draft behavior. If compensation cannot commit, the formal pending checkpoint
is retained so a later instance can recover it rather than treating the output
as an unowned orphan.

The complete artifact-set lease prevents different stale task IDs or result
extensions from creating parallel editors for one logical draft or output;
physical alias exclusion applies when the optional object identities resolve.
Draft creation acquires the source draft-slot lease before deriving a unique
task ID from the latest cache and then acquiring that task's stable lease; an
existing formal task with the requested ID is never treated as a successful
create operation. A create patch also carries an expected-absent precondition,
so a draft retained after an untrusted-cache failure cannot later replace a
same-ID task introduced while the cache was repaired.

Legacy caches that already contain multiple temporary drafts for one source
remain losslessly readable and patchable. Defining a UI recovery flow for
selecting among those historical drafts is deferred; this change does not
silently delete or merge them.

The labeling lease coordinates writers only. It does not invalidate annotation
snapshots already loaded by another instance when that labeling output changes.
Displays, filters, and sorting derived from such an annotation may therefore
continue to use the older values until the user explicitly reloads or reopens
the data, or restarts that instance. Atomic output replacement ensures that a
subsequent read observes a complete file; it does not provide live change
notification. This is expected eventual-consistency behavior rather than data
corruption, and cross-instance file watching, runtime propagation, and automatic
filter or sorting recomputation are not acceptance requirements of this change.
If those capabilities become product requirements, they should be designed as
a separate follow-up and do not by themselves require a primary process or IPC
configuration service.

Other shared settings and caches do not gain multi-writer merge semantics.
Hardening the non-atomic ImGui layout path and making independently created
performance-recording names collision-resistant are explicitly deferred; this
labeling change does not implement either follow-up. Automation instances
retain their existing isolated state roots and are outside this ordinary
multi-instance contract.
