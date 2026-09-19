# Issue #4 local implementation and acceptance

Date: 2026-09-20. Windows x64, Ninja/MSVC Debug. Base: `4dbf811`.
Implementation commits: `9cd2230`, `5f8e487`, `6457073`.
Work was performed on local `master`; no push or issue closure was performed.

## Requirements checked

| Requirement | Evidence |
| --- | --- |
| Persistent recent/new modes; new-instance upgrade default | `ApplicationSettings`, `TestExternalOpenInstancePolicy`, settings widget combo and localized labels |
| Only ordinary startup with a source routes | `wWinMain` gates automation, resource fixtures, absent source and `--new-instance`; command-line tests preserve source-free/forced launches |
| Common shell/CLI source entry | `ParseCurrentProcessSpectiaryCommandLine` and the existing production startup loader contract |
| Namespace-local recent compatible GUI | Real child-process tests verify root isolation, foreground recency changes, protocol mismatch and selection past a newer incompatible endpoint |
| New source adds and activates | Shell routed-open test exercises the production queue/resolver with controlled spectrum decoders |
| Existing source preserves state without duplicate/reload | Exact snapshot identity, decode counts, source count and selected member asserted before/after duplicate opens and source round trips |
| Bounded discovery, forwarding and activation | Shared 1500 ms budget; tests use shorter budgets for unresponsive targets, delayed focus, focus refusal and endpoint withdrawal |
| Request ownership under races | Concurrent senders, receiver exit during activation, cancellation before acceptance and acceptance followed by reply timeout are tested with actual processes/mappings |
| Ordinary routing isolated from automation | Separate message-only endpoints and mappings; registration excludes automation; existing automation tests pass |
| #2 new-instance launcher remains intact | Current-executable launcher now passes `--new-instance`; file/folder, Unicode, space/quote and failed-launch tests pass |
| #11 preferred-member semantics survive transfer | Wire payload retains original path plus invocation preference; Shell verifies non-first FITS/CSV member and preserves path through asynchronous resolution |
| Documentation and regression coverage | Product requirements, ADR 0016, ADR 0004 cross-reference and rename identity inventory updated |

## Validation results

All configure/build operations used `scripts/build-ninja-msvc-debug.ps1` with
the repository-required elevated MSVC environment. The full Debug build passed.
The last full build log is
`logs/build/spectiary-ninja-msvc-debug-build-20260920-030008.out.log`.

- `ctest --preset fast --output-on-failure`: 90/91 initially passed. The sole
  failure was an exact Chinese-string assertion for the former folder-only
  persistence message. The new combined external-source message is intentional;
  the assertion was updated, and both routing labels were added to that test.
- The rebuilt `spectiary_ui_text_tests.exe` passed (exit 0). No production code
  changed after the full fast run. Thus all 91 fast cases were verified passing,
  including the corrected test on its final source.
- `ctest --preset extended --output-on-failure`: 9/9 passed, including Shell
  source-load activation, settings panel, sample filtering and ASDF property tests.
- The IPC test was strengthened after its initial fast pass with simultaneous
  senders and receiver exit/withdrawal during activation. Its rebuilt final
  `spectiary_win32_external_open_router_tests.exe` passed (exit 0).
- `git diff --check` passed. Workflow trigger files are unchanged.

## Final review

Review covered the complete change from the base, including settings transaction
rollback, startup bypasses, namespace/version selection, transient endpoint
lifetime, foreground failure, timeout ownership, command-line quoting, source
deduplication, pending-load sequencing, preferred-member resolution and test
coverage. No unresolved P0/P1 issue was identified.

During review, queued routed opens were serialized to avoid an existing-source
activation canceling an earlier accepted load. Preferred-member navigation keeps
the original path instead of trusting a cached row when a directory can change.
Native registration failure remains optional and does not fail GUI startup.

The delivery boundary is acceptance by the running GUI, as specified in ADR 0016;
this is not durable recovery from a later process crash or explicit user close.
Windows foreground refusal causes new-instance fallback rather than bypassing
operating-system focus restrictions. No installer/file-association changes or
global GUI state synchronization were introduced.
