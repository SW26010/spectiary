# ASDF labeling v1 representation and native codec spike

> Historical experiment and implementation snapshot for #77/#74. This page
> preserves the original benchmark results, codec comparison, and integration
> narrative; it is not the current schema or persistence specification.
> The sparse pending recovery overlay and write-ahead recovery described below
> were superseded by #108/#109: formal dirty edits now remain in memory, and
> canonical saves do not depend on a local WAL. Current contracts are
> [canonical ASDF schema](../../reference/labeling/canonical_asdf_schema.md) and
> [labeling persistence ownership](../../reference/labeling/labeling_persistence_ownership.md).
> Later hardening evidence is in
> [ASDF production hardening](../asdf-labeling-production-hardening/README.md).

The historical use of “production”, “current”, and “now” below refers to the
integration stage recorded here. Benchmark measurements and retained evidence
are not updated to imply a new validation run.

## Historical decision summary

1. Explicit sample names use a standard ASDF one-dimensional Unicode string
   ndarray (`datatype: [ucs4, width]`). A source-index roster omits the names
   array and declares `identity_kind: source_index`.
2. Unlabeled values remain ordinary `int32` values containing `-1`, with the
   business meaning declared by `annotation.missing: {semantic: unlabeled,
   value: -1}`. Do not use scalar `ndarray.mask: -1` in v1.
3. The wire baseline is ASDF Standard 1.5.0, root `core/asdf-1.1.0`, and
   `core/ndarray-1.0.0`. ASDF file format remains 1.0.0.
4. The production writer profile uses internal zlib level 6 blocks, zero
   checksum, and little-endian `int32`/UCS-4 payloads. Fresh and full-document
   writes may omit the optional block index; timestamped value autosaves emit a
   newly calculated index for their new block offsets. Such an autosave rebuilds
   the YAML metadata from the durable tree, copies only the unchanged encoded
   roster block verbatim, and re-encodes the values block. The promoted
   production codec component implements and interoperably validates this path.
   The domain document store now owns source-aware open, durable-base reuse,
   and atomic replacement. The
   controller hydrates persisted canonical owners under their one-file output
   lease, retains the opened generation, and publishes value-only or full
   metadata generations through the store with write-ahead recovery and retry.
   Every produced generation carries the narrow build-source tuple compiled
   into the writer: `head` plus a full lowercase 40-hex revision, or
   `working_tree` with no revision. A production rewrite refreshes this tuple
   even when it reuses the encoded roster block.
5. The checked-in corpus contains 10 approved valid documents, 1 standard-valid
   document outside the narrow wire profile, 14 ASDF-valid semantic violations,
   and 4 structurally malformed documents.
6. The strictly scoped native codec is now the production component. `asdf-cxx`
   8.0.0 is not recommended without a substantially broader patch than its
   current error and string-array model permits.
7. Compact JSON + whole-document zlib is competitive in size and full-rewrite
   throughput, so JSON is not rejected on performance grounds. ASDF remains the
   canonical recommendation because its standard typed arrays and independently
   encoded blocks permit a verified 13-19 ms 1M-sample autosave path. The native
   production component retains the same block-preserving wire operation; a
   future regression of that property would reopen the JSON decision.

## Historical production architecture

The #74 integration was recorded as complete at this stage:

- temporary tasks remain internal drafts until explicit Save As formalizes
  them as single-file canonical ASDF owners;
- task state persists the formal owner path and format explicitly, while a
  canonical owner's long-term cache record remains structural and stores only
  local workflow state plus a sparse pending recovery overlay;
- activation acquires the task and one-file output leases before reopening the
  ASDF through the document store; the durable snapshot/generation remains a
  runtime-only controller fact;
- value-only autosaves re-emit timestamped, unknown-preserving metadata, reuse
  only the encoded roster block, re-encode values, and generate a block index
  for the new offsets; task and label metadata edits publish one complete
  metadata-and-values generation;
- failed publication retains the last trusted ASDF generation and the newest
  write-ahead overlay, then retries against a reopened current durable base;
