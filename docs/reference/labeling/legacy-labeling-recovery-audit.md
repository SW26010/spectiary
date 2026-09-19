# Legacy labeling recovery audit (#93, superseded by #108/#109)

The retired NPY/sidecar publisher remains retired. New NPY/CSV promotions create
output-free drafts. Formal legacy registrations are read-only migration inputs;
task/output leases continue protecting both legacy artifacts.

## Current bounded cutover

The old schema-4 monolith is read only when neither split labeling owner exists.
Compatible pre-canonical content may become a checkpoint. Formal pending values,
metadata, errors and retry flags are not carried into new persistence. Reopening
a legacy owner reads its actual NPY values and matching sidecar. Missing or
mismatched input remains non-publishable; unknown rows are never filled from a
placeholder and presented as complete data.

`MigrateActiveLegacyTaskToCanonicalAsdf` acquires destination leases, publishes
and validates a new ASDF, then switches ordinary registration. It has no
pre-publication WAL. Original NPY and sidecar bytes remain unchanged. A complete
ASDF may remain unregistered if publication succeeds but registration fails.

Tests cover canonical migration from unchanged legacy content, publication and
registration failures, path conflicts, lease protection, and unchanged input
bytes. The former promise to persist formal sparse recovery overlays is
superseded by [labeling persistence ownership](labeling_persistence_ownership.md).

Runtime editing and autosave do not publish legacy artifacts. Stateless NPY
export and annotation ingestion remain available. Historical publishers live
only in test fixture helpers.