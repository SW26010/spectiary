# Automation

This is the Windows-local test and debugging control plane. Ordinary startup
leaves it disabled; it is not a public end-user CLI or remote service.

| Task | Current document |
| --- | --- |
| Build, launch, send commands, or change protocol/ownership | [Control-plane contract](automation_control.md) |
| Run reproducible source/navigation/labeling/capture sequences | [Sample sequences and cleanup](automation_samples.md) |
| Run the headless gate or authorized interactive desktop tests | [Automation CI](automation_ci.md) |

Build prerequisites and the required Ninja/MSVC wrapper are documented in
[Engineering setup](../engineering_setup.md). `automation.yml` remains manual-only
with `workflow_dispatch`; real-GUI execution also requires the protected-ref,
explicit opt-in, and runner conditions in the [CI policy](automation_ci.md#manual-execution-policy).

Historical timing comparisons live in the
[2026-09-02 evidence](../../evidence/automation/20260902-scope-timing.md). They do
not define current test counts or build budgets. Return to [Development](../README.md).
