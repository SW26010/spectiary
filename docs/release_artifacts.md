# Schema 6 Release Artifacts

This document describes the build and release contract for the schema 6
`Spectiary.exe` artifact. The general developer setup and entrypoints remain in
[`engineering_setup.md`](engineering_setup.md); this page records the points
that make an executable and its metadata a single release unit.

## Metadata files and compatibility

The current sidecar is `spectiary_metadata.json`, placed beside the executable.
Production native builds create it as schema 6 after the final executable link.
Only schema 6 is accepted. Retired schema 3/4/5 sidecars and their old filenames are not compatibility aliases.

The build-output sidecar has no `deployment` object. Portable packaging adds
that object to the copy inside the package, so deployment identity remains
separate from the channel-neutral executable and its build identity.

## Schema 6 contract

Schema 6 separates application identity from display/product naming. The explicit naming source is `config/project_identity.json`; product text never determines a storage path or IPC identity.

| Location | Requirement |
| --- | --- |
| `schema_version` | Integer `6`. |
| `application_id` | Full immutable founding identity `0238d5bf7b34bb99c006f9807537d31234ca2e3d`. |
| `product.name` | Display metadata; does not participate in machine identity matching. |
| `build.cfitsio` | Current producers and Portable packages require the non-empty, unpadded CFITSIO package version parsed from the installed vcpkg SPDX metadata. The checked-in CFITSIO notice heading must carry the same version. The field is required. |
| `build.yaml_cpp` | Current producers write the non-empty, unpadded yaml-cpp package version. A missing or malformed member makes build provenance unavailable. |
| `build.completed_at_utc` | A valid UTC timestamp in exactly `YYYY-MM-DDTHH:mm:ssZ` form. It has no fractional seconds or offset. |
| `artifact.file` | Exactly `Spectiary.exe`. On Windows, the finalizer accepts this filename case-insensitively; another filename is rejected. |
| `artifact.sha256` | The lowercase, 64-character SHA-256 digest of that final `Spectiary.exe`. |

The existing required build strings must be non-empty, unpadded, and free of
control characters where applicable. Compiler and CMake versions use dotted
numeric forms. `source_mode` is `working_tree` with a JSON `null`
`source_revision`, or `head` with a full 40-character lowercase Git object ID.
The executable architecture is `amd64`.

CFITSIO and yaml-cpp are both mandatory in schema 6. Package dependency notices must match those versions.

`windows_sdk_version` may be JSON `null` for a development build when CMake
does not expose an authoritative SDK selection. A formal Portable package must
instead contain a non-empty legal dotted SDK version. This distinction keeps
local development possible without allowing a release to omit SDK provenance.

The artifact digest is an identity binding for the executable sidecar, not an
authentication signature. The build provenance tuple remains diagnostic; the
digest is what detects an EXE/metadata mix-up, while the final ZIP digest covers
the complete Portable package. When executable signing is introduced, the
signature is applied before the artifact digest is computed so the metadata
binds to the signed bytes.

## Finalization order

The production build has an explicit ordering contract:

1. CMake builds `spectiary_metadata_finalizer_tool` as a dependency of the
   native target.
2. A pre-link command removes the executable-adjacent
   `spectiary_metadata.json`, invalidating any sidecar from an older EXE.
3. `spectiary_native` links the final `Spectiary.exe`.
4. A future signing step signs the finished `Spectiary.exe`.
5. A post-build command invokes the finalizer with the signed EXE and its
   adjacent `spectiary_metadata.json` path.
6. The finalizer hashes the signed EXE, obtains the UTC completion time,
   fills the schema 6 fields, and runs the same strict validation used by the
   metadata reader.
7. It writes a temporary JSON file and atomically replaces the metadata target.

The release sequence is therefore `link → sign → hash → metadata finalization
→ package`. Until signing is implemented, the current native build executes
the equivalent unsigned path `link → hash/finalize`; signing must not be added
after metadata finalization or packaging.

