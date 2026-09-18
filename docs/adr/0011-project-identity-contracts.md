# Explicit project identity and naming contracts

Status: Accepted for #106-A. Date: 2026-09-18. Issues: #106, #107, #103.

## Independent values

`config/project_identity.json` is the naming source shared by CMake-generated
C++ constants and PowerShell packaging. Each field is independent:

| Contract | Value / meaning |
| --- | --- |
| `founding_identity` / `application_id` | `0238d5bf7b34bb99c006f9807537d31234ca2e3d`, immutable application lineage |
| `product_display_name` | `Spectiary`, descriptive product name |
| `local_app_data_leaf` | `Spectiary`, final destination leaf reserved for #103 |
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

## Runtime path seam and #103 handoff

The #103-A foundation is now defined by [ADR 0012](0012-startup-storage-context.md).
It activates the final root contract while retaining existing business-file
placement until the later physical cutover. The following records the #106-A
handoff before that foundation.

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

## Deferred work

#106-B handles IPC, locks, Win32/shell identity, ImGui/layout and cache domains.
Existing identifiers and formats remain unchanged in A; they are not accepted
as permanent old-brand contracts. Draft/recovery behavior belongs to #108/#109.
Broad README/UI/source/repository rename adoption belongs to later #107 work.
`docs/project_rename.md` is deferred until #106 closes around the final
architecture. This decision completes the foundation only, not all of #106.
