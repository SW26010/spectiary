# ASDF Labeling Block Checksum Policy

Status: Accepted.

## Context

SpecForge's production sample-labeling document uses ASDF file format 1.0.0,
declares ASDF Standard 1.5.0, uses the `core/asdf-1.1.0` and
`core/ndarray-1.0.0` tags, and carries SpecForge schema 2.0.0 metadata. These
versions describe independent layers. A binary block checksum is a low-level
wire-profile capability; it is not part of the SpecForge schema version or its
semantic validation.

The [ASDF low-level block layout][asdf-block-layout] defines `checksum` as an
optional 16-byte MD5 digest of the used data in the block. `used_size` is the
number of meaningful on-disk bytes, excluding the header. Therefore the digest
covers exactly the on-disk `used_size` bytes: compressed bytes for a compressed
block and decoded bytes for an uncompressed block. Unused allocated padding and
the block header are outside the digest. The special all-zero checksum means
that no checksum verification is to be performed.

Python ASDF does not validate block checksums by default because validation
requires reading all relevant block bytes. Its compressed-block checksum
behavior also changed recently: [Python ASDF PR 2027][asdf-pr-2027] corrected
the implementation to checksum compressed on-disk bytes, and the
[ASDF 5.3.1 compression documentation][asdf-compression] warns that files from
versions at or below 5.2.0 and files from later versions may fail cross-version
checksum validation.

SpecForge's canonical writer currently emits zlib-compressed arrays and
all-zero checksums. Its reader already rejects any nonzero checksum before
payload hydration. Issue #78 requires deciding whether broader checksum support
provides enough interoperability or integrity value to justify a larger
production profile.

## Decision

The production policy is:

```yaml
writer: emit all-zero checksum
reader: controlled rejection of nonzero checksum
```

The writer continues to put sixteen zero bytes in every block checksum field.
This is a standard-conforming request not to perform block checksum
verification; it is not an empty or invalid checksum.

The reader returns `UnsupportedProfile` for every nonzero block checksum. It
does not accept the block and ignore the digest, and it does not report the
condition as SpecForge semantic corruption. A file cannot become a durable
owner generation until every referenced block is within the supported
production wire profile.

An atomic store publication over an existing checksummed file first encounters
the same controlled reader rejection. It must leave the existing file
byte-for-byte unchanged, including supported or unsupported unknown metadata,
rather than replacing a generation it could not validate and preserve.

This decision does not change SpecForge schema 2.0.0, the declared ASDF Standard
version, the ASDF file format version, core tags, zlib compression level, or any
semantic validation rule.

### Corruption classification

The native classifications are deliberately limited to what the supported
profile can prove:

- a zero-checksum zlib stream that cannot be decompressed is a controlled
  `MalformedDocument`;
- changed zero-checksum used bytes that still decode and remain semantically
  valid are accepted, because all zero explicitly promises no checksum
  verification;
- any nonzero checksum is `UnsupportedProfile`, whether the payload is intact,
  the payload was changed, or the checksum field was changed;
- checksum failure is not application-schema semantic corruption;
- MD5 block checksums are corruption-detection data, not authentication or a
  security boundary.

If corruption turns a previously nonzero checksum into sixteen zero bytes, the
resulting header itself requests no checksum verification. No implementation can
recover the lost intent from that field alone.

## Interoperability Evidence

The pinned oracle is
[`tools/asdf_labeling_hardening/checksum_oracle.py`](../../tools/asdf_labeling_hardening/checksum_oracle.py).
It calls the production codec and store APIs through a small test executable
diagnostic surface and uses Python ASDF 5.3.1 as the independent implementation.
It does not add an MD5 implementation or dependency to the product.

The accepted run used Python 3.12.13, ASDF 5.3.1, NumPy 2.5.2, Windows AMD64,
and the native MSVC debug test executable. The decision is based on wire and
semantic results, not timings:

| Direction or mutation | Python ASDF 5.3.1 | Native production result |
| --- | --- | --- |
| Native zero-checksum zlib, validation off | Read; expected semantic summary | Read; same summary |
| Native zero-checksum zlib, validation on | Read; expected semantic summary | Read; same summary |
| Python zero-checksum zlib | Read with validation off/on | Read; durable production base |
| Python checksummed uncompressed | Read with validation off/on | Controlled `UnsupportedProfile` |
| Python checksummed zlib | Read with validation off/on | Controlled `UnsupportedProfile` |
| Corrupt zero-checksum zlib payload | Decompression failure with validation off/on | Controlled `MalformedDocument` |
| Changed checksummed payload that still decodes | Read changed values with validation off; checksum mismatch with validation on | Controlled `UnsupportedProfile` |
| Corrupt nonzero checksum field | Read with validation off; checksum mismatch with validation on | Controlled `UnsupportedProfile` |
| Changed zero-checksum used bytes that still decode | Read changed values with validation off/on | Read the same changed values |
| Store overwrite of checksummed file with unknown root mapping | Not applicable | `CodecFailure` / `UnsupportedProfile`; bytes and SHA-256 unchanged |

For both the compressed and uncompressed checksummed Python files, every
nonzero header digest equaled an independently computed MD5 of the exact on-disk
used bytes. For the native file, both roster and values blocks were zlib with
all-zero checksum fields. The native and Python semantic summaries were equal.

