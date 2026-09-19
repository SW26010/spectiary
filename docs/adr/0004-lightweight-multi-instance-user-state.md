# Lightweight Multi-Instance Runs Share User State

Spectiary supports concurrently running ordinary GUI processes as independent
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

Built-in spectral-line customization retains startup snapshots, with no watcher,
IPC owner or live propagation. The #104/#114 cutover replaces the old catalog
monolith with a complete canonical SpectralLineList plus an internal overlay and
separate session state. The same application-managed state file now has schema 7;
only immediately preceding schema 6 is a bounded read-only conversion input.
Canonical user-owned line-list saves do not inherit this merge policy.

The built-in adapter holds `<state-path>.commit.lock` through reload, trust
validation, explicit task reconciliation, effective-model validation and atomic
replacement. The OS file handle owns the short-lived lease. Corrupt, unsupported,
wrong-identity or unresolved state fails closed and is left untouched; a valid
packaged base may still be shown with a nonblocking diagnostic. Missing state is
normal first-write state. No startup repair of canonical references is allowed.

The overlay reuses canonical grouping/color submodels. Base grouping views append
with editable overlay views; an optional color collection shadows the entire base
collection. Absent means inherit, present-empty means no schemes. First mutation
copies the effective collection but does not claim every copied field. Ordinary
color reconciliation owns `(scheme_id, marker_id)`, preserving disjoint edits even
for simultaneous first overrides. Whole collection clear/replace/restore owns the
collection at that commit. Deleted edited entities cause controlled conflicts
rather than implicit resurrection. No durable tombstones or history are added.

Grouping names, memberships, identities and explicit ordering retain the bounded
entity merge. Session selections, visibility, expansion and localization metadata
are reconciled separately; automatic fallback has no explicit selection authority.
Unassigned is a UI projection, with no stored group or flag. Migration retains
ordinary IDs and stored representation order, preserves compatible session state, and is
validated/published under the same lease. The complete operational contract is
[spectral-line overlay persistence](../reference/spectral-lines/spectral_line_grouping_views.md#concurrent-user-state-write-contract).
Labeling is a separate exception because canonical ASDF documents and
pre-canonical drafts represent user-authored work. Canonical content belongs to
the user-owned ASDF; local registration/session state and draft checkpoints have
separate application-managed owners. Legacy NPY results and adjacent metadata
sidecars are read-only migration inputs. Different labeling targets may be
edited by different instances concurrently, but one logical target may have
only one Spectiary editor. A target-scoped operating-system lease protects the
editing lifetime. Shared labeling-state commits take a separate short-lived
cross-process lock, reload both JSON owners under that lock, apply only
task-level upserts or explicit deletion tombstones, validate the task and output
identities, and atomically replace each document. Ordinary task saves do not
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
protects its one document path. A recovered legacy NPY record temporarily
protects its result and `.sf-labels.json` migration inputs until explicit ASDF
adoption succeeds. These leases prevent conflicting adoption or export; they
do not enable legacy publication or autosave.

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

Labeling persistence has two application-managed owners: ordinary registration
and session preferences in `state/sample-labeling-state.json`, and complete
pre-canonical checkpoints in `unsaved/sample-labeling-drafts.json`. Their codecs
validate separate content, while the labeling controller shares commit
coordination, retry scheduling, and persistence health across both owners.
Under the existing labeling commit lock, ordinary-state replacement precedes
checkpoint replacement; either can fail independently. There is no cross-file
transaction, and failed checkpoint cleanup cannot roll back canonical
publication or a successful registration. See the
[field audit and lifecycle contract](../reference/labeling/labeling_persistence_ownership.md).

Temporary-to-formal conversion publishes a complete ASDF from the current
in-memory draft, reopens and validates it, and only then adopts the canonical
owner. Registration follows publication, then checkpoint cleanup is best-effort.
Failed publication keeps the current draft in memory; no write-ahead phase or
compensating transaction is persisted. A crash after publication may leave a
complete ASDF plus a stale checkpoint. Matching canonical registration
supersedes that checkpoint without rewriting the ASDF.

Formal edits attempt atomic ASDF saves directly. Failed edits and metadata,
retry deadlines and error presentation remain in memory only. Dirty tasks keep
their existing task/output leases across source switches until their edits are
saved or the process ends. Local-state contention does not gate canonical
publication. Abnormal termination can lose edits since the last successful
canonical write. Normal close prevents silent loss of known pending canonical
edits; OS session end is not vetoed merely to protect draft checkpoints.

The complete artifact-set lease prevents different stale task IDs or result
extensions from creating parallel editors for one logical draft or output;
physical alias exclusion applies when the optional object identities resolve.
Draft creation acquires the source draft-slot lease before deriving a unique
task ID from the latest cache and then acquiring that task's stable lease; an
existing formal task with the requested ID is never treated as a successful
create operation. A create patch also carries an expected-absent precondition,
so a draft retained after an untrusted-cache failure cannot later replace a
same-ID task introduced while the cache was repaired.

Schema-4 monoliths are bounded migration inputs only while neither new owner
exists. Ambiguous multiple-draft slots cannot be written into the new checkpoint
schema; they fail closed rather than being silently merged. Formal pending/WAL
payloads are not carried forward as a second content owner. Migration/reset never
rewrites or removes user-owned canonical files.

The labeling lease coordinates writers only. It does not invalidate annotation
snapshots already loaded by another instance when that labeling output changes.
Displays, sample filters, and sorting derived from such an annotation may
therefore continue to use the older values until the user explicitly reloads or
reopens the data, or restarts that instance. Atomic output replacement ensures
that a subsequent read observes a complete file; it does not provide live change
notification. This is expected eventual-consistency behavior rather than data
corruption, and cross-instance file watching, runtime propagation, and automatic
sample filter or sorting recomputation are not acceptance requirements of this
change. If those capabilities become product requirements, they should be
designed as a separate follow-up and do not by themselves require a primary
process or IPC configuration service.

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

Spectrum persistence has two independent complete-snapshot owners (see
[ADR 0014](0014-spectrum-persistence-ownership.md)): global curve-color
preferences in `config/spectrum-plot-preferences.json`, and the optional
source-scoped locked viewport in `state/spectrum-viewport-state.json`. Raw-spectrum, Gaussian-smoothing, and median-smoothing colors are
recorded regardless of viewport lock state. Each selection stores either Auto
or an explicit RGBA value; Auto does not persist a theme-resolved color. These
color preferences are global to the application data root rather than associated
with a source collection.

Axis limits are recorded only when the logical spectrum view is locked and are
associated with the active source-collection identity; an unlocked snapshot
records that no locked viewport should be restored. Startup may reuse the limits
only after deferred source restoration resolves to the same source-collection
identity. A missing, malformed, changed, or unavailable identity leaves the view
in automatic-fit mode without discarding otherwise valid global curve colors.
Concurrent GUI instances do not merge or live-synchronize either part of the
spectrum view. Each atomic replacement writes one complete owner document; the
last completed replacement wins independently for preferences and viewport state.
