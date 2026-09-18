# Automation sample sequences

The checked-in samples under
`tests/fixtures/automation/samples/` are a small, deterministic source
collection. They are intentionally ordinary CSV files so the sample workflow
does not depend on a developer's data directory or on a downloaded artifact.

The folder contains three source rows in lexical order:

| File | Shape | Expected role |
| --- | --- | --- |
| `alpha.csv` | `wavelength,flux` plus four finite points | first source row |
| `bravo.csv` | `wavelength,flux` plus four finite points | second source row and name-navigation target |
| `charlie.csv` | `wavelength,flux` plus four finite points | third source row |

The CTest sample runner resolves this fixture directory at runtime. The command
examples below use `<fixture-directory>` and `<state-root>` placeholders; no
machine-specific absolute path is stored in the repository.

## Sequence A: source and sample-name navigation

```text
source open <fixture-directory>
wait idle
spectrum goto name bravo.csv
state get
app quit
```

Expected terminal state:

- `source.open` completes with a source collection containing three spectra.
- `spectrum.goto` completes with `spectrum.index = 1` and
  `spectrum.name = "bravo.csv"`.
- `state.get` reports the same source path, `window.visible = true`,
  `window.minimized = false`, and an idle Shell.
- `app.quit` completes through normal application shutdown.

## Sequence B: labeling and application-rendered capture

The runner first asks the built `specforge_automation_state_isolation_tests`
fixture helper to create an internal-draft labeling seed from the same source
collection. The seed is read-only input and is never written by the GUI.

```text
source open <fixture-directory>
wait idle
spectrum goto 2
label assign 5 spectrum 1
spectrum goto 1
wait idle
frame capture <state-root>\artifacts\sample-label.png
state get
app quit
```

Expected terminal state and evidence:

- The first `spectrum.goto 2` completes with `spectrum.index = 2` and
  `spectrum.name = "charlie.csv"`; both `wait idle` commands must have
  `status = completed`.
- `label.assign` completes for source row 1 with `previous_code = -1` and
  `new_code = 5`; the explicit `spectrum.goto 1` returns to the written row
  after the production auto-advance behavior.
- `frame.capture` completes for the main application viewport and writes a
  non-empty PNG whose IHDR dimensions match the terminal width and height.
- `state.get` reports `spectrum.index = 1`, current sample label code `5`, a
  successful capture, and an idle Shell.
- After `app.quit`, the production state reader reloads code `5` from the
  isolated `state/sample-labeling-state.json` and
  `unsaved/sample-labeling-drafts.json`; the seed file's hash is unchanged.

## Ownership and cleanup

Each run owns a unique temporary state root, the state-fixture helper PID, and
the launcher-owned GUI process IDs it observes. The sample runner owns the
temporary root, the generated seed, and the capture path. The fixture CSVs are
repository inputs and remain read-only. The helper and launcher each use their
own bounded `ProcessStartInfo`/`WaitForExit` lifecycle. Each launch first
persists a `launch_pending` token, then records the executable path and
`start_ticks` through the held `System.Diagnostics.Process` handle before
clearing that token. The shared `automation_process_guard.ps1` Job Object
kills a live child if the runner is interrupted; timeout and exception paths
validate the same held handle and start identity before terminating it, and
verify exit before continuing. Fixture deletion is constrained to the runner's own
directory below the system temporary root and uses a finite retry budget.

When launched through `scripts/run-automation-ci.ps1`, CTest inherits
`SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS`, so the sample artifacts are placed
under that invocation's run-scoped `automation-samples` directory. Without the
override, local CTest uses the build tree's default path. CTest writes the two
sequence logs and a bounded JSON summary under a unique
`sample-run-<32-hex>` child of that collection directory. The CI workflow
uploads that glob on both success and failure, so independent CTest processes
cannot overwrite one another's seed, sequence logs, PNG, or summary. On
failure, the runner copies only the known seed, state cache, and PNG paths into
that run's artifact child; it never archives a broad temporary directory or
searches for unrelated processes. Complete run roots copied after cleanup
failures use unique `sample-run-root-<32-hex>` directories beneath the same
child. The artifact collection has a hard capacity of three `sample-run-<32-hex>`
children, including the next run. Startup prunes to two safe completed/stale
children before reserving a new slot, with a finite retry budget. Before
pruning any nested
`sample-run-root-<32-hex>` evidence directory, the runner validates the
owning `sample-run-<32-hex>` artifact lease; active, grace-period, unknown,
missing, or malformed parent leases defer deletion and emit a retention
diagnostic. This prevents one CTest process from deleting evidence while
another process still owns and writes that artifact child.
If a child was created without a verifiable lease, or its lease is damaged,
and the collection has no safe capacity slot, the runner writes the bounded
`sample-artifact-capacity-diagnostic.json`, fails before creating another run
child, and preserves the existing evidence for investigation. It never treats
an unowned child as safe to delete merely to make room.

Capacity pruning and child-directory creation are serialized by an OS-owned
exclusive capacity-lock handle. A crashed runner releases that handle without
leaving a permanent lock lease, while a concurrent runner waits only for the
bounded lock interval and then fails with a capacity diagnostic instead of
creating an unaccounted-for fourth child.

The system TEMP side uses the separate namespace
`specforge-automation-samples-<32-hex>`. Every run writes
`.specforge-automation-samples-ownership.json` with a run owner lease,
`heartbeat_utc`, helper/launcher/GUI PIDs, their executable paths, and exact
`owner_start_ticks` process identities. The manifest also records every
`launch_pending` role/token before a child start; an incomplete manifest or
pending token is retained as ambiguous evidence. At startup, roots older than the three
newest are considered only after the lease owner is inactive and the recorded
start identity matches; a stale owned process is terminated in launcher,
helper, GUI order and the root is removed only after every owned PID is
confirmed gone. Active, recent, malformed, legacy, or failed-cleanup roots are
retained with a diagnostic rather than risking another concurrent CTest run.
Those diagnostics make the current runner fail, so a cleanup problem cannot
produce a green CTest result or a misleading success summary. The current
failed root is copied to the CI artifact before deletion is attempted. Both
artifact-child retention and bounded TEMP retention use bounded retries and
retention. A root without its canonical manifest is never interpreted as an
empty owned process set: cleanup retains it and reports that launch state is
ambiguous. The current failed root and any cleanup-failure evidence are copied
to a unique artifact child before deletion is attempted; the system TEMP root
is removed only after exact owned identities are confirmed gone. If unresolved
roots already consume the finite TEMP retention capacity, the runner stops
before creating another root and reports the retained paths; this prevents
repeated crash/manifest failures from growing TEMP without bound.
The JSON summary records `temporary_run_root` as `removed` or
`preserved_bounded`, together with
`temporary_root_retention_count`, so success and failure reports state
whether the current TEMP root was actually deleted.

The sample runner is single-process and reuses `SpecForgeAutomation.exe`, the
existing human command protocol, and the production state seams from the
`specforge_automation_launcher` and
`specforge_automation_state_isolation_tests` targets. The separate
`specforge_multi_instance_labeling_smoke_tests` test remains the only
two-process coordination fixture; this sample does not introduce another GUI
runner or state model, so Issue #3 can land before or after this work without
maintenance drift between competing lifecycle implementations.
