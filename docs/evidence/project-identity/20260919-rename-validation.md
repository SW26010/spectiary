# Stage-7 project identity validation — 2026-09-19

Status: dated acceptance evidence for the Stage-7 rename. Current identity,
compatibility and future rename requirements are maintained in
[Project identity and rename procedure](../../development/project_rename.md).
This record describes the measured commits and does not assert a new test run.

Implementation commit: `490af6a8d298` (preceded by machine identity `85c79fb84214` and
format cutover `65c9bbd8e3a8`). Review covered immutable namespaces, legacy ownership,
canonical read/write identity, producer/consumer names, resources, CMake targets,
workflow triggers, current documentation, and every remaining old-brand source hit.
No unresolved P0/P1 finding was identified in this change review.

- Ninja/MSVC Debug: full configure/build passed, including widget and ASDF
  hardening targets.
- Ninja/MSVC static Release: full configure/build passed; final native application
  and launcher were rebuilt after the last semantic plot-ID cleanup.
- Full Debug CTest: **108 passed, 0 failed, 1 skipped** (225.18 seconds).
  This includes real GUI launcher/viewport/sample/multi-instance tests and release
  artifact, metadata rebuild, PE, icon and configured-build contracts.
- The skipped `spectiary_runtime_resource_stability` is opt-in:
  `SPECTIARY_RESOURCE_STABILITY_TIER` was unset. No smoke/soak resource-longevity
  claim is made by this validation.
- Python checksum interoperability matrix passed with ASDF 5.3.1, ASDF Standard
  1.5.0 and NumPy 2.5.2. Fixture manifest SHA-256 values were verified after the
  format-header cutover.

Local evidence is in `logs/phase7-final-ctest.log`,
`logs/phase7-asdf-oracle.json` and `logs/build/`. The full run used
`SPECTIARY_AUTOMATION_SAMPLES_ARTIFACTS=.scratch/phase7-final-samples` to avoid
reusing older retained evidence whose legacy lease markers intentionally remain
inert. An initial suspended-UI launcher timing timeout passed both the isolated
rerun and the final full run; its test threshold was not relaxed.
