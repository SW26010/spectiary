# ASDF labeling v1 representation and native codec spike

This is the decision record and reproducible evidence for GitHub issue #77. It
does not replace issue #74 as the owner of the canonical labeling schema or the
production persistence lifecycle.

## Decision summary

1. Explicit sample names use a standard ASDF one-dimensional Unicode string
   ndarray (`datatype: [ucs4, width]`). A source-index roster omits the names
   array and declares `identity_kind: source_index`.
2. Unlabeled values remain ordinary `int32` values containing `-1`, with the
   business meaning declared by `annotation.missing: {semantic: unlabeled,
   value: -1}`. Do not use scalar `ndarray.mask: -1` in v1.
3. The wire baseline is ASDF Standard 1.5.0, root `core/asdf-1.1.0`, and
   `core/ndarray-1.0.0`. ASDF file format remains 1.0.0.
4. The second-round writer recommendation is internal zlib level 6 blocks,
   zero checksum, no block index, and little-endian `int32`/UCS-4 payloads.
   Atomic label autosave copies an unchanged encoded roster block verbatim and
   re-encodes only the values block. The native subset now implements and
   interoperably validates this path; production integration remains separate.
5. The checked-in corpus contains 9 approved valid documents, 1 standard-valid
   document outside the narrow wire profile, 4 ASDF-valid semantic violations,
   and 4 structurally malformed documents.
6. The strictly scoped native codec is the recommended production starting
   point. `asdf-cxx` 8.0.0 is not recommended without a substantially broader
   patch than its current error and string-array model permits.
7. Compact JSON + whole-document zlib is competitive in size and full-rewrite
   throughput, so JSON is not rejected on performance grounds. ASDF remains the
   canonical recommendation because its standard typed arrays and independently
   encoded blocks permit a verified 13-19 ms 1M-sample autosave path. The native
   subset now proves the same block-preserving wire operation; production must
   retain that property or reconsider JSON before locking the profile.

## Reference environment

The experiment was run on Windows with Python 3.12.13, `asdf` 5.3.1,
`asdf-standard` 1.5.0, NumPy 2.5.2, and psutil 7.0.0. Dependencies are pinned in
`tools/asdf_labeling_spike/requirements.txt`. Generated benchmark evidence is
written to the ignored `build/asdf-labeling-spike-results` directory; every
experiment is reproducible from `tools/asdf_labeling_spike/README.md`.

The golden corpus is in `tests/fixtures/asdf_labeling`. Its `manifest.json`
pins SHA-256 for every file and records the expected structural and semantic
result. Semantic JSON companions deliberately compare the durable meaning,
not Python or C++ codec object types.

## Approved sample roster representation

For an explicit roster, v1 uses:

```yaml
sample_roster:
  identity_kind: explicit_names
  names: !core/ndarray-1.0.0
    source: 0
    datatype: [ucs4, 20]
    byteorder: little
    shape: [1000000]
```

The fixed-width UCS-4 payload can be larger than a YAML sequence, particularly
when one long name determines the width. It is nevertheless the better
long-term representation: the roster is a homogeneous, sample-aligned vector;
the standard ndarray makes shape/type/order explicit; it avoids turning tens
of megabytes of names into YAML metadata; and it is dramatically cheaper to
rewrite and hydrate at scale.

Measured comparison at 1M samples (one run per case, including flush and
atomic replacement for rewrite):

| Names | Representation | File | Metadata | Initial write | Atomic rewrite | Hydration | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| short ASCII | YAML sequence | 24.80 MiB | 20.98 MiB | 4.98 s | 5.44 s | 6.95 s | 556.9 MiB |
| short ASCII | string ndarray | 76.30 MiB | 1.2 KiB | 0.12 s | 0.07 s | 0.08 s | 343.7 MiB |
| variable Unicode | YAML sequence | 44.91 MiB | 41.10 MiB | 5.38 s | 5.55 s | 7.60 s | 669.1 MiB |
| variable Unicode | string ndarray | 114.44 MiB | 1.2 KiB | 0.15 s | 0.11 s | 0.12 s | 515.4 MiB |

