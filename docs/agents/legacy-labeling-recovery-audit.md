# Legacy labeling recovery audit (#93)

This audit records the compatibility boundary before retiring the NPY+sidecar
publisher. It is not a promise to retain legacy autosave.

## Supported recovery input

`sample_labeling_state_cache_io.cpp` currently accepts cache schema 4. A task's
`output.format: legacy_npy_with_sidecar` identifies its NPY base. The cache
stores pending assignments and clearing operations in `pending_values`, while
task name, label definitions, canonical metadata and `metadata_pending` retain
unsynchronized metadata. These are accepted edits, not disposable diagnostics.

On hydrated load, the reader loads the NPY values and then applies the sparse
overlay. Loading without result hydration applies the overlay to a placeholder
array; that projection must not be treated as a complete recovered result.
An unreadable base cannot establish the values outside the overlay. Retirement
must preserve the record and report the missing input rather than replace those
values with the unlabeled sentinel or discard the task.

## Bounded migration route

The existing `MigrateActiveLegacyTaskToCanonicalAsdf` route can publish a fully
hydrated recovery projection directly to ASDF without first writing the legacy
pair. It checkpoints the legacy recovery record, creates and reopens the ASDF
generation, then checkpoints the owner switch. Only after adoption succeeds does
it release the old artifact leases. Publication or adoption failure leaves the
legacy recovery record available for another explicit migration attempt.

`TestRecoveredLegacyPendingEditsMigrateWithoutLegacyPublication` covers schema-4
recovery of an assignment, a clearing operation and metadata edits, migration,
subsequent canonical editing, and reopening. It also counts legacy publisher
calls and checks both original files byte for byte. Existing migration tests
exercise publication/checkpoint failure and output conflicts.

## Remaining retirement work

- Prevent new NPY promotions from acquiring an editable legacy owner; use the
  normal temporary-task and explicit canonical Save As lifecycle.
- Keep old owners as recovery/import sources with no ordinary edit, autosave or
  retry dispatch. Preserve their pending overlay until explicit ASDF adoption.
- Remove the production legacy publisher and its injected publishing seams,
  migrating persistence tests to canonical owners while retaining historical
  input fixtures in test support.
- Remove legacy publication-only lock handling. Preserve protection against
  exports overwriting a loaded import source or another task's canonical owner.
- Update product wording and architecture documentation when the runtime
  transition is implemented; NPY annotation reading and stateless export remain.
