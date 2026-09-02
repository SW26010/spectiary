# Automation CI

SpecForge's automation workflow selects two CTest groups:

| Group | CTest label | Environment | CI behavior |
| --- | --- | --- | --- |
| Native/headless gate | `automation-headless` | No GUI window or desktop interaction | Always runs on `windows-latest` for a manual dispatch |
| Interactive desktop coverage | `real-gui` | A visible Windows desktop, normally self-hosted | Runs only for an explicitly authorized `workflow_dispatch` on protected `master` with `run_real_gui=true`; otherwise the workflow publishes an explicit diagnostic |

The required headless gate covers the automation protocol/control plane, state
root and seed isolation, the checked-in sample fixture contract, and the
automation workflow contract. The comprehensive launcher workflow, the sample
command sequences, and the
two-process labeling coordination smoke are `real-gui` tests because they
require a real Win32/D3D11 presentation path. They are not silently included
in the hosted headless gate.

`automation.yml` is manual-only and owns only those two automation selectors.
The manual `repository-verification.yml` workflow owns the broader Debug
`fast`/`extended`, static Release, and pinned ASDF oracle verification. Release
packaging, Portable, PE/import, FITS, labeling, and other repository suites do
not become automation work merely because they also retain the broad
`ci-headless` or `required` repository labels.

## Scope timing evidence

The 2026-09-02 before/after measurement used the same already-built Windows
workspace and ran each old/new phase sequentially through the repository
wrapper. It is an incremental scope comparison, not a clean hosted-run
performance estimate:

| Phase | Before ownership split | After ownership split |
| --- | ---: | ---: |
| Configure | 7.90s (Debug + static Release) | 3.10s (Debug only) |
| Build | 3.40s (Debug `all` + benchmark + Release `all`) | 1.10s (`specforge_automation_headless_targets`) |
| CTest | 298.53s (24 Debug + 24 Release `ci-headless` tests) | 4.01s (5 Debug `automation-headless` tests) |
| Measured total | 309.83s | 8.21s |

The measured local total fell by 301.62s (97.3%). The old pinned ASDF setup
and oracle pass could not run in this local environment and is excluded from
the before total, so the comparison understates the removed workflow scope.