The selected representation was also measured at every required scale. For
variable Unicode names, uncompressed atomic rewrite/hydration results were:

| Samples | File | Atomic rewrite | Hydration | Peak RSS |
|---:|---:|---:|---:|---:|
| 10k | 1.15 MiB | 0.014 s | 0.007 s | 51.4 MiB |
| 100k | 11.45 MiB | 0.028 s | 0.017 s | 94.4 MiB |
| 1M | 114.44 MiB | 0.102 s | 0.125 s | 516.3 MiB |

Mostly-labeled and mostly-unlabeled arrays had effectively identical
uncompressed costs. At 1M without explicit names, the document was 3.82 MiB,
atomic rewrite was 0.014-0.016 s, hydration was 0.007 s, and peak RSS was
53.7-68.4 MiB. This demonstrates that the roster, not the categorical values,
dominates autosave cost when names are present.

## Second-round compression, JSON, and edit-persistence experiment

The first-round `uncompressed` recommendation was withdrawn after the issue
follow-up. The second round ran 96 isolated cases over the cross product of:

- 10k, 100k, and 1M samples;
- short ASCII and variable-length Unicode names;
- mostly-labeled and mostly-unlabeled values;
- compact UTF-8 JSON, whole-document JSON + zlib, ASDF ndarrays, and ASDF +
  zlib;
- zlib levels 1, 6, and 9 for both compressed candidates.

The JSON candidate preserves the same semantic fields and adds explicit
`values_dtype: int32` and `values_shape: [N]`, since JSON integers do not carry
that information natively. ASDF uses the selected UCS-4 and int32 ndarrays.
Every save writes a temporary file, flushes it, and atomically replaces the
target. Every result is reopened and checked for sample alignment, dtype/shape
declaration, and the changed value. The raw wall time, process CPU time, file
size, and child-process peak RSS are in
`build/asdf-labeling-spike-results/second-round-benchmark.json`.

For ASDF, the experiment also implements the requested realistic label-only
rewrite. It retains the existing YAML metadata, streams the encoded roster
block from the old document into the temporary document without decoding it,
and encodes a new values block. The result is genuine ASDF accepted by the
official Python implementation. Every one-label and 1000-label case verifies
that the roster block is byte-for-byte identical.

At 1M variable-Unicode samples, ranges below cover both value distributions:

| Candidate | File (MiB) | Initial write (s) | Hydrate (s) | Full one-label save (s) | Reused-roster save (s) | Peak RSS (MiB) |
|---|---:|---:|---:|---:|---:|---:|
| A: JSON | 42.964-43.822 | 0.150-0.154 | 0.147-0.156 | 0.153-0.154 | n/a | 413.6-415.4 |
| B1: JSON + zlib 1 | 3.488-3.497 | 0.213-0.216 | 0.174-0.183 | 0.210-0.214 | n/a | 412.1-415.8 |
| B6: JSON + zlib 6 | 3.318-3.320 | 0.388-0.396 | 0.167-0.170 | 0.390-0.397 | n/a | 412.9-415.0 |
| B9: JSON + zlib 9 | 3.171-3.174 | 1.340-1.351 | 0.174-0.177 | 1.375-1.451 | n/a | 412.4-416.5 |
| C: ASDF | 114.442 | 0.151-0.152 | 0.140-0.142 | 0.095-0.096 | 0.085-0.089 | 735.5-736.8 |
| D1: ASDF + zlib 1 | 5.671-5.687 | 0.269-0.270 | 0.171-0.181 | 0.214-0.218 | 0.011 | 638.8-643.7 |
| D6: ASDF + zlib 6 | 4.633 | 0.784-0.825 | 0.172-0.182 | 0.742-0.770 | 0.016-0.018 | 640.6-642.4 |
| D9: ASDF + zlib 9 | 4.435 | 8.375-8.396 | 0.170-0.174 | 8.361-8.445 | 0.016-0.017 | 637.4-638.2 |

The 1M ASCII cases have the same shape. The important frontier points are:

