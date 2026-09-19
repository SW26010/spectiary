# Startup storage context (#103-A)

Status: Accepted. Foundation dated 2026-09-19; physical cutover completed by
[ADR 0015](0015-application-storage-cutover.md). Issue: #103.

## Authority and final namespaces

Startup resolves `RuntimePaths` once, alongside deployment metadata. The actual
executable's parent is `package_root`; neither its filename nor the product's
display name determines a storage root. Consumers receive that context, including
copies carried into background preparation, rather than resolving another default
startup. Locator codecs require a context argument. Detached controllers with no
storage paths have no implicit persistence; all-empty preparation paths use only
their supplied context, and partial path sets remain invalid.

Without an explicit isolation override, the final `application_data_root` is
`package_root` for Portable and
`%LOCALAPPDATA%\Spectiary` for local-app-data deployments. The latter consumes
`project_identity::kLocalAppDataLeaf` directly. LocalAppData is queried only when
selected and not already supplied. Missing, relative, inaccessible or non-directory
persistent roots cause an explicit error, with no fallback to temp, cwd, package
root or another profile. Resolution does not create the roots or prove future
write permission; I/O owners still report write failures.

`config_root`, `state_root`, `logs_root` and `unsaved_root` are the lowercase
children of `application_data_root`. `cache_root` and `temp_root` are separate
children under the system temporary directory, namespaced by the application
lineage ID. They describe disposable storage, not another durable content owner.
This phase does not relocate existing caches or change machine identity schemes.

## Active role paths and migration inputs

All activity now uses the final role roots; see [ADR 0015](0015-application-storage-cutover.md)
for the file mapping and bounded migration/reset policy. `local_user_state_root`
was removed. `legacy_application_data_root`, `legacy_spectrum_view_state_path`
and `legacy_sample_labeling_state_path` are migration inputs only.

An injected `local_app_data_user_state_root` supplies the physical local-app-data
root without guessing a legacy sibling. `application_data_root_override` isolates
all managed roles in either profile and disables production legacy discovery.
Automation may explicitly supply an isolated legacy input for seed materialization.
`package_root`, public resources and package-relative locators stay tied to the
executable. Overrides must be absolute valid roots, never fallback policies.

## Locators and ordinary user paths

Portable encodes files beneath its package as explicit `package_relative`
locators; other paths and all local-app-data user files use `absolute`. Decoding
an absolute locator requires an absolute path. Package-relative locators require
a Portable context, no root name/directory/drive semantics and no `..` components.
Empty, NUL-containing, unknown-kind and contradictory records fail closed. Legacy
plain strings are accepted only as literal absolute paths. A missing file is never
rebound by matching a package directory basename. Only explicit relative locators
relocate with the package.

`CheckUserFilePath` is the admission API for normal source and
document open/save workflows. Callers may accept only `Allowed`; `Invalid` also
fails closed. The four reserved directories themselves and their descendants are
rejected, using Windows case-insensitive component comparison, lexical ancestry
and resolved ancestry for existing directory aliases. Ambiguous Win32 path forms
(device paths, alternate streams, trailing dots/spaces) are rejected. The check
does not recursively inspect contents and is not a security boundary against
concurrent filesystem changes.

User files directly at the application root or in any other explicitly chosen
subdirectory remain allowed. Ancestry under the application root does not transfer
ownership. The API does not recommend that root as a user workspace, and is enforced at source preparation, annotation import, and canonical
document connect/save/export boundaries.