- standalone ASDF documents can be explicitly adopted without rewriting their
  bytes, and legacy NPY + sidecar owners can be explicitly migrated without
  silently changing the legacy artifacts;
- NPY export is a one-shot stateless operation and never becomes a persistence
  owner or autosave target.

The required native `asdf-lifecycle` CTest label locks the document, codec,
store, controller, workflow, and restart/session contracts. The dedicated
pinned-Python interoperability test continues to validate both writer/reader
directions independently of the production runtime.

## Reference environment

The experiment was run on Windows with Python 3.12.13, `asdf` 5.3.1,
`asdf-standard` 1.5.0, NumPy 2.5.2, and psutil 7.0.0. Dependencies are pinned in
`tools/asdf_labeling_spike/requirements.txt`. Generated benchmark evidence is
written to the ignored `build/asdf-labeling-spike-results` directory; every
experiment is reproducible from `tools/asdf_labeling_spike/README.md`.

The golden corpus is in `tests/fixtures/asdf_labeling`. Its `manifest.json`
pins SHA-256 for every ASDF fixture and records the expected structural and
semantic result. Semantic JSON companions deliberately compare the durable
meaning, not Python or C++ codec object types. The production native codec test
verifies every listed ASDF hash before using any fixture expectation, so ASDF
byte drift is a required CTest failure.

The production writer identity is represented at the root as:

```yaml
asdf_library: !core/software-1.0.0
  name: Spectiary
  version: 0.8.0
spectiary_build:
  source_mode: head
  source_revision: "0123456789abcdef0123456789abcdef01234567"
format_kind: "spectiary.sample_labeling"
schema_version: "2.0.0"
```

The checked fixture corpus uses this deterministic `head` tuple except for the
minimal fixture, which exercises `working_tree` with no revision; the official
Python writer supplies its own `asdf_library` provenance. The production writer
stamps its compiled current build identity instead.

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

For ASDF, the second-round benchmark prototype also implements the requested
realistic label-only rewrite. It retains the existing YAML metadata, streams the
encoded roster block from the old document into the temporary document without
decoding it, and encodes a new values block. Those measurements isolate the
roster-reuse benefit; the promoted production fast path additionally rebuilds
timestamped metadata and emits a block index for the resulting offsets. Both
forms are genuine ASDF accepted by the official Python implementation. Every
one-label and 1000-label case verifies that the roster block is byte-for-byte
identical.

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

The implementation comparison is not free for either candidate. At the time of
this spike, Spectiary used an in-tree JSON parser and DOM. Issue #88 replaces
them with `nlohmann::json`; that general DOM still stores a value per array
integer and would need a bounded streaming/specialized path for this workload.
The promoted production ASDF codec is direct-to-vector and implements
native zlib read/write plus preservation/copying of encoded unchanged blocks.
Its compressed writer, reader, and block-reuse paths pass the Python
interoperability matrix. The atomic document store now owns durable-base and
replacement lifecycle below the controller. Persisted ASDF owners open through
that store after their output lease is acquired, with local sparse recovery
overlays applied above the canonical generation. Value and metadata edits now
publish through that store, and ASDF-specific failure/recovery/retry is part of
the production owner lifecycle. Recompressing the roster on every edit would
still require reopening the JSON decision.

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
  alignment:
    mode: by_index
    target: sample_roster
  values: !core/ndarray-1.0.0
    source: 1
    datatype: int32
    byteorder: little
    shape: [N]
  missing:
    semantic: unlabeled
    value: -1
