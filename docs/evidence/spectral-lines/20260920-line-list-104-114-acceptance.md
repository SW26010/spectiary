# Line list #104 / #114 acceptance — 2026-09-20

Implemented on `master`, following `.scratch/104and114plan.md` and the user's
subsequent clarification of marker-array semantics. The original implementation
session made local commits without pushing or closing issues; those commits have
since been published to GitHub.

On 2026-09-20, a read-only GitHub API check confirmed remote `master` at
[`36b09ef`](https://github.com/SW26010/spectiary/commit/36b09effb7aeb3f09ec6998409b5b2742f728b5c).
For that exact SHA, workflow runs, check runs, and commit statuses each had a
count of zero. The combined status API reported `pending` with zero statuses;
this is not evidence of a running or passing CI job. The results below are local
validation evidence, not GitHub CI results. This is a dated snapshot, not a claim
about future remote heads. No workflow triggers were changed.

## Delivered scope

- One canonical `SpectralLineList` model and strict, bounded public v1 JSON
  codec, including Unicode, coordinates, grouping views, color schemes,
  validation and atomic writes. Packaged data now uses this production codec
  for both external Debug resources and embedded static Release resources.
- Built-in content remains immutable. The internal overlay appends owned views
  and optionally shadows the complete color-scheme collection; absent, empty,
  and populated overrides remain distinct. Session choices, visibility,
  expansion, and generated-name metadata are separate from document content.
- Internal state schema 7 accepts the immediately preceding schema 6 through
  explicit migration. Invalid or unsupported durable input fails closed.
  Saves acquire a bounded commit lease, reload, reconcile task-owned changes,
  validate, and atomically replace the file. Failed writes retain pending edits.
- Controller and plot consume the composed canonical model. Unassigned is a
  derived UI projection. Legacy TSV loading and duplicate catalog/user-state
  models were removed. Open/editor/save workflows remain with #65–#67.

`marker_ids` is an ordered JSON array whose order round-trips without destructive
normalization. v1 assigns no presentation, priority, or scientific semantics to
that order. This supersedes the original issue wording about marker order.
Migration preserves reference order; validation checks identity/reference
integrity, not ordering. UI projection may sort by wavelength. There is no
marker-reordering operation or marker-order merge intent. Group ordering remains
an explicit operation.

## Review and regression coverage

Overall review covered public parsing/validation, base ownership, overlay
composition, migration rejection, first color edits, whole collection
clear/restore, concurrent mutations, atomic replacement, and runtime packaging.
Review fixes include keeping default visibility, preserving other memberships
when moving one occurrence to Unassigned, and remapping generated-name metadata
alongside concurrently colliding view/group IDs. Regression tests cover these
cases, controlled conflicts for edited entities deleted by a peer, lease
contention/retry, and simultaneous color edits from two actual child processes.

No unresolved P0/P1 issue was found in this review. This is a code and automated
test assessment; no manual interactive GUI visual acceptance was performed.

## Validation

All MSVC builds used `scripts/build-ninja-msvc-debug.ps1` with the required
escalation. Debug full build and static Release application/test builds passed.

| Check | Result |
| --- | --- |
| Debug `ctest --preset fast -j 4` | 90/90 passed |
| Debug `ctest --preset extended -j 3` | 9/9 passed |
| Final Debug line-list, packaged-data, state, controller tests | 4/4 passed |
| Final static Release same four tests | 4/4 passed |
| Static Release `spectiary_release_artifacts_tests` | 1/1 passed |
| `git diff --check` | Passed |

The full presets preceded the final marker-order clarification; both final
four-test runs cover that clarification and the embedded-resource regression.
Release artifact validation exercised actual static packaging and its metadata
and resource contracts. Initial sandbox-only failures accessing system/temp
paths were rerun with the required access and passed.

Local detailed test output is retained under `.scratch/104114-*.log`; build logs
are under `logs/build/`. These transient logs are not committed.

## Follow-up cleanup — 2026-09-20

The controller vocabulary now uses `SpectralLineStateIntent`,
`SpectralLineStateResult`, and `SpectralLinePanelView`, with `line_list_*` and
`has_base_grouping_view` fields. Callers and localization helpers were renamed
without adding abstractions or changing behavior. Historical serialized IDs,
schema 6 fields, and stable UI localization keys remain compatible.

Persistence load diagnostics now use an explicit enum switch instead of relying
on equal enum ordinals. The Debug application and affected tests built
successfully through the MSVC wrapper. Controller, shell source-load activation,
and UI text tests passed 3/3; `git diff --check` passed. The earlier full-preset
and Release results above were not rerun for this cleanup. Local follow-up test
output is `.scratch/104114-cleanup-tests.log`.