| Candidate | File (MiB) | Full one-label save (s) | Reused-roster save (s) | Hydrate (s) |
|---|---:|---:|---:|---:|
| A: JSON | 22.936-23.795 | 0.095-0.100 | n/a | 0.089-0.093 |
| B6: JSON + zlib 6 | 2.505-2.507 | 0.190-0.202 | n/a | 0.100-0.104 |
| C: ASDF | 76.295 | 0.067-0.075 | 0.057-0.064 | 0.102-0.105 |
| D6: ASDF + zlib 6 | 2.656 | 0.551-0.552 | 0.013-0.014 | 0.118-0.120 |

The level-6 compressed candidates scale as follows; time is one-label atomic
persistence and ranges again cover both value distributions:

| Samples | Roster | JSON zlib 6: MiB / full save | ASDF zlib 6: MiB / full save / reused save |
|---:|---|---:|---:|
| 10k | ASCII | 0.025 / 0.0027-0.0028 s | 0.028 / 0.017-0.019 s / 0.0010-0.0018 s |
| 10k | Unicode | 0.034 / 0.0049-0.0051 s | 0.047 / 0.019-0.020 s / 0.0010-0.0015 s |
| 100k | ASCII | 0.251-0.252 / 0.021 s | 0.266 / 0.068-0.072 s / 0.0027-0.0029 s |
| 100k | Unicode | 0.333 / 0.042 s | 0.461 / 0.082-0.086 s / 0.0025-0.0028 s |
| 1M | ASCII | 2.505-2.507 / 0.190-0.202 s | 2.656 / 0.551-0.552 s / 0.013-0.014 s |
| 1M | Unicode | 3.318-3.320 / 0.390-0.397 s | 4.633 / 0.742-0.770 s / 0.016-0.018 s |

A batch of 1000 changed labels costs effectively the same to persist as one
change because both paths rewrite the complete values representation. At 1M,
ASDF zlib 6 with roster reuse took 0.014-0.015 s for ASCII and 0.017-0.019 s
for Unicode. The synchronous in-memory edit itself took approximately 2-6
microseconds for one label and 0.02-0.10 milliseconds for 1000 labels. This is
the requested user-visible-blocking proxy; no UI dispatcher or background task
was implemented in the spike, so the result must not be presented as a measured
production UI latency.

Level 1 is the initial-write/CPU end of the ASDF frontier. Moving from level 1
to level 6 reduces the 1M file by about 11% for ASCII and 18% for Unicode. Once
the roster block is reused, the steady-state autosave difference is only about
5-7 ms. Level 9 saves only another 4-5% but makes an initial or roster-changing
Unicode rewrite exceed 8 seconds, so it is not recommended. Level 6 is the
better steady-state default for this human-paced, label-only autosave workload;
level 1 remains a valid fallback if source-roster rewrites become frequent.

JSON was not ruled out by raw throughput. JSON zlib 1/6 produces files smaller
than ASDF zlib 1/6 and has competitive hydration and full-rewrite times with a
lower Python-oracle peak RSS. However, whole-document compression cannot reuse
its immutable roster independently. At 1M, ASDF zlib 6 block reuse reduces
label persistence from 0.19-0.40 s for JSON zlib 6 to 0.013-0.018 s. ASDF also
carries dtype and shape in standard ndarray tags instead of application fields.
Those are the measured and semantic reasons to retain ASDF, not a claim that
JSON is intrinsically slow.

The implementation comparison is not free for either candidate. SpecForge has
an in-tree JSON parser, but its generic `JsonValue` model materializes an object
per array integer and would need a bounded streaming/specialized path for this
workload. The minimal ASDF codec is direct-to-vector and now implements native
zlib read/write plus preservation/copying of encoded unchanged blocks. The
compressed writer, reader, and block-reuse paths pass the Python interoperability
matrix. Production still needs to integrate the subset without regressing this
property; recompressing the roster on every edit would require reopening the
JSON decision.

## Missing / unlabeled representation