The clean GitHub-hosted Windows comparison was completed with manual workflow
dispatches. The successful broad-scope
[before run 33486495510](https://github.com/SW26010/SpecForge/actions/runs/33486495510/job/99787686232)
still configured Debug, static Release, and the pinned ASDF tree; built the two
`all` targets plus the specialized ASDF targets; and ran both `ci-headless`
suites plus the pinned ASDF oracle. The post-merge
[after run 33573250819](https://github.com/SW26010/SpecForge/actions/runs/33573250819/job/100071558764)
configured only Debug, built `specforge_automation_headless_targets`, and ran
the five-test `automation-headless` selector:

| Hosted phase | Before ownership split | After ownership split |
| --- | ---: | ---: |
| Configure | 558.70s | 300.70s |
| Build | 1034.80s | 294.40s |
| CTest | 372.71s | 5.32s |
| Total job wall time | 2076.21s (34m 36.21s) | 635.54s (10m 35.54s) |

The hosted total fell by 1440.67s (69.4%). Configure and build values sum the
wrapper-reported elapsed times for every tree/target owned by each version of
the workflow; CTest sums the reported suite totals. Total job wall time is the
interval from the first to the last timestamp in each GitHub job log and also
includes checkout, runner setup, evidence preparation, and artifact handling.

## Manual execution policy

`automation.yml` does not run on `push` or `pull_request`, so its
`Native/headless automation gate` check must not be configured as a required
branch check: ordinary commits do not create that status. Start the workflow
explicitly with `workflow_dispatch`. Every dispatch runs the hosted
native/headless job. Real-GUI additionally requires protected `master`,
`run_real_gui=true`, and `SPECFORGE_REAL_GUI_ENABLED=true`; an unprotected
manual `master` dispatch records `unprotected_ref` without scheduling the
self-hosted job. The CTest `required` label remains a local selection property,
not a GitHub branch-protection policy.

## Local commands

Configure and build with the repository wrapper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 `
    -Target specforge_automation_headless_targets
```

Run the required gate and retain its bounded log:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-automation-ci.ps1 `
    -Mode Headless `
    -BuildDirectory build\ninja-msvc-debug `
    -ArtifactsDirectory build\ninja-msvc-debug\test-artifacts\automation-headless -SuiteTimeoutSec 300 -TestTimeoutSec 120
```

On a desktop session, run the interactive group explicitly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 `
    -Target specforge_automation_real_gui_targets
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-automation-ci.ps1 `
    -Mode RealGui `
    -BuildDirectory build\ninja-msvc-debug `
    -ArtifactsDirectory build\ninja-msvc-debug\test-artifacts\real-gui -SuiteTimeoutSec 900 -TestTimeoutSec 300
```

The CI helper fails when CTest fails or selects no tests. It always writes a
CTest log and a small JSON summary under the current run root. It also writes a
`ctest-temporary/manifest.json` plus only the files newly created or modified
by this CTest invocation from `Testing/Temporary`; unchanged historical files
are excluded. The workflow uploads the run root at
`ci-artifacts/<run_id>-<run_attempt>/<job>/`, which also contains the sample
runner's known failure artifacts under `automation-samples/`. It does not upload
the shared build `logs/build` or the entire build-tree `Testing/Temporary`.
The runner sets `SPECFORGE_AUTOMATION_SAMPLES_ARTIFACTS` only for the current
CTest process and restores the caller's value afterward.
The CTest wrapper captures the launched process's start ticks in a held
`System.Diagnostics.Process` handle and places it in the shared kill-on-close
Job Object. Timeout and exception cleanup therefore cannot fall through to a
PID-only/taskkill operation or accidentally terminate a reused PID.

The native job has a 90-minute job budget. Checkout and toolchain discovery are
each bounded to five minutes, configure to ten, build to twenty, the headless
gate step to ten, and artifact upload to five. The headless helper's
`SuiteTimeoutSec 300` is the whole CTest-process budget; `TestTimeoutSec 120`
is the timeout applied to each selected test. The five-minute suite budget
therefore runs before the ten-minute step limit and leaves job-level cleanup
headroom.

The CI helper maps `Headless` to `automation-headless` and `RealGui` to `real-gui`.
The workflow and the helper use these exact labels; `required` is an additional
CTest label on the five headless tests, not a different selector. The five tests
also retain `ci-headless` for repository-wide selection; automation does not
consume that broader label.

The real-GUI job is opt-in because an ordinary hosted Windows runner does not
provide a stable interactive desktop contract for this D3D11 path. The job
never runs for `pull_request`, even when
`SPECFORGE_REAL_GUI_ENABLED=true`; this prevents untrusted PR code from being
checked out, built, or executed on the self-hosted desktop. The repository
variable is only one prerequisite: the workflow event/ref authorization must
also match the protected `master` policy and GitHub's `github.ref_protected`
fact must be true. The `real-gui` environment can additionally require
reviewer approval on the self-hosted runner.

The real-GUI job has a 75-minute job-level budget. Its checkout, toolchain,
configure, build, CTest, and upload step limits sum to 65 minutes, leaving at
least ten minutes for runner scheduling and final artifact transfer near the
normal upper bound. Its `SuiteTimeoutSec 900` and per-test
`TestTimeoutSec 300` budgets remain inside the 20-minute CTest step limit.

When the authorization policy is not satisfied, a separate hosted job writes
an explicit `skipped` diagnostic containing the event, ref, base ref,
repository variable, manual input, `ref_protected`, headless result, runner
preflight result, and watchdog result. An allowed event on an unprotected ref
writes `unprotected_ref` and fails the diagnostic job without scheduling the
self-hosted job. For an authorized run, a hosted preflight queries for an
online idle runner with all three required labels before the self-hosted job
can be scheduled. If the runner API cannot be read or the runner is
offline/busy, the status is `runner_unavailable`. If environment approval or
the self-hosted queue remains pending beyond the bounded watchdog deadline,
the status is `approval_timeout`. An actual authorized test/build failure is
reported as `failed`, while watchdog API/target/cancellation problems are
reported as `watchdog_failure`; none of these are rewritten as `SKIPPED`.
The watchdog retains its JSON record, attempts bounded cancellation of the
workflow run (the GitHub API has no per-job cancellation endpoint) when the
target is queued or missing, and fails with an actionable diagnostic when the
Jobs API or target lookup cannot be trusted. The normal available-runner path
still executes the real-GUI tests. Every GitHub API request has an eight-second
request timeout, shorter than the fifteen-second poll interval; the ten-minute
deadline is checked independently of API return timing. A failed workflow-run
cancel is recorded as `cancel_failed` only when that cancellation request
fails, and maps to `watchdog_failure`, not to an
authorized test `failed` or a misleading `SKIPPED` result.

The real-GUI job uploads sample evidence from the unique
`ci-artifacts/<run_id>-<run_attempt>/real-gui/automation-samples/sample-run-*`
paths (the native job uses the analogous `native-headless` root). This keeps
logs and failure files isolated when independent CTest runs share the build
tree.
