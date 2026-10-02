# Sample Labeling Architecture

Spectiary models per-sample data as sample annotation results and treats storage
formats such as NPY and CSV as annotation I/O adapters, not as the domain model.
New formal labeling owners use the canonical `spectiary.sample_labeling` schema
`2.0.0` in one ASDF document. Legacy NPY results and adjacent portable
metadata sidecars are read-only compatibility inputs. Importing NPY or CSV
creates an output-free draft; explicit Save As establishes an ASDF owner
without rewriting the import files. Legacy registrations reopen their unchanged
NPY/sidecar inputs for explicit ASDF migration, with no legacy autosave or
persisted pending overlay.
See [recovery compatibility](../reference/labeling/legacy-labeling-recovery-audit.md).
Sample labeling registrations, workflow/session settings, and output paths
belong to ordinary local state; pre-canonical draft content has a separate
best-effort checkpoint. Pending formal value and metadata edits remain in memory
until successfully published to the canonical document, with no local durable
recovery payload. Save/retry state remains runtime only. Canonical documents
contain task content and provenance, not local workflow state. Annotation I/O
belongs behind a domain or service boundary; UI code consumes loaded annotation
results and save state rather than parsing dtype, shape, ASDF blocks, or
file-write capabilities.

The runtime task groups local navigation preferences in `SampleLabelingSessionState`,
output ownership/write tracking in `SampleLabelingPersistenceState`, and derived
counts in `SampleLabelingStatistics`. Separate labeling-specific wire structures
encode ordinary registration/session state and pre-canonical draft content.
Formal content belongs only to ASDF; dirty/retry state stays in memory.

`SampleLabelingController` is the single mutable owner of active tasks, drafts,
output-save state, retry scheduling, and the labeling state cache. Callers
receive a borrowed read-only view with a revision and submit atomic domain
operations. View pointers are valid only until the controller's next mutation
or destruction and must not be retained across command submission or
maintenance. Operation results report the resulting revision and output/state
persistence status, including rejected operations. `SampleWorkflowCoordinator`
still owns navigation order, sample filtering, sorting, and undo coordination. A
labeling write may request advance, but only the coordinator resolves and
applies that navigation request.

Workflow mutations cross the coordinator/session boundary as one complete
transition outcome. The outcome carries domain change flags, navigation
results, any snapshot index that must be loaded next, command-specific status,
and whether the session view must be invalidated. `SourceCollectionSession`
consumes that outcome while retaining ownership of the source roster,
activation, background follow-up, and presentation lifecycles.

Explicit source opening crosses the session boundary through `PlanSourceOpen`
and `CommitPreparedOpen`. Planning cancels pending sample navigation and captures
the source candidate, explicit member, target row, annotation attachments, and
reuse evidence together. It performs no filesystem probing; cancellable worker
preparation resolves and validates the candidate. The activation transaction owns
intent epochs, queueing, cancellation, and completion admission. After admission,
the session checks the prepared member's listing generation and coordinates
source adoption. The workflow coordinator validates known-source reuse and
reconciles prepared workflow state against live navigation and labeling state;
a newer live revision is reconciled rather than overwritten or blindly rejected.
Rejected results retain the existing session and return resources for background
retirement. Presentation of an admitted load failure follows the source-load
failure contract separately from adoption.

The coordinator's `CommitPreparedSource` returns one complete disposition:
`Adopt`, `FollowUp`, `NavigationCanceled`, or `Rejected`, together with the
committed row, follow-up target, pending-navigation completion provenance,
change flags, diagnostics, and resources for retirement. The session does not
query pending or current navigation to interpret that result. Prepared workflow
adoption, retargeting, cancellation, and remembering labeling positions remain
inside the coordinator.

Only `Adopt` permits a roster write. A prepared row reconciled to another row
never enters the roster, resident snapshot history, or presentation, including
non-activating loads and late canonical-labeling lease refreshes. Early retargets
retain the previous complete workflow and presentation; a late labeling refresh
keeps its final target deferred in the navigation owner until the matching
snapshot arrives. Deferred navigation retains its source path in the navigation
owner, and session follow-ups carry that path together with the target row.
Queueing and cancellation never infer the navigation source from the retained
roster snapshot. Snapshot reuse and presentation require both source and row to
match. The final completion publishes the matching snapshot once.
An empty navigation sequence and explicit display outside that sequence retain
their existing contracts.

Ordinary workflow transitions and maintenance finish through the same coordinator
path. Their outcome binds any requested row to its source and identifies any
source follow-up to cancel; an empty effect retains existing work. The coordinator
compares pending targets before and after the entire transition, so a matching
target can upgrade remembered-position semantics without replacing its worker,
and intermediate filter targets superseded by undo never escape. Session consumes
these effects without comparing pending navigation state or retaining a staging row.

