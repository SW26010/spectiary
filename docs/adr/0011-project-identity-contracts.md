# Explicit project identity and naming contracts

Storage-layout amendment: [ADR 0015](0015-application-storage-cutover.md)
supersedes the historical `Data/` and `%LOCALAPPDATA%\SpecForge` placement below.
Active persistence now uses the final config/state/logs/unsaved role layout.

Stage-7 amendment: the final identity and format cutover below records the
implemented #106/#107 decision. The [project rename procedure](../project_rename.md)
maintains the detailed contract inventory and future-rename checklist. The
historical sections preserve the #106-A boundary, not current pending work.

Status: Accepted. Foundation: 2026-09-18; final cutover: 2026-09-19.
Issues: #106, #107, #103.

## Independent values

`config/project_identity.json` is the naming source shared by CMake-generated
C++ constants and PowerShell packaging. Each field is independent:

| Contract | Value / meaning |
| --- | --- |
| `founding_identity` / `application_id` | `0238d5bf7b34bb99c006f9807537d31234ca2e3d`, immutable application lineage |
| `product_display_name` | `Spectiary`, descriptive product name |
| `local_app_data_leaf` | `Spectiary`, active LocalAppData leaf under ADR 0015 |
| `artifact_basename` | `Spectiary`, native executable basename |
| `metadata_filename` | `spectiary_metadata.json`, adjacent build sidecar |

The founding token is the full canonical identity, never a runtime Git lookup,
truncated token, or display-name transformation. Build source revisions are
separate provenance. No universal APP_NAME or lowercasing rule connects these
contracts. Changing product display text does not change identity or paths.

## Metadata and artifact boundary

Schema 6 separates `application_id`, `product.name`, `product.version`,
`artifact.file`, build provenance and optional deployment. Only schema 6 is
accepted; schemas 3/4/5 are not reinterpreted and old filenames are not discovery
aliases. Product text is required descriptive metadata, not an identity match
key. Artifact path validation, executable SHA-256 binding during finalization
and packaging, atomic publication and constrained failure cleanup are retained.
Runtime deployment parsing remains independent of provenance availability.
See [release artifacts](../release_artifacts.md) for the full build contract.

## Final identity and format cutover (#106/#107)

IPC, automation locks, labeling edit leases and ASDF internal digest domains use
the complete founding identity with explicit semantic suffixes. Local Win32 and
ImGui identifiers use stable brand-neutral names where global uniqueness is
unnecessary. These machine identities stay fixed during a future public rename.
Human-readable product, artifact, storage, environment and document names remain
explicit contracts whose migration must be decided separately.

Canonical ASDF uses `spectiary.sample_labeling` schema `2.0.0`, `spectiary_build`
generation provenance and producer `asdf_library.name: Spectiary` with the
current build version. The pre-1.0 cutover rejects documents with
`format_kind: specforge.sample_labeling` as unsupported and leaves their bytes
untouched. No legacy ASDF reader, automatic conversion or dual writer is added.
The schema's scientific semantics and single canonical content owner are unchanged.

Application-managed JSON has a different compatibility policy. A finite list of
historical format kinds is normalized in memory before existing schema/body
validation; all current writers emit `spectiary.*` kinds. Known built-in theme
and public-catalog identities are translated without renaming user-authored
strings. This is a bounded pre-release reader, not a generic prefix alias, and
never opens or rewrites referenced canonical documents or source files.

The old labeling monolith, combined spectrum state and historical annotation
sidecars retain their explicit read-only migration/import paths under the owning
domain contracts. Physical legacy roots are migration inputs only under ADR 0015.
The rename procedure records those readers and their authoritative code locations;
they do not authorize new old-brand writers or permanent compatibility owners.

## Historical runtime path seam and #103 handoff

The #103-A foundation in [ADR 0012](0012-startup-storage-context.md) introduced
the final root contract while retaining the previous business-file placement.
ADR 0015 subsequently completed the physical cutover. The following preserves
the earlier #106-A handoff.

`RuntimePathInputs.executable_path` is the absolute actual executable location.
`package_root` is its parent, independent of the filename. Failed executable
resolution fails explicitly rather than inventing an EXE in cwd or temp.
`local_app_data_user_state_root` accepts a fully resolved physical root.
Portable storage retains its existing package-relative Data behavior.

The generated `project_identity::kLocalAppDataLeaf` is the final Spectiary leaf,
but #106-A does not activate it as the physical storage root. The existing
`DefaultLocalAppDataUserStateRoot()` policy remains in effect until #103.
No migration, old-root scan, compatibility read, or dual-root fallback is added.
#103 can consume the final leaf directly without searching brand strings or
inventing a naming rule. Directory ownership, config/state/logs/unsaved layout,
state-file placement and data cutover remain decisions for #103/#109/#111.

## Historical deferred work at #106-A

#106-B handles IPC, locks, Win32/shell identity, ImGui/layout and cache domains.
Existing identifiers and formats remain unchanged in A; they are not accepted
as permanent old-brand contracts. Draft/recovery behavior belongs to #108/#109.
Broad README/UI/source/repository rename adoption belongs to later #107 work.
`docs/project_rename.md` is deferred until #106 closes around the final
architecture. This decision completes the foundation only, not all of #106.
