# Lightweight Multi-Instance Runs Share User State

SpecForge supports concurrently running ordinary GUI processes as independent
in-memory viewing contexts over the same resolved local user state root. It does
not introduce persisted per-instance sessions, a primary process, an IPC
configuration owner, or live state propagation. Each instance loads shared
settings, recovery state, and catalog user state as a startup snapshot; later
explicit writes update the shared defaults according to the contract of the
state being written. Ordinary complete-snapshot settings remain
last-completed-writer-wins. The bounded catalog and labeling exceptions below
do not add live propagation between instances. This keeps the common temporary
multi-spectrum viewing case lightweight while retaining the existing Portable
and LocalAppData storage contracts.

## Catalog User-State Reconciliation Is a Bounded Exception

Catalog user state retains the startup-snapshot model: ordinary GUI instances
do not observe another instance's panel edits as they happen, and no watcher,
IPC owner, or background synchronization refreshes an already loaded panel.
A task-level write is nevertheless not a blind complete-snapshot write. The
task records its explicit changes against a canonicalized reconciliation base,
then reconciles only that task-owned delta with the latest durable catalog user
state at commit time. Startup normalization such as trimming names, repairing
references, or selecting a valid fallback is part of the base and is not
treated as explicit task intent.

The catalog controller acquires the short-lived `<cache-path>.commit.lock`
lease before reloading the latest durable document and holds it through trust
validation, catalog-specific reconciliation, canonicalization and validation
of the result, and completion or failure of the atomic replacement. The live
operating-system file handle owns the lease. The lease does not span the GUI
instance lifetime or ordinary in-memory panel editing. Stable catalog view and
group identities, field ownership, marker-reference membership, explicit
ordering and selection intent, per-marker visibility and explicit-color fields,
and remapped panel expansion keys define the merge boundary. A marker color
Reset to Auto is an explicit field deletion for that stable marker identity;
disjoint marker color edits survive concurrently. The catalog controller and
its reconciliation module own those semantics; the generic atomic-file and
local-user-state facilities do not.

If the latest durable document cannot be trusted because parsing, schema,
shape, semantic-identity, or allocator-history validation fails, the commit
fails closed with the diagnostic and leaves that document untouched. A stale
startup snapshot must not replace or repair it. A missing cache remains the
normal empty first-write state, and supported legacy migration is allowed only
under the catalog contract's validation rules. The complete operational merge,
canonicalization, migration, and failure rules are specified in the
[spectral-line catalog concurrent user-state write contract](../spectral_line_catalog_contract.md#concurrent-user-state-write-contract).
This is an intentional catalog-only exception, not a reusable multi-writer
cache framework. This amendment records existing behavior and ownership; it
does not change runtime ownership or introduce a new persistence facility.

Labeling is a separate exception because its task cache and external
result/metadata pair represent user-authored work. Different labeling targets
may be edited by different instances concurrently, but one logical target may
have only one SpecForge editor. A target-scoped operating-system lease protects
the editing lifetime. Shared labeling-task-cache commits take a separate
short-lived cross-process lock, reload the latest JSON, apply only task-level
upserts or explicit deletion tombstones, validate the task and output
identities, and atomically replace the document. Ordinary task saves do not
overwrite the persisted active-task selection; only explicit activation or
deactivation may update that best-effort next-launch choice.

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
artifact set declared by their persisted owner format. A canonical ASDF owner
protects its one document path; an existing or explicitly adopted legacy NPY
owner protects both the selected result path and its derived `.sf-labels.json`
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
contains the selected `.asdf` output path, its canonical owner format, an
`initial_publication_pending` phase, and the pending recovery overlay. No ASDF
write begins until that checkpoint succeeds. The controller then publishes one
full canonical document, reopens and validates that generation, refreshes the
one-file lease, and only then adopts the formal owner. If publication or reopen
fails, a compensating checkpoint restores the existing temporary-draft behavior.
If compensation cannot commit, the formal pending checkpoint and lease are
retained and maintenance is explicitly scheduled; once the checkpoint is
durable and can be re-read, it is reconciled into a true output-free temporary
draft. Restart performs the same reconciliation rather than treating the path as
an established owner or an unowned orphan.

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

The catalog and labeling exceptions do not grant multi-writer merge semantics
to unrelated shared settings or caches; those retain their existing policies.
In particular, the shared ImGui layout uses a complete-snapshot, shared
last-completed-writer-wins protocol. ImGui automatic disk I/O is disabled;
startup reads the existing snapshot only when it passes a conservative
structural check, and each save writes a unique sibling temporary file, closes
it successfully, then atomically replaces the shared target. A writer crash
therefore leaves the previous complete snapshot or the new complete snapshot,
while an incomplete temporary sibling is ignored. A malformed existing
snapshot is treated as defaults and is repaired by the next successful explicit
or orderly-shutdown save. There is no merge or live synchronization between
instances. Any future cache that needs reconciliation requires its own bounded
domain decision rather than inheriting catalog merge behavior. Automation
instances retain their existing isolated state roots and are outside this
ordinary multi-instance contract. The test-only two-process labeling smoke
runner is a deliberate exception: it launches direct GUI copies with an
explicit persistent-output fixture opt-in and a runner-owned shared temporary
root to exercise the production target leases; it is not a user-facing
automation or state-sharing mode.

The spectrum view cache is also an ordinary complete-snapshot setting. It owns
both global curve-color selections and the optional source-scoped locked
viewport. Raw-spectrum, Gaussian-smoothing, and median-smoothing colors are
recorded regardless of viewport lock state. Each selection stores either Auto
or an explicit RGBA value; Auto does not persist a theme-resolved color. These
color preferences are global to the local user-state root rather than associated
with a source collection.

Axis limits are recorded only when the logical spectrum view is locked and are
associated with the active source-collection identity; an unlocked snapshot
records that no locked viewport should be restored. Startup may reuse the limits
only after deferred source restoration resolves to the same source-collection
identity. A missing, malformed, changed, or unavailable identity leaves the view
in automatic-fit mode without discarding otherwise valid global curve colors.
Concurrent GUI instances do not merge or live-synchronize either part of the
spectrum view state. Every atomic replacement writes the complete cache, and the
last completed replacement wins for both curve colors and viewport state.