These are the source-opening, prepared-navigation, and ordinary follow-up slices
of #47. Other stable session projections remain follow-up work. The
coordinator and labeling controller ownership and independent persistence
lifecycles are unchanged.

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

The canonical document separates task identity, editable task name, user-selected
output filename, Spectiary schema identity, build-generation provenance and ASDF
container versions. Renaming or moving an output must not change the task's
identity; a container version must not be mistaken for an application schema.

Schema 2.0 uses one explicit by-index alignment to the sample roster. It does not
introduce a general alignment subsystem. Canonical timestamps describe semantic
content mutations, while generation provenance identifies the binary that
produced the durable document. A retry retains the mutation's timestamp rather
than inventing a new edit. The producer declaration does not import local Git,
release-sidecar, compiler or machine state at runtime.

Rewrites begin with a validated durable generation. Unknown-field preservation
is bounded by the supported tree and roster; it is not an arbitrary ASDF
round-trip promise. Task lineage, edit history, document revisions, incremental
patches and local workflow/retry state remain outside the canonical format.

The complete field rules, versions, timestamps, provenance, preservation limits
and wire-tree example are maintained in the
[canonical ASDF schema](../reference/labeling/canonical_asdf_schema.md).
The [persistence ownership contract](../reference/labeling/labeling_persistence_ownership.md)
owns the separate ordinary-state/checkpoint envelopes and bounded legacy input
rules. [ADR 0011](0011-project-identity-contracts.md) records the format-name
cutover; historical names are not ongoing canonical read aliases.

## Local persistence failure semantics

The C++ runtime mirrors this ownership split. `SampleLabelingContentView`
borrows only identity, canonical metadata, labels, and a complete value vector;
canonical document construction accepts that view. Local session preferences,
persistence/retry state, and derived statistics have separate state types.
`SampleLabelingValues` holds either complete values or an explicit sparse
overlay containing the source sample count and pending row values. An absent
overlay row is unknown, including when an explicit pending value is `-1`.
Sparse state cannot yield a content view. Deactivation drops complete values
into the sparse representation; canonical projection restores complete content
from the ASDF base before applying pending edits. This is an implementation
boundary that does not change canonical schema 2.0. Sparse edits are no longer
serialized for formal tasks. See the [persistence field audit](../reference/labeling/labeling_persistence_ownership.md).

Source-session, navigation, labeling ordinary state, labeling draft checkpoints,
and workflow state have separate JSON content owners. Their codecs own schema
support and field validation. The two labeling JSON owners share one commit
coordinator, save/retry scheduler, and persistence-health status in
`SampleLabelingController`; they are not independent persistence lifecycles.
A labeling commit reloads and validates both owners under the same commit lock,
applies the task patch, then attempts ordinary-state and checkpoint replacement.
An untrusted document in either labeling owner stops that combined local commit
before either is replaced; canonical output saves remain independent of that
failure. The session aggregates the health reported by each persistence controller.

| Condition | User signal | Continue? | Clear condition |
| --- | --- | --- | --- |
| Missing cache | None; use defaults | Yes | Not applicable |
| Corrupt cache | Non-blocking owner warning; a later write attempt reports retrying | Yes, with read-only salvage or defaults; labeling cache commits fail closed and preserve the original bytes | The cache is repaired or removed, then its persistence controller successfully commits against the trusted latest state |
| Unsupported schema | Non-blocking owner warning; a later write attempt reports retrying | Yes, without reading unsupported state; labeling cache commits fail closed and preserve the original bytes | Supported state replaces the cache or the user removes it, then its persistence controller successfully commits; labeling cache schemas 1–3 are not migrated |
| Partial save | Overall `retrying` health naming failed local-state components while retries remain scheduled | Yes; attempt every other dirty cache | Each failed component succeeds; labeling ordinary state and checkpoints clear their shared status only after a successful combined commit attempt |
| Retrying | Overall `retrying` health; existing retry deadline remains active | Yes | First successful retry |
| Recovered | Overall recovery health | Yes | Next user mutation owned by the recovered cache |

There is no cross-file transaction. Once labeling validation succeeds, a failed
ordinary-state replacement does not suppress the checkpoint attempt when its
previous contents can still be trusted; checkpoint removal waits for successful
registration. Either replacement may succeed without the other, and the labeling
controller retains the patch for its shared retry scheduler. Other persistence
controllers still attempt their dirty caches. Normal shutdown consumes
per-controller flush results so an incomplete flush is not reduced to an ignored
aggregate boolean.
