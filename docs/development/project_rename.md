# Project identity and rename procedure

Spectiary was formerly SpecForge. The GitHub repository is
[`SW26010/spectiary`](https://github.com/SW26010/spectiary). Current README,
documentation and CI entry points use that repository address. Historical ADRs,
issue references and dated run evidence may retain `SW26010/SpecForge` URLs.
Repository adoption does not change the immutable identity contracts below.

## Independent contracts

`config/project_identity.json` is the source of the independent product, path,
artifact and lineage values. Never derive one from another by case conversion,
executable discovery, or a global replacement.

| Surface | Contract and rename treatment |
| --- | --- |
| Founding identity / metadata `application_id` | Immutable full `0238d5bf7b34bb99c006f9807537d31234ca2e3d`. It is a lineage token, not a runtime Git revision. |
| IPC | `\\.\pipe\0238d5bf7b34bb99c006f9807537d31234ca2e3d.automation.v1.<instance>`; immutable namespace rooted in the complete token. |
| Automation root lock | `.0238d5bf7b34bb99c006f9807537d31234ca2e3d.automation-root.v1.lock`; immutable, independent of product text. |
| Automation sample evidence | Artifact lease, capacity lock and ownership marker use the full founding token plus `.automation-samples-*`; retained old test runs stay inert. Use `SPECTIARY_AUTOMATION_SAMPLES_ARTIFACTS` for a fresh isolated evidence root. |
| Labeling coordination | Physical alias locks live under the full founding token in system temp. Edit-lease digest domain uses that token and `.sample-labeling.edit-lease.v1`. Normalized path locks retain their semantic filenames. |
| ASDF internal digest domains | Full founding token plus `.sample-labeling.asdf-preservation-identity-v2` / `.sample-labeling.asdf-values-rewrite-identity-v1`; machine-only and immutable. |
| Win32 class / ImGui IDs | Local semantic identifiers: `MainWindowV1`, `DockHostV2`, `SpectrumV2`, `DockSpaceSampleNavigationV1`, etc. They remain stable across later brand changes. |
| Theme / public catalog IDs | `builtin.theme.dark`, `builtin.theme.light`, `public-spectral-lines.v1`; semantic identities, independent of display names. |
| Product / manifest | Spectiary; human-visible branding. |
| LocalAppData leaf | Explicit `Spectiary`, a human-readable storage contract. A future change needs a deliberate migration, even if product text changes first. |
| Executable / adjacent metadata | Explicit `Spectiary.exe` / `spectiary_metadata.json`; release/package contracts, not machine identity. The launcher is `SpectiaryAutomation.exe`. |
| Environment / CMake / targets | `SPECTIARY_*`, `project(Spectiary)`, `spectiary_*`; developer interfaces. No old-brand aliases. Reconfigure presets and update custom invocations on upgrade. |
| Canonical document namespace | `spectiary.sample_labeling` schema `2.0.0`, `spectiary_build`, producer `asdf_library.name: Spectiary`; explicit human-readable document contracts. |
| Application-managed JSON | `spectiary.*` format kinds, with the bounded historical readers described below. New writers emit only current kinds. |
| C++ symbols / source filenames | `namespace spectiary`, `SpectiaryApp`, `SpectiaryStartup`, `spectiary_*.cpp/.h`; implementation names with no compatibility role. |

Ordinary GUI external-open routing also uses the full founding token:
`0238d5bf7b34bb99c006f9807537d31234ca2e3d.external-open.<config-root-digest>`
for its message-only class and session-local request mappings. It remains
independent of automation and product display text (ADR 0016).

## Cutover and compatibility

Canonical ASDF takes a clean pre-1.0 cutover. Documents with
`format_kind: specforge.sample_labeling` are rejected as unsupported; no legacy
ASDF reader, automatic conversion, or dual writing is provided. Original bytes
remain untouched. Retain the original document and use the previous application
to export values if conversion is needed. Scientific semantics (alignment,
authors, source identity, values, timestamps, and canonical ownership) are unchanged.
Current fixture headers and their manifest hashes use the new contract; their
binary blocks and block offsets remain unchanged.

Application state has a separate ownership boundary. `NormalizeLegacyApplicationState`
in `src/app/local_user_state_json.cpp` explicitly recognizes the historical settings,
navigation, workflow, grouping, spectrum and split labeling owner format kinds.
It changes identity fields in memory before existing schema/body validation;
subsequent ordinary writes use only current kinds. This protects existing unsaved
labeling values. It is not a generic prefix alias and never opens or rewrites
referenced canonical/source files. Public catalog keys/references and built-in
theme IDs are translated, while user-authored strings remain unchanged.

The schema-4 labeling monolith (`specforge.sample_labeling_tasks.cache`) and old
combined spectrum state (`specforge.spectrum_view.state`) remain explicit legacy
readers under the previous ownership rules. Historical NPY/CSV sidecars with
`specforge.sample_label_result.metadata` remain read-only annotation/migration
inputs; there is no restored sidecar publisher. Their fixture generator deliberately
keeps that historical literal to exercise real migration inputs.

Physical migration continues to read `%LOCALAPPDATA%\SpecForge` and the old
`specforge-imgui-v2.ini` leaf only as legacy inputs. New writes target the explicit
Spectiary role paths. ImGui window/dock IDs changed once in this cutover, so old
development layouts may reset to the default layout. The current layout fixture
uses the linked ImGui hash semantics, including tab and dock-parent identities.
Do not run pre-cutover and current applications concurrently against the same
writable workspace: their IPC and edit-lease namespaces differ at this one-time
pre-1.0 boundary.

## Standard sequence for a future rename

1. Inventory current machine, document, storage, developer and presentation
   contracts. Identify actual historical readers, fixtures and immutable evidence.
2. Keep founding identity and its derived namespaces fixed. Keep local semantic
   IDs fixed. Separate source provenance from application identity.
3. Decide each human-readable path/document migration before changing writers.
   Protect user-owned documents and unsaved state; test rejection and migration.
4. Update producer/consumer sets together: code, generated headers, CMake,
   scripts, test fixtures/oracles, resources, packaging and current documentation.
5. Reconfigure and build, run fast/extended, metadata/package, automation and GUI
   tests, then audit every remaining old-name occurrence. Review triggers separately.
6. Only then arrange an explicit GitHub/release adoption. Update repository URLs,
   CI repository variables (currently `SPECTIARY_REAL_GUI_ENABLED`) with the
   deployment owner. Existing `self-hosted`, `windows`, `desktop` runner labels
   remain semantic and unchanged.

Never globally replace founding tokens, digest inputs, historical ADR/evidence,
legacy root literals, retired schema fixtures, user strings, arbitrary binary
documents, or existing repository URLs. A future brand change must not silently
change lock coordination, canonical interpretation, or physical ownership.

The automation workflow remains manual-only (`workflow_dispatch`). Version-tag
release behavior is unchanged. This stage does not authorize push, repository
rename, issue closure, CI configuration changes on GitHub, or publishing.

## Validation evidence

The completed Stage-7 checks and their local artifacts are retained in the
[2026-09-19 rename validation record](../evidence/project-identity/20260919-rename-validation.md).