The ASDF ndarray schema permits a scalar mask sentinel, and official Python
ASDF correctly opened a hand-authored `mask: -1` array as
`[masked, 0, 1, masked]`. However, writing that opened tree to a new ASDF file
with `asdf` 5.3.1 removed the mask field entirely. Reopening the rewrite yielded
an ordinary unmasked array, so the missing semantic did not survive the
required open/rewrite cycle.

The explicit field retained both the declaration and raw `-1` values across
the same cycle:

```yaml
annotation:
  kind: categorical_integer
  values: !core/ndarray-1.0.0
    source: 1
    datatype: int32
    byteorder: little
    shape: [N]
  missing:
    semantic: unlabeled
    value: -1
```

Generic ASDF tools still discover the integer dtype and shape. SpecForge owns
the business interpretation of `-1`, validates that no label definition uses
that code, and keeps labeled values as ordinary categorical integers.

## ASDF standard and core baseline

The same Unicode/string-roster/int32/missing-field document successfully
round-tripped through official Python ASDF for every supported standard version
from 1.0.0 through 1.6.0. Version 1.5.0 was selected as the maintained stable
baseline. It emits:

- `#ASDF 1.0.0`;
- `#ASDF_STANDARD 1.5.0`;
- root `!core/asdf-1.1.0`;
- ndarray `!core/ndarray-1.0.0`.

Standard 1.6.0 changes this contract's ndarray tag to 1.1.0 but provides no
material v1 labeling capability that justifies raising the baseline. The
choice is independent of either native codec's current feature set.

## Binary block profile

The second-round recommended v1 writer profile is intentionally narrow:

- internal blocks only; external/streamed blocks are rejected;
- zlib level 6 for roster and values blocks;
- checksum bytes all zero;
- no block index emitted; readers may ignore a present standard block index;
- little-endian int32 values and little-endian UCS-4 roster strings;
- label-only atomic rewrites preserve the unchanged encoded roster block and
  re-encode only the values block;
- exact checked arithmetic for header, allocated/used/data sizes, shape, item
  width, source index, and payload bounds.

The second round demonstrates that the first-round 5-7x full-recompression
multiplier was the wrong steady-state model. With an immutable encoded roster
copied verbatim, 1M-sample level-6 label persistence takes 13-19 ms rather than
0.55-0.77 s. Level 6 retains the substantial storage reduction without making
the roster pay that cost on every edit.

The native subset now reads both uncompressed and zlib blocks, requires the
declared decoded size to equal the ndarray shape/dtype before allocation,
rejects truncated/corrupt streams, and writes level-6 zlib blocks. Its
label-value rewrite parses and re-encodes only the values block, copies the
encoded roster block verbatim, preserves the original metadata bytes, and emits
no block index. Nonzero checksums and unsupported compression identifiers remain
controlled rejections.

## Golden fixtures and interoperability

The corpus covers:

- minimal document;
- Unicode source/task/label/sample identity;
- folder roster;
- NPY source with companion sample names;
- NPY source-index roster without names;
- mixed labeled/unlabeled values;
- 100k-value large array;
- forward-compatible unknown fields;
- count mismatch, duplicate label code, sentinel collision, and undefined
  categorical value;
- a standard-valid zlib input that becomes a native acceptance fixture for the
  recommended profile, plus a checksum input that remains outside it;
- bad magic, invalid YAML, a truncated binary block, and a corrupt zlib payload.

All 9 approved Python-written fixtures, including the two-block Unicode zlib
profile, were read by the native spike and compared semantically equal. The
checksum fixture and all 8 malformed or semantic-invalid fixtures returned
controlled native error code 2. The native writer emitted zlib for both Unicode
roster and values blocks; Python ASDF opened and semantically validated it. The
native rewrite of the Python zlib fixture was accepted by both readers, and its
roster block was byte-for-byte identical to the input. Invalid rewrite index and
undefined replacement label also returned code 2. No tested input called
`abort`, `exit`, or crashed the process.

Unknown fields are accepted and ignored by the native reader. The label-only
block-reuse rewrite preserves the metadata prefix byte-for-byte, so unknown
metadata survives that path. A full native object-model read/write still drops
unknown nodes; production must retain them or explicitly define that a full
canonical reconstruction may discard them.