```

Generic ASDF tools still discover the integer dtype and shape. Spectiary owns
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
- fresh and full-document writes omit the optional block index; a timestamped
  value rewrite emits a newly calculated standard index for its new block
  offsets, and readers validate any present index against the scanned blocks;
- little-endian int32 values and little-endian UCS-4 roster strings;
- label-only atomic rewrites rebuild canonical metadata with supported unknown
  mappings, preserve only the unchanged encoded roster block, and re-encode the
  values block; known `spectiary_build` fields are refreshed from the current
  binary while unknown entries in that map are preserved;
- a roster is eligible for durable verbatim reuse only when its zlib stream
  declares `FLEVEL=2`, the level-6 production class; other valid zlib levels
  remain compatibility-readable but cannot seed a durable rewrite;
- exact checked arithmetic for header, allocated/used/data sizes, shape, item
  width, source index, and payload bounds.

The second round demonstrates that the first-round 5-7x full-recompression
multiplier was the wrong steady-state model. With an immutable encoded roster
copied verbatim, 1M-sample level-6 label persistence takes 13-19 ms rather than
0.55-0.77 s. Level 6 retains the substantial storage reduction without making
the roster pay that cost on every edit.

The second-round experimental native subset reads both uncompressed and zlib
blocks, requires the declared decoded size to equal the ndarray shape/dtype
before allocation,
rejects truncated/corrupt streams, and writes level-6 zlib blocks. Its
label-value rewrite parses and re-encodes only the values block, copies the
encoded roster block verbatim, preserves the original metadata bytes, and emits
no block index. The promoted production codec instead rebuilds timestamped
metadata and emits a recalculated index while retaining the same roster-only
reuse property. Nonzero checksums and unsupported compression identifiers
remain controlled rejections.

Production compression is file-backed and bounded: values and UCS-4 roster
words are generated in fixed-size chunks, streamed through level-6 deflate to
temporary spools, then emitted as a small block header followed by the spool.
The codec never holds a raw ndarray payload, a complete compressed payload, and
a second complete block copy at the same time.

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
production writer's complete YAML-special scalar matrix is escaped and
round-trips unchanged through the pinned official ASDF 5.3.1 reader. The same
oracle opens production explicit-roster and source-index full writes and durable
rewrites, compares every canonical semantic field, and verifies that the
encoded roster block is byte-for-byte identical across the explicit-roster
rewrite. Python-origin explicit-roster and source-index fixtures are also
rewritten through the production component and then compared by the official
reader. A differential duplicate-key document is interpreted with the last
value by official ASDF 5.3.1 but is rejected by the production reader/rewrite
before any output is created. Invalid rewrite index and undefined replacement
label return code 2. No tested input called `abort`, `exit`, or crashed the
process.

Unknown fields are accepted without projecting them into the canonical domain
model. They remain owned by the validated durable generation instead. The
timestamped block-reuse rewrite reparses that validated tree, replaces the known
canonical fields from the complete replacement document, preserves supported
unknown mappings, and copies only the exact encoded roster block. A
metadata-changing rewrite uses the same unknown-preserving tree builder but
re-encodes the roster as well. Unknown entries are retained at the root and
inside the standard software, source, roster, ndarray descriptor,
annotation/missing, task, and
label maps; label-entry metadata follows the stable label code across name or
shortcut edits. Removed labels and roster constructs do not retain metadata
that belonged only to the removed known entity.

The merge preserves YAML scalar types rather than only scalar text. Parsed
quoted/string-tagged unknown scalars and every replacement canonical string are
emitted with an explicit YAML string type, while genuine unknown booleans and
integers retain their non-string types. The pinned ASDF 5.3.1 oracle checks
ambiguous spellings such as string `"true"`, string `"1"`, numeric shortcut
`"1"`, boolean `true`, and integer `1` by runtime type rather than by `str(...)`
normalization.

This preservation path deliberately requires a durable base from a successful
read. A detached canonical object can create a new document, but it cannot
claim ownership of unknown metadata that it never observed. The atomic store
uses the preservation rewrite for snapshot-based metadata edits and also when a
path-based full write finds an existing readable production-profile v1 file.
If an existing file cannot provide a safe durable base, replacement is rejected
instead of silently discarding forward metadata. Every successful full rewrite
starts a new generation and requires reopening before another rewrite. The
source/roster identity, annotation kind, and stable task id must still match the
opened generation; preservation is rejected rather than carrying opaque fields
into a different logical document. This identity is bound into the codec's
durable state and enforced by the public rewrite primitive itself; the store
check provides an earlier domain-specific error but is not the only guard.

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
v1 rather than patched into Spectiary.

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

The dependency entered the production manifest when this codec was promoted
from the spike. Production presets now require yaml-cpp alongside zlib; the
dedicated spike presets remain only for reproducing the #77 measurements. The
measured artifacts were:

- static MSVC Release executable: 578,048 bytes;
- dynamic MSVC Debug executable: 931,328 bytes;
- dynamic Debug yaml-cpp DLL: 996,352 bytes.
- dynamic Debug zlib DLL: 214,016 bytes.

The production profile audit distinguishes compatibility reads from durable
emission. The reader may hydrate zero-checksum uncompressed or big-endian
ASDF 1.5 inputs so approved research fixtures can be migrated, but such an
explicit roster never becomes a reusable durable base. Every full write and
label-only rewrite emits the fixed little-endian, zlib-level-6, zero-checksum
block profile; the label-only path also emits a recalculated block index. Reader
hydration, full writes, and durable rewrites all run the same shape/resource
preflight for sample and roster counts, ndarray sizes, metadata, durable
metadata/roster bytes, and file sizes. Its 512 MiB contract covers
codec-controlled bulk allocations, decoded payloads, canonical text, and explicitly sized
scratch buffers; it is not a strict bound on process RSS, yaml-cpp's internal
DOM, allocator overhead, or platform library internals. Before copying an
alias-expanded scalar into the canonical document, the reader accounts that
canonical text and the label vector/sorted-index capacity. The writer uses a
counting stream to obtain the exact escaped metadata size and performs the same
preflight before reserving or building the YAML buffer. Roster uniqueness uses
a roster-sized sorted index vector instead of copied strings in hash nodes, so
its scratch capacity is included exactly; writer/rewrite compression uses
fixed-memory, file-backed deflate spools whose codec scratch allowance is
included in the same preflight. All declared-size arithmetic is checked,
profile counts have explicit limits, and allocation failures return controlled
errors.

Every YAML mapping in the accepted profile, including unknown nested mappings,
must use unique scalar keys. The production reader checks this recursively
before field extraction and refuses to create a durable base for ambiguous
metadata. This is required because yaml-cpp 0.9 selects the first duplicate
while the pinned ASDF 5.3.1 oracle selects the last.

Canonical YAML scalars are emitted as double-quoted UTF-8. The writer escapes
the full C0/DEL/C1 control ranges, quote and backslash, NEL/NBSP, Unicode line
and paragraph separators, BOM, and the YAML boundary noncharacters. The native
reader and pinned ASDF 5.3.1 oracle both round-trip the complete scalar matrix
without semantic changes.

This route remains the recommended architecture because the code and dependency
surface track the contract directly, its errors are controllable, the static
footprint remains bounded, and it now passes the compressed read, compressed
write, and block-reuse interoperability directions.

| Candidate | Python writer -> native reader | Native writer -> Python validator | Malformed input | Decision |
|---|---|---|---|---|
| asdf-cxx 8.0.0 | Fail: no string ndarray | Fail: cannot emit approved roster | Fail: abort/exit paths | Reject |
| strict minimal subset | Pass: 9/9 approved fixtures, including Python zlib | Pass: both blocks zlib; Python semantic equality | 9/9 golden rejections plus 2 invalid rewrites controlled | Recommend |

## Historical production hardening follow-up

The #74 production component now uses bounded streaming block I/O instead of
the spike's whole-file `ReadAll` seam, and its atomic store is fully connected
to output ownership, write-ahead recovery, publication, and retry. That product
lifecycle work is complete.

Remaining hardening is tracked separately in
[GitHub issue #78](https://github.com/SW26010/spectiary/issues/78):

1. benchmark the exact production native reuse/read/write paths at 1M scale;
2. broaden bounded fuzz/property coverage for YAML, block headers/indexes,
   Unicode, integer/offset/shape limits, truncated inputs, and unknown fields;
3. re-evaluate checksum policy only with separate interoperability and cost
   evidence.

These items do not change the approved semantic representation or production
owner state machine and are not blockers for #74 product acceptance. The pinned
Python oracle and dedicated native-spike preset remain enforced by the required
`spectiary_asdf_labeling_interoperability` CTest in CI.
