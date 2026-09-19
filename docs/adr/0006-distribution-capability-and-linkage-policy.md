# Distribution Capability and Dependency Linkage Policy

Storage-layout amendment: [ADR 0015](0015-application-storage-cutover.md)
supersedes the historical `Data/` and `%LOCALAPPDATA%\SpecForge` placement below.
Active persistence now uses the final config/state/logs/unsaved role layout.

Identity/metadata amendment: [ADR 0011](0011-project-identity-contracts.md)
supersedes schema 5 and the historical artifact names with schema 6,
`Spectiary.exe` and `spectiary_metadata.json`. The distribution capability and
dependency linkage decisions below remain in effect.

Status: Accepted. Supersedes
[ADR 0003: Runtime Deployment Metadata Selects Storage](0003-runtime-deployment-metadata.md).

ADR 0003 is retained as decision history. This ADR replaces its release-artifact
model rather than treating the former single-executable rule as a constraint to
work around.

## Context

ADR 0003 solved a real problem: Portable and Installed storage policy had been
encoded into compile-time release profiles, which unnecessarily produced
different executables. It replaced that model with deployment metadata and a
single channel-neutral `SpecForge.exe`.

The single-executable result was useful, but it mixed several independent
questions into one artifact rule:

- what capabilities a release provides;
- which implementation provides a capability;
- whether a dependency is linked statically or dynamically;
- how a release is packaged and distributed;
- where SpecForge-owned mutable state is stored;
- how an individual executable is identified and verified.

These concerns should not constrain one another.

SpecForge's product direction is `Lightweight. Fast. Fluid.`. That requires the
baseline application to stay small, responsive, and operationally simple. It
does not require every distribution to expose the same capability set, every
release channel to contain byte-identical executables, or every dependency to be
statically linked.

FITS is the immediate example. FITS is a core SpecForge capability, but that
does not imply that SpecForge should maintain its own FITS implementation. If
CFITSIO is the appropriate supported implementation, using CFITSIO directly is
preferable to retaining a second handwritten reader only so a dependency-free
Standalone executable can preserve feature parity or binary identity.

ASDF demonstrates the same issue at a larger scale. A Python-based ASDF adapter
may naturally require a Python runtime and Python packages. Folding those into
every artifact merely to preserve one executable shape would make the baseline
product heavier for users who do not need ASDF.

The architecture therefore needs to preserve product behavior and release
clarity without treating executable identity as a product requirement.

## Decision

### Distribution formats do not define executable identity

Standalone, Portable, Installer, WinGet, Scoop, and future distribution formats
are not required to contain byte-identical `SpecForge.exe` files.

They may continue to share one executable whenever that is naturally convenient.
The current build and packaging paths do not need to change merely because this
ADR is accepted. Shared executable bytes are an implementation property, not an
architecture invariant.

No future dependency, capability, or packaging decision should be distorted
solely to preserve cross-distribution executable hash equality.

### Standalone is not the normative full-feature artifact

Standalone is a convenience distribution optimized for being small and
self-contained. It does not guarantee the complete SpecForge capability set.

A capability may be absent from Standalone even when it is a core product
capability, if providing it would require dependencies or runtime components
that conflict materially with the Standalone size or self-containment goal.

Such differences must be explicit, deterministic, documented, and tested.
Standalone must not silently substitute a materially different or incomplete
implementation while presenting it as equivalent to the supported implementation
used elsewhere.

### Capability, distribution, and storage are separate dimensions

A capability set describes what an artifact can do. Distribution describes how
that artifact is delivered. Storage profile describes where SpecForge-owned
mutable state is written.

None of these dimensions is inferred from linkage shape, DLL presence, install
path, parent process, registry state, or other environmental heuristics.

The deployment metadata model introduced by ADR 0003 remains useful and is
carried forward here:

- `distribution` records the packaging channel;
- `storage_profile` explicitly selects SpecForge-owned mutable-state storage;
- `portable` storage uses `<package-root>\Data`;
- `local_app_data` storage uses `%LOCALAPPDATA%\SpecForge`;
- absent deployment metadata continues to represent Standalone with
  `local_app_data` storage unless a later ADR changes that behavior;
- invalid explicit deployment metadata continues to fail startup rather than
  silently selecting another storage root.

Capability composition is allowed to differ between artifacts. It must not be
encoded indirectly by abusing `distribution` or `storage_profile` as a feature
flag.

### Static and dynamic linkage are both accepted

SpecForge has no all-static or all-dynamic linkage policy.

Linkage is decided per dependency according to the engineering properties of
that dependency, including:

- whether the associated capability is baseline or optional;
- binary and runtime footprint;
- maintenance and upgrade cadence;
- ABI and toolchain constraints;
- packaging and deployment complexity;
- licensing and redistribution requirements;
- startup, memory, and runtime-performance effects;
- whether independent replacement, isolation, or adapter boundaries are useful.

A core capability may use a dynamically linked library. An optional capability
may use a statically linked library. "Core" and "optional" are product concepts,
not mechanical linkage rules.

### Prefer one supported implementation over packaging-driven fallbacks

SpecForge should use mature upstream libraries where they are the better
implementation choice and should not maintain duplicate infrastructure merely to
preserve Standalone parity or executable identity.

