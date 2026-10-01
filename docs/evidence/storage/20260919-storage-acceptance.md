# Storage final joint acceptance — #103, stage 8

Status: dated acceptance evidence. Current storage and labeling ownership are
maintained in [runtime data boundaries](../../development/runtime_data.md#运行身份与存储边界),
[project identity](../../development/project_rename.md), and
[labeling persistence ownership](../../reference/labeling/labeling_persistence_ownership.md).

Publication follow-up (2026-09-19): the final commits below were subsequently
published and [#103](https://github.com/SW26010/spectiary/issues/103) closed after
the [final joint acceptance review](https://github.com/SW26010/spectiary/issues/103#issuecomment-5739583887).
The original pre-publication record below is retained; this follow-up adds no new
test or CI result.

Date: 2026-09-19. Scope: audit the combined stages 1–7 against the final #103
contract, with focused regression tests and documentation corrections. This is
not another storage refactor. All changes and the closure summary below remain
local; no push, issue comment, or GitHub closure is included in this pass.

## Acceptance matrix

Tests listed below are CTest targets with the `spectiary_` prefix and `_tests`
suffix unless an individual test function is named.

| Contract | Evidence |
| --- | --- |
| Portable data root equals package root; LocalAppData uses `Spectiary`; package resources retain package meaning | `runtime_storage_context`: both profiles, every actual config/state/checkpoint/log path, independent public catalog path |
| Root failure is explicit, with no profile/temp/CWD fallback; Portable does not require LocalAppData | `runtime_storage_context`: throwing/empty/relative platform roots, invalid overrides, non-directory ancestors in both profiles |
| Downstream locators retain startup context | `runtime_storage_context`: relocated session restoration and background workflow preparation; production default startup resolution remains confined to `runtime_paths.cpp` |
| Exactly one active layout; legacy is input only | `runtime_storage_context`: concurrent allowlisted import, existing corrupt destination wins, new state survives repeated startup, blocked config/state import and ordinary writes fail without modifying legacy |
| Migration is flat and non-destructive | `runtime_storage_context`: malformed input, unknown nested files, old logs, root-level canonical and nested source sentinels; blocked import/retry/reset preserve all other owners |
| Only four reserved children are application-owned | `runtime_storage_context`: root-level and arbitrary nested user files allowed; reserved directories/descendants, traversal and Win32 aliases rejected. `source_collection_preparation`: reserved sources/annotations/companions. `source_collection_session`: reserved canonical Save As/adoption/restoration; user-subdirectory ASDF survives migration. Export admission also reviewed in the controller |
| Portable inside-package locators relocate; outside-package locators stay absolute | `runtime_storage_context`: package relocation plus external user file matrix |
| LocalAppData user locators stay absolute, including user files inside its data root | `runtime_storage_context`: package file and data-root user file round trips |
| Malformed locators fail closed; no basename rebasing | `runtime_storage_context`: relative-as-absolute, rooted/drive/UNC/traversing package references, NUL/unknown kinds and missing legacy absolute path |
| Canonical file is sole durable content owner | `sample_labeling_filter`: `TestSplitDraftCheckpointCleanupAndClose` now runs both real profile layouts with root-level user-selected ASDF; checks registration has no content/retry payload and canonicalized checkpoint becomes empty |
| Failed canonical saves do not persist pending recovery overlays | `sample_labeling_filter`: `TestFailedCanonicalEditsRemainRuntimeOnly`, `TestCanonicalOwnerNeverRestoresLocalPendingCode`, `TestCanonicalWriteDoesNotWaitForStateCommit`; `sample_labeling_persistence_owner` rejects persistent overlays |
| Atomic writes and orderly failure handling survive cutover | `sample_labeling_asdf_store`, `sample_labeling_asdf_mutation`, `atomic_file`, and labeling controller publication/reopen failure cases |
| `unsaved/` is pre-canonical, hidden, atomically replaced; cleanup does not acquire other owners | `sample_labeling_persistence_owner`; both-profile lifecycle test injects checkpoint/registration sharing denial and malformed checkpoint, then checks previous canonical file and config/state/log/cache/source/other-checkpoint sentinels |
| Grouping views remain one state owner; spectrum roles remain split | `runtime_storage_context`, `spectral_line_user_state`, `spectrum_view_session` including partial migration, corrupt destination, publication race and legacy retirement |
| Cache stays disposable; resets remain independent | System temp/cache path assertions; actual preference reset/session clearing preserve cache, logs, checkpoints and user files; no persistent `application_data_root/cache` created |
| Portable package has the final role directories | `portable_runtime_state`, metadata and release artifact contract tests; build/package scripts retain the four-directory manifest |

## Code review boundaries

Reviewed startup resolution/migration, locator admission, domain migration,
labeling ownership/cleanup and build/package cleanup. Active paths use the final
roles; `Data` and `%LOCALAPPDATA%/SpecForge` are explicit migration inputs.
Remaining old-brand format strings are bounded readers or historical fixtures,
not unexplained active storage owners. No new locator kind is needed.

Application production code does not recursively delete the application data
root. Spectrum migration retires only its exact known legacy state leaf after
healthy split owners exist. Checkpoint cleanup updates its specific owner file.
Package generation removes its validated build output under `dist`; it is not
an installed application reset/uninstall operation. There is no general runtime
root-cleanup/uninstaller API to exercise, and this stage introduces none.

The whole application root is not an exclusive application namespace. A future
reset/uninstaller must preserve user-selected files outside the four reserved
children. Tests use byte sentinels for this boundary, and actual ASDF publication
for canonical ownership. Cache sentinels establish separation; they do not imply
a new cache cleanup feature or retention policy.

No #104 ViewProfile split, canonical schema redesign, document framework,
migration framework, identity redesign or new crash-recovery guarantee is added.
The previous canonical write/close and best-effort pre-canonical contracts remain
as specified in [labeling persistence ownership](../../reference/labeling/labeling_persistence_ownership.md).

## Validation

Windows tests run outside the restricted sandbox because they need normal
system-temp and Windows file-sharing APIs. Configure/build use the repository
MSVC wrapper. No production behavior change was required by this acceptance pass;
the optional #103-8B fix commit is therefore omitted.

| Command / check | Result |
| --- | --- |
| `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 -Configure -TimeoutSec 180` | Passed |
| Same wrapper without `-Configure`, `-TimeoutSec 240` | All Debug targets built; changed test targets rebuilt after final assertions |
| `ctest --preset fast` | 89/89 passed; final `runtime_storage_context` additions rebuilt and rerun successfully |
| `ctest --preset extended` | All 9 targets passed across the initial run and corrected labeling rerun: 8/9 initially; full `sample_labeling_filter` rerun passed in 68.15 s |
| `ctest --test-dir build/ninja-msvc-debug -R '^spectiary_(release_artifacts\|portable_pe_contract\|release_workflow_structure_contract)_tests$' --output-on-failure` | 3/3 passed |
| Final combined diff / ownership review and `git diff --check` | No P0/P1 findings; no workflow trigger changes |

The first extended run exposed a new test assertion comparing a newline-normalized
text helper with a raw sentinel string. The assertion now compares raw bytes;
this was a test defect, not a production deletion. The affected full labeling
target passed after that correction. Native GUI integration, long-running
resource stability, fresh static Release builds and the pinned external ASDF
oracle are outside this local storage acceptance run; no result is claimed for them.

The test changes are recorded in local commit `a78d1c0085bd`
(`test(storage): complete final ownership and path acceptance matrix`). The
following documentation commit records this final acceptance. No P0/P1 issue
remained identified by this review and the executed suites.

## Local #103 closure summary

Stage 8 completes the final ownership/path acceptance matrix for the combined
storage cutover. Both deployment profiles retain one active role layout, explicit
root failure, startup-bound locators and reserved-child admission. Failed migration,
preference reset, session clearing and checkpoint cleanup preserve user-owned
source/canonical files. Canonical content remains owned solely by its ASDF file;
application state stores registration/session information and `unsaved` serves
only pre-canonical work. Grouping views remain intact under `state`; disposable
caches remain outside persistent storage.

ADR 0015 and current engineering documentation match the final contract. This
summary is prepared for the later #103 progress/closure comment, after the local
commits are published. It does not claim an already-closed GitHub issue or CI run.
