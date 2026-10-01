# Source Loading Dependency Boundaries

## Decision

`SourceCollectionLoadQueue` owns asynchronous source collection loading: worker
lifetime, cancellation, completion ordering, speculative work and retirement.
Preparation remains a private implementation collaborator. Callers submit source
requests and consume completions; they do not assemble preparation steps.

For issue #45, replace the nine
callback fields in `SourceCollectionPreparationAdapters` with four narrow
dependencies in `SourceCollectionLoadDependencies`. This is a reduction in
replaceable behavior, not just a renamed container. The preparation class is no
longer included or invoked by tests. Its internal header is used only by its
implementation and the load queue implementation.

## Field Audit

| Previous field | Decision | Reason and replacement coverage |
| --- | --- | --- |
| `snapshot_loader` | Keep | File decoding is the existing format boundary; slow, failed and canceled decoders exercise queue scheduling and publication without depending on disk timing. Built-in format algorithms retain direct tests. |
| `folder_snapshot_loader` | Keep | Decodes the selected member from an already enumerated folder; controlled decoders test concurrent directory changes and speculative races. Folder enumeration and validation remain internal. |
| `folder_scanner` | Remove | Only one production algorithm. Call `ScanSourceCollectionFolder` directly. Real temporary directories and existing load latency reports verify listing refresh/reuse; removal before enumeration exercises the real error path. |
| `source_open_probe` | Keep | An external-open request performs filesystem metadata I/O before decoding. This concrete blocking boundary must stay off the UI thread and honor cancellation checkpoints. `TestSourceOpenResolutionRunsOnWorkerAndCancels` needs a controlled pause specifically before resolution, which a paused decoder cannot cover. The pure resolution algorithm remains directly testable. |
| `folder_change_generation_factory` | Remove | Duplicates the lower registration boundary and bypasses the real monitor. All generation tests now pass through that monitor. |
| `folder_change_generation_registration_factory` | Keep | #89 establishes notification lifetime and requires deterministic cancellation, timeout, uncooperative registration and late-result release tests. See ADR 0008 and the directory registration validation record. |
| `workflow_cache_loader` | Remove | Cache reading has one implementation; isolation is represented by explicit cache paths. Tests read real absent or populated caches and verify the batch shares the same immutable cache object. |
| `file_context_builder` | Remove | Internal orchestration of identity and manifest construction. Tests inspect real prepared payloads, reuse proofs and existing context-reuse reports. |
| `folder_context_builder` | Remove | Internal orchestration using the real listing. Folder mutation, preferred-member and navigation tests run the production context builder. |
| `workflow_cache_paths` | Keep configuration | Portable/local state location and load policy are production configuration, not a replacement algorithm. As of #103-A, all-empty paths use the supplied startup context only; a detached empty context disables persistence. Partially specified paths remain invalid. Tests explicitly select isolated paths. See ADR 0012. |

The queue's existing test execution options are separate: worker limits exercise
bounded scheduling, and `before_worker_start` reproduces thread-creation failure.
Neither exposes preparation stages. No generic injection container, preparation
pipeline hooks or replacement algorithm layer is introduced.

## Test Surface

Preparation and source-session integration tests drive `Enqueue`, `EnqueuePrefetch`,
`EnqueueBatch`, `Cancel` and `TakeCompleted`. Test drivers only submit and wait with
a deadline; they cannot replace preparation steps. Existing latency reports
replace injected scanner/context counters. Direct monitor tests remain appropriate
for the OS boundary and do not require source parsing to reproduce a registration
failure.

`TestProductionQueueLoadsRealSourcesAndReusesGenerations` uses the public production
constructor with isolated cache paths and no injected dependencies. It loads real
CSV files, navigates a real folder, prefetches, reuses unchanged context/listings,
observes a native directory change and refreshes, resolves an external preferred
member, and checks missing-source failure publication. Other integration tests may
control decoding or OS timing while the remaining preparation executes normally.
Restore, follow-up navigation, live workflow reconciliation, stale-result rejection,
completion ordering and retirement retain their existing assertions.

Test cache fixtures must not silently load the developer's saved state. Empty
fixtures use unique absent paths; populated cache tests use their explicit fixture
paths. The batch test checks shared cache identity instead of replacing the reader
with a counter. Production cache defaults and storage formats are unchanged.

## Relationship to #40 and #89

#40 reserves user-extensible spectrum loaders. The retained decoder functions are
not the future external API/ABI. A future loader module owns format parsing and
valid-point selection, while queue scheduling, cache preparation, folder generation
tracking and workflow state stay here. This change neither designs a plugin system
nor makes preparation dependencies available to loader authors as a plugin contract.

#89's registration thread and cancellation/timeout/late-result semantics are
unchanged. The lower factory is deliberately retained even though production has
one Win32 registration implementation. Documented OS fault and lifetime contracts
justify that seam; simply counting production adapters would lose needed coverage.
No hard bound on a permanently stuck native call's shutdown is claimed here.

Future changes must identify which production behavior varies, or which specific
OS failure/lifetime contract needs deterministic testing, before adding another
dependency. Moving fields to another header or reducing a field count alone is
not sufficient evidence of a better loading boundary.