## Native codec comparison

### asdf-cxx 8.0.0

The evaluated upstream main revision was
`6f39fb7a3dfdc8e3163cadb41dab75d893485dab` (2,475 lines across its nine `.cxx`
files, excluding headers). Its own conformance notes and source establish:

- fixed-length string ndarrays are unsupported, so it cannot implement the
  approved roster;
- malformed input commonly reaches assertions, `std::abort`, or `exit(2)`;
- its yaml-cpp emitter produces YAML 1.2 while declaring YAML 1.1;
- there is no Windows/MSVC CI job;
- yaml-cpp is required, with optional OpenSSL and several compression
  libraries;
- its block index is recreated rather than used, and its Python comparison
  tests are still listed as work to do.

Required production patches would include string ndarray read/write, a
library-wide structured error model replacing process termination, YAML 1.1
conformance work, MSVC/vcpkg integration, and the same golden/interoperability
matrix. Those are not a narrow local delta, so this candidate is rejected for
v1 rather than patched into SpecForge.

Its required matrix therefore fails at contract coverage rather than producing
a misleading partial pass: Python writer -> asdf-cxx reader cannot consume the
approved string roster, and asdf-cxx writer -> Python validator cannot emit a
complete approved document. Malformed-input safety also fails before patching.
No compliant MSVC artifact exists to measure; the relevant footprint evidence
is the 2,475-line implementation plus mandatory yaml-cpp and optional checksum /
compression libraries.

### Strict minimal native subset

The implemented subset is 1,147 C++ lines across codec/header/CLI and depends on
yaml-cpp plus zlib 1.3.2 (zlib is already a production dependency). It supports
the approved metadata maps/sequences/scalars, UCS-4 string roster ndarray,
int32 values ndarray, one or two internal blocks, little/big-endian reads,
little-endian level-6 zlib writes, exact-size bounded zlib decompression,
unknown-field tolerance, business invariant validation, and label-only rewrites
that preserve the encoded roster block. It explicitly rejects streamed,
checksummed, corrupt/truncated-zlib, oversized, or otherwise unsupported input.

The dependency is isolated behind the non-default vcpkg
`asdf-labeling-spike` feature and dedicated CMake presets. Ordinary production
presets remove yaml-cpp. The
measured artifacts were:

- static MSVC Release executable: 578,048 bytes;
- dynamic MSVC Debug executable: 931,328 bytes;
- dynamic Debug yaml-cpp DLL: 996,352 bytes.
- dynamic Debug zlib DLL: 214,016 bytes.

This route remains the recommended architecture because the code and dependency
surface track the contract directly, its errors are controllable, the static
footprint remains bounded, and it now passes the compressed read, compressed
write, and block-reuse interoperability directions.

| Candidate | Python writer -> native reader | Native writer -> Python validator | Malformed input | Decision |
|---|---|---|---|---|
| asdf-cxx 8.0.0 | Fail: no string ndarray | Fail: cannot emit approved roster | Fail: abort/exit paths | Reject |
| strict minimal subset | Pass: 9/9 approved fixtures, including Python zlib | Pass: both blocks zlib; Python semantic equality | 9/9 golden rejections plus 2 invalid rewrites controlled | Recommend |

## Remaining blockers before production use

The spike is evidence, not the #74 production implementation. Before locking
the production codec, #74 must still:

1. integrate the subset as a library with the existing output lease,
   write-ahead recovery, retry, and atomic-file owner;
2. replace the spike's whole-file `ReadAll` seam with the production streaming/
   mapped I/O owner and measure the native reuse path itself at 1M scale;
3. decide and test unknown-field preservation on rewrite;
4. fuzz YAML/block headers, integer bounds, Unicode, and truncated inputs;
5. continue rejecting nonzero checksums unless separately justified and broaden
   fuzz coverage beyond the checked corrupt-zlib/truncated fixtures;
6. add CI provisioning for the pinned Python oracle and dedicated native-spike
   preset.

These blockers do not change the approved semantic representation.
