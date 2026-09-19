# Application-managed storage physical cutover

Status: Implemented locally; joint acceptance before closing #103. Date: 2026-09-19.
Issues: #103 and the storage-coupled subset of #106.

## Final owners

Portable uses `application_data_root == package_root`. LocalAppData uses the
explicit `project_identity::kLocalAppDataLeaf` (`Spectiary`). The full founding
identity `0238d5bf7b34bb99c006f9807537d31234ca2e3d` is unchanged; neither roots
nor machine identity are computed from display text.

| Role | Files |
| --- | --- |
| `config/` | UI language, appearance, UI scale, input, external-source and profile settings; spectrum plot preferences |
| `state/` | `imgui-layout-v2.ini`, panel visibility, source session, sample navigation, sample workflow, labeling registrations, spectrum viewport, complete `spectral-line-grouping-views.json` |
| `logs/` | Profile logs; frame captures under `logs/captures/` |
| `unsaved/` | Pre-canonical labeling checkpoints only; Windows Hidden is applied at startup and checkpoint publication and is not identity/security |

Disposable caches and indexes stay in system cache/temp facilities. Resolved
cache/temp roots use the stable application identity. No persistent `cache/`
child or generic `Data/` owner is introduced. The Portable package contains
four role directories beside its executable and metadata.

## Bounded pre-release migration

Startup imports before constructing application settings/UI. `RuntimePaths`
has no shared active/legacy root field. Production migration inputs are explicitly
`<package-root>/Data` or `%LOCALAPPDATA%/SpecForge`; injected roots never discover
production legacy data. Automation may explicitly opt into its isolated seed root.

The flat-file allowlist covers the six settings files, panel visibility, source
session, sample navigation, sample workflow, grouping views, and the old ImGui
layout leaf. JSON imports must be objects and files are capped at 16 MiB; malformed
or larger inputs reset. Existing destinations, even malformed ones, always win.
Each complete staged file is published without replacement, including during
concurrent startup. Legacy profile output-directory overrides reset to the new
`logs/` default so a restored logger cannot return to the retired managed root.
Old layout bytes retain their existing validation and neutral filename.

Spectrum and labeling retain their bounded domain migration rules: missing split
spectrum owners may import validated legacy view state; schema-4 labeling monoliths
are consulted only while neither ordinary state nor unsaved owner exists. Present
new owners outrank legacy and domain load failures retain existing writeback safety.
Spectrum migration also publishes without replacing a concurrently created owner.
A failed spectrum import retains the destination/defaults, never a queued ordinary
autosave of legacy values; a later startup may retry only while the target is absent.
All subsequent saves use the final role paths. There is no legacy save fallback.

No whole-root scan, recursive move or recursive delete is performed. Old logs,
captures, unknown files and other inert leftovers remain. The spectrum migration
may retire only its exact known legacy state file after healthy split owners exist.
No canonical ASDF, source dataset or file selected by a persisted locator is moved,
rewritten or removed by physical storage migration.

## User-file admission and scope

Normal source opens (including startup/restoration and folder members), annotation
imports, canonical connect/Save As/automatic saves and label exports check reserved
namespaces using the authoritative runtime context. `config`, `state`, `logs` and
`unsaved`, including case/path aliases handled by `CheckUserFilePath`, are rejected.
Files at the root or beneath other user-selected children remain legal. Portable
locators remain package-relative and LocalAppData locators remain absolute.

The public built-in catalog remains a package resource. Grouping views move as one
state file; #104 is unchanged. #108/#109 canonical ownership/checkpoint lifecycle
is unchanged. IPC, Win32/shell identity, ImGui internal IDs, format/digest domains,
environment/CLI naming and broad public rename remain outside this phase.
#103 remains open for joint acceptance; #106 and #107 remain open for their remaining
identity/adoption work. No push or issue closure is part of the local cutover.