The executable path must name the canonical `Spectiary.exe` artifact. The
metadata path must be exactly its normalized, executable-adjacent
`spectiary_metadata.json`; another filename or a path resolving anywhere else
is rejected before any cleanup. After validation, the finalizer derives its
write and cleanup target from the executable path rather than deleting the
caller-provided path. The pre-link invalidation occurs before the linker and
all post-link commands, so a link, app-local deployment, resource copy,
obsolete-file cleanup, or finalizer failure cannot leave an older sidecar
beside a new EXE. Once the finalizer has accepted the constrained paths, a
failure before replacement removes the metadata target and the temporary file.
A failed finalizer makes the build fail rather than publishing metadata that the runtime
would reject; a later native relink can recreate the sidecar.

Finalization occurs after `spectiary_native` links and its post-build commands
run. The `spectiary_metadata` target declares the executable-adjacent sidecar
as a byproduct, depends on the native executable and finalizer tool, and runs a
freshness check. It checks the canonical `application_id`, `schema_version` equal to 6,
`build.completed_at_utc` is present and non-empty, `artifact.file` is exactly
`Spectiary.exe`, and `artifact.sha256` matches the current EXE hash. A missing
or invalid sidecar, or any failed identity check, invokes the finalizer again.
A timestamp-only edit that leaves those checks valid is intentionally preserved
by a CMake no-op; the CTest regression verifies that it does not rewrite the
sidecar or alter the EXE. This freshness check is not the complete strict
schema/package validation performed by packaging and runtime. Release
automation builds this target so an up-to-date EXE cannot cause an
identity-invalid or stale sidecar to be silently accepted. The CTest
metadata build regression removes only the EXE to force one real
relink/finalization, then runs a second real no-op build; it verifies the
executable hash is unchanged by a metadata timestamp change and that the no-op
build does not rewrite the sidecar. Ninja/MSVC rebuilds use the repository's
bounded build wrapper so each nested build initializes the MSVC and Windows SDK
environment.

## Portable packaging and verification

Run the working-tree or isolated-`HEAD` entrypoint described in
[`engineering_setup.md`](engineering_setup.md). The shared Portable builder:

1. builds the release executable and requires schema 6 metadata beside it;
2. validates the strict schema, source tuple, canonical artifact filename,
   completion timestamp, dependency provenance, and formal Portable SDK
   requirement;
3. checks that `artifact.sha256` equals the hash of the build-directory EXE;
4. copies `Spectiary.exe` and metadata to a package root containing only the
   executable file, metadata file, and `config/`, `state/`, `logs/`, `unsaved/` directories; exact enumeration
   includes hidden entries and validates each entry type;
5. adds `deployment.distribution: "portable"` and
   `deployment.storage_profile: "portable"` to the package metadata copy;
6. checks the metadata digest against the packaged EXE;
7. writes the ZIP and its lowercase SHA-256 sidecar; and
8. invokes `scripts/verify-portable.ps1` to verify the ZIP entry set and check
   that the ZIP entries for the executable and metadata have the same SHA-256
   digests as their package-root counterparts. For a Release artifact, the
   verifier also parses the ordinary and delay-load PE import tables and rejects
   a CFITSIO runtime DLL or curl/bzip2 DLLs from accidentally enabled CFITSIO
   features. The exact package
   and ZIP entry sets independently forbid shipping `cfitsio.dll` beside the
   executable.

The three executable values below must be identical before a package is
accepted:

```text
SHA256(build-directory Spectiary.exe)
        == artifact.sha256 in metadata
        == SHA256(packaged Spectiary.exe)
```

The standalone verifier is also available for an existing package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-portable.ps1 `
    -BuildExecutable <path-to-build-Spectiary.exe> `
    -PackageRoot <path-to-Spectiary-portable> `
    -ZipPath <path-to-Spectiary-portable.zip>
```

Changing the build EXE, packaged EXE, package metadata digest, or either ZIP
entry must make verification fail. A failed verification is a release failure;
it must not be recorded as a successful Portable package.