The store overwrite probe also checked that the unknown-mapping token remained
present and that the complete before/after bytes and SHA-256 were identical.
The diagnostic process returned normally for all native rejection cases; no
abort or uncaught exception was observed.

### Reproducing the matrix

Use the repository's required Windows build wrapper, then install the pinned
oracle dependencies into an isolated target directory:

The commands below use the current Spectiary target and executable names after
the [project rename](../development/project_rename.md); the decision and recorded evidence
above retain their historical names.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  scripts\build-ninja-msvc-debug.ps1 `
  -Configure `
  -TimeoutSec 300

powershell -NoProfile -ExecutionPolicy Bypass -File `
  scripts\build-ninja-msvc-debug.ps1 `
  -Target spectiary_sample_labeling_asdf_codec_tests `
  -TimeoutSec 300

python -m pip install `
  --target build\asdf-labeling-checksum-policy\python `
  -r tools\asdf_labeling_spike\requirements.txt

$env:PYTHONPATH = (Resolve-Path build\asdf-labeling-checksum-policy\python)
python tools\asdf_labeling_hardening\checksum_oracle.py `
  --native build\ninja-msvc-debug\spectiary_sample_labeling_asdf_codec_tests.exe `
  --output build\asdf-labeling-checksum-policy\checksum-matrix.json `
  --work-dir build\asdf-labeling-checksum-policy\files
```

The oracle refuses to run with an ASDF version other than 5.3.1. It writes one
structured JSON result, asserts every expected classification, and exits
nonzero if the matrix changes. Generated ASDF files and JSON evidence stay under
`build/` and are not repository fixtures.

## Consequences

The canonical writer remains simple and retains its existing zlib output,
without a second pass or streaming digest state. The reader never enters the
unsafe middle state of accepting a digest it does not verify.

This policy does not provide block-level detection for every otherwise legal
payload mutation. Existing structural validation, zlib stream validation,
semantic validation, bounded decoding, atomic replacement, reopen verification,
and optional artifact-level hashes remain separate protections with separate
contracts. None should be described as checksum verification when the block
field is zero.

Supporting nonzero checksums would require all of the following, not merely an
MD5 function:

- exact compressed and uncompressed used-byte semantics;
- bounded or streaming validation integrated with the reader;
- additional reads, buffering, or digest state and their resource accounting;
- corruption classification that remains distinct from schema errors;
- checksum-aware preservation and atomic rewrite behavior;
- cross-version compatibility tests, including the pre/post-5.2.1 Python ASDF
  compressed-block difference;
- ongoing interoperability and mutation coverage for each supported compression
  profile.

There is currently no product requirement to adopt a checksummed third-party
file as a canonical writable owner. The observed Python interoperability of the
standard-conforming zero-checksum zlib profile provides no countervailing need
to pay these costs now.

## Rejected Alternatives

### Accept nonzero checksums without validating them

Rejected. This would claim compatibility while discarding the only meaning of
the field. It is less safe than an explicit controlled rejection.

### Add MD5 and accept checksummed blocks now

Rejected. The experiment showed that the current zero-checksum profile already
interoperates with Python ASDF 5.3.1. No current product requirement or measured
integrity benefit justifies the implementation and maintenance surface.

The rejection is not based on MD5's age. The checksum is not an authentication
mechanism; the decision is based on supported-profile scope, cost, and product
need.

### Emit nonzero checksums while continuing to reject them

Rejected. SpecForge must be able to reopen every canonical file it publishes.

### Treat checksum support as a SpecForge schema revision

Rejected. Block checksums belong to the ASDF wire profile and do not change the
sample-labeling semantic model.

## Reopening This Decision

A later ADR may add verified nonzero-checksum support when at least one of these
conditions exists:

- a product workflow must adopt checksummed third-party files as writable
  canonical owners;
- field evidence shows material data-integrity failures that block checksums
  would detect and existing end-to-end integrity measures do not address;
- an interoperability partner requires checksummed output or verified input;
- measured implementation cost, memory, and latency are acceptable for the
  production autosave and open paths.

Reopening requires a design that verifies the exact on-disk used bytes for every
accepted compression profile, never accepts nonzero checksums without
verification, and expands the pinned oracle and production mutation/publication
tests. It must separately decide reader and writer policy; accepting verified
input does not automatically require emitting checksums.

## Related Decisions and Evidence

- [Issue #78](https://github.com/SW26010/SpecForge/issues/78) defines the ASDF
  production-hardening scope.
- [ASDF Standard low-level file layout][asdf-block-layout] defines block fields
  and zero-checksum semantics.
- [Python ASDF 5.3.1 change log][asdf-changelog] records the compressed checksum
  correction and support for disabling checksums.
- [Python ASDF PR 2027][asdf-pr-2027] documents the compressed used-byte fix and
  validation overhead.
- [Python ASDF compression documentation][asdf-compression] records the
  compatibility boundary around version 5.2.1 and the default validation mode.

[asdf-block-layout]: https://www.asdf-format.org/projects/asdf-standard/en/1.4.0/file_layout.html#block-header
[asdf-changelog]: https://www.asdf-format.org/projects/asdf/en/stable/asdf/changes.html
[asdf-compression]: https://www.asdf-format.org/projects/asdf/en/stable/asdf/arrays.html#compression
[asdf-pr-2027]: https://github.com/asdf-format/asdf/pull/2027
