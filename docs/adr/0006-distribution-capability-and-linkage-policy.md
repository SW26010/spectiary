# Distribution Capability and Linkage Are Not Binary-Identity Constraints

Status: Accepted.

This decision supersedes only the cross-distribution executable-identity
requirement in
[ADR 0003: Runtime Deployment Metadata Selects Storage](0003-runtime-deployment-metadata.md).
ADR 0003's deployment metadata, storage-profile selection, preflight, and
artifact-binding rules remain accepted.

## Context

ADR 0003 deliberately removed compile-time Portable and Installed profiles so
one channel-neutral `SpecForge.exe` could be reused across Standalone, Portable,
and future installed packaging channels. That is a useful implementation shape
while the dependency graph permits it, and the current build does not need to
change merely because this ADR exists.

It is not, however, a useful long-term architecture constraint.

SpecForge's product principle is to remain lightweight, fast, and focused. That
principle does not require every distribution to contain the same executable
bytes, every capability to exist in a single-file Standalone artifact, or every
third-party dependency to be linked statically. Treating executable identity as
a hard invariant can instead work against the product goal by forcing one or
more of the following:

- statically linking a large or frequently updated dependency only so all
  channels can share one EXE;
- carrying optional or heavyweight runtimes in the baseline artifact even when
  most users do not need them;
- maintaining a handwritten fallback implementation beside a mature library
  solely so a dependency-free Standalone build can claim the same feature;
- choosing a dependency or integration architecture based on packaging shape
  rather than correctness, maintenance cost, and the user-facing capability.

FITS is the immediate motivating example. FITS is a core user-facing capability,
but core capability does not imply that its implementation must be handwritten
or statically linked. If CFITSIO is selected as the supported FITS implementation,
SpecForge should not keep a second minimal FITS reader merely to preserve
byte-identical executables or full Standalone parity.

ASDF illustrates the same issue from the other direction. A Python-based ASDF
adapter may naturally bring a Python runtime and Python packages. Requiring that
runtime to be folded into every distribution would make the baseline artifact
heavier without improving users who never open ASDF data.

The invariant that matters is behavioral: for a given SpecForge version and a
given declared capability set, supported inputs and operations should have the
same defined semantics. SHA-256 equality between executables from different
distribution channels is not a product invariant.

## Decision

SpecForge adopts the following release and dependency policy.

### Cross-distribution executable identity is not required

Standalone, Portable, Installer, WinGet, Scoop, or future distribution channels
are not required to contain byte-identical `SpecForge.exe` files.

Installed and Portable packages may continue to reuse one executable when that
falls out naturally from the build and packaging design. Standalone may also
continue to use that executable today. This is an implementation convenience,
not a contract that future dependencies must preserve.

No immediate build-system or packaging change is required by this ADR.

### Standalone does not guarantee the full capability set

Standalone is allowed to expose a reduced capability set when full parity would
materially increase artifact size, introduce heavyweight runtime requirements,
or distort dependency architecture.

Any such reduction must be explicit, deterministic, documented, and testable.
A release must not silently present a feature as equivalent when the Standalone
artifact actually uses a materially different or incomplete implementation.

A capability omitted from Standalone may remain fully supported in Portable and
installed distributions.

### Dynamic linking is an accepted implementation choice

Static linkage is not a SpecForge architecture requirement. Dynamic linkage is
accepted when it is the better engineering choice.

Linkage is decided per dependency using the factors relevant to that dependency,
including:

- whether the capability belongs to the baseline product or is optional;
- dependency and runtime footprint;
- maintenance and upgrade cadence;
- ABI and toolchain constraints;
- packaging and deployment complexity;
- licensing and redistribution constraints;
- startup, memory, and runtime-performance effects;
- whether independent replacement or isolation is valuable.

A core capability may use a dynamic library. An optional capability may still be
statically linked when that is simpler and sufficiently small. The labels
"core" and "optional" influence the decision but do not mechanically determine
linkage.

### Do not duplicate mature implementations to preserve packaging symmetry

SpecForge must not maintain a second parser, codec, backend, or equivalent
infrastructure implementation solely to keep Standalone dependency-free or to
preserve identical executable bytes across distribution channels.

In particular, if CFITSIO becomes the supported FITS implementation, the
handwritten FITS path may be removed rather than retained as a Standalone
fallback. A separate implementation is justified only when it has an independent
product or engineering purpose that would remain valid even if cross-channel
binary identity were irrelevant.

Optional adapters are different: a deliberately optional adapter may be absent,
and its absence may remove the corresponding capability. That is capability
composition, not a hidden fallback between two supposedly equivalent
implementations.

