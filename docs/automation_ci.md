# Automation CI

SpecForge's automation workflow selects two CTest groups:

| Group | CTest label | Environment | CI behavior |
| --- | --- | --- | --- |
| Native/headless gate | `ci-headless` | No GUI window or desktop interaction | Required on `windows-latest` |
| Interactive desktop coverage | `real-gui` | A visible Windows desktop, normally self-hosted | Runs only when the event/ref policy and `github.ref_protected == true` allow a protected `master` push, or an explicitly authorized `workflow_dispatch` on protected `master` with `run_real_gui=true`; otherwise the workflow publishes an explicit diagnostic |

The required headless gate covers the automation protocol/control plane, state
root and seed isolation, the checked-in sample fixture contract, and the bounded
sample-labeling controller/session regressions that protect canonical ASDF
formalization and restart recovery. The comprehensive launcher workflow, the
sample command sequences, and the
two-process labeling coordination smoke are `real-gui` tests because they
require a real Win32/D3D11 presentation path. They are not silently included
in the hosted headless gate.

## Required branch check

The `native-headless` job is the required CI gate; the exact GitHub check name
is `Native/headless automation gate`. Repository administrators must add that
check to `master` branch protection. The `required` CTest label only makes the
local headless selection fail when a required test fails or no tests are
selected; it cannot configure GitHub branch protection by itself.
The current repository-level API check reports `master` as unprotected and
GitHub rejects branch-protection configuration for this private repository with
HTTP 403, so this external gate remains an administrator/platform blocker until
the repository plan or visibility permits it. Consequently, a current
master push has `github.ref_protected == false`, is reported as
`unprotected_ref`, and cannot schedule the self-hosted Real-GUI job.

## Local commands

Configure and build with the repository wrapper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Target all
```

Run the required gate and retain its bounded log:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-automation-ci.ps1 `
    -Mode Headless `
    -BuildDirectory build\ninja-msvc-debug `
    -ArtifactsDirectory build\ninja-msvc-debug\test-artifacts\ci-headless -SuiteTimeoutSec 300 -TestTimeoutSec 120
```

On a desktop session, run the interactive group explicitly:

```powershell
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

The native job has a 60-minute job budget. Checkout and toolchain discovery are
each bounded to five minutes, configure to ten, build to twenty, the headless
gate step to ten, and artifact upload to five. The headless helper's
`SuiteTimeoutSec 300` is the whole CTest-process budget; `TestTimeoutSec 120`
is the timeout applied to each selected test. The five-minute suite budget
therefore runs before the ten-minute step limit and leaves job-level cleanup
headroom.

The CI helper maps `Headless` to `ci-headless` and `RealGui` to `real-gui`.
The workflow and the helper use these exact labels; `required` is an additional
CTest label on the seven headless tests, not a different selector.

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
