# Runtime Deployment Metadata Selects Storage

Storage-layout amendment: [ADR 0015](0015-application-storage-cutover.md)
supersedes the historical `Data/` and `%LOCALAPPDATA%\SpecForge` placement below.
Active persistence now uses the final config/state/logs/unsaved role layout.


Status: Superseded by
[ADR 0006: Distribution Capability and Dependency Linkage Policy](0006-distribution-capability-and-linkage-policy.md).

This document is retained as decision history. ADR 0006 replaces its release
artifact model while carrying forward the deployment-metadata and storage rules
that remain useful.

## Context

ADR 0002 coupled release identity and user-state storage to the compile-time
`SPECFORGE_RELEASE_PROFILE` value. Portable and Installed presets therefore
produced different executables even when their application code and build
inputs were otherwise identical. That prevented one verified EXE from being
reused across a standalone download, a Portable ZIP, and future packaging
channels.

Build provenance, deployment identity, and storage policy answer different
questions:

- product/build metadata describes what was built and how;
- distribution describes which packaging flow prepared the artifact;
- storage profile selects where SpecForge-owned mutable state is written.

Distribution is an explicit artifact claim, not runtime proof of how a user
obtained or launched the EXE.

## Decision

SpecForge builds one channel-neutral executable. No Portable or Installed
compile definition participates in the executable. Debug, Release, static, and
dynamic dependency choices remain build-shape concerns, not deployment
profiles.

The adjacent application-prefixed file is `specforge_metadata.json`. Schema 5
separates product, build, finalized executable identity, and optional deployment
data:

```json
{
  "schema_version": 5,
  "product": {
    "name": "SpecForge",
    "version": "0.7.1"
  },
  "build": {
    "source_mode": "working_tree",
    "source_revision": null,
    "configuration": "Release",
    "compiler_id": "MSVC",
    "compiler_version": "...",
    "cmake_version": "...",
    "generator": "...",
    "target_architecture": "amd64",
    "windows_sdk_version": "...",
    "dear_imgui": "...",
    "implot": "...",
    "zlib": "...",
    "completed_at_utc": "2026-08-05T09:21:32Z"
  },
  "artifact": {
    "file": "SpecForge.exe",
    "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  },
  "deployment": {
    "distribution": "portable",
    "storage_profile": "portable"
  }
}
```

`target_architecture` names the executable instruction set and therefore uses
`amd64`. Visual Studio platform arguments, vcpkg triplets, and their preset
names retain the tools' native `x64` spelling.

The allowed distribution values are `installer`, `winget`, `portable`, and
`scoop`. They are displayed as Installer, WinGet, Portable, and Scoop. No
storage or feature behavior branches on distribution.

The allowed storage profiles are:

- `portable`: use `<package-root>\Data`;
- `local_app_data`: use `%LOCALAPPDATA%\SpecForge`.

Missing metadata, or schema 4/5 metadata without `deployment`, means Standalone
distribution with `local_app_data` storage. Schema 5 additionally requires a valid UTC
`completed_at_utc` and an `artifact` containing the canonical `SpecForge.exe`
filename and a lowercase SHA-256 digest. When `deployment` exists, both fields
are mandatory, string-typed, and restricted to the allowed values. Malformed
JSON, an unsupported or indeterminate schema, an invalid deployment object, or
an unknown deployment value fails startup. `wWinMain` performs this preflight
before constructing `SpecForgeApp`, so no application user-state object can
load, create, or write state after an invalid declaration.

Schema 3 remains a compatibility input under the legacy
`specforge_build_metadata.json` filename. `release_profile=Portable` maps to
`portable`; `release_profile=Installed` maps to `local_app_data`. Schema 3 has
no independent distribution declaration, so About displays Standalone. Its
legacy `target_architecture=x64` value is treated as equivalent to schema 4 or
5 `amd64` for provenance matching. The current filename takes precedence when
both files exist, and an invalid current file never falls back to the legacy
file.

Build provenance validation is independent of deployment parsing. A valid
deployment controls storage even when the sidecar's product/build tuple is
unavailable or does not match the executable. About always displays the
SHA-256 of the executable that is running and exposes metadata-derived fields
only after the existing metadata validation and artifact comparison succeed.
Missing metadata, or a successfully parsed supported envelope whose
build-provenance or artifact portion is invalid, incomplete, core-mismatched,
or artifact-mismatched, simply omits those fields; About does not display the
sidecar digest or verification/mismatch status. Invalid JSON, an unsupported
schema, or an invalid deployment continues to fail startup under the existing
preflight contract, so About is not reached. None of this changes the selected
state root.

The production build removes the executable-adjacent
`specforge_metadata.json` before linking the final EXE and running any
post-link work. The post-build finalizer then computes the EXE's SHA-256 and UTC
completion time, validates the complete schema 5 model, writes a temporary
sidecar, and atomically replaces the target. It rejects a non-canonical
executable filename and any metadata path that is not exactly the normalized,
executable-adjacent `specforge_metadata.json`. Once path validation succeeds,
the finalizer derives the write and cleanup target from the executable path;
any failed finalization removes only that constrained target after cleaning the
temporary file. Path-validation failures occur before cleanup and remain
side-effect free. Together, pre-link invalidation and finalizer cleanup prevent
link or post-link failures from retaining a valid-looking sidecar for an older
EXE.

Future signed artifacts must preserve the release ordering
`link → sign → hash → metadata finalization → package`. Signing therefore
precedes the finalizer's SHA-256; the recorded artifact digest must cover the
signed executable bytes, and packaging consumes that already-bound pair.

The Portable packaging flow consumes schema 5 build-output metadata without a
deployment section, adds the Portable deployment declaration only to the
package metadata, creates `Data`, and copies the unchanged EXE. Formal Portable
packaging requires a non-null valid Windows SDK version even though development
metadata may use `windows_sdk_version: null`. It verifies that the build EXE,
metadata artifact digest, packaged EXE, and ZIP entries agree. Third-party
notices and data-source attributions are embedded in that shared executable and
remain available through About without adjacent documents. The
ZIP root is exactly:

- `SpecForge.exe`
- `specforge_metadata.json`
- `Data\`

Future Installer, WinGet, and Scoop work must write their distribution
explicitly in their own independently testable packaging flows. Registry
values, directory names, parent processes, launch source, and channel
auto-detection are not accepted inputs.

## Consequences

All channels can reuse one executable and independently describe deployment.
Copying a new EXE into an old schema 3 Portable folder continues to select its
existing `Data` directory. A missing deployment safely behaves as Standalone,
while a present but invalid declaration cannot silently redirect state to
LocalAppData.

Installer implementation, channel manifests, install/upgrade/uninstall
lifecycle, and user-data migration remain outside this decision.