### Lightweight describes baseline cost, not the sum of every extension

The `Lightweight. Fast. Fluid.` product direction applies primarily to the cost
required to obtain and run the baseline spectrum-inspection workflow.

Heavyweight or language-runtime-backed capabilities may be distributed as
separate adapters or dependency sets so users who do not need them do not pay
that download, install, startup, or maintenance cost. Such extensions must not
move heavyweight work into the plot interaction hot path.

## Consequences

The current release architecture may remain unchanged until a real dependency
or packaging decision benefits from divergence.

Future packaging is free to choose, for example:

```text
Standalone
  smaller / self-contained
  may expose a reduced capability set

Portable
  full selected capability set
  may include adjacent DLLs or adapters

Installed
  full selected capability set
  may include adjacent DLLs or adapters
```

This freedom has several positive consequences:

- mature libraries can replace handwritten infrastructure without requiring a
  duplicate fallback;
- a large dependency does not have to be forced into the baseline executable;
- Python or other language runtimes can remain scoped to capabilities that need
  them;
- static versus dynamic linkage can be chosen for engineering reasons rather
  than cross-channel hash equality;
- Standalone can remain genuinely small even if the overall product grows.

It also increases the release matrix once distributions actually diverge:

- support and diagnostics may need to identify distribution and capability set;
- tests must cover the capability contract of each shipped artifact;
- release documentation must state material capability differences;
- cross-channel verification cannot assume that executable hashes are equal.

Each individual artifact still requires strong internal identity binding. The
schema 5 `artifact.sha256` continues to bind a metadata sidecar to the exact
`SpecForge.exe` beside it. Portable verification may continue to require that
the build-directory EXE and the EXE copied into that same Portable package are
identical. This ADR removes only the requirement that an EXE from one release
channel must also equal the EXE from another channel.

Deployment distribution and storage profile remain orthogonal to feature and
linkage decisions. A dynamic dependency must not be used as an implicit signal
for Portable versus Installed storage behavior; ADR 0003 remains the authority
for that policy.

## Rejected Alternatives

### Require one byte-identical executable for every distribution

Rejected as a long-term invariant. It is simple to verify, but it can force
static linkage, heavyweight baseline dependencies, duplicate fallbacks, or other
architecture choices whose only purpose is preserving executable identity.

Byte-identical reuse remains welcome when it is naturally achievable.

### Require Standalone to provide every product capability

Rejected because a single-file or otherwise highly self-contained artifact
should not become the lowest common denominator for the complete product.
Standalone may trade capability breadth for a smaller and simpler artifact as
long as the difference is explicit.

### Keep a built-in fallback for every dynamically supplied core dependency

Rejected. This makes the executable appear self-sufficient while creating two
implementations whose behavior, edge cases, security fixes, and tests must stay
in sync. A fallback is justified only by an independent product requirement,
not by binary-identity preservation.

### Require all dependencies to be dynamic

Rejected for the same reason that all-static linkage is rejected: linkage is a
per-dependency engineering decision. Small, stable, ubiquitous dependencies may
remain simpler to link statically.

## Validation Contract

Until distributions actually diverge, existing build and Portable-package
verification remain valid and do not need to be weakened.

When a future change introduces different executables or capability sets, that
change must add validation appropriate to the new shape:

- every shipped EXE is finalized and bound to its own metadata digest;
- a package verifier compares an EXE only with the source artifact for that
  package, not with unrelated distribution channels;
- material capability differences are declared and covered by artifact-level
  smoke or integration tests;
- shared capabilities use common conformance tests across distributions where
  practical;
- a missing optional adapter produces an explicit unsupported/unavailable
  capability state rather than silently switching to a materially different
  implementation;
- if a mature library replaces a handwritten core implementation, tests target
  the selected supported implementation rather than requiring parity with a
  second implementation kept only for Standalone.

A future metadata schema may record capability information if release
composition becomes complex enough that human-facing release documentation is
insufficient. This ADR does not require that schema change before there is an
actual divergent artifact to describe.

## Related Decisions and Documents

- [ADR 0003: Runtime Deployment Metadata Selects Storage](0003-runtime-deployment-metadata.md)
  remains authoritative for distribution metadata and storage-profile
  selection.
- [Release Artifacts](../release_artifacts.md) remains authoritative for the
  current schema 5 executable/metadata binding and Portable package verifier.
- [Technical Direction](../technical_direction.md) remains authoritative for
  the lightweight native product direction and dependency-selection principles.
