# Schema 5 Release Artifacts

This document describes the build and release contract for the schema 5
`SpecForge.exe` artifact. The general developer setup and entrypoints remain in
[`engineering_setup.md`](engineering_setup.md); this page records the points
that make an executable and its metadata a single release unit.

## Metadata files and compatibility

The current sidecar is `specforge_metadata.json`, placed beside the executable.
Production native builds create it as schema 5 after the final executable link.
The runtime still reads schema 3, 4, and 5 for compatibility:

- `specforge_metadata.json` accepts schema 4 or 5.
- The legacy `specforge_build_metadata.json` name accepts schema 3.
- Schema 3 and 4 remain readable, but they do not carry the schema 5 finalized
  timestamp and executable digest.

The build-output sidecar has no `deployment` object. Portable packaging adds
that object to the copy inside the package, so deployment identity remains
separate from the channel-neutral executable and its build identity.

## Schema 5 contract

Schema 5 retains the product and build provenance fields and adds two required
finalization fields:

| Location | Requirement |
| --- | --- |
| `schema_version` | Integer `5`. |
| `build.completed_at_utc` | A valid UTC timestamp in exactly `YYYY-MM-DDTHH:mm:ssZ` form. It has no fractional seconds or offset. |
| `artifact.file` | Exactly `SpecForge.exe`. On Windows, the finalizer accepts this filename case-insensitively; another filename is rejected. |
| `artifact.sha256` | The lowercase, 64-character SHA-256 digest of that final `SpecForge.exe`. |

The existing required build strings must be non-empty, unpadded, and free of
control characters where applicable. Compiler and CMake versions use dotted
numeric forms. `source_mode` is `working_tree` with a JSON `null`
`source_revision`, or `head` with a full 40-character lowercase Git object ID.
The executable architecture is `amd64`.

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

1. CMake builds `specforge_metadata_finalizer_tool` as a dependency of the
   native target.
2. A pre-link command removes the executable-adjacent
   `specforge_metadata.json`, invalidating any sidecar from an older EXE.
3. `specforge_native` links the final `SpecForge.exe`.
4. A future signing step signs the finished `SpecForge.exe`.
5. A post-build command invokes the finalizer with the signed EXE and its
   adjacent `specforge_metadata.json` path.
6. The finalizer hashes the signed EXE, obtains the UTC completion time,
   fills the schema 5 fields, and runs the same strict validation used by the
   metadata reader.
7. It writes a temporary JSON file and atomically replaces the metadata target.

The release sequence is therefore `link → sign → hash → metadata finalization
→ package`. Until signing is implemented, the current native build executes
the equivalent unsigned path `link → hash/finalize`; signing must not be added
after metadata finalization or packaging.

The executable path must name the canonical `SpecForge.exe` artifact. The
metadata path must be exactly its normalized, executable-adjacent
`specforge_metadata.json`; another filename or a path resolving anywhere else
is rejected before any cleanup. After validation, the finalizer derives its
write and cleanup target from the executable path rather than deleting the
caller-provided path. The pre-link invalidation occurs before the linker and
all post-link commands, so a link, app-local deployment, resource copy,
obsolete-file cleanup, or finalizer failure cannot leave an older sidecar
beside a new EXE. Once the finalizer has accepted the constrained paths, a
failure before replacement removes the metadata target and the temporary file.
A failed finalizer makes the build fail rather than publishing metadata that the runtime
would reject; a later native relink can recreate the sidecar.

Finalization occurs after `specforge_native` links and its post-build commands
run. The `specforge_metadata` target declares the executable-adjacent sidecar
as a byproduct, depends on the native executable and finalizer tool, and runs a
freshness check. That check proves
the sidecar is schema 5, has a completion timestamp, names `SpecForge.exe`,
and records the current EXE hash;
if any of those checks fail or the sidecar is missing, it invokes the finalizer
again. Release automation builds this target so an up-to-date EXE cannot cause
a missing, modified, or stale sidecar to be silently accepted. The CTest
metadata build regression removes only the EXE to force one real
relink/finalization, then runs a second real no-op build; it verifies the
executable hash is unchanged by a metadata timestamp change and that the no-op
build does not rewrite the sidecar. Ninja/MSVC rebuilds use the repository's
bounded build wrapper so each nested build initializes the MSVC and Windows SDK
environment.

## Portable packaging and verification

Run the working-tree or isolated-`HEAD` entrypoint described in
[`engineering_setup.md`](engineering_setup.md). The shared Portable builder:

1. builds the release executable and requires schema 5 metadata beside it;
2. validates the strict schema, source tuple, canonical artifact filename,
   completion timestamp, dependency provenance, and formal Portable SDK
   requirement;
3. checks that `artifact.sha256` equals the hash of the build-directory EXE;
4. copies `SpecForge.exe` and metadata to a package root containing only the
   executable, metadata, and `Data/`;
5. adds `deployment.distribution: "portable"` and
   `deployment.storage_profile: "portable"` to the package metadata copy;
6. checks the metadata digest against the packaged EXE;
7. writes the ZIP and its lowercase SHA-256 sidecar; and
8. invokes `scripts/verify-portable.ps1` to verify the ZIP entry set and check
   that the ZIP entries for the executable and metadata have the same SHA-256
   digests as their package-root counterparts.

The three executable values below must be identical before a package is
accepted:

```text
SHA256(build-directory SpecForge.exe)
        == artifact.sha256 in metadata
        == SHA256(packaged SpecForge.exe)
```

The standalone verifier is also available for an existing package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-portable.ps1 `
    -BuildExecutable <path-to-build-SpecForge.exe> `
    -PackageRoot <path-to-SpecForge-portable> `
    -ZipPath <path-to-SpecForge-portable.zip>
```

Changing the build EXE, packaged EXE, package metadata digest, or either ZIP
entry must make verification fail. A failed verification is a release failure;
it must not be recorded as a successful Portable package.
