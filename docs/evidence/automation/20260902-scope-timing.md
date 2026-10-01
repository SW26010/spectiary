# CI scope and fast-tier timing — 2026-09-02

Status: dated measurement evidence for the automation ownership split. The current
manual execution policy and runner commands are maintained in
[Automation CI](../../development/automation/automation_ci.md); current CTest
selection is maintained in [Engineering setup](../../development/engineering_setup.md#ctest-验证层级).
These measurements describe their original commits/environments, not current
suite counts or a performance guarantee for a new build. Historical repository
URLs are retained as run evidence.

## Scope timing evidence

The 2026-09-02 before/after measurement used the same already-built Windows
workspace and ran each old/new phase sequentially through the repository
wrapper. It is an incremental scope comparison, not a clean hosted-run
performance estimate:

| Phase | Before ownership split | After ownership split |
| --- | ---: | ---: |
| Configure | 7.90s (Debug + static Release) | 3.10s (Debug only) |
| Build | 3.40s (Debug `all` + benchmark + Release `all`) | 1.10s (`spectiary_automation_headless_targets`) |
| CTest | 298.53s (24 Debug + 24 Release `ci-headless` tests) | 4.01s (5 Debug `automation-headless` tests) |
| Measured total | 309.83s | 8.21s |

The measured local total fell by 301.62s (97.3%). The old pinned ASDF setup
and oracle pass could not run in this local environment and is excluded from
the before total, so the comparison understates the removed workflow scope.

The clean GitHub-hosted Windows comparison was completed with manual workflow
dispatches. The successful broad-scope
before run 33486495510 (job `99787686232`; historical run, not migrated)
still configured Debug, static Release, and the pinned ASDF tree; built the two
`all` targets plus the specialized ASDF targets; and ran both `ci-headless`
suites plus the pinned ASDF oracle. The post-merge
after run 33573250819 (job `100071558764`; historical run, not migrated)
configured only Debug, built `spectiary_automation_headless_targets`, and ran
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

## Repository fast tier

2026-09-02 合并后的代表性 GitHub-hosted Windows/MSVC/Ninja 验证在
repository verification run 33573199441 (job `100071400948`; historical run, not migrated)
中通过同一正式入口运行了 `ctest --preset fast`：74/74 项测试通过，
`Total Test time (real) = 54.18 sec`。该结果满足低于 1 分钟的受控耗时门槛，
但尚未达到约 30 秒的首选预算。
