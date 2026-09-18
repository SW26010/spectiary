# Spectrum-view automatic writeback authority (#110)

Status: Accepted. Date: 2026-09-19.

`LoadSpectrumViewStateCache` retains the existing
`VersionedJsonCacheLoadIssueKind`. ShellUi owns a startup-snapshot boolean:
missing files and supported loads permit automatic writes; `ReadFailed`,
`InvalidDocument`, and `UnsupportedFormatOrSchema` do not. Runtime fallback
remains available in every case. This does not change the JSON loader, schema,
storage path, canonical documents, or generic persistence lifecycle.

Without authority, maintenance and shutdown skip the spectrum-view lifecycle's
save operation, and its deadline does not wake the maintenance loop. Skipping
is not a save failure. The original load warning and diagnostic remain visible;
runtime color or viewport changes cannot authorize replacing the original file.
This is startup-load protection, not commit-time concurrent-writer validation.

Supported schema 1/2 field-level recovery remains deliberate: an invalid color
uses Auto for that curve; unusable locked limits use automatic range. These
recognized documents remain writable. Whole-document parsing and version errors
are protected instead of automatically repaired.

There is currently no enabled whole-spectrum-state reset action. The disabled
global erase/reset settings buttons do not grant authority. Reset to Auto for
one curve and Restore Default Layout do not authorize deleting other unknown
data in the combined document. Repairing/removing the file outside the running
application and restarting allows a supported/missing load again. A future
explicit whole-file reset must deliberately authorize replacement; this change
does not introduce that feature.

## Bounded owner audit

- `ApplicationSettings`: constructors retain warnings. Dirty transitions follow
  setting intents; ordinary `Flush()` does not mark fallback settings dirty.
  Panel visibility belongs to this same owner. No change.
- `spectral_lines_panel_controller`: startup canonicalization requires
  `loaded_cache_can_rewrite`; commits additionally validate the latest document.
  Preserve the stronger existing domain policy.
- Sample labeling: structured load issues and task commit trust checks already
  protect user work. No changes to labeling or canonical `.asdf` saves.
- Source session: a failed structured load produces an empty source list;
  `PrepareDeferredSourceSessionRestore` returns before beginning restoration.
  `Flush` only consumes an existing dirty/dirty-after-restore transition.
  No failed-load-to-startup-normalization write chain was found.
- Sample navigation: loading/adopting a cache retains a warning without marking
  dirty. `ActivatePreparedSource`, used for background startup restoration, also
  does not persist the fallback row. Subsequent live navigation and the older
  synchronous `ActivateSource` path persist the active index. The existing
  resettable corrupt-cache policy is explicitly covered by
  `TestControllerClearsNavigationLoadWarningAfterFlush`; it is not changed or
  generalized to preferences by this issue.
- Sample workflow: load/adoption and restoration of an empty fallback do not
  mark dirty. A failed document supplies no saved sort/source selections to
  normalize. Later workflow mutations can save; the existing corrupt-state
  recovery/retry contract is covered by
  `TestCoordinatorFlushesWorkflowIndependentlyAndRecovers`. No uniform permission
  layer is added.
- ImGui layout: ADR 0004 explicitly permits rebuilding malformed layout on
  orderly shutdown. This deliberate resettable-layout policy remains intact.
- Runtime resource workload configuration: failed load returns an error; it is
  not an automatic writeback owner for its input configuration.

The regression uses real ShellUi startup, maintenance, final flush and teardown.
It checks byte preservation for schema 999, malformed JSON, non-object JSON and
a read failure whose path becomes writable before flush. Missing/supported
loads remain writable, and protected loads retain diagnostics without reporting
a save failure. The existing spectrum-view tests cover field-level recovery.

Splitting plot preferences from viewport state (#111), physical relocation
(#103), and checkpoint/draft changes (#108/#109) are separate work.
