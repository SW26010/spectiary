# Release Profiles Are Separate Artifacts

SpecForge treats Portable and Installed builds as a release-variant build
contract, not as a runtime setting. The build artifact decides the local user
state root before any user state is loaded: Installed builds use the Windows
per-user local application data known folder. The first Installed artifact is a
per-user installed app, not a machine-wide `Program Files` install, so its
program files live in a per-user application location while its local user state
remains separate under local application data. Portable builds use an app-owned
profile under `Data` in the package root. For the current no-launcher Portable
artifact, the package root is the executable directory: the zip root contains
`SpecForge.exe`, and `Data` sits beside it. The artifact does not introduce a
launcher or a `bin` subdirectory. CMake validates the selected release profile
and emits one explicit target compile definition such as
`SPECFORGE_RELEASE_PROFILE_PORTABLE` or
`SPECFORGE_RELEASE_PROFILE_INSTALLED`, so the C++ startup path fails at compile
time if the release profile is missing or ambiguous. CMake may use a cache
string for validation, but C++ must branch on mutually exclusive compile
definitions rather than parse a string macro such as
`SPECFORGE_RELEASE_PROFILE="Portable"`.

The initial delivery sequence is Portable first. Installed remains a defined
release profile so the storage decision stays explicit, but the v1 deliverable
is the Portable artifact rather than an installer.

Release artifact presets must name the release profile explicitly, for example
with a `portable` or future `installed` segment. Terms such as `static` describe
linkage and runtime-library shape only; they are not a substitute for the
storage-profile contract.

Build configuration and release profile are orthogonal. Debug and Release
describe optimization/debugging shape; Portable and Installed describe where the
artifact writes user state. App targets must be built with exactly one release
profile compile definition in both Debug and Release configurations. Developer
presets should be profile-explicit rather than implying that Portable or
Installed is the privileged local-development default. Portable must have a
Debug preset so the `Data` root and profile-log behavior can be tested without
waiting for a release-package build.
Presets that build the app target should not keep profile-less names such as
`ninja-msvc-debug` or `vs2022-x64-debug`; their names should say whether the
build is Portable or Installed.

Runtime path decisions should have one owning implementation surface rather than
being re-derived separately by local-state, profile logging, or future package
path code. The exact C++ file and function names may follow the implementation,
but the release profile, package root, local user state root, and default
profile-log directory should come from the same startup path policy.

Portable verification must exercise the storage contract rather than only prove
that the binary starts. Tests or smoke checks should cover both default writes
under `Data` for Portable and the distinct Installed local-state root. The exact
commands, targets, and evidence format may follow the implementation.

We reject UI preferences, empty marker files, environment variables, launchers,
or other runtime side channels for selecting the profile root because storage
location is a startup contract of the release artifact. Portable and Installed
checksums should differ when they write user state to different places.

For Portable builds, SpecForge-owned default writes must stay under the
app-owned package `Data` directory rather than the current working directory or
a Windows user profile directory. The first implementation should preserve the
current local-user-state shape with minimal path changes instead of inventing a
deep directory taxonomy up front. This flat `Data` layout is not a permanent
taxonomy commitment; later releases may introduce a more organized layout with
an explicit migration strategy. `Data` is not a user workspace and should not be
used for user-selected source, annotation, or output files.

Portable profile logs are also SpecForge-owned default writes, so the Portable
default profile directory is `Data/logs`. An explicit diagnostic override such
as `SPECFORGE_PROFILE_DIR` may still redirect profile output because that is a
developer-selected path, not the artifact's default storage location.
Release artifacts expose the same profiler through a runtime `Performance` toolbar menu,
while `SPECFORGE_PROFILE=1` remains the startup automation contract.
High-frequency events enter a bounded in-memory queue and a background writer
owns all steady-state file writes. The queue mutex is never held during file I/O;
ordinary producer/writer lock contention waits for the short in-memory critical
section instead of discarding evidence. Only actual queue-capacity pressure drops
and counts records. Menu stop requests an asynchronous drain, and recorder state
changes wake the event-driven UI. Every completed file ends with a recorder
summary. A session stops after five minutes or 100 MiB so an accidentally left
enabled recorder cannot grow package-local state without bound.
Dear ImGui layout state is also SpecForge-owned default state. Portable builds
store the default ImGui ini file under `Data` instead of writing it beside the
current working directory.

SpecForge may preserve package portability for explicit user-selected paths
that already live under the package root by storing those references relative
to the package root. This support applies to source, annotation, and output
paths, including paths a user independently places under a conventional
`workspace` directory. The application should not create, default to, advertise,
or steer users toward `workspace` as the recommended data location; it is a
compatibility convenience for user choices inside the package root, not a
product-level storage recommendation.

Package-relative path persistence must use structured path references rather
than bare strings, for example a `path_kind` of `package_relative` or
`absolute` plus the stored path text. This keeps package-relative references
distinct from ordinary relative strings. The Portable release-profile contract
does not promise migration for older pre-contract state files that stored raw
paths. Package-relative user path persistence is the first follow-up slice after
the initial Portable artifact succeeds; it is urgent, but it should not block
the initial change that moves SpecForge-owned default writes under `Data`.

A future Installed artifact supports manual in-place updates by running a newer
installer over the existing per-user install. It does not include a background
auto-updater. Uninstall removes installed program files, shortcuts, and
uninstall registration, but preserves local user state by default.

References: Microsoft documents `FOLDERID_LocalAppData` as the per-user local
application data known folder; PortableApps.com Format uses `Data` for user
settings, configuration, and other app data that a local install would usually
store under app data; CMake `target_compile_definitions` is the target-scoped
mechanism for injecting compile definitions.