For FITS, if CFITSIO is selected as the supported implementation, the handwritten
FITS reader may be removed rather than retained as a fallback. A second FITS
implementation is justified only if it has an independent product or engineering
purpose that would still exist without any Standalone or cross-channel binary
identity requirement.

The same rule applies to parsers, codecs, runtimes, backends, and similar
infrastructure.

Optional adapters are not fallbacks. If an adapter is deliberately optional,
its absence means the corresponding capability is unavailable; SpecForge should
report that state explicitly.

### Lightweight means low baseline cost

`Lightweight. Fast. Fluid.` describes the cost of obtaining and running the
baseline spectrum-inspection workflow, not the sum of every capability that the
project may eventually support.

Heavyweight dependencies, language runtimes, or specialized adapters should be
scoped to the artifacts or capability packages that need them when doing so
keeps the baseline product smaller and simpler.

This rule does not require aggressive modularization in advance. Dependencies
should remain statically linked or embedded while that is the simpler and better
engineering choice. Separation is introduced when a real dependency justifies
it.

### Artifact identity remains per artifact

Dropping cross-distribution executable equality does not weaken verification of
an individual artifact.

Schema 6 `artifact.sha256` continues to bind metadata to the exact
`Spectiary.exe` beside it, following ADR 0011. The production ordering remains:

```text
link -> sign -> hash -> metadata finalization -> package
```

when signing is introduced.

A package verifier may and should require that the executable copied into that
specific package is identical to the finalized build artifact from which that
package was produced. It must not assume that an executable belonging to one
distribution is identical to an executable produced for another distribution.

## Consequences

The current release pipeline may remain exactly as it is until a concrete
change benefits from a different linkage or capability shape.

Future artifacts are free to evolve toward a model such as:

```text
Standalone
  small and self-contained
  capability set may be reduced

Portable
  selected full capability set
  may contain adjacent DLLs, runtimes, or adapters

Installed
  selected full capability set
  may contain adjacent DLLs, runtimes, or adapters
```

This permits mature libraries to replace handwritten infrastructure without
requiring duplicate fallback paths. Large or specialized dependencies no longer
need to be forced into every artifact. Python and similar runtimes can remain
scoped to the capabilities that actually require them.

Once artifact capability sets diverge, the release matrix becomes more explicit:

- release documentation must state material capability differences;
- diagnostics should identify the running artifact and available capabilities;
- artifact-level tests must cover the capability contract that is actually
  shipped;
- shared capabilities should use common conformance tests across artifacts where
  practical;
- missing optional components must produce an explicit unavailable state.

A future metadata schema may record capabilities when composition becomes
complex enough to justify machine-readable declarations. This ADR does not
require such a schema change before an actual divergent artifact exists.

## Rejected Alternatives

### Preserve one byte-identical executable across every distribution

Rejected as an architecture invariant. It is convenient while cheap, but it can
force dependency, implementation, or packaging choices whose only purpose is to
preserve hash equality.

Byte-identical reuse remains welcome when it happens naturally.

### Guarantee full Standalone feature parity

Rejected. Standalone should not become the lowest common denominator that
determines which dependencies or implementations the rest of SpecForge may use.
Its reduced capability set is acceptable when that is the appropriate tradeoff
for a small self-contained artifact.

### Maintain a built-in fallback for every dynamically supplied dependency

Rejected. Two nominally equivalent implementations create duplicated parsing or
backend logic, separate edge cases, duplicated tests, and independent maintenance
burden. Packaging symmetry alone is not sufficient justification.

### Require every core dependency to be static

Rejected. A dependency can serve a core capability while remaining dynamically
linked when size, maintenance, ABI, upgrade, or deployment considerations make
that the better choice.

### Require every optional dependency to be dynamic

Rejected. Optionality does not imply dynamic linkage. Small and stable optional
components may still be simpler to compile into an artifact.

## Validation Contract

Existing verification remains valid until a release actually diverges. This ADR
must not be used as a reason to weaken current checks preemptively.

When different executables or capability sets are introduced, the implementing
change must update validation so that:

- each shipped executable is finalized and bound to its own metadata digest;
- each package is verified against the exact source executable used to build
  that package;
- no cross-distribution hash equality is assumed;
- material capability differences are declared and covered by artifact-level
  smoke or integration tests;
- shared capabilities retain common behavioral tests where practical;
- missing optional components produce an explicit unsupported/unavailable state;
- replacing a handwritten implementation with a mature library tests the chosen
  supported implementation rather than preserving a second implementation only
  for Standalone.

## Related Decisions and Documents

- [ADR 0002: Release Profiles Are Separate Artifacts](0002-release-profile-artifacts.md)
  is historical and was already superseded by ADR 0003.
- [ADR 0003: Runtime Deployment Metadata Selects Storage](0003-runtime-deployment-metadata.md)
  is superseded by this ADR.
- [Release Artifacts](../release_artifacts.md) remains the contract for the
  current schema 6 artifact and Portable pipeline, with identity and metadata
  boundaries defined by [ADR 0011](0011-project-identity-contracts.md).
- [Technical Direction](../technical_direction.md) remains authoritative for
  the native, lightweight, performance-first product direction.
