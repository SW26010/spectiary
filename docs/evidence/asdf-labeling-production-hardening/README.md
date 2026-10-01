# Schema 2.0 ASDF Labeling Production Hardening Evidence

Date: 2026-08-31 (UTC measurement). Status: accepted historical evidence for the
recorded implementation commit. Current schema and persistence ownership are
maintained in [sample labeling](../../reference/labeling/sample_labeling.md) and
[labeling persistence ownership](../../reference/labeling/labeling_persistence_ownership.md).
Old product names and run URLs in the raw records describe the measured version.

Status: accepted evidence for [GitHub issue #78](https://github.com/SW26010/SpecForge/issues/78)

This report records the bounded hardening and scale evidence for the schema 2.0
sample-labeling production codec and store. The measured binary came from clean
commit `064d461d1b6f5b76675755e965ceb3ff7c7e2d98`. The evidence commit adds only
this report and its small structured records; it does not change the measured
implementation.

The machine-readable records are:

- [`environment.json`](environment.json)
- [`commands.txt`](commands.txt)
- [`results.json`](results.json)

Generated 1M `.asdf` files, per-process stdout/stderr, build artifacts, and
mutation working files remain reproducible build output and are not checked in.

## Decision summary

Both fixed one-million-sample datasets completed all five production operations
in five measured subprocesses after one warm-up subprocess per operation. All
50 measured records reported success, semantic or generation validation passed,
and no benchmark error kind was emitted. The fixed 73-case mutation corpus, the
atomic publication property matrix, and the pinned Python interoperability and
checksum matrix also passed.

No production codec or store defect was found, so issue #78 did not require a
synthetic `fix(labeling)` commit. CI did expose two reproducibility defects in
the test infrastructure: binary fixture bytes were vulnerable to checkout line
ending conversion, and the Python oracle needed an explicitly compatible ABI.
They were fixed independently by commits `60a5b019015d` and `064d461d1b6f`.

The checksum policy remains the decision in
[ADR 0007](../../adr/0007-asdf-labeling-block-checksum-policy.md): the writer emits an
all-zero block checksum and the reader rejects every nonzero checksum as the
controlled `UnsupportedProfile` wire profile. It never accepts a checksum
without verifying it.

## Environment

The benchmark ran on Windows 11 Pro build 26200, on an AMD Ryzen 7 9850X3D
(8 physical cores, 16 logical CPUs) with 33,727,803,392 bytes of physical
memory. The benchmark workspace was on the `C:` NTFS volume.

The production benchmark used MSVC 19.44.35228.0, CMake 4.4.2, Ninja, Release,
the `x64-windows-static` vcpkg triplet, yaml-cpp 0.9.0#1, and zlib 1.3.2#1.
The dedicated preset was
`ninja-msvc-release-static-asdf-labeling-hardening`; the benchmark target is
excluded from the default build.

The accepted GitHub run used Python 3.12.10, ASDF 5.3.1, ASDF Standard 1.5.0,
and NumPy 2.5.2 for the independent oracle. Exact local commands, rather than a
shell-history paraphrase, are preserved in `commands.txt`.

## Deterministic datasets

Both cases use `sample_count = 1,000,000`, the same fixed source identity,
schema 2.0 metadata, four labels (`accepted`, `review`, `rejected`, and
`uncertain`, stable codes 0 through 3), and generator version
`asdf-production-benchmark-source-v1`.

The base values vector starts as `-1`. Every index divisible by 100 receives
`(index / 100) modulo 4`, giving this exact distribution:

| Value | Count |
| ---: | ---: |
| `-1` | 990,000 |
| `0` | 2,500 |
| `1` | 2,500 |
| `2` | 2,500 |
| `3` | 2,500 |

The two roster cases are fixed as follows:

- **source-index:** `identity_kind = source_index`, with no explicit names
  block and a recorded roster width of zero.
- **explicit-unicode:** `identity_kind = explicit_names`; name `i` is
  `样本-%08u-α-𐐷.fits`. Names are deterministic and unique, combine ASCII,
  Chinese, Greek, and non-BMP Unicode, and have a fixed maximum width of 20
  Unicode code points.

The one-value operation changes one legal value. The 1,000-value operation
changes 1,000 evenly spaced legal values. Both call the same production values
rewrite API; they intentionally show that mutation count does not select a
different store path even though the current codec re-encodes the values
ndarray.

## Benchmark protocol

The wrapper configured and built Release, then ran each case/operation pair in
an independent subprocess. Each pair received one warm-up and five measured
runs. Every subprocess used a unique temporary directory; that directory and
its generated `.asdf` were deleted after the record was captured.

Document construction and non-measured setup happened before the memory sampler
started. The sampler ran only around the named production API call at a 1 ms
sampling interval. `sampled peak` therefore describes the API interval;
`process peak working set` remains the separate lifetime counter.

The APIs under measurement were:

- initial write: `WriteSampleLabelingAsdfDocumentAndOpenAtomically`;
- read/open: `OpenSampleLabelingAsdfDocumentStore`;
- one-value and 1,000-value rewrite:
  `RewriteSampleLabelingAsdfValuesAtomically`;
- metadata rewrite: `RewriteSampleLabelingAsdfDocumentAndReopenAtomically`.

All rewrite setup documents contained seeded supported unknown mappings. Read,
values rewrite, and metadata rewrite checked that they remained present. Full
write and metadata rewrite succeeded only after the production API reopened the
file and `KnownDocumentGenerationMatches` compared the published semantic
generation. Values rewrite succeeded only when the production snapshot values
equaled the intended replacement. The explicit-roster values cases additionally
required encoded roster block reuse.

## Scale results

The following columns are min / median / max across the five measured runs.
Memory is MiB (1,048,576 bytes). File size is the median exact byte count; it
was stable across all five runs. Full per-run wall and CPU samples plus byte
statistics for every memory field are in `results.json`.

### Source-index roster

| Operation | Wall ms | CPU ms | File bytes | Sampled peak WS MiB | Sampled peak private MiB | Process peak WS MiB | Roster reused |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Initial write + reopen | 22.772 / 23.939 / 26.252 | 0.000 / 31.250 / 31.250 | 26,135 | 8.51 / 12.29 / 15.66 | 4.96 / 8.53 / 12.37 | 16.10 / 16.10 / 16.11 | N/A (no block) |
| Read/open | 10.586 / 10.693 / 10.981 | 0.000 / 0.000 / 0.000 | 26,327 | 8.67 / 8.72 / 16.04 | 4.69 / 4.97 / 12.62 | 16.38 / 16.39 / 16.46 | N/A |
| Rewrite 1 value | 13.739 / 14.132 / 17.379 | 15.625 / 15.625 / 31.250 | 27,188 | 20.30 / 20.52 / 20.55 | 16.38 / 16.61 / 16.62 | 20.54 / 20.55 / 20.58 | N/A (no block) |
| Rewrite 1,000 values | 13.408 / 13.506 / 16.186 | 15.625 / 15.625 / 15.625 | 27,919 | 20.54 / 20.55 / 20.57 | 16.61 / 16.61 / 16.63 | 20.55 / 20.55 / 20.57 | N/A (no block) |
| Metadata rewrite + reopen | 22.947 / 23.500 / 24.995 | 15.625 / 31.250 / 31.250 | 27,162 | 16.73 / 20.31 / 23.66 | 12.78 / 16.34 / 20.16 | 24.12 / 24.14 / 24.15 | N/A |

### Explicit Unicode roster

| Operation | Wall ms | CPU ms | File bytes | Sampled peak WS MiB | Sampled peak private MiB | Process peak WS MiB | Roster reused |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Initial write + reopen | 1578.767 / 1594.430 / 1613.213 | 1531.250 / 1578.125 / 1593.750 | 2,762,553 | 231.66 / 236.28 / 239.20 | 237.11 / 242.01 / 243.07 | 240.80 / 240.80 / 240.81 | No (fresh encode) |
| Read/open | 478.792 / 482.871 / 499.261 | 453.125 / 484.375 / 484.375 | 2,762,745 | 236.47 / 237.33 / 238.73 | 240.90 / 243.25 / 243.41 | 240.99 / 241.02 / 241.07 | N/A |
| Rewrite 1 value | 300.442 / 302.931 / 305.993 | 281.250 / 281.250 / 296.875 | 2,763,706 | 260.29 / 260.31 / 260.39 | 265.36 / 265.41 / 265.63 | 260.29 / 260.31 / 260.39 | Yes |
| Rewrite 1,000 values | 298.728 / 307.577 / 314.898 | 281.250 / 281.250 / 296.875 | 2,764,437 | 260.29 / 260.30 / 260.33 | 265.41 / 265.43 / 265.64 | 260.29 / 260.30 / 260.33 | Yes |
| Metadata rewrite + reopen | 1677.870 / 1697.332 / 1726.106 | 1640.625 / 1671.875 / 1703.125 | 2,763,670 | 396.39 / 401.19 / 402.32 | 407.41 / 410.58 / 412.83 | 404.49 / 404.65 / 404.68 | No (full encode) |

Every row reported `success = true`, `error_kind = none`, and the expected
publication result. Read/open has no publication. The two explicit Unicode
values rows reported `roster_block_reused = true`; source-index has no roster
block to reuse. Windows process CPU accounting was quantized at 15.625 ms on
this host, so a zero CPU observation for a sub-quantum source-index call means
“below this counter's resolution,” not “no CPU work.”

## Bounded mutation evidence

The required corpus used seed `0x78c0de2026`, 73 cases, and a 1,048,576-byte
per-input maximum. It completed with 13 controlled accepts and 60 typed rejects:

| Category | Cases |
| --- | ---: |
| YAML structure | 16 |
| YAML scalar | 10 |
| Block header | 14 |
| Block index | 11 |
| Payload | 16 |
| Supported unknown metadata | 6 |

The corpus exercises the production disk reader, deterministic regeneration,
canonical validation for accepted cases, `ResourceLimitExceeded` or other
non-`None` typed errors where required, durable-base handling, and supported
unknown-mapping read/rewrite/read preservation. Before a production call it
places the current bounded input and pending mutation metadata in the artifact
directory; a successful case removes them. A crash, abort, or timeout therefore
still leaves the active reproducer for CI upload, while successful cases do not
accumulate corpus artifacts.

## Atomic publication evidence

The standalone store property executable passed in 6.10 seconds. Its six
operation cases cover first publication, overwrite, values rewrite, metadata
rewrite, rewrite-and-reopen, and repeated values publication through one
snapshot. Each operation matrix cell injects a before-replace exception and
then retries the same intended generation.

Ten focused failure cases cover semantic and resource preflight, preservation
identity, missing durable base, temporary stream failure, replacement failure,
external replacement, reopen generation mismatch, unsupported checksum
profile, and unknown-metadata construction failure. The assertions compare
bytes and SHA-256, values, timestamps, unknown mappings, snapshots, durable
bases, temporary siblings, typed error flags, recovery state, and retry output.
They use document content and explicit result flags, not sleeps or file times.

## Checksum and Python interoperability evidence

The pinned checksum oracle passed under Python 3.12.10 / ASDF 5.3.1. Native
zero-checksum zlib output was readable with Python checksum validation both off
and on, and the native and Python semantic summaries were equal. A Python
zero-checksum file opened natively with a durable base.

Python nonzero-checksum uncompressed and zlib files were both rejected by the
native reader as `UnsupportedProfile`. Corrupt zero-checksum zlib was classified
as `MalformedDocument`. A changed checksummed payload and a changed checksum
field were both locked to checksum-mismatch failures when Python validation was
enabled. Attempting to overwrite a checksummed file preserved its complete
bytes, SHA-256, and unknown mapping while returning the controlled unsupported
profile.

The broader pinned interoperability evidence contained 36 records and zero
failures. GitHub Actions run
[`33411277559`](https://github.com/SW26010/SpecForge/actions/runs/33411277559)
ran at the same source commit. Its native/headless required gate and the two
pinned interoperability/checksum CTests passed. The uploaded headless evidence
artifact digest is recorded in `results.json`.

## Limitations and interpretation

- Read/open is an ordinary warm-cache observation after setup on the same
  machine. No OS cache flush was requested or claimed.
- Working set and private bytes are process observations. They are not the
  codec's 512 MiB resident-budget contract, which bounds codec-controlled bulk
  allocations rather than total process RSS.
- These measurements describe one machine and are not a cross-machine SLA or a
  performance pass/fail threshold.
- The benchmark calls production store APIs directly. It is not a direct
  measurement of product UI response time, scheduling, rendering, or user
  interaction latency.
- The lack of a benchmark time or RSS threshold does not remove hard resource
  boundaries. Codec preflight allocation limits, decoded-block limits, bounded
  mutation input, and file-size validation remain enforced separately.
- CPU time for very short calls is limited by the host counter resolution; wall
  time is the more informative observation for those source-index rows.

These constraints are part of the evidence, not caveats to erase on a future
rerun. New measurements should preserve them or explicitly document a changed
protocol.
