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
subsequent canonical editing, and reopening. It checks both original files byte
for byte and rejects migration when the base NPY is missing. Existing migration tests
exercise publication/checkpoint failure and output conflicts.

## Runtime boundary

New NPY promotions use output-free temporary tasks, as CSV promotions do.
Controller mutations reject legacy recovery owners. Save dispatch and retry
selection never publish them. The production publisher and its injected
constructor argument are removed. Historical result/sidecar writers and the
old output-selection helper exist only in `tests/legacy_annotation_fixture_io.h`
and `tests/legacy_labeling_test_support.h`.

The UI identifies recovered legacy records as read-only migration sources,
suppresses canonical editing and label shortcuts, and retains explicit Save As.
The legacy output-format tag remains only for schema-4 recovery, annotation
recognition and migration input protection. Its artifact leases prevent an
export or another adoption from overwriting migration inputs, with no two-file
publication branch. Stateless NPY export and annotation ingestion remain.

Controller persistence tests now exercise canonical owners. Historical cache
fixtures cover the bounded migration bridge, including missing bases, pending
values, publication/checkpoint failures and output conflicts. Session tests
cover independent NPY imports, explicit canonical Save As, source-file
preservation and canonical lease switching. Shell maintenance tests use ASDF.
