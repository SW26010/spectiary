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
the complete Portable package.

## Finalization order

The production build has an explicit ordering contract:

1. CMake builds `specforge_metadata_finalizer_tool` as a dependency of the
   native target.
2. `specforge_native` links the final `SpecForge.exe`.
3. A post-build command invokes the finalizer with the finished EXE and its
   adjacent `specforge_metadata.json` path.
4. The finalizer hashes the finished EXE, obtains the UTC completion time,
   fills the schema 5 fields, and runs the same strict validation used by the
   metadata reader.
5. It writes a temporary JSON file and atomically replaces the metadata target.

The executable path must name the canonical `SpecForge.exe` artifact and must
not be the same file as, or an alias of, the metadata path. A failure before
replacement leaves an existing metadata file unchanged and removes the
temporary file. A failed finalizer makes the build fail rather than publishing
metadata that the runtime would reject.

Finalization occurs only when `specforge_native` actually relinks and its
post-build commands run. The `specforge_metadata` build target is a convenience
dependency on `specforge_native`; it does not declare the sidecar as an output
or byproduct and is not a sidecar-freshness verifier. If the executable is
already up to date, invoking this target alone does not rerun the finalizer for
a missing, modified, or stale metadata file. Release automation must use the
actual native build and the Portable validation steps below to establish a
fresh metadata/EXE pair.

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
6. checks the metadata digest against the packaged EXE, then checks both files
   against their corresponding ZIP entries; and
7. writes the ZIP and its lowercase SHA-256 sidecar.

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
