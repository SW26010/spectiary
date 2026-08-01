# Runtime Deployment Metadata Selects Storage

Status: Accepted. Supersedes
[ADR 0002: Release Profiles Are Separate Artifacts](0002-release-profile-artifacts.md).

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

The adjacent application-prefixed file is `specforge_metadata.json`. Schema 4
separates product, build, and optional deployment data:

```json
{
  "schema_version": 4,
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
    "zlib": "..."
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

Missing metadata, or schema 4 metadata without `deployment`, means Standalone
distribution with `local_app_data` storage. When `deployment` exists, both
fields are mandatory, string-typed, and restricted to the allowed values.
Malformed JSON, an unsupported or indeterminate schema, an invalid deployment
object, or an unknown deployment value fails startup. `wWinMain` performs this
preflight before constructing `SpecForgeApp`, so no application user-state
object can load, create, or write state after an invalid declaration.

Schema 3 remains a compatibility input under the legacy
`specforge_build_metadata.json` filename. `release_profile=Portable` maps to
`portable`; `release_profile=Installed` maps to `local_app_data`. Schema 3 has
no independent distribution declaration, so About displays Standalone. Its
legacy `target_architecture=x64` value is treated as equivalent to schema 4
`amd64` for provenance matching. The current filename takes precedence when
both files exist, and an invalid current file never falls back to the legacy
file.

Build provenance validation is independent of deployment parsing. A valid
deployment controls storage even when the sidecar's product/build tuple is
unavailable or does not match the executable. About continues to report build
metadata unavailable or mismatch without changing the selected state root.

The Portable packaging flow consumes schema 4 build-output metadata without a
deployment section, adds the Portable deployment declaration only to the
package metadata, creates `Data`, and copies the unchanged EXE. The EULA,
third-party notices, and data-source attributions are embedded in that shared
executable and remain available through About without adjacent documents.
Packaging and artifact tests compare the build-output and packaged EXE bytes
and SHA-256. The ZIP root is exactly:

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
